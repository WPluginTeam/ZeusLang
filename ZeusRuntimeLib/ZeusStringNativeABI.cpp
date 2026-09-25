#include "ZeusStringNativeABI.hpp"

#include <initializer_list>

namespace {

void invokeStringBuiltin(
    ZeusValue* out,
    const char* builtinName,
    std::initializer_list<const ZeusValue*> arguments
) {
    zeus_value_native_call(
        out,
        builtinName,
        static_cast<int32_t>(arguments.size()),
        arguments.begin()
    );
}

} // namespace

extern "C" {

void zeus_string_builtin_str(ZeusValue* out, const ZeusValue* value) {
    invokeStringBuiltin(out, "__builtin_str", {value});
}

void zeus_string_builtin_type(ZeusValue* out, const ZeusValue* value) {
    invokeStringBuiltin(out, "type", {value});
}

void zeus_string_builtin_len(ZeusValue* out, const ZeusValue* value) {
    invokeStringBuiltin(out, "len", {value});
}

void zeus_string_get(ZeusValue* out, const ZeusValue* value, const ZeusValue* index) {
    invokeStringBuiltin(out, "__str_get", {value, index});
}

void zeus_string_slice(ZeusValue* out, const ZeusValue* value, const ZeusValue* start, const ZeusValue* end) {
    invokeStringBuiltin(out, "__str_slice", {value, start, end});
}

void zeus_string_index(ZeusValue* out, const ZeusValue* value, const ZeusValue* substring) {
    invokeStringBuiltin(out, "__str_index", {value, substring});
}

void zeus_string_rindex(ZeusValue* out, const ZeusValue* value, const ZeusValue* substring) {
    invokeStringBuiltin(out, "__str_rindex", {value, substring});
}

void zeus_string_replace(
    ZeusValue* out,
    const ZeusValue* value,
    const ZeusValue* oldValue,
    const ZeusValue* replacement
) {
    invokeStringBuiltin(out, "__str_replace", {value, oldValue, replacement});
}

void zeus_string_split(ZeusValue* out, const ZeusValue* value, const ZeusValue* separator) {
    invokeStringBuiltin(out, "__str_split", {value, separator});
}

void zeus_string_startswith(ZeusValue* out, const ZeusValue* value, const ZeusValue* prefix) {
    invokeStringBuiltin(out, "__str_startswith", {value, prefix});
}

void zeus_string_endswith(ZeusValue* out, const ZeusValue* value, const ZeusValue* suffix) {
    invokeStringBuiltin(out, "__str_endswith", {value, suffix});
}

void zeus_string_lower(ZeusValue* out, const ZeusValue* value) {
    invokeStringBuiltin(out, "__str_lower", {value});
}

void zeus_string_upper(ZeusValue* out, const ZeusValue* value) {
    invokeStringBuiltin(out, "__str_upper", {value});
}

void zeus_string_count(ZeusValue* out, const ZeusValue* value, const ZeusValue* substring) {
    invokeStringBuiltin(out, "__str_count", {value, substring});
}

} // extern "C"
