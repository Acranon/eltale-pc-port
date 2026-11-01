/* An RSP interpreter (scalar unit, COP0 with SP DMA, vector unit with ares' accumulator, clamp,
 * flag and divide rules), shared by the audio check (port/audio/rsp_lle.c, the game's aspMain) and
 * the graphics reference (tools/rdpref, the game's F3DEX 1.23).
 *
 * The caller owns the memory model: DRAM is reached only through `dram_read` / `dram_write`, one
 * big-endian byte at a time, at the address the DMA registers hold (24 bits plus whatever the row
 * arithmetic adds). COP0 registers 8-15 (the RDP's DPC registers) go to `dpc_read` / `dpc_write`;
 * without them they read 0 and writes are dropped. */
#ifndef PORT_RSP_H
#define PORT_RSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { uint16_t e[8]; } RspVreg;

typedef struct Rsp {
    uint8_t dmem[0x1000], imem[0x1000];
    uint32_t r[32];
    uint32_t pc, npc, nnpc; /* this instruction, the next (a delay slot after a branch), the one after */
    int halted;
    uint32_t sp_mem_addr, sp_dram_addr;
    uint32_t sp_status;     /* the signal bits a microcode set (SP_STATUS bits 7-14) */
    RspVreg vr[32];
    int64_t acc[8];
    uint16_t vco, vcc;
    uint8_t vce;
    int16_t div_in, div_out; /* VRCPH/VRSQH state */
    int div_dp;
    uint32_t pc_hist[32];
    /* memory and the RDP */
    void *ctx;
    uint8_t (*dram_read)(void *ctx, uint32_t addr);
    void (*dram_write)(void *ctx, uint32_t addr, uint8_t v);
    uint32_t (*dpc_read)(void *ctx, int reg);            /* reg 0..7 = DPC_START..DPC_TMEM */
    void (*dpc_write)(void *ctx, int reg, uint32_t v);
    void (*fatal)(const char *what); /* an instruction the interpreter lacks; NULL: print and abort */
} Rsp;

/* Clear the registers and start at IMEM `pc` (the memories are kept). */
void Rsp_Reset(Rsp *rsp, uint32_t pc);
/* Run until the microcode halts (BREAK or SP_SET_HALT); 0 if `max_steps` ran out first (the last
 * 32 program counters are printed to stderr). */
int Rsp_Run(Rsp *rsp, uint64_t max_steps);

/* DMEM big-endian accessors for callers that set up a task */
void Rsp_StoreDmem32(Rsp *rsp, uint32_t a, uint32_t v);
uint32_t Rsp_LoadDmem32(const Rsp *rsp, uint32_t a);

#ifdef __cplusplus
}
#endif

#endif
