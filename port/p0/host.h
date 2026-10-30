#ifndef ELTALE_P0_HOST_H
#define ELTALE_P0_HOST_H

#include <stddef.h>
#include <stdint.h>

int Port_HostInit(const char *rom_path);
unsigned Port_HostDmaCount(void);
size_t Port_HostDmaBytes(void);
unsigned Port_HostThreadCount(void);
unsigned Port_HostStartedThreadCount(void);
unsigned Port_HostRunThreadCount(void);
unsigned Port_HostIntroUpdateCount(void);
int Port_HostRomSpanMatches(uint32_t rom_offset, uintptr_t ram_address, size_t length);
__attribute__((noreturn)) void Port_HostFatal(const char *what);
void Port_HostPace(uint64_t virtual_ticks);
/* Save the program's state just before boot, and put it back for a reset (Windows). */
void Port_HostSnapshot(void);
void Port_HostRestore(void);
uint32_t Port_HostNormalizeFrameAddress(uint32_t word);

#endif
