#pragma once

#include <string_view>
#include <vector>

namespace ZeusBuiltins {

// A source type has one backend representation policy. Dynamic-only types stay
// in ZeusValue; scalar and native data pointers cross native calls without boxing.
enum class NativeValueABI {
    ZeusValue,
    Int64,
    Bool1,
    NativeString,
    NativeBytes,
    RawBytesRange
};

struct NativeTypeABI {
    std::string_view canonicalName;
    std::vector<std::string_view> aliases;
    std::string_view llvmType;
    NativeValueABI valueABI;
};

inline bool isNativePointerABI(const NativeTypeABI* abi) noexcept {
    return abi != nullptr &&
        (abi->valueABI == NativeValueABI::NativeString ||
         abi->valueABI == NativeValueABI::NativeBytes);
}

inline std::string_view nativePointerFromDynamicHelper(const NativeTypeABI* abi) noexcept {
    return abi != nullptr && abi->valueABI == NativeValueABI::NativeBytes
        ? "zeus_value_copy_bytes"
        : "zeus_value_intern_string";
}

inline std::string_view nativePointerToDynamicHelper(const NativeTypeABI* abi) noexcept {
    return abi != nullptr && abi->valueABI == NativeValueABI::NativeBytes
        ? "zeus_value_from_native_bytes"
        : "zeus_value_from_native_string";
}

inline std::string_view nativePointerInternHelper(const NativeTypeABI* abi) noexcept {
    return abi != nullptr && abi->valueABI == NativeValueABI::NativeBytes
        ? "zeus_bytes_clone"
        : "zeus_string_intern";
}

inline std::string_view nativePointerFromDataHelper(const NativeTypeABI* abi) noexcept {
    return abi != nullptr && abi->valueABI == NativeValueABI::NativeBytes
        ? "zeus_bytes_from_data"
        : "zeus_string_from_data";
}

inline const std::vector<NativeTypeABI>& nativeTypeABIs() {
    static const std::vector<NativeTypeABI> types = {
        {"int", {"int32", "int64"}, "i64", NativeValueABI::Int64},
        {"bool", {}, "i1", NativeValueABI::Bool1},
        {"string", {"str", "String"}, "ptr", NativeValueABI::NativeString},
        {"bigint", {}, "ptr", NativeValueABI::ZeusValue},
        {"float", {"float32", "float64"}, "ptr", NativeValueABI::ZeusValue},
        {"nil", {}, "ptr", NativeValueABI::ZeusValue},
        {"Array", {"list", "array"}, "ptr", NativeValueABI::ZeusValue},
        {"dict", {"map"}, "ptr", NativeValueABI::ZeusValue},
        {"bytes", {}, "ptr", NativeValueABI::NativeBytes},
        {"&bytes", {}, "%ZeusRawBytesRange", NativeValueABI::RawBytesRange},
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
