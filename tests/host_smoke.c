/* The window/renderer library on its own, without the game or a ROM: load libeltale_host (beside this
 * program, as port/gfx/host_bridge.c loads it), open the window with the native renderer (Vulkan: MoltenVK
 * on macOS), present a moving test picture through the CPU-frame path for a second and a half, shut down.
 *     cc -std=gnu11 -O2 -Iport/include tests/host_smoke.c -o out/p1-macos/host-smoke && out/p1-macos/host-smoke
 * (ELTALE_HIDDEN_WINDOW=1 keeps the window hidden.) Prints HOST_SMOKE_OK. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#define HOST_LIB "libeltale_host.dylib"
#else
#include <unistd.h>
#define HOST_LIB "libeltale_host.so"
#endif

typedef int (*InitFn)(const char *, int, int);
typedef void (*CpuFrameFn)(const uint16_t *, int, int);
typedef int (*PollFn)(void);
typedef void (*VoidFn)(void);
typedef const char *(*DirFn)(void);

int main(void) {
    char path[4096];
#ifdef __APPLE__
    uint32_t size = sizeof path - 32;
    if (_NSGetExecutablePath(path, &size) != 0) return 1;
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 32);
    if (n <= 0) return 1;
    path[n] = 0;
#endif
    char *slash = strrchr(path, '/');
    strcpy(slash ? slash + 1 : path, HOST_LIB);
    void *lib = dlopen(path, RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "host_smoke: %s\n", dlerror());
        return 1;
    }
    InitFn init = (InitFn)dlsym(lib, "ElHost_Init");
    CpuFrameFn present = (CpuFrameFn)dlsym(lib, "ElHost_PresentCpuFrame");
    PollFn poll = (PollFn)dlsym(lib, "ElHost_Poll");
    VoidFn shutdown = (VoidFn)dlsym(lib, "ElHost_Shutdown");
    DirFn data_dir = (DirFn)dlsym(lib, "ElHost_DataDir");
    if (!init || !present || !poll || !shutdown || !data_dir) {
        fprintf(stderr, "host_smoke: the library lacks an entry point\n");
        return 1;
    }
    if (init("Eltale host smoke test", 640, 480) != 0) {
        fprintf(stderr, "host_smoke: ElHost_Init failed\n");
        return 1;
    }
    static uint16_t pixels[320 * 240];
    for (int frame = 0; frame < 90; frame++) {
        for (int y = 0; y < 240; y++)
            for (int x = 0; x < 320; x++) { /* RGBA5551 bands that move */
                const unsigned r = (unsigned)(x + frame * 2) >> 3 & 31, g = (unsigned)y >> 3 & 31, b = (unsigned)frame & 31;
                pixels[y * 320 + x] = (uint16_t)(r << 11 | g << 6 | b << 1 | 1);
            }
        present(pixels, 320, 240);
        if (poll() < 0) break;
    }
    printf("host_smoke: data folder %s\n", data_dir());
    shutdown();
    printf("HOST_SMOKE_OK\n");
    return 0;
}
