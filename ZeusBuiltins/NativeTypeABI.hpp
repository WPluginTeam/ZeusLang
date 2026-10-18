#pragma once

#include <string_view>
#include <vector>
#include "NativeTypeIDs.hpp"

namespace ZeusBuiltins {

struct NativeTypeABI {
    std::string_view canonicalName;
    std::vector<std::string_view> aliases;
    std::string_view llvmType;
    NativeValueABI valueABI;
    unsigned integerBits = 0;
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
    static const std::vector<NativeTypeABI> types = [] {
        std::vector<NativeTypeABI> result;
        for (const auto& type : nativeTypeIDs()) {
            if (type.valueABI == NativeValueABI::NativePointer) continue;
            result.push_back({type.canonicalName,
                std::vector<std::string_view>(type.aliases.begin(),
                    type.aliases.begin() + type.aliasCount),
                type.llvmType, type.valueABI, type.bits});
        }
        // These are compiler/runtime type policies, not C++ native type IDs.
        // Keep them available to existing dynamic-value lowering paths.
        result.push_back({"bigint", {}, "ptr", NativeValueABI::ZeusValue});
        result.push_back({"float", {"float32", "float64"}, "ptr", NativeValueABI::ZeusValue});
        result.push_back({"nil", {}, "ptr", NativeValueABI::ZeusValue});
        result.push_back({"Array", {"list", "array"}, "ptr", NativeValueABI::ZeusValue});
        result.push_back({"dict", {"map"}, "ptr", NativeValueABI::ZeusValue});
        result.push_back({"object", {}, "ptr", NativeValueABI::ZeusValue});
        result.push_back({"func", {"function"}, "ptr", NativeValueABI::ZeusValue});
        return result;
    }();
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

inline const NativeTypeABI* findNativeSignedIntegerABI(std::string_view name) {
    const auto* abi = findNativeTypeABI(name);
    return abi && abi->valueABI == NativeValueABI::SignedInteger ? abi : nullptr;
}

inline const NativeTypeABI* findNativeUnsignedIntegerABI(std::string_view name) {
    const auto* abi = findNativeTypeABI(name);
    return abi && abi->valueABI == NativeValueABI::UnsignedInteger ? abi : nullptr;
}

inline const NativeTypeABI* findNativeIntegerABI(std::string_view name) {
    const auto* abi = findNativeTypeABI(name);
    return abi && (abi->valueABI == NativeValueABI::SignedInteger ||
                   abi->valueABI == NativeValueABI::UnsignedInteger)
        ? abi : nullptr;
}

inline unsigned nativeFloatingPointBits(std::string_view name) {
    const auto* abi = findNativeTypeABI(name);
    return abi && abi->valueABI == NativeValueABI::FloatingPoint ? abi->integerBits : 0;
}

inline bool isNativeFloatingPointType(std::string_view name) {
    return nativeFloatingPointBits(name) != 0;
}

} // namespace ZeusBuiltins
