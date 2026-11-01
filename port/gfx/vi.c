/* The VI on the host: the mode, black and special-feature state the game sets, and the VI
 * registers they give for a framebuffer, as libultra's VI manager computes them (used to show a
 * frame with RT64; see port/gfx/host_bridge.c). */
#include "port/gfx.h"

static OSViMode *vi_mode;
static int vi_black;
static u32 vi_features_ctrl_set, vi_features_ctrl_clear;

void osViSetMode(OSViMode *mode) { vi_mode = mode; }
void osViBlack(u8 active) { vi_black = active != 0; }

void osViSetSpecialFeatures(u32 f) {
    /* OS_VI_GAMMA_ON 1, _OFF 2, GAMMA_DITHER_ON 4, _OFF 8, DIVOT_ON 0x10, _OFF 0x20,
     * DITHER_FILTER_ON 0x40, _OFF 0x80 -> VI_CTRL bits 8, 4, 0x10, 0x10000 */
    static const struct { u32 on, off, bit; } map[] = {
        { 0x01, 0x02, 0x00008 }, { 0x04, 0x08, 0x00004 }, { 0x10, 0x20, 0x00010 }, { 0x40, 0x80, 0x10000 },
    };
    for (unsigned i = 0; i < 4; i++) {
        if (f & map[i].on) { vi_features_ctrl_set |= map[i].bit; vi_features_ctrl_clear &= ~map[i].bit; }
        if (f & map[i].off) { vi_features_ctrl_clear |= map[i].bit; vi_features_ctrl_set &= ~map[i].bit; }
    }
}

int Port_ViRegs(u32 framebuffer, PortViRegs *r) {
    if (!vi_mode || !framebuffer) return 0;
    const OSViCommonRegs *c = &vi_mode->comRegs;
    const OSViFieldRegs *fld = &vi_mode->fldRegs[0];
    u32 phys = framebuffer & 0x00FFFFFF;
    r->status = (c->ctrl | vi_features_ctrl_set) & ~vi_features_ctrl_clear;
    r->origin = phys + fld->origin;
    r->width = c->width;
    r->intr = fld->vIntr;
    r->v_current = 0;
    r->timing = c->burst;
    r->v_sync = c->vSync;
    r->h_sync = c->hSync;
    r->leap = c->leap;
    r->h_start = vi_black ? 0 : c->hStart;
    r->v_start = fld->vStart;
    r->v_burst = fld->vBurst;
    r->x_scale = c->xScale;
    r->y_scale = fld->yScale;
    return 1;
}
