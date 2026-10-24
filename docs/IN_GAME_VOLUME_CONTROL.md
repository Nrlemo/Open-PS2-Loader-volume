# In-game master volume control (proof of concept)

- `START + R1`: increase the game master volume by 5 percentage points.
- `START + R2`: decrease it by 5 percentage points.
- The initial scale is relative to master-volume values captured on the first change; 0% mutes, 100% preserves the baseline, and up to 150% is requested.

The EE-side IGR polling thread sends the requested percentage over SIF RPC to the IOP module. OPL embeds `volctrl.irx` into its module storage and attempts to load it after IOP initialization. The service uses LIBSD master-volume parameters for both SPU2 cores.

## Validation status

This is an uncompiled proof of concept. Button-byte masks, LIBSD parameter encoding, RPC imports and behavior across games must be checked against the exact PS2SDK toolchain and on real hardware. Games may reconfigure their master volume, and titles that reboot the IOP may unload the service. Do not treat it as a release build.
