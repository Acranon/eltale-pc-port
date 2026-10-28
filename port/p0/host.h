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
int Port_HostRomSpanMatches(uint32_t rom_offset, uintptr_t ram_address, size_t length);
__attribute__((noreturn)) void Port_HostFatal(const char *what);

#endif
