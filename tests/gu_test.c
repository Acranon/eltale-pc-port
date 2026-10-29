#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "gu test: %s\n", message);
        exit(1);
    }
}

int main(void) {
    float mf[4][4] = {{0}};
    Mtx packet;
    const float subword = 3.0f / 262144.0f; /* 0.75 in 16.16 units */
    mf[0][0] = 1.5f;
    mf[0][1] = -0.5f;
    mf[0][2] = subword;
    mf[0][3] = -subword;
    memset(&packet, 0, sizeof packet);
    guMtxF2L(mf, &packet);
    const u32 *words = (const u32 *)&packet;
    check(words[0] == 0x0001FFFFu && words[8] == 0x80008000u,
          "1.5/-0.5 packed 16.16 words differ");
    check(words[1] == 0 && words[9] == 0,
          "float-to-fixed must truncate 0.75/-0.75 toward zero");
    guMtxIdent(&packet);
    check(words[0] == 0x00010000u && words[8] == 0,
          "identity matrix conversion differs");
    check(sins(0) == 0 && sins(0x4000) > 30000 && sins(0xC000) < -30000,
          "fixed-angle sine quadrants differ");
    puts("GU_FIXED_OK");
    return 0;
}
