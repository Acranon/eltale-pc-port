#ifndef ELTALE_PORT_OS_H
#define ELTALE_PORT_OS_H

#include "common.h"
#include <stdint.h>

/* P1 OS scheduler entry from the host, plus deterministic observation points. */
void Port_OsInit(void);
void Port_OsRun(void);
__attribute__((noreturn)) void Port_OsStop(void);
void Port_OsSetFrameLimit(unsigned frames);
void Port_OsSetFast(int fast);
void Port_OsPostPi(OSMesgQueue *queue, OSMesg message);
unsigned Port_OsThreadCount(void);
unsigned Port_OsStartedCount(void);
unsigned Port_OsRunCount(void);
unsigned Port_OsFrameCount(void);
unsigned Port_OsGfxTaskCount(void);
uint64_t Port_OsLastGfxHash(void);
uint64_t Port_OsViCount(void);
uint32_t Port_NormalizeDlAddress(uint32_t word);
/* The body of a game loop that polls without an OS call: let virtual time reach the next event. */
void Port_OsSpinWait(void);

#endif
