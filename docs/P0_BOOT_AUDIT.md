# P0 first-display-list audit

The Windows and Linux boot tests run the same native C path:

`Boot_Main` → `Boot_IdleThread` → `Core_MainThread` → scheduler/VI/controller
setup → `Gfx_InitFrames` → boss/chest/spirit/player/item/environment/text/BGM
initialization → `Core_ResetGameState` → `Menu_Init` → `Core_RunIntroLoop` →
`Overlay_LoadIntro` → `Intro_Init` → first `Intro_Update` →
`Gfx_BuildFrameDisplayList` → `Port_P0DisplayListReady`.

The Controller Pak scan is a host success response. On this first update,
`Intro_Update` marks the check complete and returns before sprite drawing.
The first display list therefore contains the game's clear/setup commands
and its full-sync/end commands, with 18 `Gfx` words. The scheduler's three
worker threads are registered but are not run before the checkpoint; boot
threads 1 and 6 run synchronously in the P0 stand-in.

## Data read before the checkpoint

The game copies the PakMenu overlay, font assets, initial segment data,
player animation block and intro overlay from the user ROM. The host checks
six spans byte for byte, including the complete intro overlay at
ROM `0xDC62B0..0xE22690` into RAM `0x8020C0D0`. There is no multi-byte
ROM or save record interpreted on this first-update path. DMA intentionally
preserves raw big-endian bytes; `port/include/port/endian.h` provides read/write helpers
for the later paths that interpret records.

The executable checks `sizeof(u32)==4`, `sizeof(void*)==8`, `sizeof(Gfx)==8`,
`sizeof(Mtx)==64`, the display-list offset in `GfxFrameData`, and the
`ActorBattle` stats offset after its handle pointers widen. The first-list
hash is `0cf266fd815bb6a0` on both native Windows Clang and Linux GCC.
The host selects round-to-nearest FP mode and disables FP contraction in
both builds. No division or rounding edge case is reached before this list.

The complete 47-module Windows compile gate remains useful but is not a
runtime ABI proof for later frames. `tools/port_census.py` lists candidate
pointer and endian sites. Full gameplay, audio, Pak/EEPROM save behavior,
real scheduler fibers, ROM/save record conversion and rendering follow the
first-list milestone.
