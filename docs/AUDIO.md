# Sound

The game's sound is native, top to bottom. Nothing of the N64's audio hardware is emulated in play:
the game's synthesizer is C compiled for the PC, and the RSP microcode it was written for is replaced by
C that gives the microcode's results bit for bit. The microcode itself runs only in a test, as the
reference that proves it.

## How a sound frame is made

```
game/audio (Audio_ThreadMain, the decomp)        libultra's synthesizer, compiled for the host
  alAudioFrame -> an aspMain command list         (sequence players, voices, envelopes, the reverb)
osSpTaskStartGo (port/os/kernel.c, M_AUDTASK)
  Port_AudioTaskHook = Port_RunAudioTask          port/audio/aspmain.c: the list run in C
osAiSetNextBuffer -> Port_AiBufferHook            the host's audio ring (port/host), or the
                                                  headless hash (P1_AUDIO) and ELTALE_AUDIO_WAV
```

One audio task per audio frame (about 1/60 s, roughly 370 stereo samples at the game's 22 kHz): a few
hundred commands.

## The ABI: aspMain, ABI 1

The game links libultra's plain synthesizer (`alAudioFrame`, `PR/libaudio.h`, not the `n_` one), and its
audio task carries libultra's **aspMain** microcode (text ROM 0x4DA80, RDRAM 0x8004CE80; data ROM
0x71660, RDRAM 0x80070A60, `ucode_data_size` 0x800). `port/audio/aspmain.c` is that microcode in C:

| op | command | what it does |
| -- | ------- | ------------ |
| 0 | SPNOOP | nothing |
| 1 | ADPCM | decodes 9-byte VADPCM frames (header: scale, predictor; 16 4-bit residuals) with the loaded book; INIT starts from silence, A_LOOP from the SETLOOP state; writes the last 16 samples back as the state |
| 2 | CLEARBUFF | zeroes a buffer, 16 bytes at a time |
| 3 | ENVMIXER | the envelope mixer: 8 lanes of 16.16 volume per side, ramped by the rate each block, held at the target; dry left/right and (A_AUX) wet left/right outputs; a 0x50-byte state |
| 4 | LOADBUFF | DMA from RAM into a buffer |
| 5 | RESAMPLE | 4-tap polyphase resampler (the microcode's 64 x 4 filter table), 16.16 pitch, a 0x20-byte state |
| 6 | SAVEBUFF | DMA from a buffer to RAM |
| 7 | SEGMENT | segment table entry (the game sets segment 0 to 0 and uses none) |
| 8 | SETBUFF | in, out, count (A_AUX: dry right, wet left, wet right) |
| 9 | SETVOL | start volume (A_VOL), target and 16.16 rate (A_LEFT for left), dry and wet gains (A_AUX) |
| 10 | DMEMMOVE | buffer to buffer, 16 bytes at a time |
| 11 | LOADADPCM | the ADPCM book (DMEM 0x4C0) |
| 12 | MIXER | out = out x 0x7FFF + in x gain, rounded, 32 bytes at a time |
| 13 | INTERLEAVE | left and right into stereo pairs |
| 14 | POLEF | the reverb's 2-pole filter (A_INIT) |
| 15 | SETLOOP | the ADPCM loop state's address |

What makes it bit for bit (each item was found by the reference check below):
- The vector unit's arithmetic: each `vmulf` rounds; the 48-bit accumulator and its clamps (middle and low
  slices); 8- and 16-sample blocks; saturating adds in pairs in the resampler; ADPCM sums that wrap at
  32 bits before the clamp.
- The microcode's state layouts (ADPCM 0x20, RESAMPLE 0x20, ENVMIXER 0x50, POLEF 8 bytes), passed through
  its scratch area at DMEM 0xF90; POLEF's INIT clears only half of the scratch it reads its history from,
  and scales the book's second row in place.
- **Every task starts from the microcode's data**: the RSP's boot loads `ucode_data_size` (0x800) bytes at
  DMEM 0, so the parameter block (0x360), the ADPCM book (0x4C0) and the first 0x240 bytes of the buffers
  (from 0x5C0) start each task as that data (`Port_AspTaskStart`). A list that saves part of a buffer it
  has not filled saves those bytes.
- **One parameter block**: SETBUFF, SETVOL and SETLOOP write the microcode's halfwords at DMEM
  0x360-0x37F; SETLOOP's 32-bit address lies over the left target and rate. ENVMIXER counts down a signed
  count.
- **The order of loads and stores** where an output may run into its input: RESAMPLE computes a block of
  8 then stores it; MIXER stores its first half before reading the next step's first input half;
  INTERLEAVE reads 8 samples of each side, then stores 8 pairs.
- **The RSP's DMA**: addresses rounded down to 8 bytes, lengths up to 8 (LOADBUFF, SAVEBUFF, LOADADPCM).
- ADPCM predictors 8-15 (none in the game's data) take their book from the buffers, as DMEM 0x4C0 +
  predictor x 32 does.

From the ROM (the user's, at run time): the resampler's filter table (aspMain data + 0xC0) and the data
the boot loads. Not modelled, because libultra never does it: buffers that are not 16-byte aligned (the
vector unit's quad loads and stores would split them), segments other than 0, DMA lengths over 4 KiB.
One deliberate difference from the console: on the N64 a graphics task runs between two audio tasks and
leaves its own bytes in DMEM above 0x800 (where POLEF's INIT reads stale history); the port, like the
reference, keeps the last audio task's.

### Why the command list, not a new mixer

The synthesizer was already native C (libultra, from the decomp, compiled for the host); the only
emulated piece was the microcode. Replacing it at the command list is the narrowest native boundary that
can be proven: every task can be compared byte for byte with the microcode itself, and every audio hash
stays as it was. A native mixer in place of the synthesizer (a float or 32-bit mixer driven by the
sequence players) would round, envelope and resample differently. It might sound as good, but it could
not be checked against anything and would change every hash. The command-list C is also cheap (below).

## The reference check (tests only)

`port/audio/rsp_lle.c` runs the game's own aspMain (text and data from the ROM, in RDRAM) on the RSP
interpreter `port/rsp` (also used by eltale-rdpref for F3DEX), with the game's native memory. It is never
the play path. It is switched on in the game exe by environment variables, as the frame image is for the
graphics reference:

- `ELTALE_AUDIO_CHECK=task`: each whole task runs on both sides independently, each from its own state
  (the microcode's DMEM carried from task to task on its own). Then every DRAM byte either side wrote and
  the buffers in DMEM are compared. A task that differs is run again a command at a time from the same
  start, to name the first differing command and sample. The summary also times the microcode
  (`P1_AUDIO_TIME microcode`).
- `ELTALE_AUDIO_CHECK=cmd` (or `1`): every command runs on both from the same DMEM, and each one that
  differs is reported.
- `ELTALE_AUDIO_FUZZ=k` (with either): every k-th task is first run as a mutated copy, a command at a
  time. It uses random volumes, targets, rates, pitches, gains, envelope, resampler, filter and ADPCM
  states, INIT/LOOP/AUX flags, odd and changed lengths, and full-scale noise in the buffers. All its
  writes are undone afterwards. Set the seed with `ELTALE_AUDIO_FUZZ_SEED`.
- The microcode's writes are always undone, so the game plays the native result and every hash is the
  same as without the check.
- Output goes to stderr:
  - `P1_AUDIO_CHECK mode=... tasks= commands= differing_tasks= differing_commands=`, plus a line per
    difference with the first differing address, the native and microcode values and the 16 samples
    around it;
  - `P1_AUDIO_FUZZ ...`;
  - `P1_AUDIO_COVER` (commands counted by flags).
- The ROM DMA hook it needs (which memory holds ROM bytes, which the RSP sees as bytes rather than
  samples) is installed only when the check is on.

`tests/audio_checks.sh exe rom [saves] [--quick]` runs the check on New Game, the intro and title, the
ending, the camera tour, the map tour (every sub-area, battles casting spells), a battle and the
spell-casting battle (the battle-stuck quick save), the item menu, the items and compass-spirit saves,
and a save loaded from the title. Each case runs twice, plain and checked, and the two audio hashes must
match. Results on 2026-11-02 (feat/native-audio):

| run | audio hash | tasks | commands | fuzzed tasks / commands |
| --- | ---------- | ----- | -------- | ----------------------- |
| new game | be0b67b7b718648e | 1429 | 471662 | 1429 / 471662 |
| intro | 927568b889cf01f1 | 7998 | 2946110 | 1999 / 735487 |
| ending | 7657b00eb7132bbb | 20421 | 6809356 | 5105 / 1703778 |
| camera tour | adda1b5ec9eb093e | 20628 | 3910722 | - |
| map tour | a2d2e81245d538d4 | 62736 | 21464764 | 7842 / 2688059 |
| battle | 8496de36d46cd66f | 3093 | 1273900 | 1546 / 636816 |
| spells | 19d61811f08ca051 | 3299 | 1376554 | 3299 / 1376554 |
| item menu | 37ecf7e9fa579a23 | 1323 | 547071 | 1323 / 547071 |
| items | 6de8f62a367d340f | 530 | 199150 | 530 / 199150 |
| spirit | 8144c472f8550283 | 201 | 51766 | 201 / 51766 |
| load from the title | 049c257bd8a6f599 | 518 | 105669 | 518 / 105669 |

That is 122,176 tasks and 39.2 million commands, with 0 differing, and 23,792 mutated tasks (8.5 million
commands), also with 0 differing.

## Speed

CPU time per audio task (one per audio frame), New Game (1429 tasks), on the owner's PC:
- native, in play (`ELTALE_AUDIO_TIME=1`): **about 23 µs**;
- the microcode on the RSP interpreter (the reference): **about 495 µs**.

The native code is about 21 times faster. The native path was 22 µs before the exactness work above
(block-wise stores and the task-start data).
