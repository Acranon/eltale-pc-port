# From King's Field IV (holyland, MIT): its Granite patch, unchanged but for the names and paths.
# Eltale applies it to third_party/granite at configure (port/host/CMakeLists.txt).
# Granite (third_party/granite) with the hooks the native GS's pipeline list needs (gsn_pipeline_warm.inc,
# docs/port/RENDERER.md "Pipelines made ahead"): a pipeline made by Granite is told to the native GS (what it was made
# from, so that it can be made again at the next start before a draw needs it), a compatible render pass made for
# the first time is told too (its attachments' formats and samples, which a pipeline is made against), and the
# pipeline's hash can be worked out again from outside a command buffer. The code is cmake/granite_pipeline_warm.inc,
# included into command_buffer.cpp; the edits are made as cmake/granite_dynamic_state.cmake makes its own (at
# configure, once, beside an anchor line of Granite's). Run after eltale_granite_dynamic_state.
function(eltale_granite_pipeline_warm granite)
    set(hpp ${granite}/vulkan/command_buffer.hpp)
    set(cpp ${granite}/vulkan/command_buffer.cpp)
    set(dev ${granite}/vulkan/device.cpp)
    eltale_granite_edit(${hpp} "\tstatic Pipeline build_compute_pipeline(Device *device, const DeferredPipelineCompile &compile, CompileMode mode);"
        "\tstatic void holyland_rehash(DeferredPipelineCompile &compile, bool compute); static VkPipelineCache holyland_pipeline_cache(Device *device); static const RenderPass &holyland_render_pass(Device *device, const RenderPassInfo &info); // (holyland)" FALSE)
    eltale_granite_edit(${cpp} "static void log_compile_time(const char *tag, Hash hash,"
        "#include \"../../../cmake/granite_pipeline_warm.inc\" // (holyland)" TRUE)
    eltale_granite_edit(${cpp} "\tlog_compile_time(\"compute\", compile.hash, end_ts - start_ts, vr, mode);"
        "\tif (holyland_on_compile) holyland_on_compile(device, compile, true, end_ts - start_ts, mode == CompileMode::Sync, vr == VK_SUCCESS && compute_pipeline != VK_NULL_HANDLE); // (holyland)" FALSE)
    eltale_granite_edit(${cpp} "\tlog_compile_time(\"graphics\", compile.hash, end_ts - start_ts, res, mode);"
        "\tif (holyland_on_compile) holyland_on_compile(device, compile, false, end_ts - start_ts, mode == CompileMode::Sync, res == VK_SUCCESS && pipeline != VK_NULL_HANDLE); // (holyland)" FALSE)
    eltale_granite_edit(${dev} "const RenderPass &Device::request_render_pass(const RenderPassInfo &info, bool compatible)"
        "extern void (*holyland_on_render_pass)(Util::Hash hash, const RenderPassInfo &info); // (holyland: cmake/granite_pipeline_warm.inc)" TRUE)
    # (Eltale, R7) a render pass's hash names its attachments' sample counts too: Granite leaves them out, so
    # passes at MSAA 2x, 4x and 8x were one pass (the first made) and a change of MSAA in play drew into the
    # wrong one; the pipeline list keeps a pass per count
    eltale_granite_replace(${dev} "\t\th.u32(info.color_attachments[i]->get_image().get_swapchain_layout());\n\t}\n"
        "\t\th.u32(info.color_attachments[i]->get_image().get_swapchain_layout());\n\t\th.u32(info.color_attachments[i]->get_image().get_create_info().samples); // (eltale: MSAA)\n\t}\n\tif (info.depth_stencil)\n\t\th.u32(info.depth_stencil->get_image().get_create_info().samples); // (eltale: MSAA)\n")
    eltale_granite_replace(${dev} "\t\tret = render_passes.emplace_yield(hash, hash, this, info);\n"
        "\t{\n\t\tret = render_passes.emplace_yield(hash, hash, this, info);\n\t\tif (compatible && holyland_on_render_pass) holyland_on_render_pass(hash, info); // (holyland)\n\t}\n")
endfunction()
