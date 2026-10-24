/*
 * Experimental in-game volume OSD for Open PS2 Loader.
 *
 * This implementation transfers a small bitmap into the active GS display
 * framebuffer. It deliberately avoids modifying the game's normal drawing
 * context registers. It is still a proof of concept: framebuffer updates and
 * DMA scheduling vary between games and require testing on real hardware.
 */
#include <tamtypes.h>
#include <kernel.h>
#include <string.h>
#include "volume_overlay.h"

#define OVL_WIDTH              128
#define OVL_HEIGHT             32
#define OVL_PIXELS             (OVL_WIDTH * OVL_HEIGHT)
#define OVL_MAX_IMAGE_QWORDS   (OVL_PIXELS / 4)
#define OVL_PACKET_QWORDS      (OVL_MAX_IMAGE_QWORDS + 10)
#define OVL_VISIBLE_FRAMES     90

#define VIF1_STAT              ((volatile u32 *)0x10003C00)
#define VIF1_STAT_BUSY_MASK    0x1F000000
#define VIF1_STAT_FDR           (1 << 23)
#define VIF1_NOP                0
#define VIF1_MSKPATH3(mask)    ((u32)(mask) | ((u32)0x06 << 24))
#define VIF1_FLUSHA             ((u32)0x13 << 24)
#define VIF1_DIRECT(count)     ((u32)(count) | ((u32)0x50 << 24))

#define DMA_VIF1_CHCR          ((volatile u32 *)0x10009000)
#define DMA_VIF1_MADR          ((volatile u32 *)0x10009010)
#define DMA_VIF1_QWC           ((volatile u32 *)0x10009020)
#define GS_CSR                 ((volatile u64 *)0x12001000)
#define GS_CSR_FINISH          (1ULL << 1)

#define GS_PMODE               ((volatile u64 *)0x12000000)
#define GS_DISPFB1              ((volatile u64 *)0x12000070)
#define GS_DISPLAY1             ((volatile u64 *)0x12000080)
#define GS_DISPFB2              ((volatile u64 *)0x12000090)
#define GS_DISPLAY2             ((volatile u64 *)0x120000A0)

#define GS_GIF_AD               0x0E
#define GS_BITBLTBUF            0x50
#define GS_TRXPOS               0x51
#define GS_TRXREG               0x52
#define GS_TRXDIR               0x53
#define GS_FINISH                0x61

#define GS_PSM_CT32              0x00
#define GS_PSM_CT24              0x01
#define GS_PSM_CT16              0x02
#define GS_PSM_CT16S             0x0A

#define PMODE_EN1(x)             ((x) & 0x01)
#define PMODE_EN2(x)             (((x) >> 1) & 0x01)
#define DISPFB_FBP(x)            ((u16)((x) & 0x1FF))
#define DISPFB_FBW(x)            ((u8)(((x) >> 9) & 0x3F))
#define DISPFB_PSM(x)            ((u8)(((x) >> 15) & 0x1F))
#define DISPFB_DBX(x)            ((u16)(((x) >> 32) & 0x7FF))
#define DISPFB_DBY(x)            ((u16)(((x) >> 43) & 0x7FF))
#define DISPLAY_DW(x)            ((u16)(((x) >> 32) & 0xFFF))
#define DISPLAY_MAGH(x)          ((u8)(((x) >> 23) & 0xF))

#define GIF_TAG(nloop, eop, flg, nreg) \
    ((u64)(nloop) | ((u64)(eop) << 15) | ((u64)(flg) << 58) | ((u64)(nreg) << 60))

static u128 VolumeOverlayPacket[OVL_PACKET_QWORDS] __attribute__((aligned(64)));
static int VolumeOverlayPercent = 100;
static int VolumeOverlayFramesLeft;

static u32 Colour32(u8 r, u8 g, u8 b)
{
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0x80000000U;
}

static u16 Colour16(u8 r, u8 g, u8 b)
{
    return (u16)(((r >> 3) & 0x1F) |
                 (((g >> 3) & 0x1F) << 5) |
                 (((b >> 3) & 0x1F) << 10) | 0x8000);
}

static void PutPixel(int index, u8 psm, u8 r, u8 g, u8 b)
{
    if (psm == GS_PSM_CT32 || psm == GS_PSM_CT24) {
        u32 *pixels = (u32 *)&VolumeOverlayPacket[7];
        pixels[index] = Colour32(r, g, b);
    } else {
        u16 *pixels = (u16 *)&VolumeOverlayPacket[7];
        pixels[index] = Colour16(r, g, b);
    }
}

static void FillRect(u8 psm, int x, int y, int w, int h,
                     u8 r, u8 g, u8 b)
{
    int xx, yy;
    if (w <= 0 || h <= 0)
        return;
    for (yy = y; yy < y + h; yy++) {
        for (xx = x; xx < x + w; xx++) {
            if (xx >= 0 && xx < OVL_WIDTH && yy >= 0 && yy < OVL_HEIGHT)
                PutPixel(yy * OVL_WIDTH + xx, psm, r, g, b);
        }
    }
}

static u8 GlyphRow(char ch, int row)
{
    static const u8 digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
        {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12}
    };
    static const u8 letter_v[7] = {17,17,17,17,17,10,4};
    static const u8 letter_o[7] = {14,17,17,17,17,17,14};
    static const u8 letter_l[7] = {16,16,16,16,16,16,31};
    static const u8 percent[7] = {17,2,4,8,16,17,0};

    if (row < 0 || row >= 7)
        return 0;
    if (ch >= '0' && ch <= '9')
        return digits[ch - '0'][row];
    switch (ch) {
        case 'V': return letter_v[row];
        case 'O': return letter_o[row];
        case 'L': return letter_l[row];
        case '%': return percent[row];
        default: return 0;
    }
}

static void DrawText(u8 psm, int x, int y, const char *text,
                     u8 r, u8 g, u8 b)
{
    int i, col, row;
    for (i = 0; text[i] != '\0'; i++) {
        for (row = 0; row < 7; row++) {
            u8 bits = GlyphRow(text[i], row);
            for (col = 0; col < 5; col++) {
                if (bits & (1 << (4 - col)))
                    PutPixel((y + row) * OVL_WIDTH + x + i * 6 + col,
                             psm, r, g, b);
            }
        }
    }
}

static void BuildOverlayImage(u8 psm, int percent)
{
    int i, fill;
    char text[5];
    int digits = 0;
    int value = percent;

    if (value >= 100) {
        text[digits++] = (char)('0' + value / 100);
        text[digits++] = (char)('0' + (value / 10) % 10);
        text[digits++] = (char)('0' + value % 10);
    } else if (value >= 10) {
        text[digits++] = (char)('0' + value / 10);
        text[digits++] = (char)('0' + value % 10);
    } else {
        text[digits++] = (char)('0' + value);
    }
    text[digits++] = '%';
    text[digits] = '\\0';

    for (i = 0; i < OVL_PIXELS; i++)
        PutPixel(i, psm, 20, 22, 30);

    /* Subtle frame and a compact volume bar. */
    FillRect(psm, 0, 0, OVL_WIDTH, 1, 100, 110, 130);
    FillRect(psm, 0, OVL_HEIGHT - 1, OVL_WIDTH, 1, 100, 110, 130);
    FillRect(psm, 0, 0, 1, OVL_HEIGHT, 100, 110, 130);
    FillRect(psm, OVL_WIDTH - 1, 0, 1, OVL_HEIGHT, 100, 110, 130);

    DrawText(psm, 8, 5, "VOL", 245, 247, 255);
    DrawText(psm, OVL_WIDTH - 8 - (digits * 6 - 1), 5,
             text, 245, 247, 255);

    FillRect(psm, 8, 18, 112, 8, 100, 110, 130);
    FillRect(psm, 9, 19, 110, 6, 35, 38, 48);
    fill = (percent * 110) / 150;
    if (fill > 110)
        fill = 110;
    if (fill < 0)
        fill = 0;
    FillRect(psm, 9, 19, fill, 6, 40, 190, 245);
}

static int SupportedPSM(u8 psm)
{
    return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 ||
           psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
}

/*
 * Return 1 if written, 0 if the VIF1/DMA path is busy (retry next poll),
 * and -1 when the current display format cannot be handled.
 */
static int DrawOnFramebuffer(u64 dispfb, u64 display, int percent)
{
    u16 fbp = DISPFB_FBP(dispfb);
    u8 fbw = DISPFB_FBW(dispfb);
    u8 psm = DISPFB_PSM(dispfb);
    u16 dbx = DISPFB_DBX(dispfb);
    u16 dby = DISPFB_DBY(dispfb);
    u16 dw = DISPLAY_DW(display);
    u8 magh = DISPLAY_MAGH(display);
    int visible_width, x, y;
    int qwords, direct_qwords, total_qwords;
    u64 bitbltbuf, trxpos, trxreg;
    u32 *vif;
    u64 *gif;
    u32 chcr_state;

    if (!SupportedPSM(psm))
        return -1;
    if (fbw < 3)
        return -1;

    visible_width = ((int)dw + 1) / ((int)magh + 1);
    if (visible_width < OVL_WIDTH + 16 || visible_width > ((int)fbw << 6))
        visible_width = (int)fbw << 6;

    x = (int)dbx + visible_width - OVL_WIDTH - 8;
    if (x + OVL_WIDTH > ((int)fbw << 6))
        x = ((int)fbw << 6) - OVL_WIDTH - 8;
    y = (int)dby + 16;
    if (x < 0 || y < 0 || x + OVL_WIDTH > ((int)fbw << 6) ||
        y + OVL_HEIGHT > 2048)
        return -1;

    BuildOverlayImage(psm, percent);
    qwords = (psm == GS_PSM_CT32 || psm == GS_PSM_CT24)
                 ? OVL_PIXELS / 4
                 : OVL_PIXELS / 8;

    /* If the game's VIF1/GIF transfer is active, skip this frame. */
    if ((*DMA_VIF1_CHCR & 0x100) ||
        ((*VIF1_STAT & VIF1_STAT_BUSY_MASK) != 0))
        return 0;

    bitbltbuf = ((u64)(fbp << 5) << 32) |
                ((u64)fbw << 48) | ((u64)psm << 56);
    trxpos = ((u64)x << 32) | ((u64)y << 48);
    trxreg = (u64)OVL_WIDTH | ((u64)OVL_HEIGHT << 32);

    /*
     * One VIF1 DIRECT packet:
     *   5 qwords for BITBLTBUF/TRXPOS/TRXREG/TRXDIR,
     *   1 IMAGE tag + image qwords,
     *   2 qwords to issue GS FINISH,
     * followed by a VIF command to unmask GIF PATH3.
     */
    vif = (u32 *)&VolumeOverlayPacket[0];
    vif[0] = VIF1_NOP;
    vif[1] = VIF1_MSKPATH3(0x8000);
    vif[2] = VIF1_FLUSHA;
    direct_qwords = qwords + 8;
    vif[3] = VIF1_DIRECT(direct_qwords);

    gif = (u64 *)&VolumeOverlayPacket[1];
    gif[0] = GIF_TAG(4, 0, 0, 1);
    gif[1] = GS_GIF_AD;
    gif[2] = bitbltbuf; gif[3] = GS_BITBLTBUF;
    gif[4] = trxpos;    gif[5] = GS_TRXPOS;
    gif[6] = trxreg;    gif[7] = GS_TRXREG;
    gif[8] = 0;         gif[9] = GS_TRXDIR;

    gif = (u64 *)&VolumeOverlayPacket[6];
    gif[0] = GIF_TAG(qwords, 1, 2, 0);
    gif[1] = 0;

    gif = (u64 *)&VolumeOverlayPacket[7 + qwords];
    gif[0] = GIF_TAG(1, 1, 0, 1);
    gif[1] = GS_GIF_AD;
    gif[2] = 1;
    gif[3] = GS_FINISH;

    vif = (u32 *)&VolumeOverlayPacket[9 + qwords];
    vif[0] = VIF1_MSKPATH3(0);
    vif[1] = VIF1_NOP;
    vif[2] = VIF1_NOP;
    vif[3] = VIF1_NOP;

    total_qwords = qwords + 10;
    *GS_CSR = GS_CSR_FINISH;
    FlushCache(0);

    /*
     * Recheck immediately before claiming VIF1. A game thread can start a
     * transfer while the overlay packet is being prepared.
     */
    if ((*DMA_VIF1_CHCR & 0x100) ||
        ((*VIF1_STAT & VIF1_STAT_BUSY_MASK) != 0))
        return 0;
    chcr_state = *DMA_VIF1_CHCR;

    *DMA_VIF1_QWC = total_qwords;
    *DMA_VIF1_MADR = (u32)VolumeOverlayPacket;
    *DMA_VIF1_CHCR = 0x101;
    asm volatile("sync.l");

    while (*DMA_VIF1_CHCR & 0x100)
        ;
    while ((*GS_CSR & GS_CSR_FINISH) == 0)
        ;
    while ((*VIF1_STAT & VIF1_STAT_BUSY_MASK) != 0)
        ;

    *DMA_VIF1_CHCR = chcr_state;
    asm volatile("sync.l");
    return 1;
}

static int DrawVolumeOverlay(int percent)
{
    u64 pmode = *GS_PMODE;
    u64 dispfb1 = *GS_DISPFB1;
    u64 display1 = *GS_DISPLAY1;
    u64 dispfb2 = *GS_DISPFB2;
    u64 display2 = *GS_DISPLAY2;
    int result, drawn = 0;

    if (PMODE_EN1(pmode)) {
        result = DrawOnFramebuffer(dispfb1, display1, percent);
        if (result < 0)
            return -1;
        if (result == 0)
            return 0;
        drawn = 1;
    }

    if (PMODE_EN2(pmode)) {
        /* Avoid duplicate transfers if both circuits scan out one framebuffer. */
        if (!(PMODE_EN1(pmode) &&
              DISPFB_FBP(dispfb1) == DISPFB_FBP(dispfb2) &&
              DISPFB_PSM(dispfb1) == DISPFB_PSM(dispfb2))) {
            result = DrawOnFramebuffer(dispfb2, display2, percent);
            if (result < 0)
                return -1;
            if (result == 0)
                return 0;
            drawn = 1;
        }
    }

    return drawn ? 1 : -1;
}

void VolumeOverlaySetPercent(int percent)
{
    if (percent < 0)
        percent = 0;
    if (percent > 150)
        percent = 150;
    VolumeOverlayPercent = percent;
    VolumeOverlayFramesLeft = OVL_VISIBLE_FRAMES;
}

void VolumeOverlayTick(void)
{
    int result;

    if (VolumeOverlayFramesLeft <= 0)
        return;

    result = DrawVolumeOverlay(VolumeOverlayPercent);
    if (result > 0)
        VolumeOverlayFramesLeft--;
    else if (result < 0)
        VolumeOverlayFramesLeft = 0;
}
