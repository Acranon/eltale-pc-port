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
- `osSpTaskStartGo` hashes each game's display list and posts SP/DP events.
  The hash normalizes address-bearing words through a generated host-symbol
  to pinned-N64-address map; the actual 32-bit Gfx words stay unchanged.
- `--headless --fast --frames N` stops after N completed graphics frames.
  Windows and Linux matched **all 60 frame hashes and command counts**, and
  repeated Windows runs matched exactly. Frame 60 has 120 commands and hash
  `92394da5842922b0`.
- `tests/os_events_test.c` checks a timer at half a VI, retrace timing,
  deferred PI completion and SP/DP event delivery. Fiber and scheduler tests
  continue to pass on Windows and Linux.

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

## Next slices

- OS3: native pure-C libultra `gu/` and math, with fixed-point conversion
  checks; remove the matching fail-fast stubs.
- OS4: scripted input and Controller Pak `.mpk` HLE, then boot through title
  to a new game and save/load.
