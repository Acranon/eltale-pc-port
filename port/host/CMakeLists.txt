# eltale_host.dll (port/host/eltale_host.h): the window, input, sound, settings and F1 menu (port/host)
# and the native renderer (port/render), built with the same MinGW clang as the game. Everything is
# linked into the one DLL (libc++ too, -static) but SDL3, which is copied beside it.
#   python tools/build_host.py        (or: cmake -S port/host -B out/host -G Ninja
#                                      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++)
# Third-party code (docs/renderer/HOST_LAYER.md):
#   third_party/granite   submodule, pinned to King's Field IV's Granite (16e7395f), with KF4's three
#                         patches applied in its checkout at configure (cmake/granite_*.cmake)
#   third_party/imgui     Dear ImGui 1.92.9b, copied (core, SDL3 and Vulkan backends)
#   third_party/cjson     cJSON 1.7.18, copied
#   SDL3                  the 3.4.16 MinGW development package, downloaded and checked (SHA-256), or
#                         ELTALE_SDL3_DIR naming an unpacked one (its x86_64-w64-mingw32 folder)
#                         (Linux: a system SDL3, else the 3.4.16 source built here; tools/build_linux.sh)
cmake_minimum_required(VERSION 3.20)
project(eltale_host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

# ---- SDL3 ---------------------------------------------------------------------------------------
if(WIN32)
    set(ELTALE_SDL3_VERSION 3.4.16)
    set(ELTALE_SDL3_SHA256 c7ef65bd72eabac6e5b535411dbd8d5824d0aab24fd62ff8812666b336f18a9c)
    set(ELTALE_SDL3_DIR "" CACHE PATH "An unpacked SDL3 MinGW package's x86_64-w64-mingw32 folder (else downloaded)")
    if(NOT ELTALE_SDL3_DIR)
        set(archive "${CMAKE_BINARY_DIR}/SDL3-devel-${ELTALE_SDL3_VERSION}-mingw.tar.gz")
        set(ELTALE_SDL3_DIR "${CMAKE_BINARY_DIR}/SDL3-${ELTALE_SDL3_VERSION}/x86_64-w64-mingw32")
        if(NOT EXISTS "${ELTALE_SDL3_DIR}/include/SDL3/SDL.h")
            message(STATUS "Downloading SDL3 ${ELTALE_SDL3_VERSION} (MinGW)")
            file(DOWNLOAD "https://github.com/libsdl-org/SDL/releases/download/release-${ELTALE_SDL3_VERSION}/SDL3-devel-${ELTALE_SDL3_VERSION}-mingw.tar.gz"
                 "${archive}" EXPECTED_HASH SHA256=${ELTALE_SDL3_SHA256} TLS_VERIFY ON STATUS status)
            list(GET status 0 code)
            if(NOT code EQUAL 0)
                message(FATAL_ERROR "Cannot download SDL3 (${status}): set ELTALE_SDL3_DIR to an unpacked SDL3 MinGW package")
            endif()
            file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_BINARY_DIR}")
        endif()
    endif()
    add_library(SDL3 SHARED IMPORTED)
    set_target_properties(SDL3 PROPERTIES
        IMPORTED_LOCATION "${ELTALE_SDL3_DIR}/bin/SDL3.dll"
        IMPORTED_IMPLIB "${ELTALE_SDL3_DIR}/lib/libSDL3.dll.a"
        INTERFACE_INCLUDE_DIRECTORIES "${ELTALE_SDL3_DIR}/include")
else()
    # Linux: a system SDL3 (find_package), ELTALE_SDL3_DIR naming an SDL3 install prefix, or else the
    # 3.4.16 source release, downloaded and checked (SHA-256) and built here as libSDL3.so.0, which
    # goes beside libeltale_host.so (found there through its $ORIGIN rpath)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)  # its static libraries go into the .so
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    set(ELTALE_SDL3_VERSION 3.4.16)
    set(ELTALE_SDL3_SRC_SHA256 7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
    set(ELTALE_SDL3_DIR "" CACHE PATH "An SDL3 install prefix (else a system SDL3, else built from source)")
    if(ELTALE_SDL3_DIR)
        find_package(SDL3 REQUIRED CONFIG PATHS "${ELTALE_SDL3_DIR}" NO_DEFAULT_PATH)
    else()
        find_package(SDL3 QUIET CONFIG)
    endif()
    if(SDL3_FOUND)
        add_library(SDL3 ALIAS SDL3::SDL3)
    else()
        set(archive "${CMAKE_BINARY_DIR}/SDL3-${ELTALE_SDL3_VERSION}.tar.gz")
        set(sdl_src "${CMAKE_BINARY_DIR}/SDL3-${ELTALE_SDL3_VERSION}")
        if(NOT EXISTS "${sdl_src}/CMakeLists.txt")
            message(STATUS "No system SDL3: downloading the SDL3 ${ELTALE_SDL3_VERSION} source")
            file(DOWNLOAD "https://github.com/libsdl-org/SDL/releases/download/release-${ELTALE_SDL3_VERSION}/SDL3-${ELTALE_SDL3_VERSION}.tar.gz"
                 "${archive}" EXPECTED_HASH SHA256=${ELTALE_SDL3_SRC_SHA256} TLS_VERIFY ON STATUS status)
            list(GET status 0 code)
            if(NOT code EQUAL 0)
                message(FATAL_ERROR "Cannot download SDL3 (${status}): install SDL3 or set ELTALE_SDL3_DIR to an SDL3 install prefix")
            endif()
            file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${CMAKE_BINARY_DIR}")
        endif()
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        set(SDL_TESTS OFF CACHE BOOL "" FORCE)
        set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
        add_subdirectory("${sdl_src}" SDL3 EXCLUDE_FROM_ALL)
        add_library(SDL3 ALIAS SDL3-shared)
    endif()
endif()

# ---- Granite (as King's Field IV builds it: standalone, no renderer, no shader compiler) ----------
set(GRANITE_DIR "${ROOT}/third_party/granite")
if(NOT EXISTS "${GRANITE_DIR}/third_party/volk/volk.h")
    message(FATAL_ERROR "third_party/granite is not checked out with its volk and Vulkan-Headers submodules "
                        "(git submodule update --init third_party/granite, then in it: "
                        "git submodule update --init third_party/volk third_party/khronos/vulkan-headers)")
endif()
include("${ROOT}/cmake/granite_depth_bounds.cmake")
eltale_granite_depth_bounds("${GRANITE_DIR}")
include("${ROOT}/cmake/granite_dynamic_state.cmake")
eltale_granite_dynamic_state("${GRANITE_DIR}")
include("${ROOT}/cmake/granite_pipeline_warm.cmake")
eltale_granite_pipeline_warm("${GRANITE_DIR}")
set(GRANITE_FAST_MATH OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_RENDERER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SYSTEM_HANDLES OFF CACHE BOOL "" FORCE)
set(GRANITE_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)
set(GRANITE_PLATFORM "null" CACHE STRING "" FORCE)
set(GRANITE_TOOLS OFF CACHE BOOL "" FORCE)
set(GRANITE_INSTALL_TARGETS OFF CACHE BOOL "" FORCE)
add_subdirectory("${GRANITE_DIR}" granite EXCLUDE_FROM_ALL)

# ---- Dear ImGui and cJSON -------------------------------------------------------------------------
set(IMGUI_DIR "${ROOT}/third_party/imgui")
add_library(eltale_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(eltale_imgui PUBLIC ${IMGUI_DIR})
target_compile_definitions(eltale_imgui PUBLIC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
target_link_libraries(eltale_imgui PUBLIC SDL3 Vulkan::Headers)

add_library(eltale_cjson STATIC "${ROOT}/third_party/cjson/cJSON.c")
target_include_directories(eltale_cjson PUBLIC "${ROOT}/third_party/cjson")

# ---- eltale_host.dll ------------------------------------------------------------------------------
add_library(eltale_host SHARED
    host.cpp settings.cpp input.cpp audio.cpp menu.cpp menu_graphics.cpp
    ../render/render_native.cpp ../render/nd_renderer.cpp ../render/nd_interp.cpp ../render/present_vk.cpp ../render/png.c
    ../render/nd_pipeline_warm.cpp)
# eltale_host.dll on Windows, libeltale_host.so on Linux (the names port/gfx/host_bridge.c loads)
set_target_properties(eltale_host PROPERTIES OUTPUT_NAME eltale_host)
if(WIN32)
    set_target_properties(eltale_host PROPERTIES PREFIX "")
endif()
target_include_directories(eltale_host PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${ROOT}/port/render" "${ROOT}/port/include" "${ROOT}")
target_compile_definitions(eltale_host PRIVATE ELTALE_HOST_BUILD)
target_link_libraries(eltale_host PRIVATE granite-vulkan eltale_imgui eltale_cjson SDL3)
if(WIN32)
    # libc++, libunwind and winpthreads inside the DLL: only SDL3.dll goes beside it
    target_link_options(eltale_host PRIVATE -static -Wl,--no-undefined)
    add_custom_command(TARGET eltale_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ELTALE_SDL3_DIR}/bin/SDL3.dll" $<TARGET_FILE_DIR:eltale_host>)
endif()
if(NOT WIN32)
    # libSDL3.so.0 (when built here) beside libeltale_host.so; the exe dlopens this from its own folder
    set_target_properties(eltale_host PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
    target_link_options(eltale_host PRIVATE -Wl,--no-undefined)
endif()
