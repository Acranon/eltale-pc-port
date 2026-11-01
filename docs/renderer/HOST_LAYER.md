# The host layer (R0, 2026-10-31)

R0 of [PLAN.md](PLAN.md): the PC side of the game moved out of RT64's DLL into shared host code,
with a renderer switch. RT64 still draws by default and the game plays as before. The native
renderer (Granite) exists as a frame: a clear colour in gameplay, the Controller Pak manager's
CPU-drawn screens as one texture, and the F1 menu over both.

## Layout

```
port/
  gfx/host_bridge.c       (in the exe) loads eltale_host.dll, --renderer rt64|native, sets the
                          OS hooks (graphics task, retrace, live pad, AI buffers); replaces rt64_bridge.c
  host/                   eltale_host.dll (C++, SDL3, Dear ImGui), MinGW clang
    eltale_host.h         the C interface the exe calls (ElHost_*)
    host.h                the insides: settings, window, input, audio, menu, the Renderer interface
    host.cpp              ElHost_*: window, events, poll codes, present flow, the menu's pause loop
    settings.cpp          data folder, settings.json (version 2), presets
    input.cpp             keyboard + first gamepad -> N64 pad (fixed bindings, as before)
    audio.cpp             the audio ring (drift steering, underrun fade) on an SDL3 audio stream
    menu.cpp              F1 menu core pages (Game, Audio, Controls), hint, notices, ELTALE_MENU_TEST
    menu_graphics.cpp     the Graphics page: preset + every switch; each renderer wires its own
    renderer_rt64.cpp     --renderer rt64: loads eltale_rt64.dll, hands it the menu's draw lists
    CMakeLists.txt        eltale_host.dll: host + port/render + Granite + ImGui + cJSON (+ SDL3.dll)
  render/                 (inside eltale_host.dll) the native renderer
    render_native.cpp     --renderer native: Granite Context/Device, the 320x240 picture image
    present_vk.cpp/.h     the presenter, from KF4's present_vk.cpp (swapchain, vsync, 4:3 fit, ImGui pass)
    png.c/.h              PNG writer, from KF4's video.c
  rt64/                   eltale_rt64.dll (clang-cl): now RT64 only. No window, SDL, input or menu
                          of its own: it draws on the host's HWND and draws the host's menu lists
cmake/granite_*.cmake/.inc  KF4's three Granite patches (depth bounds, dynamic state, pipeline warm)
third_party/
  granite                 submodule, Themaister/Granite pinned to KF4's 16e7395f; only its volk and
                          khronos/vulkan-headers submodules are needed
  imgui                   Dear ImGui 1.92.9b, copied as KF4 does (core + SDL3 + Vulkan backends)
  cjson                   cJSON 1.7.18, copied (KF4's)
  (SDL3)                  3.4.16 MinGW devel package, downloaded by CMake, SHA-256 checked
tools/build_host.py       configure + build out/host (CMake + Ninja, MinGW clang)
tests/host_checks.sh      the hidden-window checks for both renderers
```

Why SDL3: the plan's choice (better gamepad and high-DPI support, the KF1 recipe for the MinGW
package). The RT64 DLL keeps its own SDL2.dll only because RT64's static library imports it; it is
never initialised.

### The DLLs

- `eltale_host.dll` is built with the game's MinGW clang (llvm-mingw 22). libc++, libunwind and
  winpthreads are linked in (`-static`): beside it goes only `SDL3.dll`.
- Reset restores the exe's writable sections (port/p0/host.c), so the window, the sound stream,
  ImGui and the Vulkan device all live in the DLL. The exe's bridge keeps only function pointers,
  set before the snapshot.
- `eltale_rt64.dll` stays clang-cl (RT64 needs the MSVC ABI). The host loads it for
  `--renderer rt64` and passes the window's HWND (`ElRt64_Init`). The menu crosses the ABI as plain
  data: the host's ImGui runs without a renderer backend (the atlas built up front, ImGui's legacy
  path), its draw lists are flattened (`ElRt64Overlay`), and the RT64 DLL replays them into the
  foreground list of RT64's own ImGui (its Inspector, Win32 platform side, no input), whose font
  texture is the host's atlas (`ElRt64_SetOverlayAtlas`).
- The menu's pause is the host's: `ElHost_Present` loops while the menu is open, presenting the
  last picture again with the menu over it (RT64: its debugger pause, so it re-presents; native:
  the presenter shows the picture image again).

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
- A version-1 file (eltale_rt64's menu) is read once: its RT64 `graphics` block maps to the
  switches RT64 had (resolution, multiplier, MSAA, scaling filter, three-point, frame rate), the
  rest as Everything on, and the preset is the one that matches (else Custom).
- **Presets** (owner decision 3) only set the switches; changing a switch makes the preset
  whichever one matches, else Custom.

| Switch | Classic | Classic + visuals | Everything on (default) | RT64 | native |
|---|---|---|---|---|---|
| Resolution | original 320x240 | match window | match window | wired | wired (R7b) |
| MSAA | off | 4x | 4x | wired | wired (R7b) |
| Scaling to the window | sharp | smooth pixel edges | smooth pixel edges | wired | wired (sharp: nearest blit; smooth: linear blit; smooth pixel edges: area filter, NATIVE_DRAW.md "2D at a scale") |
| N64 three-point filtering | on | on | on | wired | wired (R2) |
| Frame rate | original 30 | original 30 | fixed 240 | wired (RT64's interpolation) | wired (R6: pictures between ticks) |
| Vsync | on | on | on | — | wired |
| Widescreen (off / 16:9 / match the window) | off | off | 16:9 | later | wired (R7a: NATIVE_DRAW.md, "Widescreen") |
| N64 border (8 px) | on | off | off | later | wired (R2) |
| Draw distance | original (1x) | far (2x) | far (2x) | later | wired (R7b; also farther, 3x) |
| N64 fog | on | on | off | later | wired (R2) |
| N64 dither | on | off | off | later | wired (R7b) |
| Shader warm-up | on | on | on | later | wired (R7b) |

"later" switches are kept and shown greyed with "(later)" in the Graphics page.

## Build

```
python tools/build_windows.py --p1-os --build-only --rom <rom.z64>   # the exe (MinGW clang)
python tools/build_host.py                                          # out/host: eltale_host.dll + SDL3.dll
tools\build_rt64.cmd                                                # out/rt64: eltale_rt64.dll (clang-cl, VS Build Tools)
tools\play.cmd <rom.z64> [--renderer native]                        # copies both sets of DLLs, plays
```

- The first `build_host.py` downloads SDL3 (or set `ELTALE_SDL3_DIR` to an unpacked
  `SDL3-3.4.16/x86_64-w64-mingw32`).
- Configure applies KF4's Granite patches in `third_party/granite` (the submodule then shows as
  modified, as in KF4).
- A fresh checkout needs `git submodule update --init third_party/granite`, then inside it
  `git submodule update --init third_party/volk third_party/khronos/vulkan-headers`.

## Tests

- `tests/p1_checks.sh` (headless) prints exactly what it printed before R0.
- `tests/host_checks.sh exe rom [rt64|native]`: hidden window (`ELTALE_HIDDEN_WINDOW=1`), volume 0,
  Japanese text, a temporary data folder per run. Per renderer:
  - New Game 700 frames: `P1_OS_OK ... frames=700 ... dma_chunks=416` and
    `HOST_AUDIO queued=… played=…` (sound reached the device). RT64: `last_hash=74b8baf63f9c1889` (as
    headless). Native: `last_hash=6472f32d00cadc74`, the hash of the emptied display list (every kind is drawn
    natively since R5, so the frame list the hash is taken of holds none of the game's commands); the game is
    the same (frames, DMA chunks and audio as headless, the 4:3 dumps' and `--wide`'s `P1_OS_OK` lines).
  - `ELTALE_MENU_TEST=200`: menu open, reset into the Pak manager (`P1_RESET pak-manager`), its
    screens, quit.
    - RT64 prints `[rt64] overlay drawn` once the menu's lists reached it.
    - Native prints `P1_NATIVE_CPU_FRAME n hash` for each screen read back from the GPU. The
      hashes equal the game's own `P1_CPU_FRAME` (`80a69197c1fb9325`, `43842e6ff23bd261`).
    - With `ELTALE_PRESENT_DUMP=dir` it also writes PNGs: `native_cpu_N` (the picture, 320x240),
      `native_window_N` (the window as shown), `native_menu` (the first picture with the menu),
      `native_clear` (the first gameplay picture).
  - Quick save at VI 1500 and quick load from the title: the same `P1_QUICKSAVE`/`P1_QUICKLOAD`
    lines as headless. Native also writes the window with the F2 notice over it
    (`out/host_checks/native-notice/native_notice.png`: `ELTALE_PRESENT_DUMP` writes `native_notice` at the
    third picture a notice is on, once ImGui has sized its box) and `tools/notice_check.py` prints
    `NOTICE_CHECK OK WxH text N px in (x0,y0)-(x1,y1) lines=L` (white text in the top-left corner, above the HP
    meters' numbers). The notice box sits at the top left, a font's 0.6 in, over the top of the HP meter; at
    1706x960 its text is in (27,33)-(413,82): the start-up hint "F1 or Back: menu..." and under it "Quick saved
    (slot 0)".
- New env: `ELTALE_PRESENT_DUMP` (native only). `ELTALE_HIDDEN_WINDOW` also turns the native
  presenter's vsync off, so a hidden window never waits for a display.

## Since R2

`eltale_host.h` also has `ElHost_InitHeadless` (the native renderer without a window or SDL: for
`--dump-frames`), `ElHost_RenderApi` (the native renderer's function table, port/include/port/render.h)
and `ElHost_SetDump`. `Renderer::init_headless` is the native renderer's. The native renderer wires the
N64 border, N64 texture filtering and N64 fog switches. See [NATIVE_DRAW.md](NATIVE_DRAW.md).

## What R2 starts from

- `Renderer` (port/host/host.h) is the seam. `NativeRenderer::present_game` clears the picture
  image; R2 puts the recorded tick's draws there. `draw_task` is where a display-list bridge would
  go, if one is wanted.
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
  - a true headless (windowless) Granite device;
  - the Linux .so.
