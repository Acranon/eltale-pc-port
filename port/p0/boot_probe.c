#include "port/boot_probe.h"
#include "game/core/boot.h"
#include "game/core/main.h"
#include "game/event/cutscene.h"
#ifdef ELTALE_P1_OS
#include "port/gfx.h"
extern void (*Port_GfxTaskHook)(OSTask *task, unsigned frame);
extern void (*Port_AudioTaskHook)(OSTask *task);
extern void (*Port_PiDmaHook)(void *dst, u32 size);
#ifdef ELTALE_P1_AUDIO
#include "port/audio.h"
#endif
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

#ifdef ELTALE_P1_AUDIO
extern void (*Port_AiBufferHook)(void *buffer, u32 bytes, u32 rate);
/* A hash of everything the game plays (headless audio check). */
static uint64_t ai_hash = UINT64_C(0xCBF29CE484222325);
static unsigned ai_buffers, ai_nonzero;
static FILE *ai_wav;
static u32 ai_wav_rate, ai_wav_bytes;
static void hash_ai_buffer(void *buffer, u32 bytes, u32 rate) {
    const u8 *p = buffer;
    if (ai_wav) { /* ELTALE_AUDIO_WAV=path: the AI output as a WAV file (16-bit stereo) */
        if (!ai_wav_rate) ai_wav_rate = rate;
        fwrite(buffer, 1, bytes, ai_wav);
        ai_wav_bytes += bytes;
    }
    for (u32 i = 0; i < bytes; i++) {
        ai_hash = (ai_hash ^ p[i]) * UINT64_C(0x100000001B3);
        ai_nonzero += p[i] != 0;
    }
    ai_buffers++;
}
static void put_le(FILE *f, u32 v, int n) { for (int i = 0; i < n; i++) fputc((int)(v >> (8 * i)) & 0xFF, f); }
static void ai_wav_close(void) {
    if (!ai_wav) return;
    fseek(ai_wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, ai_wav); put_le(ai_wav, 36 + ai_wav_bytes, 4); fwrite("WAVEfmt ", 1, 8, ai_wav);
    put_le(ai_wav, 16, 4); put_le(ai_wav, 1, 2); put_le(ai_wav, 2, 2); put_le(ai_wav, ai_wav_rate, 4);
    put_le(ai_wav, ai_wav_rate * 4, 4); put_le(ai_wav, 4, 2); put_le(ai_wav, 16, 2);
    fwrite("data", 1, 4, ai_wav); put_le(ai_wav, ai_wav_bytes, 4);
    fclose(ai_wav);
    ai_wav = NULL;
}
#endif

int main(int argc, char **argv) {
    const char *rom = NULL;
#ifdef ELTALE_P1_OS
    const char *input = NULL, *pak = NULL;
#endif
    unsigned requested_frames = 1;
    int frames_given = 0;
    int fast = 0;
    int window = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom = argv[++i];
#ifdef ELTALE_P1_OS
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input = argv[++i];
        else if (!strcmp(argv[i], "--pak") && i + 1 < argc) pak = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            requested_frames = (unsigned)strtoul(argv[++i], NULL, 10);
            frames_given = 1;
            if (!requested_frames || requested_frames > 10000)
                Port_HostFatal("--frames must be between 1 and 10000");
        }
#endif
        else if (!strcmp(argv[i], "--fast")) fast = 1;
        else if (!strcmp(argv[i], "--headless")) { }
#ifdef ELTALE_P1_OS
        else if (!strcmp(argv[i], "--window")) window = 1;
#endif
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
    /* a window runs until it is closed, unless --frames was given */
    Port_OsSetFrameLimit(window && !frames_given ? 0 : requested_frames);
    Port_OsSetFast(fast);
    if (getenv("ELTALE_FRAME_IMAGE")) Port_GfxTaskHook = Port_FrameImageTrace;
#ifdef ELTALE_P1_AUDIO
    Port_AudioTaskHook = (void (*)(OSTask *))Port_RunAudioTask;
    /* the reference microcode: play with it, or check the HLE against it on every task */
    if (getenv("ELTALE_AUDIO_LLE")) Port_AudioTaskHook = (void (*)(OSTask *))Port_RunAudioTaskLle;
    if (getenv("ELTALE_AUDIO_CHECK")) Port_AudioTaskHook = (void (*)(OSTask *))Port_CheckAudioTask;
    Port_PiDmaHook = Port_LleNotePiDma;
    Port_AiBufferHook = hash_ai_buffer;
    if (getenv("ELTALE_AUDIO_WAV") && (ai_wav = fopen(getenv("ELTALE_AUDIO_WAV"), "wb")))
        fseek(ai_wav, 44, SEEK_SET);
#endif
    if (window && Port_Rt64Start() != 0) return 1;
    Boot_Main(0);
    Port_OsRun();
    if (!window && (Port_OsFrameCount() != requested_frames ||
        Port_OsGfxTaskCount() < requested_frames || intro_pak_check_done != 1))
        Port_HostFatal("P1 OS did not complete the requested intro frames");
#ifdef ELTALE_P1_AUDIO
    ai_wav_close();
    Port_CheckAudioReport();
    printf("P1_AUDIO buffers=%u nonzero_bytes=%u hash=%016llx\n", ai_buffers, ai_nonzero, (unsigned long long)ai_hash);
#endif
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
