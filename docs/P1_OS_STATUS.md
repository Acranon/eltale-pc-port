# P1a OS status

Brief: `~/eltale-notes/P1_OS.md` (2026-10-28). The P0 build remains the
regression target while the P1 OS replaces its synchronous host stand-in.

## OS1 foundation

- `port/os/fiber.c` adapts Holyland's fixed-slot x64 fiber switch for Windows
  and Linux. It saves nonvolatile registers and FP control state, and the
  scheduler requires its stacks at the fixed address before running the game.
- `port/os/kernel.c` schedules the highest-priority runnable N64 thread.
  StartThread, SetThreadPri and message send wake-ups preempt immediately for
  a higher priority; equal priorities use FIFO order. Receive on an empty
  queue and send on a full queue block, while NOBLOCK returns -1. JamMesg
  inserts at the front. The interrupt mask API returns its old value.
- `python tools/build_windows.py --p1-os --rom ...` boots through all five
  game/scheduler threads on fibers. The P0 path remains a separate regression
  build.
- Windows and Linux fiber/scheduler tests cover switching, priority
  preemption, receiver and sender blocking, NOBLOCK, JamMesg and equal-priority
  FIFO. P0 Windows/WSL boot checks remain green.

## OS2: virtual events and completed frames

- The OS clock advances at 46.875 MHz (781,250 counts per NTSC VI), matching
  libultra's initialized `osClockRate`. `osGetCount`, `osGetTime`, one-shot and
  repeating timers use that virtual clock. `--fast` skips wall pacing; normal
  mode paces virtual time without feeding host time into game logic.
- PI DMA copies the ROM bytes and posts completion at the next scheduler
  switch. VI retraces, SP and DP completion use registered OS event queues.
  The game scheduler thread handles those messages and graphics worker
  finishes tasks through `osViSwapBuffer`.
- AI accepts up to two virtual PCM transfers, derives their completion times
  from the programmed sample rate, reports remaining bytes/status and posts
  AI events. PCM output itself is reserved for P1c. SP yield reports that the
  headless task has already completed.
- `osSpTaskStartGo` hashes each game's display list and posts SP/DP events.
  The hash normalizes address-bearing words through a generated host-symbol
  to pinned-N64-address map; the actual 32-bit Gfx words stay unchanged.
  P1 `osVirtualToPhysical` passes 32-bit addresses through, including KSEG0
  and the host frame data pointer. Hashing maps that frame pointer to a stable
  trace address on each host.
- `--headless --fast --frames N` stops after N completed graphics frames.
  Windows and Linux matched **all 60 frame hashes and command counts**, and
  repeated Windows runs matched exactly. After the P1 address-model correction,
  frame 60 has 120 commands and hash `3ec25a41519e00b0`.
- `tests/os_events_test.c` checks a timer at half a VI, retrace timing,
  deferred PI completion, SP/DP event delivery, and the two-entry AI FIFO.
  Fiber and scheduler tests
  continue to pass on Windows and Linux.

## OS3: pure-C libultra math

- The P1 executable compiles and links all ten `decomp/src/libultra/gu/*.c`
  sources natively. The matching `gu*` and `sins` fail-fast stand-ins are gone.
- `tests/gu_test.c` checks `guMtxF2L`/`guMtxIdent` packet words, including
  truncation of ±0.75 fixed-point units toward zero, and checks sine-table
  quadrants. It passes with native Windows Clang and Linux GCC. No other
  libultra/libc pure-C object is needed by the current intro task path.

## Busy-wait audit

`Core_RunGameplayLoop`, `Core_RunMessageCutsceneLoop` and
`Core_RunCameraCutsceneLoop` each poll `nnsc_retrace_count < 2` without a
blocking OS call. They need a cooperative yield point before those modes
run. Their audio-stop waits call `Audio_GetSeqState` inside a loop; the native
audio implementation must advance virtual time there. `audio.c` polls
`osAiGetLength` in its task loop; that host call must yield/advance a VI when
the queue is empty. `nnScWaitTaskReady` polls the framebuffer with a blocking
message receive, so it already yields. The idle thread's `for(;;)` is handled
by the kernel: after it starts main, the scheduler advances an event instead
of running that spin again.

## OS4: scripted input and Controller Pak

Windows title smoke run:

```powershell
python tools/build_windows.py --p1-os --frames 100 --input tests/intro-to-title.input --pak out/p1-windows/journey.mpk --rom C:\path\to\eltalemonsters.jp.z64
```

Linux/WSL equivalent:

```sh
make P1_OS=1 OUT=out/p1 p1-os-run ROM=/path/to/baserom.jp.z64 FRAMES=100 INPUT=tests/intro-to-title.input PAK=out/p1/journey.mpk
```

- `--input path` reads ordered `VI held_buttons stick_x stick_y` rows. The
  controller state holds between rows; button edges are still computed by the
  game's `Input_ReadController`. `--pak path` uses a raw 32 KiB Controller Pak
  image, creating a formatted image when the file is absent. With no path,
  the Pak is volatile. `tests/pak_test.c` allocates a two-page game file,
  writes it, reloads the image, reads it back, checks the mirrored inode table
  and raw page bytes, and deletes it.
- `osPfs*` file operations use the Pak's big-endian ID, inode and directory
  layout. The game now calls these functions through its actual input module.
  Motor calls are harmless success; the GB Pak probe reports a device error.
- VI time now advances at each presented frame. Before that correction, SP
  tasks could complete hundreds of frames while the VI count stayed at zero,
  leaving scripted inputs unconsumed. The 60-frame intro hash matches on
  Windows and Linux after the correction.
- `tests/intro-to-title.input` reaches the title through the game's own
  intro/title loops. Windows and Linux both finish a 100-frame run with
  `mode=3`, 63 DMA chunks and hash `9945f9cc1c2b7a5e`. Both create a
  32 KiB `.mpk` image. Native title code is linked in P1.
- `tests/title-new-game.input` enters the title's New Game path. Map loading
  currently requests invalid ROM offset `0x7796e15f`. The map area data in
  RDRAM is still raw N64 bytes, while the native `EnvAreaSettings` type has
  64-bit pointers and a different stride. Decoding and relocating those
  records belongs to the next data port slice. Consequently the OS4 brief's
  full gameplay save/load milestone is not yet met; the Pak API write/reload
  path is verified independently. Game save record byte order is also still
  the separate endian pass named in the brief.

## Remaining P1 work

- Decode and relocate the map area data that gameplay reads after the title,
  then drive a save and load through the in-game menu.
- Native audio synthesis and rendering remain the later P1 slices. Their
  sequence and SFX calls are quiescent stand-ins for the headless OS path.
