#include "ZeusStringNativeABI.hpp"

#include "ZeusDynamicABI.hpp"

#include <charconv>
#include <initializer_list>

namespace {

void setDynamicStringLiteral(ZeusValue* out, const char* text, int64_t length) {
    const ZeusNativeString literal = zeus_string_literal_from_data(text, length);
    zeus_value_from_native_string(out, literal);
}

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
    if (out && value) {
        switch (value->tag) {
        case ZEUS_VALUE_TAG_NIL:
            setDynamicStringLiteral(out, "nil", 3);
            return;
        case ZEUS_VALUE_TAG_BOOL:
            if (value->bool_payload) {
                setDynamicStringLiteral(out, "true", 4);
            } else {
                setDynamicStringLiteral(out, "false", 5);
            }
            return;
        case ZEUS_VALUE_TAG_INT64: {
            char buffer[32];
            const auto result = std::to_chars(
                buffer, buffer + sizeof(buffer), value->int64_payload);
            const ZeusNativeString text = zeus_string_from_data(
                buffer, static_cast<int64_t>(result.ptr - buffer));
            zeus_value_from_native_string(out, text);
            zeus_string_release(text);
            return;
        }
        case ZEUS_VALUE_TAG_STRING:
            zeus_value_clone(out, value);
            return;
        default:
            break;
        }
    }
    invokeStringBuiltin(out, "__builtin_str", {value});
}

void zeus_string_builtin_type(ZeusValue* out, const ZeusValue* value) {
    if (out && value) {
        switch (value->tag) {
        case ZEUS_VALUE_TAG_NIL:
            setDynamicStringLiteral(out, "nil", 3);
            return;
        case ZEUS_VALUE_TAG_BOOL:
        case ZEUS_VALUE_TAG_REF_BOOL:
            setDynamicStringLiteral(out, "bool", 4);
            return;
        case ZEUS_VALUE_TAG_INT64:
        case ZEUS_VALUE_TAG_BIGINT:
        case ZEUS_VALUE_TAG_REF_INT64:
            setDynamicStringLiteral(out, "int", 3);
            return;
        case ZEUS_VALUE_TAG_BIGDECIMAL:
            setDynamicStringLiteral(out, "float", 5);
            return;
        case ZEUS_VALUE_TAG_STRING:
            setDynamicStringLiteral(out, "string", 6);
            return;
        case ZEUS_VALUE_TAG_BYTES:
            setDynamicStringLiteral(out, "bytes", 5);
            return;
        case ZEUS_VALUE_TAG_BOXED:
            if (zeus_value_is_array(value) == 1) {
                setDynamicStringLiteral(out, "Array", 5);
                return;
            }
            if (zeus_value_is_dict(value) == 1) {
                setDynamicStringLiteral(out, "dict", 4);
                return;
            }
            break;
        default:
            break;
        }
    }
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

void zeus_string_compare(ZeusValue* out, const ZeusValue* left, const ZeusValue* right) {
    invokeStringBuiltin(out, "__str_compare", {left, right});
}

} // extern "C"
