#include <libsd.h>
#include <loadcore.h>
#include <sifrpc.h>
#include <thbase.h>

#include "volume_rpc.h"

IRX_ID("volctrl", 1, 1);

static SifRpcDataQueue_t volume_rpc_queue;
static SifRpcServerData_t volume_rpc_server;
static unsigned char volume_rpc_buffer[64] __attribute__((aligned(64)));

static s16 master_volume_baseline[4];
static int master_volume_baseline_valid;

static void *VolumeControlRpcHandler(int command, void *data, int size)
{
    int percent, i;
    s32 scaled;
    static const u16 params[4] = {
        SD_PARAM_MVOLL, SD_PARAM_MVOLR, SD_PARAM_MVOLL, SD_PARAM_MVOLR
    };
    static const u16 cores[4] = {0, 0, 1, 1};

    if (command != OPL_VOLUME_RPC_SET || data == NULL || size < sizeof(int))
        return data;

    percent = *(int *)data;
    if (percent < 0) percent = 0;
    if (percent > OPL_VOLUME_MAX_PERCENT) percent = OPL_VOLUME_MAX_PERCENT;

    if (!master_volume_baseline_valid) {
        for (i = 0; i < 4; i++)
            master_volume_baseline[i] = (s16)sceSdGetParam(cores[i] | params[i]);
        master_volume_baseline_valid = 1;
    }

    for (i = 0; i < 4; i++) {
        scaled = ((s32)master_volume_baseline[i] * percent) / 100;
        if (scaled > 0x7FFF) scaled = 0x7FFF;
        else if (scaled < -0x7FFF) scaled = -0x7FFF;
        sceSdSetParam(cores[i] | params[i], (u16)(s16)scaled);
    }
    return data;
}

static void VolumeControlRpcThread(void *arg)
{
    (void)arg;
    SifInitRpc(0);
    SifSetRpcQueue(&volume_rpc_queue, GetThreadId());
    SifRegisterRpc(&volume_rpc_server, OPL_VOLUME_RPC_ID,
                   VolumeControlRpcHandler, volume_rpc_buffer,
                   NULL, NULL, &volume_rpc_queue);
    SifRpcLoop(&volume_rpc_queue);
}

int _start(int argc, char *argv[])
{
    iop_thread_t thread;
    (void)argc;
    (void)argv;
    thread.attr = TH_C;
    thread.option = 0;
    thread.thread = VolumeControlRpcThread;
    thread.priority = 0x20;
    thread.stacksize = 0x800;
    int thread_id = CreateThread(&thread);
    if (thread_id <= 0) return MODULE_NO_RESIDENT_END;
    if (StartThread(thread_id, NULL) < 0) return MODULE_NO_RESIDENT_END;
    return MODULE_RESIDENT_END;
}
