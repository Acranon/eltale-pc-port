# eltale-rdpref: the offline pixel reference (port/rsp running the ROM's F3DEX, then paraLLEl-RDP).
# It links its own paraLLEl-RDP/Granite subset (third_party/parallel-rdp) and nothing from port/render
# or port/host, so the reference and the native renderer never share a Vulkan stack.
#
# Build (MinGW clang, as the port): python tools/build_rdpref.py
cmake_minimum_required(VERSION 3.20)
project(eltale_rdpref C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
set(PRDP ${ROOT}/third_party/parallel-rdp)

file(GLOB PRDP_RDP ${PRDP}/parallel-rdp/*.cpp)
set(PRDP_SOURCES
  ${PRDP_RDP}
  ${PRDP}/vulkan/buffer.cpp ${PRDP}/vulkan/buffer_pool.cpp ${PRDP}/vulkan/command_buffer.cpp
  ${PRDP}/vulkan/command_pool.cpp ${PRDP}/vulkan/context.cpp ${PRDP}/vulkan/cookie.cpp
  ${PRDP}/vulkan/descriptor_set.cpp ${PRDP}/vulkan/device.cpp ${PRDP}/vulkan/event_manager.cpp
  ${PRDP}/vulkan/fence.cpp ${PRDP}/vulkan/fence_manager.cpp ${PRDP}/vulkan/image.cpp
  ${PRDP}/vulkan/indirect_layout.cpp ${PRDP}/vulkan/memory_allocator.cpp ${PRDP}/vulkan/pipeline_event.cpp
  ${PRDP}/vulkan/query_pool.cpp ${PRDP}/vulkan/render_pass.cpp ${PRDP}/vulkan/sampler.cpp
  ${PRDP}/vulkan/semaphore.cpp ${PRDP}/vulkan/semaphore_manager.cpp ${PRDP}/vulkan/shader.cpp
  ${PRDP}/vulkan/texture/texture_format.cpp
  ${PRDP}/util/arena_allocator.cpp ${PRDP}/util/logging.cpp ${PRDP}/util/thread_id.cpp
  ${PRDP}/util/aligned_alloc.cpp ${PRDP}/util/timer.cpp ${PRDP}/util/timeline_trace_file.cpp
  ${PRDP}/util/environment.cpp ${PRDP}/util/thread_name.cpp
  ${PRDP}/volk/volk.c)

add_library(parallel_rdp STATIC ${PRDP_SOURCES})
target_include_directories(parallel_rdp PUBLIC
  ${PRDP}/parallel-rdp ${PRDP}/volk ${PRDP}/vulkan ${PRDP}/vulkan-headers/include ${PRDP}/util)
target_compile_options(parallel_rdp PRIVATE -w)
if(WIN32)
  target_compile_definitions(parallel_rdp PUBLIC VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)
  target_link_libraries(parallel_rdp PUBLIC winmm)
else()
  target_link_libraries(parallel_rdp PUBLIC dl pthread)
endif()

add_executable(eltale-rdpref rdpref.cpp ${ROOT}/port/rsp/rsp.c)
target_include_directories(eltale-rdpref PRIVATE ${ROOT}/port/rsp)
target_link_libraries(eltale-rdpref PRIVATE parallel_rdp)
if(WIN32)
  # one exe, no libc++.dll / libunwind.dll beside it
  target_link_options(eltale-rdpref PRIVATE -static)
endif()
