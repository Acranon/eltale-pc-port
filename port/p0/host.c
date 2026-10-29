#ifndef _WIN32
#define _GNU_SOURCE
#endif
#include "port/p0/host.h"
#include "port/boot_probe.h"
#include "game/core/main.h"
#ifdef ELTALE_P1_OS
#include "port/os.h"
#endif

#include <errno.h>
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

enum { ROM_BYTES = 16 * 1024 * 1024, RDRAM_BYTES = 8 * 1024 * 1024 };
static const uintptr_t RDRAM_BASE = 0x80000000u;
static unsigned char *rom_bytes;
static unsigned dma_count;
static size_t dma_bytes;
#ifdef ELTALE_P1_OS
static uintptr_t image_start_address, image_end_address;
#endif
#ifndef ELTALE_P1_OS
static unsigned thread_count;
static unsigned started_count;
static unsigned run_count;

typedef struct HostThread {
    OSThread *n64;
    void (*entry)(void *);
    void *arg;
    OSId id;
} HostThread;
static HostThread threads[16];
#endif

s32 osTvType = OS_TV_NTSC;
OSViMode osViModeTable[16];

static uint64_t monotonic_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, count;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&count);
    uint64_t ticks = (uint64_t)count.QuadPart;
    uint64_t rate = (uint64_t)frequency.QuadPart;
    return (ticks / rate) * UINT64_C(1000000000) +
           (ticks % rate) * UINT64_C(1000000000) / rate;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
#endif
}

void Port_HostPace(uint64_t virtual_ticks) {
    static uint64_t start_ns;
    if (!start_ns) start_ns = monotonic_ns();
    uint64_t target = start_ns + virtual_ticks * UINT64_C(1000000000) / UINT64_C(46875000);
    uint64_t now;
    while ((now = monotonic_ns()) < target) {
        uint64_t remaining = target - now;
#ifdef _WIN32
        if (remaining > UINT64_C(2000000)) Sleep((DWORD)((remaining - UINT64_C(1000000)) / 1000000));
        else SwitchToThread();
#else
        struct timespec pause = { (time_t)(remaining / UINT64_C(1000000000)),
                                  (long)(remaining % UINT64_C(1000000000)) };
        nanosleep(&pause, NULL);
#endif
    }
}

void Port_HostFatal(const char *what) {
    fprintf(stderr, "P0 error: %s\n", what);
    exit(1);
}

int Port_HostInit(const char *rom_path) {
    if (fesetround(FE_TONEAREST) != 0)
        Port_HostFatal("cannot set N64 boot rounding mode");
    unsigned char header[4] = { 0x80, 0x37, 0x12, 0x40 };
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        perror(rom_path);
        return -1;
    }
    rom_bytes = malloc(ROM_BYTES);
    if (!rom_bytes || fread(rom_bytes, 1, ROM_BYTES, f) != ROM_BYTES || fgetc(f) != EOF) {
        fprintf(stderr, "P0 error: expected a 16 MiB z64 ROM\n");
        fclose(f);
        return -1;
    }
    fclose(f);
    if (memcmp(rom_bytes, header, sizeof header) != 0) {
        fprintf(stderr, "P0 error: ROM is not big-endian z64 format\n");
        return -1;
    }
#ifdef _WIN32
    void *mem = VirtualAlloc((void *)RDRAM_BASE, RDRAM_BYTES,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != (void *)RDRAM_BASE) {
        fprintf(stderr, "P0 error: cannot reserve N64 address range (Win32 %lu)\n",
                (unsigned long)GetLastError());
        return -1;
    }
#else
    void *mem = mmap((void *)RDRAM_BASE, RDRAM_BYTES, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "P0 error: cannot reserve N64 address range: %s\n", strerror(errno));
        return -1;
    }
#endif
    /* As IPL3 does: the first MiB after the ROM header lands at 0x80000400. Native code
     * keeps its own copy of this data; the RDRAM copy is what fixed display lists
     * (PORT_ADDR) point at, as raw big-endian N64 bytes. */
    memcpy((void *)(RDRAM_BASE + 0x400), rom_bytes + 0x1000, 0x100000);
    /* Display-list words carry 32-bit addresses (PR/gbi.h). That only works if the
     * game's image sits below 4 GiB and clear of the RDRAM range. Stacks are not
     * checked: the RSP reads a list after the code that built it has returned, so
     * the game never puts a stack address in one. */
#ifdef _WIN32
    extern IMAGE_DOS_HEADER __ImageBase;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)&__ImageBase + __ImageBase.e_lfanew);
    uintptr_t image_start = (uintptr_t)&__ImageBase;
    uintptr_t image_end = image_start + nt->OptionalHeader.SizeOfImage;
#else
    extern char __executable_start, end;
    uintptr_t image_start = (uintptr_t)&__executable_start;
    uintptr_t image_end = (uintptr_t)&end;
#endif
    if (image_end > RDRAM_BASE) {
        fprintf(stderr, "P0 error: image %#llx..%#llx is not below the RDRAM range\n",
                (unsigned long long)image_start, (unsigned long long)image_end);
        return -1;
    }
#ifdef ELTALE_P1_OS
    image_start_address = image_start;
    image_end_address = image_end;
#endif
    return 0;
}

unsigned Port_HostDmaCount(void) { return dma_count; }
size_t Port_HostDmaBytes(void) { return dma_bytes; }
#ifdef ELTALE_P1_OS
unsigned Port_HostThreadCount(void) { return Port_OsThreadCount(); }
unsigned Port_HostStartedThreadCount(void) { return Port_OsStartedCount(); }
unsigned Port_HostRunThreadCount(void) { return Port_OsRunCount(); }
#else
unsigned Port_HostThreadCount(void) { return thread_count; }
unsigned Port_HostStartedThreadCount(void) { return started_count; }
unsigned Port_HostRunThreadCount(void) { return run_count; }
#endif
int Port_HostRomSpanMatches(uint32_t rom_offset, uintptr_t ram_address, size_t length) {
    if (rom_offset > ROM_BYTES || length > ROM_BYTES - rom_offset ||
        ram_address < RDRAM_BASE || length > RDRAM_BYTES ||
        ram_address - RDRAM_BASE > RDRAM_BYTES - length) return 0;
    return memcmp((void *)ram_address, rom_bytes + rom_offset, length) == 0;
}

void func_80036AC0(void) { /* osInitialize: P0 host initialization precedes Boot_Main. */ }
void osCreatePiManager(OSPri pri, OSMesgQueue *q, OSMesg *msgs, s32 n) {
    (void)pri;
    osCreateMesgQueue(q, msgs, n);
}

#ifndef ELTALE_P1_OS
void osCreateThread(OSThread *t, OSId id, void (*entry)(void *), void *arg,
                    void *stack, OSPri pri) {
    (void)stack;
    if (thread_count == 16) Port_HostFatal("thread registry full");
    threads[thread_count++] = (HostThread){ t, entry, arg, id };
    t->id = id;
    t->priority = pri;
}

void osStartThread(OSThread *t) {
    for (unsigned i = 0; i < thread_count; i++) {
        if (threads[i].n64 != t) continue;
        started_count++;
        if (threads[i].id == 1 || threads[i].id == 6) {
            run_count++;
            threads[i].entry(threads[i].arg);
        }
        /* Scheduler workers are registered in P0. P1 runs them as fibers. */
        return;
    }
    Port_HostFatal("starting an unknown thread");
}

void osCreateMesgQueue(OSMesgQueue *q, OSMesg *msgs, s32 count) {
    q->mtqueue = NULL;
    q->fullqueue = NULL;
    q->validCount = 0;
    q->first = 0;
    q->msgCount = count;
    q->msg = msgs;
}

s32 osSendMesg(OSMesgQueue *q, OSMesg msg, s32 flag) {
    (void)flag;
    if (q->validCount == q->msgCount) Port_HostFatal("message queue full");
    q->msg[(q->first + q->validCount) % q->msgCount] = msg;
    q->validCount++;
    return 0;
}

s32 osRecvMesg(OSMesgQueue *q, OSMesg *out, s32 flag) {
    (void)flag;
    if (q->validCount == 0) Port_HostFatal("P0 would block on an empty message queue");
    if (out) *out = q->msg[q->first];
    q->first = (q->first + 1) % q->msgCount;
    q->validCount--;
    return 0;
}
#endif

s32 osPiStartDma(OSIoMesg *mb, s32 pri, s32 direction, u32 dev_addr,
                 void *dst, u32 size, OSMesgQueue *queue) {
    (void)pri;
    if (direction != OS_READ || dev_addr > ROM_BYTES || size > ROM_BYTES - dev_addr) {
        fprintf(stderr, "P0 DMA invalid source rom=%08x bytes=%u direction=%d\n",
                dev_addr, size, direction);
        Port_HostFatal("ROM DMA outside input image");
    }
    uintptr_t addr = (uintptr_t)dst;
    int in_rdram = addr >= RDRAM_BASE && size <= RDRAM_BYTES &&
                   addr - RDRAM_BASE <= RDRAM_BYTES - size;
#ifdef ELTALE_P1_OS
    /* Native game globals also receive ROM DMA (for example, the clock TLUT).
     * Their host storage lives in the executable image below the RDRAM map. */
    int in_native_data = addr >= image_start_address && size <= image_end_address - image_start_address &&
                         addr - image_start_address <= image_end_address - image_start_address - size;
#else
    int in_native_data = 0;
#endif
    if (!in_rdram && !in_native_data) {
        fprintf(stderr, "P0 DMA invalid rom=%08x ram=%016llx bytes=%u\n",
                dev_addr, (unsigned long long)addr, size);
        Port_HostFatal("ROM DMA destination outside mapped RDRAM");
    }
    memcpy(dst, rom_bytes + dev_addr, size);
    dma_count++;
    dma_bytes += size;
    if (dma_count <= 8) fprintf(stderr, "P0 DMA rom=%08x ram=%08lx bytes=%u\n",
                                dev_addr, (unsigned long)addr, size);
#ifdef ELTALE_P1_OS
    Port_OsPostPi(queue, (OSMesg)mb);
#else
    osSendMesg(queue, (OSMesg)mb, OS_MESG_BLOCK);
#endif
    return 0;
}

void osInvalICache(void *addr, s32 len) { (void)addr; (void)len; }
void osInvalDCache(void *addr, s32 len) { (void)addr; (void)len; }
void osWritebackDCache(void *addr, s32 len) { (void)addr; (void)len; }
void osWritebackDCacheAll(void) { }

void osCreateViManager(OSPri pri) { (void)pri; }
void osViSetMode(OSViMode *mode) { (void)mode; }
void osViBlack(u8 active) { (void)active; }
void osViSetSpecialFeatures(u32 features) { (void)features; }
#ifndef ELTALE_P1_OS
void osViSetEvent(OSMesgQueue *q, OSMesg msg, u32 fields) {
    (void)q; (void)msg; (void)fields;
}
#endif
#ifndef ELTALE_P1_OS
void osSetEventMesg(OSEvent event, OSMesgQueue *q, OSMesg msg) {
    (void)event; (void)q; (void)msg;
}
OSIntMask osSetIntMask(OSIntMask mask) { return mask; }
#endif
#ifndef ELTALE_P1_OS
void *osViGetCurrentFramebuffer(void) { return NULL; }
void osViSwapBuffer(void *framebuffer) { (void)framebuffer; }
#endif

#ifndef ELTALE_P1_OS
s32 osContInit(OSMesgQueue *q, u8 *pattern, OSContStatus *status) {
    (void)q;
    *pattern = 1;
    memset(status, 0, 4 * sizeof *status);
    return 0;
}
s32 osContSetCh(u8 channels) { (void)channels; return 0; }
s32 osContStartReadData(OSMesgQueue *queue) {
    osSendMesg(queue, NULL, OS_MESG_NOBLOCK);
    return 0;
}
void osContGetReadData(OSContPad *pads) { memset(pads, 0, 4 * sizeof *pads); }
#endif

u32 osVirtualToPhysical(void *ptr) {
    uintptr_t addr = (uintptr_t)ptr;
    if (addr >= RDRAM_BASE && addr < RDRAM_BASE + RDRAM_BYTES)
        return (u32)(addr - RDRAM_BASE);
    for (unsigned i = 0; i < 2; i++) {
        uintptr_t data = (uintptr_t)&port_gfx_frames[i].data;
        if (addr >= data && addr < data + sizeof port_gfx_frames[i].data) {
            /* Stable trace-only frame address across host struct ABIs. */
            return 0x00600000u + i * 0x10000u + (u32)(addr - data);
        }
    }
    Port_HostFatal("physical address requested for an unknown host pointer");
}

#ifndef ELTALE_P1_OS
OSTime osGetTime(void) { return 0; }
void osSpTaskLoad(OSTask *task) { (void)task; Port_HostFatal("RSP task before P1"); }
void osSpTaskStartGo(OSTask *task) { (void)task; Port_HostFatal("RSP task before P1"); }
#endif
void osSpTaskYield(void) { Port_HostFatal("RSP yield before P1"); }
OSYieldResult osSpTaskYielded(OSTask *task) { (void)task; Port_HostFatal("RSP yield before P1"); }
