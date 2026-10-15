#pragma once

#include <array>
#include <string_view>

namespace ZeusBuiltins {

// These native callbacks are independent of a live VM instance and can be
// invoked through the standalone ZeusValue ABI used by LLVM/AOT code.
inline bool hasStandaloneValueCallABI(std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 53> names = {
        "__builtin_str",
        "type",
        "len",
        "int",
        "float",
        "__bytes_slice",
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
        "__thread_sleep",
        "__thread_current_id",
        "__socket_create",
        "__socket_connect",
        "__socket_bind",
        "__socket_listen",
        "__socket_accept",
        "__socket_send",
        "__socket_send_all",
        "__socket_recv",
        "__socket_recv_exact",
        "__socket_close",
        "__socket_is_connected",
        "bytes",
        "__bytes_create",
        "__bytes_get",
        "__bytes_set",
        "__bytes_len",
        "__bytes_concat",
        "__bytes_decode",
        "__bytes_to_list"
    };


    for (const auto candidate : names) {
        if (candidate == name) return true;
    }
    return false;
}

} // namespace ZeusBuiltins
