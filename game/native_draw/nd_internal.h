/* nd_internal.h: what the native draw core (native_draw.c) gives each kind's file (nd_map.c, ...).
 * PC-only. docs/renderer/NATIVE_DRAW.md, "Adding a kind". */
#ifndef ND_INTERNAL_H
#define ND_INTERNAL_H

#include "game/native_draw/native_draw.h"
#include "game/native_draw/nd_model.h"

/* The renderer's function table (only called while nd_kinds != 0) */
const ElRenderApi *nd_api(void);

/* A libultra Mtx (s15.16 halves) as 16 floats, m[row * 4 + col] (row vectors, as the RSP) */
void nd_decode_mtx(const void *mtx, float *m);

/* The kinds done: native_draw.c's table, one row per kind's file. `attach` runs once the renderer is
 * up (build the kind's entry lists there). */
typedef struct NdKind {
    int kind;             /* EL_KIND_* (-1 ends the table) */
    void (*attach)(void);
} NdKind;
extern const NdKind nd_kinds_table[];

/* ---- each kind's file ---- */
void nd_map_attach(void); /* nd_map.c */

#endif
