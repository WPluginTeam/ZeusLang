#pragma once

#include <string_view>
#include <vector>

namespace ZeusBuiltins {

// A source type has one backend representation policy. Dynamic-only types stay
// in ZeusValue; scalar and string-view types cross native calls without boxing.
enum class NativeValueABI {
    ZeusValue,
    Int64,
    Bool1,
    StringView
};

struct NativeTypeABI {
    std::string_view canonicalName;
    std::vector<std::string_view> aliases;
    std::string_view llvmType;
    NativeValueABI valueABI;
};

inline const std::vector<NativeTypeABI>& nativeTypeABIs() {
    static const std::vector<NativeTypeABI> types = {
        {"int", {"int32", "int64"}, "i64", NativeValueABI::Int64},
        {"bool", {}, "i1", NativeValueABI::Bool1},
        {"string", {"str"}, "ptr", NativeValueABI::StringView},
        {"bigint", {}, "ptr", NativeValueABI::ZeusValue},
        {"float", {"float32", "float64"}, "ptr", NativeValueABI::ZeusValue},
        {"nil", {}, "ptr", NativeValueABI::ZeusValue},
        {"list", {"array"}, "ptr", NativeValueABI::ZeusValue},
        {"dict", {"map"}, "ptr", NativeValueABI::ZeusValue},
        {"bytes", {}, "ptr", NativeValueABI::ZeusValue},
        {"char", {}, "ptr", NativeValueABI::ZeusValue},
        {"object", {}, "ptr", NativeValueABI::ZeusValue},
        {"func", {"function"}, "ptr", NativeValueABI::ZeusValue}
    };
    return types;
}

inline const NativeTypeABI* findNativeTypeABI(std::string_view name) {
    for (const auto& type : nativeTypeABIs()) {
        if (type.canonicalName == name) return &type;
        for (const auto alias : type.aliases) {
            if (alias == name) return &type;
        }
    }
    return nullptr;
}

} // namespace ZeusBuiltins
