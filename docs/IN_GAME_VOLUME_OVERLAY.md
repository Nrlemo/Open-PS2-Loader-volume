# In-game volume overlay (experimental)

This branch is derived from `feature/in-game-volume-control` and adds a compact on-screen volume indicator when START+R1 or START+R2 changes the requested volume.

## Expected behavior

- The indicator is placed near the upper-right corner of the currently scanned-out GS framebuffer.
- It displays `VOL`, the requested percentage, and a horizontal 0–150% bar.
- It is redrawn for up to 90 successful frame polls (roughly 1.5–1.8 seconds at common refresh rates).
- The renderer supports GS PSMCT32, PSMCT24, PSMCT16 and PSMCT16S. Other display formats cause the indicator to disable itself for that appearance.

## Important limitations

This is a proof of concept, not a release-ready OSD. It transfers pixels into the active framebuffer through VIF1/GIF. It checks for a busy VIF1 path and avoids changing the game's drawing-context registers, but game-specific DMA scheduling, framebuffer flipping, interlaced output, and GS pixel formats still need validation on real hardware. The transfer path is shared with game rendering, so even with the idle check there is a race window; flicker, rendering interference, or a game-specific failure is possible.

No build or hardware test has been run in this environment. Keep this branch separate and do not merge it until it has built with the intended PS2SDK, been tested on at least one game in each relevant display mode, and shown that graphics/audio and IGR remain stable. The base volume-control code also remains an uncompiled proof of concept.
