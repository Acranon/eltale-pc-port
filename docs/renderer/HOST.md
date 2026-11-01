# Eltale native renderer: the host side and what we can reuse (research, 2026-10-31)

> The survey made before R0. RT64 is gone since R8: the host ([HOST_LAYER.md](HOST_LAYER.md)) and the
> native renderer ([NATIVE_DRAW.md](NATIVE_DRAW.md)) do everything section 1 lists.

This covers the host layer and reuse. The game's own draw code is surveyed separately.
Sources read:
- Eltale (C:\code\eltale): PLAN.md (P1, P2), docs/P1_OS_STATUS.md, port/rt64/*, port/gfx/*, port/os/kernel.c (hooks), port/p0/host.c (pacing, reset), port/p0/boot_probe.c (main loop), port/audio/rsp_lle.c, tests/p1_checks.sh, tools/build_rt64.cmd, tools/build_windows.py, tools/play.cmd, Makefile.
- KF4 (C:\code\kf4-holyland, read only): docs/port/RENDERER.md, NATIVE_DRAW.md, NATIVE_DRAW_P8.md, CMakeLists.txt, CMakePresets.json, port/platform/present_vk.cpp, video.c, port/overlay/*, port/gs/native/*, port/render/*, tools/*.
- KF1 (C:\code\kf1-recomp\decomp-port): CMakeLists.txt, src/renderer/.

---

## 1. What the RT64 path does today: the duties to take over

The chain today:
- `port/os/kernel.c` has `Port_GfxTaskHook` (called in `osSpTaskStartGo` for each graphics task) and `Port_ViHook` (called at every VI retrace, 60 Hz, with the current framebuffer).
- `port/gfx/rt64_bridge.c` (MinGW clang, inside the exe) loads `eltale_rt64.dll` with LoadLibrary/dlopen and sets those hooks, plus `Port_LivePad` and `Port_AiBufferHook`.
- `eltale_rt64.dll` (clang-cl, MSVC ABI, CMake) owns SDL2, RT64 and the menu.

| # | Duty | Where it is today | Tied to RT64? | Notes for the new host |
|---|---|---|---|---|
| 1 | **Window**: SDL2 window, 1280x960, resizable, `SDL_WINDOW_VULKAN`, title | `ElRt64_Init` | No (plain SDL) | Take it over directly. |
| 2 | **Fullscreen**: F11 / Alt+Enter, `SDL_WINDOW_FULLSCREEN_DESKTOP`, kept in settings.json `window.fullscreen` | menu.cpp `set_fullscreen`, `handle_event` | No | Take it over directly. |
| 3 | **Resize**: RT64 follows the window size (resolution "Match the window") | RT64 internals | Yes | Swapchain re-made on resize or out-of-date (KF4 present_vk.cpp `stale`). |
| 4 | **Graphics API**: D3D12 or Vulkan (`GraphicsAPI::Automatic`, which picks D3D12 on Windows), RT64's plume RHI | RT64 | Yes | The new renderer would be Vulkan only, as KF4 is. RT64 stays available as the fallback for GPUs without Vulkan while it remains. |
| 5 | **Drawing**: `ElRt64_DrawTask`: byte-swaps the whole 8 MiB frame image into RT64's RDRAM (host-order words) on every task, except the "keep" ranges (both framebuffers and Z: 0x3B5000, 0x3DA800, 0x2DA800), then `loadUCodeGBI` and `processDisplayLists`; `forceBranch` and the dither noise flag set as in the Quest 64 recomp | eltale_rt64.cpp | Yes | RT64-only, and the keep ranges are an RT64 workaround. The native path draws from the game's own calls (P2), or from the display list (step 1). That avoids the 8 MiB copy-and-swap per frame. |
| 6 | **Present and VI**: `ElRt64_Present` at every retrace, with VI registers made by `port/gfx/vi.c` (from OSViMode, `osViBlack` which zeroes h_start, and `osViSetSpecialFeatures`: gamma, gamma dither, divot, dither filter). RT64 emulates the VI scan-out (origin, x/y scale, h/v start) | rt64_bridge.c `present`, vi.c | Yes (RT64 reads VI regs) | The new presenter needs: the framebuffer the VI shows, black (`osViBlack`), the 320x240 area from the VI scale and start, gamma on or off, and the dither filter as a "N64 look" switch (off by default under the modern-default rule). |
| 7 | **CPU-drawn frames** (Controller Pak manager, before the game's frames exist): any framebuffer other than GAME_FB0/FB1 is treated as CPU-drawn. `Port_BuildCpuFrameImage` puts its RGBA5551 pixels into the image with a DL of 40 texture rectangles that copies them into FB0, which RT64 draws | rt64_bridge.c `present`, frame_image.c | The texrect trick exists only because RT64 needs a display list | Natively: upload the 320x240 RGBA5551 framebuffer as a texture and present it. The headless `P1_CPU_FRAME` hash already covers the content. |
| 8 | **Menu (F1 / controller Back)**: ImGui with pages Game (Resume, Text language, English font, Reset, Reset to the Pak manager, save folder path and "Open", Quit with confirm popups), Audio (volume, mute in background), Graphics, Controls (fixed table). The game pauses inside `ElRt64_Present` while it is open (a loop of `SDL_Delay(15)`, events, `updateScreen` with RT64's `debuggerInspector.paused` so it re-presents). Start-up hint (6 s) and notices (4 s, mutex-guarded, set from the game thread) | menu.cpp | **Yes**: ImGui is set up and drawn through `RT64::Inspector` (made on RT64's device and swapchain), `newFrame(app->framebufferGraphicsWorker)`, drawn from RT64's present hook `RT64::SetRenderHooks(nullptr, draw_hook, deinit_hook)`. The pause relies on RT64's debugger pause. The Graphics page edits `RT64::UserConfiguration` | Needs a new home: our own ImGui context with `imgui_impl_sdl3` (or sdl2) and `imgui_impl_vulkan` drawn in the presenter's pass (exactly KF4's present_vk.cpp + overlay.cpp). The pause becomes "present the last picture plus the menu" in our presenter. Game, Audio and Controls pages move as they are; only Graphics is rewritten. |
| 9 | **Graphics settings** (Graphics page): resolution Original 320x240 / Match the window / multiple 1-8x; MSAA off/2/4/8 (offered only when sample locations are supported); scaling to the window Sharp / Smooth / Smooth pixel edges; N64 three-point texture filtering; frame rate Original 30 / Match the display (smoothed) / Fixed 30-240 | menu.cpp `graphics_page` | Yes (`RT64::UserConfiguration`, `updateMultisampling`, `updateUserConfig`) | New settings of our own. The frame-rate option is RT64's interpolation; ours comes from P2's pictures between. |
| 10 | **settings.json** in the data folder: `audio.volume`, `audio.mute_in_background`, `window.fullscreen`, `game.language` (en/jp), `game.font` (0-5), and `graphics` = RT64's UserConfiguration JSON (nlohmann). No "version" key | menu.cpp `load_settings` / `save_settings` | The "graphics" block is RT64's | Add `"version": 2` and a new `"graphics"` (or `"display"`) block. Keep reading the RT64 block for `--renderer rt64` while it exists. KF4's settings.c migration (version 2, defaults changed) is the pattern to follow. |
| 11 | **Data folder**: `ELTALE_DATA_DIR`, else `%APPDATA%\eltale`, else `eltale-data`; created if missing. The game uses it for `controller-pak.mpk` and `quicksaves/` | menu.cpp `data_dir`, `ElRt64_DataDir`; boot_probe.c, quicksave.c | No | Move to the host. Linux should use `$XDG_DATA_HOME/eltale` (today it falls back to `eltale-data` because APPDATA is unset). |
| 12 | **Text language and font** for en_text.c (`ElRt64_Language`, `ElRt64_TextFont`; -1 or 0 without a window) | menu.cpp | No | Host API. |
| 13 | **Input**: SDL keyboard state plus the first SDL_GameController (hot-plug) mapped to N64 buttons and stick (-80..80, dead zone 7000), fixed bindings, zeroed while the menu is open. Feeds `Port_LivePad` | eltale_rt64.cpp `ElRt64_GetPad` | No | Move as is. Remapping is a later item (KF4's controls.c/bindings is the model). |
| 14 | **Hot keys**: F1 / Esc / Back for the menu; F2 / F4 quick save/load, taken by the game through `TakeQuickKey`, with a notice if not taken within 8 retraces (bridge); F11 / Alt+Enter fullscreen | menu.cpp `handle_event`, bridge `present` | No | Move as is. KF4 adds F8 (flash recorder) and F3 (counter); add those with the tools. |
| 15 | **Audio output**: SDL2 audio device opened at the AI rate the game sets (re-opened when the rate changes), a 32k-frame ring, start at ~67 ms fill, drift steering by at most 0.5% with linear interpolation, underrun fade, drop to target when far ahead. Volume and mute-in-background from the menu | eltale_rt64.cpp `ElRt64_QueueAudio`, `audio_callback` | No (it only sits in the RT64 DLL) | Move as is. With SDL3, `SDL_AudioStream` + `SDL_SetAudioStreamFrequencyRatio` could replace the hand-made resampling, but the ring is proven: port it first. |
| 16 | **Poll / lifecycle**: `ElRt64_Poll` pumps events and returns carry on / quit / reset / reset into the Pak manager; quit on window close | eltale_rt64.cpp, menu.cpp | Partly (it's in the DLL) | Host API (same codes). |
| 17 | **Reset**: `Port_HostSnapshot` / `Port_HostRestore` copy back **every writable section of the exe** plus RDRAM (Windows only). This works only because "the window and sound live in eltale_rt64.dll" | host.c | Indirectly | **Constraint:** any renderer, window, audio or ImGui state linked statically into the exe would be wiped by a reset (stale Vulkan handles, the audio ring). So the host and renderer must live in a DLL, or in sections the snapshot skips. Linux has no reset yet (it would need the ELF writable segments). |
| 18 | **Pacing**: the game's virtual clock is paced to wall time by `Port_HostPace` (46.875 MHz ticks; it rebases when more than 100 ms behind, e.g. after the menu). The game runs 30 fps at 2 VIs per frame. `ViHook` presents at 60 Hz. RT64 adds its own presentation pacing and its interpolated "frame rate" option. There's no vsync option | host.c, kernel.c, RT64 | The interpolation and present pacing are RT64's | New: a present queue with due times, vsync on/off (FIFO, else immediate/mailbox), and pictures between drawn at display rate from the game's data (P2). KF4's video.c is the model, including `KF4_PRESENT_SIM` (headless pacing test). |
| 19 | **MSAA, resolution scale, filtering, three-point** | RT64 options | Yes | Own implementation: render-target scale, MSAA with a resolve attachment, present filters (KF4's present_scale.frag / present_sharpen.frag), N64 three-point texture filtering as an "N64 look" option. |
| 20 | **Hidden-window mode**: `ELTALE_HIDDEN_WINDOW` makes the SDL window hidden. It's used with `ELTALE_MENU_TEST=N` (open the menu at frame N, reset into the Pak manager after 30, quit after 300) for the unattended menu and reset check | eltale_rt64.cpp, menu.cpp `test_tick` | RT64 needs a window even when hidden | Keep both. Also add a **true headless renderer** (no window or surface, offscreen device; KF4 does the same with Granite and `VK_EXT_headless_surface`) that can dump pictures: today a headless run draws nothing at all. |
| 21 | **DLL boundary**: a C API (`ElRt64_*`, 15 functions) loaded at run time, so headless builds need no graphics. It keeps the MSVC-ABI C++ (clang-cl) apart from the MinGW game. Linux `libeltale_rt64.so` is named but never built ("Not yet: Linux build of the DLL") | eltale_rt64.h, rt64_bridge.c | The clang-cl part is RT64's (D3D12, dxc) | The new host can be built with the **same MinGW clang** as the game (KF4 builds Granite, ImGui and SDL2 with llvm-mingw clang). Keep a DLL anyway, for reasons 17 and 21. |
| 22 | **Debug env vars** that touch drawing: `ELTALE_FRAME_IMAGE`, `ELTALE_DUMP_IMAGE`, `ELTALE_TRACE_COPIES`, `ELTALE_DUMP_CPU_FRAMES`, `ELTALE_DUMP_DL` | frame_image.c, kernel.c | No | Keep. The frame image becomes the reference renderers' input (section 4). |
| 23 | **Texture overrides / English pictures**: `Port_FrameHook = Port_ImagesUpdate` before each graphics task (en_images.c), working at the game's data level | boot_probe.c | No | Unaffected, as long as the native renderer reads textures from the same game memory. A hash-keyed texture-pack path later (KF4 `texture_pack.py`) fits P3.6. |

**Summary of what is tied to RT64 classes and needs a new home:**
- ImGui setup, frame and draw through `RT64::Inspector` and `RT64::SetRenderHooks`;
- the menu pause through `debuggerInspector.paused`;
- the Graphics page and the `"graphics"` settings block (`RT64::UserConfiguration`);
- resolution, MSAA, filtering and frame-rate interpolation;
- VI scan-out emulation;
- the D3D12/Vulkan device and swapchain.

Everything else (window, input, audio, data folder, language and font, quick keys, notices, poll codes, settings I/O) is plain SDL/C++ that happens to live in the RT64 DLL. It can be moved into shared host sources with no change in behaviour.

---

## 2. Reuse from King's Field IV (holyland) and KF1

### 2.1 What KF4's renderer is built on
- **Granite (Themaister, MIT)**, as the git submodule of paraLLEl-GS (LGPL), pinned at Granite 16e7395f (2026-08-31). It's used in "standalone" configuration:
  - `GRANITE_PLATFORM null`, `GRANITE_RENDERER OFF`, `GRANITE_SHIPPING ON`;
  - no SPIRV-Cross, no runtime shader compiler;
  - it needs only Granite's `vulkan/` and `util/` with the volk and Vulkan-Headers submodules.
- **Granite's WSI is not used.** `gsn.cpp` makes a `Vulkan::Context` itself (`init_instance` / `init_device` with surface extensions, 2 thread indices; headless with `VK_EXT_headless_surface`). `present_vk.cpp` makes its own swapchain with `SDL_Vulkan_CreateSurface` on that device, with raw Vulkan through function pointers from Granite's instance.
- **Three local Granite patches**, applied at configure time by CMake scripts in `cmake/`: `granite_depth_bounds`, `granite_dynamic_state` (extended dynamic state, fewer pipeline binds) and `granite_pipeline_warm` (hooks in `build_graphics_pipeline` / `build_compute_pipeline` that record every pipeline made).
- **Window, input, audio, overlay**:
  - SDL2: `port/platform/video.c` (window, pacing, dumps), with SDL_Renderer as a fallback presenter;
  - ImGui: `imgui_impl_sdl2` + `imgui_impl_vulkan` (`IMGUI_IMPL_VULKAN_NO_PROTOTYPES`) for the F1 overlay (`port/overlay/overlay.cpp`);
  - settings: cJSON in C (`port/overlay/settings.c`, versioned settings.json).
- **Compiler**: everything (Granite C++, ImGui, game C) builds with llvm-mingw clang (`windows-clang` preset), plus libc++.dll and libunwind.dll copied beside the exe. Linux uses gcc or clang. That's the same clang (22.1.8, x86_64-w64-windows-gnu) that Eltale's `build_windows.py` already uses.

### 2.2 The relevant KF4 pieces, and how each carries over to an N64 game

| KF4 piece | What it does | Carries over as | PS2-specific part |
|---|---|---|---|
| Granite standalone build + the three cmake patch scripts | Vulkan device, command buffers, render-pass cache, descriptor and staging management, threads | **Library**: same pin, same CMake flags; copy `granite_dynamic_state` and `granite_pipeline_warm` (depth bounds probably not needed) | none |
| `present_vk.cpp` (1,325 lines) | swapchain on the renderer's device, vsync (FIFO / immediate / mailbox), letterbox fit (4:3, 16:9, stretch), scale filters (`present_scale.frag`, `present_sharpen.frag`), ImGui pass, flash recorder ring (F8, last 2 s of pictures on the GPU) | **Near-copy**: replace the gsn scanout hooks with the new renderer's "picture ready" image | `gsn_scanout_take` / paraLLEl-GS pixel path, deinterlace |
| `video.c` present queue + pacing | pictures queued with due times (vblank + phase), newest due shown, pictures between at their time, vsync, `KF4_PRESENT_SIM` headless pacing simulation, PNG dumps without a library | **Pattern + code**: the queue and the PNG writer port almost as is; the vblank becomes Eltale's VI tick (60 Hz, game at 30) | field/interlace handling, 640x448 |
| Pictures between ("frame_interp", P8 recording/replay) | the frame's draws recorded once and replayed per picture with camera and models at time t; pairing by draw identity (NATIVE_DRAW.md 3.5) | **Pattern**: Eltale's P2 gets the exact version from the start: per-draw identity (kind, instance address, part), camera from the game's camera struct, matrix decompose and slerp, a visible fallback (KF4's "CUT N"), no hidden safety nets | VIF unpack hash pairing, GIF packet replay, near-cut shard pairing |
| The "backend thread" (`gsn_backend.inc`) | renderer thread writes a stream of commands; a second thread makes the Granite calls and submits | **Pattern**, only if needed: Eltale's frames are ~2,650 commands; start single-threaded (render thread off the game thread), add the stream when profiling says so | none |
| Modern standalone renderer (P8) | own targets: D32F **reversed Z** (clear 0, GEQUAL/GREATER, larger = nearer), MSAA as real samples, composite of 2D over the scene | **Pattern**: reversed-Z D32F from day one (the N64's 16-bit Z and the RDP's Z compare/decal modes become real depth; decals need a slope or polygon offset, as in KF4's "ties" lessons) | GS Z / 2^24 math, ties to the GS's rounding, page-ownership table, VRAM write-backs |
| Pipeline cache + warm-up (`gsn_pipeline_warm.inc`, `gsn_pipeline_list.h`, `tools/pipeline_list.py`) | every pipeline recorded (SPIR-V hash, render-pass formats and samples, state); a shipped list built from map tours plus a data-folder list; made ahead on threads behind the title ("Building shaders N/M"); the VkPipelineCache saved 2 s after compiles and at exit | **Library + pattern**: the Granite hook is generic; the list format and the tool carry over (map tour exists: `ELTALE_MAP_TOUR`) | which states exist (Eltale's combiner/othermode census replaces the GS's) |
| Shader build (`tools/build_shaders.py`) | glslangValidator compiles `shaders/*.{vert,frag,comp}` (+ named variants with defines) into SPIR-V words in a **committed header**; building needs no shader compiler | **Tool, copy as is** (change paths). glslang is already built at C:\code\kf4-tools\glslang-build\StandAlone\glslangValidator.exe | none |
| `--renderer gs|native|both`, `--compare-renderers` + heat map, `--stats` | two renderers on the same input, compared per picture ("compare what is shown") | **Pattern** (section 4) | — |
| `tools/frame_test.py` | headless, fixed clock, scripted pad: dump pictures at chosen vblanks; run twice, byte-compare; none single-colour; last-picture-alone equals dense run (reading a picture changes nothing) | **Tool, adapt**: Eltale already has `--headless --fast --frames --input`, needs `--dump-frames` | the GS-specific dense stretch |
| `tools/flash_find.py` | finds "one picture off" pictures in a run of pictures (neighbours agree, picture differs, 5x5 solid rule; `--frames` mode compares pictures between with their game frames) | **Tool, copy as is**: it works on PNG folders | none |
| `tools/pace_test.py`, `KF4_HITCH_MS`, `KF4_GSN_GPU_TIME`, `tools/sampler/` | pictures per second a window would show; hitch reports; GPU time per kind; sampling profiler (Windows) | **Tools/patterns**: pace_test and the GPU timestamp marks carry over; the sampler is generic | GS-thread split |
| `tools/map_tour.py` + per-map baseline sums | capture per map, compare renderers, keep the sum and the worst | **Pattern**: Eltale has `ELTALE_MAP_TOUR` (258 sub-areas) already | — |
| `tools/gsreplay` (`kf4-gsreplay`) | replays a GS capture through either renderer, draw by draw | **Pattern**: Eltale's equivalent is the frame image (`ELTALE_DUMP_IMAGE`) replayed by a small reference tool (section 4) | GS capture format |
| `port/render/render.h` API (`rd_mesh_from_block`, `rd_camera`, `rd_draw`, `rd_pass`, `rd_begin_frame`), the adapter in `game/native_draw/` + one-`if` patches | the game-level draw interface | **Pattern** for P2's API shape (mesh by key+gen, per-draw identity, data captured at queue time, each kind switchable live); the contents are N64-specific (F3DEX vertices, combiner, othermode) | VU1 batches, GIF tags, A+D packets |
| `overlay.cpp` + `settings.c` | F1 overlay pages, settings.json versioning and migration, controls bindings | **Pattern / partial code**: Eltale's menu.cpp is smaller and already fine; take the settings versioning rule and the Debug page idea (live per-kind switches) | KF4 pages |
| `texture_pack.py` | dumped textures as pictures and back, by hash | **Pattern** for P3.6 image text / texture packs | VRAM swizzle |
| `vu1_hle*.c` as oracle, `nd_*_oracle` shader variants | the CPU model checks the GPU shader bit for bit | **Pattern**: Eltale's oracle for vertex maths is the F3DEX 1.23 microcode itself (run by an RSP interpreter, section 4) | VU1 |

**KF4 lessons to carry over:**
- Compare what is shown, not memory.
- Never judge from nearest-downscaled sheets.
- With pictures between, frames are drawn in a different order than shown.
- NVIDIA's MSAA resolve isn't a function of the samples (use a resolve attachment).
- Agents can't judge looks: each phase needs reference captures, compare tools and owner gates.
- Data-driven headless repro for one-picture flashes.
- Never screen-grab the desktop.
- `KF4_DATA_DIR`-style temp data folders in every test (Eltale has `ELTALE_DATA_DIR`).

### 2.3 KF1 port (kf1-recomp/decomp-port)
- **Stack**: SDL3 3.4.16 (MinGW development package downloaded by CMake), OpenGL ES 3 through SDL with a small `glshim`, ImGui with the SDL3 + OpenGL3 backends, `-static` C++ runtime (only SDL3.dll beside the exe), Nix for Linux.
- **Renderer** (`src/renderer/renderer.cpp`, 1,300 lines): CPU-sorted triangle lists, PS1-style, GLES.
- **What to take**:
  - the **SDL3 MinGW download recipe** (CMake `file(DOWNLOAD …)` of the pinned devel package);
  - the static C++ runtime link.
- The GLES renderer itself is not "modern Vulkan" and offers nothing over KF4's for this plan.

---

## 3. Technology choice

| | (a) Granite/Vulkan as KF4 | (b) thin own Vulkan (volk + VMA) | (c) SDL3 GPU | (d) RT64 kept as reference |
|---|---|---|---|---|
| Shared code with KF4 | **Most**: presenter, pipeline warm hook + list tool, cmake patches, flash recorder, shader header tool, backend-thread pattern, headless device code | presenter is already raw Vulkan (portable); Granite-based pieces must be rewritten | little: shader tool only (SPIR-V; D3D12 needs DXIL via SDL_shadercross); presenter, warm-up, recorder rewritten | n/a (not a renderer choice) |
| Team experience | Granite proven over KF4 steps 3 and P8 (both owner and agents), incl. its quirks (row_length 0, render-pass cache ignores sample count) | raw Vulkan in present_vk.cpp only | KF1 uses SDL3 (not SDL_GPU); no SDL_GPU experience | known |
| Build | MinGW clang + CMake/Ninja, as KF4's windows-clang preset; no VS Build Tools | same, smaller | same; plus shadercross for DXIL if D3D12 wanted | clang-cl + VS Build Tools + dxc |
| Features needed (MSAA resolve control, reversed Z D32F, pipeline cache save/load + warm-up, dynamic state, timestamps, compute for replays, sample-rate shading) | all, with KF4's patches | all, by hand | MSAA yes; **no VkPipelineCache serialization or warm-up control**, no dynamic state control, limited compute/storage options, timestamp queries absent | — |
| ImGui | `imgui_impl_vulkan` (KF4 exact) | same | `imgui_impl_sdlgpu3` | via RT64 Inspector |
| Linux | Vulkan everywhere (KF4: Andrew tests RADV/lavapipe) | same | Vulkan | RT64 Linux build never done here |
| Windows without Vulkan | no (rare; KF4 ships Vulkan-only) | no | D3D12 fallback | D3D12 |
| paraLLEl-RDP (section 4) | **same Granite**: paraLLEl-RDP is Granite-based; one pin can serve both | separate Vulkan stacks | separate | — |

**Recommendation: (a) Granite/Vulkan, with (d) RT64 kept as the play fallback and visual reference during P2, and paraLLEl-RDP added as the exact reference.**

Why:
1. **The most reuse.** The presenter, pacing queue, pipeline warm-up (the cure for KF4's "frozen first visit"), flash recorder, shader header tool and headless device code are Granite-based or Vulkan-based. They're already debugged on the owner's NVIDIA card and on Linux by Andrew.
2. **The same team habits.** Agents and the owner know Granite's model and quirks. KF4's lessons (resolve attachments, barrier merging through one `cmd()`, pipeline list) apply word for word.
3. **One compiler.** Granite, ImGui and SDL build with the MinGW clang the game already uses (proven by KF4), so the new DLL needs no clang-cl or VS Build Tools. Only the legacy RT64 DLL keeps needing them, and that goes when RT64 does.
4. **paraLLEl-RDP is Granite's sibling** (same author, built on Granite's `vulkan/`). The exact reference and the new renderer can share one Vulkan stack if they ever run in one process.
5. **Why not the others:**
   - SDL3 GPU's missing pipeline-cache control alone would bring back the first-visit hitches KF4 had to fix, and it drops all KF4 tooling.
   - A thin own layer means rewriting what Granite gives (render-pass cache, descriptors, staging, threading) for no gain.

**SDL version.** Go with **SDL3** for the new host (window, input, audio, ImGui backend `imgui_impl_sdl3`):
- modern, better high-DPI and gamepad support, `SDL_AudioStream` frequency ratio;
- KF1 already has the MinGW download recipe;
- the KF4 SDL2 code that moves over (video.c parts, overlay) is mechanical to port.

The legacy RT64 DLL keeps its own SDL2 (they're never active together: the renderers are exclusive per run). If minimal change matters more than being modern, SDL2 2.30 (KF4's) is the fallback choice. Nothing in the plan depends on SDL3.

---

## 4. Reference and compare strategy

### What exists
- `tests/p1_checks.sh`:
  - display-list hashes per graphics task (`P1_GFX_TASK … hash`, `last_hash` in `P1_OS_OK`), normalised through the host-to-N64 address map;
  - the audio output hash (`P1_AUDIO`);
  - `P1_CPU_FRAME n state hash` for the Pak manager's CPU-drawn screens;
  - New Game 700 frames, ending, tour; with `--tour`: every map (`ELTALE_MAP_TOUR`), save and load, Pak manager, quick save and load.
- Unit tests: abi, endian, fiber, gu, os_events, os_scheduler, pak.
- Frame-image tools: `ELTALE_FRAME_IMAGE`, `ELTALE_DUMP_IMAGE=<frame>[+step]` (writes out/frame_N.bin), `tools/dl_dump.py` (walks it like the RSP), `tools/rdp2d.py` (a Python RDP for the 2D part only).
- **An RSP interpreter already exists**: `port/audio/rsp_lle.c` (668 lines, scalar + vector units, DMA, runs the ROM's own aspMain for the audio check).
- These guard the game side. **Nothing checks pixels today**: headless runs draw nothing.

### Options
1. **RT64 as the reference (`--renderer rt64|native|both`).**
   - It's HLE and not exact. It needs a window (hidden works), and the C interface has no read-back.
   - "both" would mean two devices and two swapchains, with RT64 owning its own SDL2.
   - Good as a **visual** reference (side by side, play fallback); poor as a pixel oracle.
2. **paraLLEl-RDP + an LLE RSP (PLAN.md P1's original choice).**
   - It's exact: it is the RDP's bit-exact rasteriser, combiner, blender, dither and Z.
   - It runs headless with no window, and its framebuffer lands in its RDRAM, so read-back is free.
   - It's MIT licensed (unlike paraLLEl-GS's LGPL) and Granite-based.
   - Its input is RDP commands, so it needs F3DEX 1.23 run on an RSP. That can be **our own interpreter, generalised from rsp_lle.c**, running the ROM's F3DEX text and data (UCODE 0x8004BA50, UCODE_DATA 0x80070260, already named in rt64_bridge.c) over the frame image, which is already an N64-format RDRAM image. The interpreter's `mtc0` to the DPC registers (and the XBUS/FIFO output) feeds paraLLEl-RDP's command processor.
   - The ucode bytes come from the user's ROM at run time, so nothing enters the repo.

### Recommendation
- **Keep RT64** as `--renderer rt64` (the default until the native renderer passes the owner's gate), for play and side-by-side looks. Retire it after (PLAN.md's end state has no RT64).
- **Add paraLLEl-RDP as the exact reference in an offline headless tool first**, `eltale-rdpref`. It takes `out/frame_N.bin` frame images and writes PNGs: the RDRAM framebuffer at 320x240, and optionally paraLLEl-RDP's VI scan-out.
  - This keeps the reference out of the game exe and the DLL.
  - It reuses `ELTALE_DUMP_IMAGE`.
  - Its own Granite copy can't clash with the renderer's.
- A live `--renderer both` (one process, both drawing every frame, heat map) can come later, once both share one Granite pin.
- **Step-1 bridge.** If the native renderer's first step consumes display lists (an own F3DEX HLE in Vulkan, PLAN P1), compare it against paraLLEl-RDP for every frame of the existing scripted runs at 1x. Then move draws to game level kind by kind (P2), each kind switchable live and compared against the same reference. This is KF4's hosted → standalone path.

### Per-frame headless compare (design)
1. `eltale-p1 --headless --fast --frames N --input tests/X.input --renderer native --scale 1 --msaa 1 --dump-frames A-B --dump-dir D`.
   - The native renderer draws offscreen at 320x240, with nearest filtering, "N64 look" switches on (three-point filtering, dither off or matching), and no pictures between.
   - It writes `D/frame_<gfxtask>.png`, plus the VI-shown buffer at each retrace.
2. The same run with `ELTALE_FRAME_IMAGE=1 ELTALE_DUMP_IMAGE=A+1` writes `out/frame_N.bin` for the same task numbers (deterministic: the DL hashes already match run to run and across Windows/Linux).
3. `eltale-rdpref out/frame_N.bin --png R/frame_N.png` runs F3DEX on the RSP interpreter, then paraLLEl-RDP, and reads FB0/FB1 (RGBA5551) back from RDRAM.
4. `tools/render_compare.py D R`:
   - quantise the native RGBA8 to 5 bits per channel;
   - a pixel differs when any channel is more than 1 LSB away (the RDP dithers and the native renderer doesn't);
   - count only solid 3x3 areas (flash_find's rule), to ignore single-pixel edge-rule differences;
   - report per frame: differing px, worst frame, and a heat map PNG for the worst.
   - Keep a **baseline sum per test** (KF4's "no map gets worse without a reason you can name"), not a 0-px demand: the N64's edge walker, 3-point filter and combiner rounding won't be bit-matched by a modern renderer at 1x.
5. Add to `p1_checks.sh` (or a `tests/render_checks.sh`):
   - frame_test: the same pictures twice; the last picture dumped alone equals the dense run; no single-colour pictures;
   - the compare on New Game 700 and on the map tour (one frame per sub-area, 258 frames).
   - The pixel hash of the native output joins `P1_OS_OK` as `pic_hash`, a GPU-dependent check run locally, not in CI.
6. Pictures between: `flash_find.py --frames` on `--frame-rate 240 --dump-frames` runs (KF4 method), and `pace_test` with a present simulation (`ELTALE_PRESENT_SIM=HZ`).
7. Rules: `ELTALE_DATA_DIR` set to a temp folder in every test; no visible windows (headless, or `ELTALE_HIDDEN_WINDOW`); `--no-audio` equivalent (audio isn't opened headless today, good).

**Risks:**
- F3DEX 1.23's RDP output path. F3DEX writes RDP commands to an RDRAM FIFO, or to DMEM over XBUS, depending on the variant. Check which one this ROM's ucode uses; both are simple to route.
- paraLLEl-RDP's RDRAM word order is emulator-style host words. The frame image is big-endian bytes, so swap once on load, as `ElRt64_DrawTask` does.
- The LLE RSP's speed (fine for tests).
- RT64's `forceBranch` hint suggests the game's DLs rely on BRANCH_Z behaviour; the LLE runs the real ucode, so it's exact by construction.

---

## 5. Repository and build layout

The proposed tree follows the existing `port/` style and the "no ROM content" rule:

```
port/
  gfx/                    (C, in the exe; MinGW clang via build_windows.py / Makefile)
    frame_image.c         kept: input to RT64 and to the reference tool
    vi.c                  kept: VI state -> presenter (black, scale, gamma, dither filter)
    host_bridge.c         replaces rt64_bridge.c: loads eltale_host.dll (or eltale_rt64.dll for
                          --renderer rt64) behind one C API; sets Port_GfxTaskHook/ViHook/LivePad/AiBufferHook
  host/                   (C++, eltale_host.dll; SDL3, ImGui) — window, fullscreen, input, audio ring,
    host.h                data folder, settings.json (versioned), poll codes, quick keys, notices, pacing
    window.cpp            / present queue, ELTALE_HIDDEN_WINDOW, ELTALE_MENU_TEST
    input.cpp audio.cpp settings.cpp data_dir.cpp
    menu/                 menu core + pages (game, audio, controls: moved from port/rt64/menu.cpp;
                          graphics: new, per renderer)
  render/                 (C++, inside eltale_host.dll) the renderer core
    render.h              C API the game side calls (P2: meshes, cameras, draws, passes, 2D; frame begin/end)
    device.cpp            Granite Context (instance/device, headless when no window)
    present_vk.cpp        from KF4: swapchain, fit, scale filters, ImGui pass, flash recorder (F8)
    pipeline_warm.cpp     from KF4 gsn_pipeline_warm.inc + render_pipeline_list.h (shipped list)
    shaders/*.vert|frag|comp
    render_shaders.h      committed, made by tools/build_shaders.py
  rt64/                   unchanged legacy DLL (eltale_rt64.dll, clang-cl) until retired; its menu
                          and audio switch to the shared port/host sources so behaviour stays one copy
  rsp/                    the RSP interpreter, generalised from port/audio/rsp_lle.c (memory backends:
                          native audio memory, frame image), shared by the audio check and the reference
game/native_draw/         P2 adapter: game structs -> render.h (PLAN's `game/` folder; one-`if` patches)
tools/
  build_host.py (or .cmd) CMake + Ninja, MinGW clang, out/host/eltale_host.dll (+ SDL3.dll)
  build_shaders.py        from KF4 (glslangValidator path / $GLSLANG; Granite's glslang if needed)
  pipeline_list.py        from KF4
  rdpref/                 eltale-rdpref.exe: frame image -> RSP (port/rsp) -> paraLLEl-RDP -> PNG
  render_compare.py frame_test.py flash_find.py pace_test.py
tests/render_checks.sh    the pixel checks (local, GPU)
cmake/                    granite_dynamic_state.cmake, granite_pipeline_warm.cmake (+ .inc) from KF4
third_party/
  granite                 submodule, pinned to KF4's 16e7395f (its volk + Vulkan-Headers submodules only)
  imgui                   pinned (KF4's copy, or upstream tag; docking branch not needed)
  parallel-rdp            submodule (MIT), later, for tools/rdpref; pin its own Granite or build
                          against ours if the API matches
  rt64                    kept while --renderer rt64 exists
  (SDL3)                  not vendored: CMake downloads the pinned MinGW devel package (KF1's recipe,
                          3.4.16 with a hash check) on Windows; system package on Linux
  (json)                  nlohmann single header (already used via RT64's tree) or cJSON as KF4
```

**DLL or static.** Use a **DLL**, `eltale_host.dll` / `libeltale_host.so`. Reasons:
1. The reset snapshot restores every writable section of the exe, so window, GPU, audio and ImGui state must be outside it.
2. Headless builds keep needing no graphics.
3. The C ABI boundary lets `--renderer rt64|native` pick a DLL at run time.
4. libc++ stays out of the game. Link it statically into the DLL (`-static`, as KF1 does), so only SDL3.dll ships beside it.

The game-level calls (P2) cross the DLL boundary as a C function table, filled once at load (`ElHost_GetRenderApi()`), so there's no per-call GetProcAddress.

**Build.**
- `build_windows.py` keeps building the exe and gains `--host` (or a separate `tools/build_host.py`) that runs CMake + Ninja with the same clang and copies the DLLs into `out/p1-windows/`.
- Linux: `make` builds the .so via CMake too, which closes the "Linux build of the DLL not done" gap that RT64 left open.
- Shaders are compiled into a committed header, so building needs no glslang; only changing a shader does (KF4 rule).

**No ROM content.**
- The shipped pipeline list holds hashes and state words only.
- Reference ucode and frame images are read from the user's ROM / out/ at run time.
- Test baselines are numbers (px sums, hashes), never pictures. Pictures from tests stay in out/ or temp.
