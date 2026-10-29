#include "port/boot_probe.h"
#include "game/core/boot.h"
#include "game/core/main.h"
#include "game/event/cutscene.h"
#ifdef ELTALE_P1_OS
#include "port/gfx.h"
extern void (*Port_GfxTaskHook)(OSTask *task, unsigned frame);
#endif
#include "port/p0/host.h"
#ifdef ELTALE_P1_OS
#include "port/controller.h"
#include "port/os.h"
#include "port/pak.h"
#endif

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
static uint64_t display_list_hash;

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
    display_list_hash = UINT64_C(0xCBF29CE484222325);
    for (unsigned i = 0; i < words; i++) {
        uint32_t pair[2] = { dl[i].words.w0, dl[i].words.w1 };
        for (unsigned j = 0; j < 2; j++) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                display_list_hash ^= (pair[j] >> shift) & 0xFFu;
                display_list_hash *= UINT64_C(0x100000001B3);
            }
        }
    }
#ifdef ELTALE_P1_OS
    Port_OsStop();
#else
    longjmp(boot_done, 1);
#endif
}

int main(int argc, char **argv) {
    const char *rom = NULL;
#ifdef ELTALE_P1_OS
    const char *input = NULL, *pak = NULL;
#endif
    unsigned requested_frames = 1;
    int fast = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
#ifdef ELTALE_P1_OS
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--pak") && i + 1 < argc) pak = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            requested_frames = (unsigned)strtoul(argv[++i], NULL, 10);
            if (!requested_frames || requested_frames > 10000)
                Port_HostFatal("--frames must be between 1 and 10000");
        }
#endif
        else if (!strcmp(argv[i], "--fast")) fast = 1;
        else if (!strcmp(argv[i], "--headless")) { }
        else {
#ifdef ELTALE_P1_OS
            fprintf(stderr, "usage: eltale-p1 --rom path [--headless --fast --frames N --input script --pak image.mpk]\n");
#else
            fprintf(stderr, "usage: eltale-p0 --rom path [--headless --fast]\n");
#endif
            return 2;
        }
    }
    if (!rom) rom = getenv("ELTALE_ROM");
    if (!rom) Port_HostFatal("supply --rom or ELTALE_ROM");
    if (Port_HostInit(rom) != 0) return 1;
#ifdef ELTALE_P1_OS
    if (Port_ControllerLoadScript(input) || Port_PakLoad(pak)) return 1;
    Port_OsInit();
    Port_OsSetFrameLimit(requested_frames);
    Port_OsSetFast(fast);
    if (getenv("ELTALE_FRAME_IMAGE")) Port_GfxTaskHook = Port_FrameImageTrace;
    Boot_Main(0);
    Port_OsRun();
    if (Port_OsFrameCount() != requested_frames ||
        Port_OsGfxTaskCount() < requested_frames || intro_pak_check_done != 1)
        Port_HostFatal("P1 OS did not complete the requested intro frames");
    printf("P1_OS_OK frames=%u gfx_tasks=%u threads_run=%u dma_chunks=%u mode=%u last_hash=%016llx\n",
           Port_OsFrameCount(), Port_OsGfxTaskCount(), Port_OsRunCount(),
           Port_HostDmaCount(), core_game_mode, (unsigned long long)Port_OsLastGfxHash());
    return 0;
#else
    if (setjmp(boot_done) == 0) {
        Boot_Main(0);
        Port_HostFatal("Boot_Main returned before a display list");
    }
#endif
    if (Port_HostDmaCount() < 3 || Port_HostThreadCount() < 5 ||
        Port_HostRunThreadCount() !=
#ifdef ELTALE_P1_OS
            5
#else
            2
#endif
        )
        Port_HostFatal("boot skipped DMA or thread creation");
    if (core_game_mode != 4 || intro_pak_check_done != 1 || Port_HostIntroUpdateCount() != 1)
        Port_HostFatal("the first intro update did not complete");
    if (display_list_hash != UINT64_C(0x0CF266FD815BB6A0))
        Port_HostFatal("first intro display-list command hash changed");
    if (!Port_HostRomSpanMatches(0xDA4570, 0x80100000, 0x7080) ||
        !Port_HostRomSpanMatches(0xD3A8B0, 0x80332D30, 0x80) ||
        !Port_HostRomSpanMatches(0xD3A930, 0x803245A0, 0xE790) ||
        !Port_HostRomSpanMatches(0x71920, 0x80095AF0, 0x3AD0) ||
        !Port_HostRomSpanMatches(0x869620, 0x80206000, 0x60D0) ||
        !Port_HostRomSpanMatches(0xDC62B0, 0x8020C0D0, 0x5C3E0))
        Port_HostFatal("boot DMA bytes differ from ROM");
    printf("P0_BOOT_OK intro_updates=%u threads_created=%u threads_started=%u threads_run=%u dma_chunks=%u dma_bytes=%zu dl_words=%u dl_hash=%016llx\n",
           Port_HostIntroUpdateCount(),
           Port_HostThreadCount(), Port_HostStartedThreadCount(), Port_HostRunThreadCount(),
           Port_HostDmaCount(), Port_HostDmaBytes(), display_list_words,
           (unsigned long long)display_list_hash);
    return 0;
}
