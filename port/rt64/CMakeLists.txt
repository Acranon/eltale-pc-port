# eltale_rt64.dll: RT64 (third_party/rt64) behind a C interface (eltale_rt64.h), loaded by the host
# (eltale_host.dll, port/host) for --renderer rt64. The window, input, sound and menu are the host's.
# RT64 links SDL2 (its own code asks for it), so SDL2.dll is still copied beside it.
# Windows: configure with clang-cl (the MSVC ABI RT64 is built with), e.g.
#   cmake -S port/rt64 -B out/rt64 -G Ninja -DCMAKE_BUILD_TYPE=Release
#         -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build out/rt64
cmake_minimum_required(VERSION 3.20)
project(eltale_rt64 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(RT64_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/rt64")
set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)
add_subdirectory(${RT64_DIR} ${CMAKE_BINARY_DIR}/rt64)

include(FetchContent)
if (WIN32)
    FetchContent_Declare(sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-devel-2.30.3-VC.zip)
    FetchContent_MakeAvailable(sdl2)
    set(SDL2_INCLUDE ${sdl2_SOURCE_DIR}/include)
    set(SDL2_LIBDIR ${sdl2_SOURCE_DIR}/lib/x64)
else()
    find_package(SDL2 REQUIRED)
    set(SDL2_INCLUDE ${SDL2_INCLUDE_DIRS})
endif()

add_library(eltale_rt64 SHARED eltale_rt64.cpp)
target_include_directories(eltale_rt64 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${RT64_DIR}/src ${RT64_DIR}/src/contrib ${RT64_DIR}/src/contrib/hlslpp/include
    ${RT64_DIR}/src/contrib/dxc/inc ${CMAKE_BINARY_DIR}/rt64/src
    ${SDL2_INCLUDE})
if (WIN32)
    target_link_directories(eltale_rt64 PRIVATE ${SDL2_LIBDIR})
    target_link_libraries(eltale_rt64 PRIVATE rt64 SDL2)
    add_custom_command(TARGET eltale_rt64 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SDL2_LIBDIR}/SDL2.dll
            ${RT64_DIR}/src/contrib/dxc/bin/x64/dxil.dll
            ${RT64_DIR}/src/contrib/dxc/bin/x64/dxcompiler.dll
            $<TARGET_FILE_DIR:eltale_rt64>)
else()
    target_link_libraries(eltale_rt64 PRIVATE rt64 ${SDL2_LIBRARIES})
endif()
