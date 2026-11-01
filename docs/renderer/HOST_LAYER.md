# The host layer (R0, 2026-10-31)

R0 of [PLAN.md](PLAN.md): the PC side of the game (window, input, sound, settings, the F1 menu) is
shared host code in `eltale_host.dll` (`libeltale_host.so` on Linux), together with the native
renderer (Granite), the only renderer since R8. It draws the game from its recordings
([NATIVE_DRAW.md](NATIVE_DRAW.md)), the Controller Pak manager's CPU-drawn screens as one texture,
and the F1 menu over both.

## Layout

```
port/
  gfx/host_bridge.c       (in the exe) loads eltale_host.dll (Linux: libeltale_host.so beside the
                          exe, else dlopen's search), sets the OS hooks (retrace, pacing, live pad,
                          AI buffers)
  host/                   eltale_host.dll (C++, SDL3, Dear ImGui), MinGW clang
    eltale_host.h         the C interface the exe calls (ElHost_*)
    host.h                the insides: settings, window, input, audio, menu, the Renderer interface
    host.cpp              ElHost_*: window, events, poll codes, present flow, the menu's pause loop
    settings.cpp          data folder, settings.json (version 2), presets
    input.cpp             keyboard, mouse + first gamepad -> N64 pad (bindings: docs/CONTROLS.md)
    audio.cpp             the audio ring (drift steering, underrun fade) on an SDL3 audio stream
    menu.cpp              F1 menu core pages (Game, Audio, Controls), hint, notices, ELTALE_MENU_TEST
    menu_graphics.cpp     the Graphics page: preset + every switch
    CMakeLists.txt        eltale_host.dll / libeltale_host.so: host + port/render + Granite + ImGui
                          + cJSON (+ SDL3.dll / libSDL3.so.0)
  render/                 (inside eltale_host.dll) the native renderer
    render_native.cpp     Granite Context/Device, the picture image
    present_vk.cpp/.h     the presenter, from KF4's present_vk.cpp (swapchain, vsync, 4:3 fit, ImGui pass)
    png.c/.h              PNG writer, from KF4's video.c
cmake/granite_*.cmake/.inc  KF4's three Granite patches (depth bounds, dynamic state, pipeline warm)
third_party/
  granite                 submodule, Themaister/Granite pinned to KF4's 16e7395f; only its volk and
                          khronos/vulkan-headers submodules are needed
  imgui                   Dear ImGui 1.92.9b, copied as KF4 does (core + SDL3 + Vulkan backends)
  cjson                   cJSON 1.7.18, copied (KF4's)
  (SDL3)                  3.4.16, downloaded by CMake, SHA-256 checked: on Windows the MinGW devel
                          package; on Linux the source release, built (unless a system SDL3 is found)
tools/build_host.py       configure + build out/host (CMake + Ninja, MinGW clang)
tools/build_linux.sh      Linux: the exe (Makefile, gcc) and out/host-linux (CMake), side by side
tests/host_checks.sh      the hidden-window checks
```

Why SDL3: the plan's choice (better gamepad and high-DPI support, the KF1 recipe for the MinGW
package).

### The DLLs

- `eltale_host.dll` is built with the game's MinGW clang (llvm-mingw 22). libc++, libunwind and
  winpthreads are linked in (`-static`): beside it goes only `SDL3.dll`.
- Reset restores the exe's writable sections (port/p0/host.c), so the window, the sound stream,
  ImGui and the Vulkan device all live in the DLL. The exe's bridge keeps only function pointers,
  set before the snapshot.
- On Linux the library is `libeltale_host.so` (gcc), with `libSDL3.so.0` beside it when SDL3 was
  built from source (found through its `$ORIGIN` rpath).
- The menu's pause is the host's: `ElHost_Present` loops while the menu is open, presenting the
  last picture again with the menu over it.

### Settings (settings.json, version 2)

```json
{ "version": 2,
  "audio": { "volume": 70, "mute_in_background": false },
  "window": { "fullscreen": false },
  "game": { "language": "en", "font": 0 },
  "graphics": { "preset": "everything", "resolution": "window", "resolution_scale": 4, "msaa": 4,
    "frame_rate": "fixed", "frame_rate_target": 240, "three_point": true,
    "scaling": "smooth_pixel_edges", "vsync": true, "widescreen": "16:9", "border": false,
    "draw_distance": "far", "n64_fog": false, "n64_dither": false, "shader_warmup": true } }
```

- `"controls"` (2026-11-01, docs/CONTROLS.md): mouse camera speed, invert, and per action three bindings
  (two keyboard/mouse, one controller), rebound on the Controls page. The host also hands the game the
  mouse camera's input (`ElHost_GetCamera`, read with `ElHost_GetPad`).
- A file from the RT64 builds (version 1: no `"version"`, RT64's options under `"graphics"`, e.g.
  `"antialiasing"`, `"refreshRate"`, `"resolutionMultiplier"`) loads cleanly: its sound, window
  (fullscreen), language, font and controls are kept; its graphics were RT64's, so the picture starts
  at the default preset, Everything on (stderr: `[host] settings.json: RT64's graphics options left
  out (the picture: Everything on)`).
- Keys this build does not know (RT64's, a `"renderer"`) are ignored and dropped the next time the
  menu saves (it then writes version 2). A setting left out of the file keeps its default
  (Everything on).
- **Presets** (owner decision 3) only set the switches; changing a switch makes the preset
  whichever one matches, else Custom.

| Switch | Classic | Classic + visuals | Everything on (default) | Notes |
|---|---|---|---|---|
| Resolution | original 320x240 | match window | match window | R7b |
| MSAA | off | 4x | 4x | R7b |
| Scaling to the window | sharp | smooth pixel edges | smooth pixel edges | sharp: nearest blit; smooth: linear blit; smooth pixel edges: area filter (NATIVE_DRAW.md "2D at a scale") |
| N64 three-point filtering | on | on | on | R2 |
| Frame rate | original 30 | original 30 | fixed 240 | R6: pictures between ticks |
| Vsync | on | on | on | the presenter |
| Widescreen (off / 16:9 / match the window) | off | off | 16:9 | R7a: NATIVE_DRAW.md, "Widescreen" |
| N64 border (8 px) | on | off | off | R2 |
| Draw distance | original (1x) | far (2x) | far (2x) | R7b; also farther, 3x |
| N64 fog | on | on | off | R2 |
| N64 dither | on | off | off | R7b |
| Shader warm-up | on | on | on | R7b |

Every switch changes the picture at once, except shader warm-up, which takes effect at the next start.

## Build

```
python tools/patches.py apply
python tools/build_windows.py --p1-os --build-only --rom <rom.z64>   # the exe (MinGW clang)
python tools/build_host.py                                          # out/host: eltale_host.dll + SDL3.dll
python tools/build_rdpref.py                                        # tests only: eltale-rdpref.exe (REFERENCE.md)
tools\play.cmd <rom.z64> [options]                                  # copies out\host\*.dll beside the exe, plays
```

`--window` plays with the native renderer; no flag is needed. `--renderer native` is still accepted
(it changes nothing), and `--renderer rt64` prints `host: --renderer rt64 is gone; the native renderer
draws` and plays.

Linux (built in WSL Ubuntu 24.04; the window is not yet tried there):

```
python3 tools/patches.py apply
bash tools/build_linux.sh --rom <rom.z64>      # or: make linux ROM=<rom.z64>; also --no-host, --clean
out/p1-linux/eltale-p1 --window --rom <rom.z64>
```

- `build_linux.sh` builds `out/p1-linux/eltale-p1` (the Makefile, gcc) and `out/host-linux`
  (CMake + Ninja), and copies `libeltale_host.so` (and `libSDL3.so.0`, when built) beside the exe.
  The exe loads `libeltale_host.so` from its own folder, else from dlopen's usual search. SDL3 is a
  system SDL3 if CMake finds one, or the prefix `ELTALE_SDL3_DIR` names, or else the 3.4.16 source
  release (downloaded, SHA-256 checked, built).
- It needs build-essential, cmake, ninja-build, pkg-config, libvulkan-dev, python3-yaml, python3-pil,
  python3-fonttools, and the X11/Wayland/ALSA/PulseAudio/PipeWire/udev/dbus/xkbcommon/libdecor/EGL/
  DRM/GBM development packages SDL3 builds against (the list is in the script's header).
- The data folder is `$XDG_DATA_HOME/eltale` (or `~/.local/share/eltale`).
- Headless on Linux, New Game 700 frames prints the same `P1_OS_OK` as Windows (`dma_chunks=416`,
  `last_hash=74b8baf63f9c1889`, audio `be0b67b7b718648e`), and a headless `--dump-frames` run
  (offscreen Vulkan) writes correct pictures. The window (input, sound, menu) is not yet tried.

- The first `build_host.py` downloads SDL3 (or set `ELTALE_SDL3_DIR` to an unpacked
  `SDL3-3.4.16/x86_64-w64-mingw32`).
- Configure applies KF4's Granite patches in `third_party/granite` (the submodule then shows as
  modified, as in KF4).
- A fresh checkout needs `git submodule update --init decomp third_party/granite`, then inside
  granite `git submodule update --init third_party/volk third_party/khronos/vulkan-headers`. The
  tests' reference (`build_rdpref.py`) also needs `third_party/parallel-rdp`.

## Tests

- `tests/p1_checks.sh` (headless) prints exactly what it printed before R0.
- `tests/host_checks.sh exe rom`: hidden window (`ELTALE_HIDDEN_WINDOW=1`), volume 0, Japanese text,
  a temporary data folder per run:
  - New Game 700 frames: `P1_OS_OK ... frames=700 ... dma_chunks=416` and
    `HOST_AUDIO queued=… played=…` (sound reached the device). `last_hash=6472f32d00cadc74` is the hash
    of the emptied display list (every kind is drawn natively, so the frame list the hash is taken of
    holds none of the game's commands); the game is the same (frames, DMA chunks and audio as headless,
    the 4:3 dumps' and `--wide`'s `P1_OS_OK` lines).
  - `ELTALE_MENU_TEST=200`: menu open, reset into the Pak manager (`P1_RESET pak-manager`), its
    screens, quit. `P1_NATIVE_CPU_FRAME n hash` for each screen read back from the GPU; the hashes
    equal the game's own `P1_CPU_FRAME` (`80a69197c1fb9325`, `43842e6ff23bd261`).
    - With `ELTALE_PRESENT_DUMP=dir` it also writes PNGs: `native_cpu_N` (the picture, 320x240),
      `native_window_N` (the window as shown), `native_menu` (the first picture with the menu),
      `native_clear` (the first gameplay picture).
  - Quick save at VI 1500 and quick load from the title: the same `P1_QUICKSAVE`/`P1_QUICKLOAD`
    lines as headless. It also writes the window with the F2 notice over it
    (`out/host_checks/native-notice/native_notice.png`: `ELTALE_PRESENT_DUMP` writes `native_notice` at the
    third picture a notice is on, once ImGui has sized its box) and `tools/notice_check.py` prints
    `NOTICE_CHECK OK WxH text N px in (x0,y0)-(x1,y1) lines=L` (white text in the top-left corner, above the HP
    meters' numbers). The notice box sits at the top left, a font's 0.6 in, over the top of the HP meter; at
    1706x960 its text is in (27,33)-(413,82): the start-up hint "F1 or Back: menu..." and under it "Quick saved
    (slot 0)".
  - Alt+Enter in the intro (`ELTALE_INPUT_TEST=tests/alt-enter.events`) is the fullscreen key, not
    Start: `ALT_ENTER OK` when that run ends as a run with no keys does and `tests/enter.events` (Enter
    alone) does not. A hidden window stays windowed when fullscreen is toggled.
  - `tools/lifetime_test.py` (a plain Reset in play in a hidden window, and full buffers headless;
    NATIVE_DRAW.md "Resets and full buffers"): a line per case, OK or FAIL.
- `ELTALE_PRESENT_DUMP=dir` writes the presented pictures as PNGs. `ELTALE_HIDDEN_WINDOW` also turns
  the presenter's vsync off, so a hidden window never waits for a display.
- `host::fatal(message)` (host.cpp): the game cannot go on (the native renderer's GPU device was lost): stderr, the
  data folder's `error.log`, a message box (not with `ELTALE_HIDDEN_WINDOW`), and `ElHost_Poll` returns
  `EL_POLL_QUIT`. The native presenter's tests for it, minimising and resizing (`ELTALE_SIM_GPU_LOST`,
  `ELTALE_SIM_MINIMISED`, `ELTALE_SIM_RESIZE`): NATIVE_DRAW.md, "The presenter's robustness".

## Since R2

`eltale_host.h` also has `ElHost_InitHeadless` (the native renderer without a window or SDL: for
`--dump-frames`), `ElHost_RenderApi` (the native renderer's function table, port/include/port/render.h)
and `ElHost_SetDump`. `Renderer::init_headless` is the native renderer's. The native renderer wires the
N64 border, N64 texture filtering and N64 fog switches. See [NATIVE_DRAW.md](NATIVE_DRAW.md).

## What R2 starts from

- `Renderer` (port/host/host.h) is the seam. `NativeRenderer::present_game` draws the recorded tick
  into the picture image.
- The picture is a 320x240 RGBA8 Granite image in GENERAL layout, blitted 4:3 by the presenter.
  R7's resolution and MSAA, widescreen fit and present filters (KF4's `present_scale.frag`) change
  the image's size and the `fit()` in present_vk.cpp.
- KF4's Granite patches are applied but unused: the dynamic-state and pipeline-warm hooks are
  ready for R2/R7 (`holyland_on_compile`, `holyland_on_render_pass`).
- Not done in R0:
  - shaders and `tools/build_shaders.py`;
  - the pipeline list;
  - the flash recorder (F8) and the present queue with due times (KF4's video.c pacing): done in R6
    (NATIVE_DRAW.md, "Pictures between ticks");
  - a true headless (windowless) Granite device: done in R2 (`ElHost_InitHeadless`);
  - the Linux .so: done in R8 (Build, above).
