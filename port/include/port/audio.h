#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#include "ultra64.h"
#include "PR/libaudio.h"

/* port/audio/bank.c: native copies of a big-endian bank file (.ctl, with `table` the ROM
 * address of its sample data) and of the sequence file header (`base` the ROM address of the
 * sequences), in place of alBnkfNew / alSeqFileNew. */
ALBankFile *Port_BankFileNew(const void *file, const void *table);
ALSeqFile *Port_SeqFileNew(const void *file, const void *base);

#endif
