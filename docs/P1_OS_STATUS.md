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
- `python tools/build_windows.py --p1-os --rom ...` boots to the first intro
  display list using all five real game/scheduler threads on fibers. It still
  uses the P0 display-list checkpoint and synchronous PI completion.
- Windows and Linux fiber/scheduler tests cover switching, priority
  preemption, receiver and sender blocking, NOBLOCK, JamMesg and equal-priority
  FIFO. P0 Windows/WSL boot checks remain green.

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

- OS2: virtual VI clock, retrace delivery, PI completion as an event,
  SP/DP task completion hook and `--frames N` intro runs with stable hashes.
- OS3: native pure-C libultra `gu/` and math, with fixed-point conversion
  checks; remove the matching fail-fast stubs.
- OS4: scripted input and Controller Pak `.mpk` HLE, then boot through title
  to a new game and save/load.
