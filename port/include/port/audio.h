#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include "ultra64.h"
#include "PR/libaudio.h"

/* port/audio/bank.c: native copies of a big-endian bank file (.ctl, with `table` the ROM
 * address of its sample data) and of the sequence file header (`base` the ROM address of the
 * sequences), in place of alBnkfNew / alSeqFileNew. */
ALBankFile *Port_BankFileNew(const void *file, const void *table);
ALSeqFile *Port_SeqFileNew(const void *file, const void *base);

/* port/audio/aspmain.c: run an audio task's aspMain command list (ABI 1) on the CPU, natively:
 * the only way the game's sound is made. */
void Port_RunAudioTask(const OSTask *task);
void Port_AspCommand(const OSTask *task, u32 w0, u32 w1);
void Port_AspTaskStart(const OSTask *task); /* before a task's first command */
u8 *Port_AspDmem(void); /* its DMEM: host order, buffers at the command lists' offsets */
/* tests only (the reference check): */
void Port_AspSaveState(void);    /* DMEM, the ADPCM book and the SETBUFF/SETVOL/SETLOOP values */
void Port_AspRestoreState(void);
extern void (*Port_AspWriteHook)(void *dst, u32 bytes); /* told before each DRAM write */

/* port/audio/rsp_lle.c, tests only: the reference, the game's aspMain microcode on an RSP
 * interpreter (port/rsp), compared with the native code on every task. Port_AudioRefInit reads
 * ELTALE_AUDIO_CHECK / ELTALE_AUDIO_FUZZ and returns 1 if the check is on; then the boot runs
 * Port_CheckAudioTask in place of Port_RunAudioTask (the native result is what the game hears) and
 * gives it every ROM DMA (Port_LleNotePiDma: which memory holds bytes, not samples). */
int Port_AudioRefInit(void);
void Port_CheckAudioTask(const OSTask *task);
void Port_CheckAudioReport(void);
void Port_LleNotePiDma(void *dst, u32 size);

#endif
