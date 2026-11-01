// Pipelines made ahead (R7, docs/renderer/NATIVE_DRAW.md "Modern options"): the pipeline cache in the data
// folder, and every pipeline the native renderer makes written down, so the next start makes them on threads of
// its own before a draw needs them (nd_pipeline_warm.cpp, from King's Field IV's gsn_pipeline_warm.inc).
#pragma once

#include <cstdint>
#include <string>

namespace Vulkan {
class Device;
}

namespace render {

// At the device's start, before any pipeline is made: the pipeline cache (data_dir/native_pipelines.bin), the
// lists (the shipped nd_pipeline_list.h and data_dir/native_pipeline_list.txt), Granite's hooks. `warm`: the
// Graphics page's "Build shaders at start" (off: the lists are still kept, nothing is made ahead).
void pipeline_warm_open(Vulkan::Device &device, const std::string &data_dir, bool warm);
// The pipelines of the lists for this MSAA sample count, made on threads of their own (once per count; a
// settings change starts another count's). Nothing when warm-up is off.
void pipeline_warm_start(uint32_t samples);
// Each picture: the cache and the data folder's list written a while after pipelines were made
void pipeline_warm_tick();
// The Graphics page's lines (ImGui)
void pipeline_warm_page();
// At the end: the cache and the list written, the threads stopped; prints P1_PIPELINES
void pipeline_warm_close();

} // namespace render
