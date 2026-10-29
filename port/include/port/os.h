#ifndef ELTALE_PORT_OS_H
#define ELTALE_PORT_OS_H

/* P1 OS scheduler entry from the host, plus its boot-test observation points. */
void Port_OsInit(void);
void Port_OsRun(void);
__attribute__((noreturn)) void Port_OsStop(void);
unsigned Port_OsThreadCount(void);
unsigned Port_OsStartedCount(void);
unsigned Port_OsRunCount(void);

#endif
