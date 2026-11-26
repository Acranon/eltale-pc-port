# Granite (third_party/granite) on macOS (docs/MACOS.md): what its pinned version needs to build there. Applied
# at configure, on Apple only, as cmake/granite_dynamic_state.cmake applies its own edits (once; a checkout that has
# it is left alone). Run after eltale_granite_dynamic_state (eltale_granite_replace is its).
#  - util/timer.cpp: macOS has no clock_nanosleep; sleep_until_nsecs sleeps the time left on the same
#    monotonic clock (get_current_time_nsecs) with nanosleep instead, again after an early wake.
function(eltale_granite_macos granite)
    eltale_granite_replace(${granite}/util/timer.cpp
        "\tint ret;\n\twhile ((ret = clock_nanosleep(timebase, TIMER_ABSTIME, &ts, nullptr)) == EINTR) {}\n"
        "#ifdef __APPLE__ // (eltale: macOS has no clock_nanosleep)\n\t(void)timebase;\n\t(void)ts;\n\tfor (int64_t left; (left = timepoint - get_current_time_nsecs()) > 0;)\n\t{\n\t\tstruct timespec rel = { time_t(left / 1000000000ll), long(left % 1000000000ll) };\n\t\tnanosleep(&rel, nullptr);\n\t}\n#else\n\tint ret;\n\twhile ((ret = clock_nanosleep(timebase, TIMER_ABSTIME, &ts, nullptr)) == EINTR) {}\n#endif\n")
endfunction()
