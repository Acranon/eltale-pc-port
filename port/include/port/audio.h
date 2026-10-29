#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include "ultra64.h"
#include "PR/libaudio.h"

/* port/audio/bank.c: native copies of a big-endian bank file (.ctl, with `table` the ROM
 * address of its sample data) and of the sequence file header (`base` the ROM address of the
 * sequences), in place of alBnkfNew / alSeqFileNew. */
ALBankFile *Port_BankFileNew(const void *file, const void *table);
ALSeqFile *Port_SeqFileNew(const void *file, const void *base);

/* port/audio/aspmain.c: run an audio task's aspMain command list (ABI 1) on the CPU. */
void Port_RunAudioTask(const OSTask *task);
void Port_AspCommand(const OSTask *task, u32 w0, u32 w1);
u8 *Port_AspDmem(void); /* its DMEM: host order, buffers at the command lists' offsets */

/* port/audio/rsp_lle.c: the reference, the game's aspMain microcode on an RSP interpreter.
 * Port_CheckAudioTask runs both and reports bytes where they differ (the HLE's result stays). */
void Port_RunAudioTaskLle(const OSTask *task);
void Port_CheckAudioTask(const OSTask *task);
void Port_CheckAudioReport(void);
void Port_LleNotePiDma(void *dst, u32 size);

#endif
