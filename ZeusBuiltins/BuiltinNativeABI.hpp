#pragma once

#include <array>
#include <string_view>

namespace ZeusBuiltins {

// These native callbacks are independent of a live VM instance and can be
// invoked through the standalone ZeusValue ABI used by LLVM/AOT code.
inline bool hasStandaloneValueCallABI(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 47> names = {
        "__builtin_str",
        "type",
        "len",
        "int",
        "float",
        "__array_push",
        "__array_pop",
        "__array_get",
        "__array_set",
        "__array_clear",
        "__array_slice",
        "__bytes_slice",
        "__dict_delete",
        "__dict_clone",
        "__str_get",
        "__str_slice",
        "__str_index",
        "__str_rindex",
        "__str_replace",
        "__str_split",
        "__str_startswith",
        "__str_endswith",
        "__str_lower",
        "__str_upper",
        "__str_count",
        "__gc_native_nodes",
        "__gc_collect",
        "__time_time",
        "__time_time_ms",
        "__time_time_ns",
        "__time_clock",
        "__time_perf_counter",
        "__time_perf_counter_ns",
        "__time_localtime",
        "__time_gmtime",
        "__time_mktime",
        "__time_strftime",
        "__time_monotonic",
        "__time_monotonic_ns",
        "__time_sleep",
        "__mutex_create",
        "__mutex_lock",
        "__mutex_trylock",
        "__mutex_unlock",
        "__mutex_destroy",
        "__thread_sleep",
        "__thread_current_id"
    };


    for (const auto candidate : names) {
        if (candidate == name) return true;
    }
    return false;
}

} // namespace ZeusBuiltins
