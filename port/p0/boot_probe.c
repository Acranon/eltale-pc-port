#include "port/boot_probe.h"
#include "game/core/boot.h"
#include "game/core/main.h"
#include "port/p0/host.h"

#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(Gfx) == 8, "N64 Gfx commands must stay eight bytes");
_Static_assert(sizeof(Mtx) == 64, "N64 matrix packets must stay 64 bytes");

GfxFrame port_gfx_frames[2];
static jmp_buf boot_done;
static unsigned display_list_words;

void Port_P0DisplayListReady(struct GfxFrame *raw_frame) {
    GfxFrame *frame = (GfxFrame *)raw_frame;
    Gfx *dl = frame->data.dl;
    unsigned words = frame->task.list.t.data_size / sizeof(Gfx);
    if (words < 12 || words > ARRAY_COUNT(frame->data.dl))
        Port_HostFatal("invalid first display-list length");
    fprintf(stderr, "P0 DL words=%u last=%08x:%08x expected_end=%02x\n",
            words, dl[words - 1].words.w0, dl[words - 1].words.w1, (u8)G_ENDDL);
    if ((dl[words - 1].words.w0 >> 24) != (u8)G_ENDDL)
        Port_HostFatal("first display list has no end command");
    display_list_words = words;
    longjmp(boot_done, 1);
}

int main(int argc, char **argv) {
    const char *rom = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
        else if (!strcmp(argv[i], "--headless") || !strcmp(argv[i], "--fast")) { }
        else {
            fprintf(stderr, "usage: eltale-p0 --rom path [--headless --fast]\n");
            return 2;
        }
    }
    if (!rom) rom = getenv("ELTALE_ROM");
    if (!rom) Port_HostFatal("supply --rom or ELTALE_ROM");
    if (Port_HostInit(rom) != 0) return 1;
    if (setjmp(boot_done) == 0) {
        Boot_Main(0);
        Port_HostFatal("Boot_Main returned before a display list");
    }
    if (Port_HostDmaCount() < 3 || Port_HostThreadCount() < 5 || Port_HostRunThreadCount() != 2)
        Port_HostFatal("boot skipped DMA or thread creation");
    if (!Port_HostRomSpanMatches(0xDA4570, 0x80100000, 0x7080) ||
        !Port_HostRomSpanMatches(0xD3A8B0, 0x80332D30, 0x80) ||
        !Port_HostRomSpanMatches(0xD3A930, 0x803245A0, 0xE790) ||
        !Port_HostRomSpanMatches(0x71920, 0x80095AF0, 0x3AD0))
        Port_HostFatal("boot DMA bytes differ from ROM");
    printf("P0_BOOT_OK threads_created=%u threads_started=%u threads_run=%u dma_chunks=%u dma_bytes=%zu dl_words=%u\n",
           Port_HostThreadCount(), Port_HostStartedThreadCount(), Port_HostRunThreadCount(),
           Port_HostDmaCount(), Port_HostDmaBytes(), display_list_words);
    return 0;
}
