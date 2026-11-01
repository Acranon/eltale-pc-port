/* N64 records that keep their N64 layout on the host are swapped to host byte order in place,
 * in RDRAM, after they are loaded. A bitmap over RDRAM remembers which halfwords are already
 * swapped, so records shared between tables are swapped once, and a DMA that overwrites a range
 * forgets it (Port_DataForget, from osPiStartDma).
 *
 * Animation: an AnimSet table (AnimSet, AnimNode, KfAnim/Keyframe, the display-list address
 * tables) is swapped by Port_SwapAnimSets. The tables have no stored length; one ends at the
 * first record that is not a valid big-endian AnimSet. */
#include "port/data.h"
#include "game/actor/anim.h"

#include <stdint.h>
#include <string.h>

__attribute__((noreturn)) void Port_HostFatal(const char *what);

enum { RDRAM_BASE = 0x80000000u, RDRAM_BYTES = 8 << 20 };
static u8 swapped[RDRAM_BYTES / 2 / 8]; /* one bit per halfword */

static int in_ram(u32 a, u32 bytes) { return a >= RDRAM_BASE && a - RDRAM_BASE <= RDRAM_BYTES - bytes; }
static int done(u32 a) { u32 h = (a - RDRAM_BASE) / 2; return swapped[h / 8] >> (h % 8) & 1; }
static void mark(u32 a) { u32 h = (a - RDRAM_BASE) / 2; swapped[h / 8] |= (u8)(1 << (h % 8)); }

void Port_DataForget(u32 start, u32 bytes) {
    if (!bytes || start >= RDRAM_BASE + RDRAM_BYTES || start + bytes <= RDRAM_BASE) return;
    u32 lo = start < RDRAM_BASE ? 0 : (start - RDRAM_BASE) / 2;
    u32 hi = (start + bytes - RDRAM_BASE + 1) / 2;
    if (hi > RDRAM_BYTES / 2) hi = RDRAM_BYTES / 2;
    for (u32 h = lo; h < hi; h++) swapped[h / 8] &= (u8)~(1 << (h % 8));
}

static u16 be16(u32 a) { const u8 *p = (const u8 *)(uptr)a; return (u16)(p[0] << 8 | p[1]); }
static u32 be32(u32 a) { return (u32)be16(a) << 16 | be16(a + 2); }

void Port_Swap16(u32 a) {
    if (!in_ram(a, 2)) Port_HostFatal("swap outside RDRAM");
    if (done(a)) return;
    u16 v = be16(a);
    memcpy((void *)(uptr)a, &v, 2);
    mark(a);
}

void Port_Swap32(u32 a) {
    if (!in_ram(a, 4)) Port_HostFatal("swap outside RDRAM");
    if (done(a)) return;
    u32 v = be32(a);
    memcpy((void *)(uptr)a, &v, 4);
    mark(a);
    mark(a + 2);
}

/* Read a field that may or may not be swapped yet. */
static u32 rd32(u32 a) { u32 v; if (done(a)) { memcpy(&v, (void *)(uptr)a, 4); return v; } return be32(a); }
static u16 rd16(u32 a) { u16 v; if (done(a)) { memcpy(&v, (void *)(uptr)a, 2); return v; } return be16(a); }

static void swap_keyframes(u32 kfs, u32 count) {
    if (!kfs || !in_ram(kfs, 0x34u * count)) return;
    for (u32 k = 0; k < count; k++) {
        u32 kf = kfs + k * 0x34;
        for (u32 w = 0; w < 12; w++) Port_Swap32(kf + w * 4);
        Port_Swap16(kf + 0x30);
        Port_Swap16(kf + 0x32);
    }
}

/* Byte-swap a host buffer record by `shape` (w: 4 bytes, h: 2, b: 1); returns the bytes covered. */
u32 Port_SwapRecord(void *p, const char *shape) {
    u8 *b = p, t;
    u32 off = 0;
    for (const char *s = shape; *s; s++) {
        if (*s == 'w') { t = b[off]; b[off] = b[off + 3]; b[off + 3] = t; t = b[off + 1]; b[off + 1] = b[off + 2]; b[off + 2] = t; off += 4; }
        else if (*s == 'h') { t = b[off]; b[off] = b[off + 1]; b[off + 1] = t; off += 2; }
        else off += 1;
    }
    return off;
}

/* A native u16 array a ROM DMA filled with big-endian halfwords (palettes the game blends). */
void Port_SwapHalfwords(void *p, u32 bytes) {
    u8 *b = p;
    for (u32 i = 0; i + 1 < bytes; i += 2) { u8 t = b[i]; b[i] = b[i + 1]; b[i + 1] = t; }
}

/* A KfAnim record (kfs, count, last) and its keyframes. */
void Port_SwapKfAnim(u32 anim) {
    if (!in_ram(anim, 8)) return;
    Port_Swap32(anim);
    Port_Swap16(anim + 4);
    Port_Swap16(anim + 6);
    swap_keyframes(rd32(anim), rd16(anim + 4));
}

static int valid_set(u32 a) {
    if (!in_ram(a, 0xC) || (a & 3)) return 0;
    s16 nodes = (s16)rd16(a);
    u32 node_addr = rd32(a + 4), dls = rd32(a + 8);
    return nodes > 0 && nodes <= 128 && in_ram(node_addr, 0x20u * (u32)nodes) && !(node_addr & 3) &&
           (dls == 0 || (in_ram(dls, 4) && !(dls & 3)));
}

static void swap_set(u32 a) {
    Port_Swap16(a);
    Port_Swap16(a + 2);
    Port_Swap32(a + 4);
    Port_Swap32(a + 8);
    const AnimSet *set = (const AnimSet *)(uptr)a;
    int max_dl = -1;
    for (s32 i = 0; i < set->nodeCount; i++) {
        u32 n = set->nodes + (u32)i * 0x20;
        Port_Swap32(n);             /* track.kfs */
        Port_Swap16(n + 4);         /* track.count */
        Port_Swap16(n + 6);         /* track.last */
        for (u32 k = 0; k < 3; k++) Port_Swap32(n + 8 + k * 4); /* pivot */
        Port_Swap32(n + 0x18);      /* dlSeq */
        Port_Swap16(n + 0x1C);      /* dlSeqLen */
        const AnimNode *node = (const AnimNode *)(uptr)n;
        swap_keyframes(node->track.kfs, node->track.count);
        if (node->dl > max_dl) max_dl = node->dl;
        if (node->dlSeq && in_ram(node->dlSeq, 4u * node->dlSeqLen)) {
            for (u32 k = 0; k < node->dlSeqLen; k++) {
                Port_Swap32(node->dlSeq + k * 4);
                s32 v = ((const s32 *)(uptr)node->dlSeq)[k];
                if (v > max_dl) max_dl = v;
            }
        }
    }
    if (set->dls)
        for (s32 i = 0; i <= max_dl; i++) Port_Swap32(set->dls + (u32)i * 4);
}

void Port_SwapAnimSetDls(const AnimSet *set) {
    int max_dl = -1;
    for (s32 i = 0; i < set->nodeCount; i++) {
        const AnimNode *node = (const AnimNode *)(uptr)(set->nodes + (u32)i * 0x20);
        if (node->dl > max_dl) max_dl = node->dl;
        for (u32 k = 0; node->dlSeq && k < node->dlSeqLen; k++) {
            s32 v = ((const s32 *)(uptr)node->dlSeq)[k];
            if (v > max_dl) max_dl = v;
        }
    }
    if (set->dls && in_ram(set->dls, 4))
        for (s32 i = 0; i <= max_dl; i++) Port_Swap32(set->dls + (u32)i * 4);
}

/* Swap one AnimSet and what it points to, if not already done: the anim functions call this,
 * since a set's nodes and keyframes can be in a block loaded after its table. */
void Port_SwapAnimSet(u32 set) {
    if (valid_set(set)) swap_set(set);
}

/* An AnimSet's length for game code that reads it itself (ANIM_SET_LENGTH, game/actor/anim.h): the
 * set swapped first, as the anim functions would on its first use; read through the bitmap, so a set
 * that cannot be swapped yet still gives the right number. */
s16 Port_AnimSetLength(AnimSet *set) {
    u32 a = (u32)(uptr)set;
    if (!in_ram(a, 0xC)) return set->length;
    Port_SwapAnimSet(a);
    return (s16)rd16(a + 2);
}

/* A table loaded just now, from the block [lo, hi): a set whose nodes are in that block is swapped
 * whole; of the others only the header (count, nodes, display lists). Their nodes are in a block
 * loaded later (the player's, at 0x80200000, comes with each map), so here they hold whatever the
 * last session left: nothing after a power-on, but after the session restarts (the end of the
 * ending, a quick load) the last map's data, whose display-list numbers ran the swap of the
 * display-list table into the set table itself. The anim functions swap the rest on use
 * (Port_SwapAnimSet), once the nodes are there. */
void Port_SwapAnimSets(u32 table, u32 lo, u32 hi) {
    for (u32 a = table; valid_set(a); a += 0xC) {
        u32 nodes = rd32(a + 4);
        if (nodes >= lo && nodes < hi) {
            swap_set(a);
        } else {
            Port_Swap16(a);
            Port_Swap16(a + 2);
            Port_Swap32(a + 4);
            Port_Swap32(a + 8);
        }
    }
}

/* The game's own AnimNodes (menu.c) point their tracks at the keyframe tables' RDRAM copies
 * (the boot copy of the first ROM MiB): swap those once at startup. */
extern AnimNode mapobjanim_type23_anim, D_80057D24, mapobjanim_type23_odd_anim, mapobjanim_type4_anim;
extern AnimNode mapobj_loop_anims[4];

void Port_SwapNativeKeyframes(void) {
    const AnimNode *nodes[] = { &mapobjanim_type23_anim, &D_80057D24, &mapobjanim_type23_odd_anim,
                                &mapobjanim_type4_anim, &mapobj_loop_anims[0], &mapobj_loop_anims[1],
                                &mapobj_loop_anims[2], &mapobj_loop_anims[3] };
    for (unsigned i = 0; i < sizeof nodes / sizeof nodes[0]; i++)
        swap_keyframes(nodes[i]->track.kfs, nodes[i]->track.count);
}
