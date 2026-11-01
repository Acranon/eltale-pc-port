# Dear ImGui

[Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, release `v1.92.9b`. MIT licence: see
`LICENSE.txt`. It is the same copy King's Field IV keeps (third_party/imgui there), with the SDL3
backend from the same release (King's Field I's copy), line endings LF.

It draws the F1 menu (port/host/menu.cpp) in eltale_host.dll.

| file | what it is |
| --- | --- |
| `imgui*.cpp`, `imgui*.h`, `imconfig.h`, `imstb_*.h` | the library |
| `backends/imgui_impl_sdl3.*` | the platform backend: SDL3's events, mouse, keyboard and gamepads |
| `backends/imgui_impl_vulkan.*` | the native renderer's backend (port/render/present_vk.cpp), built with `IMGUI_IMPL_VULKAN_NO_PROTOTYPES` |

Not copied: the demo, the examples, the misc folder and the other backends. Keep these files as they
are upstream, so they can be updated by copying a newer release over them.
