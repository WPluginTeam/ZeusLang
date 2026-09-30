#pragma once

#include <string>

#include "../ZeusBuiltins/BuiltinRegistry.hpp"
#include "../ZeusBuiltins/NativeTypeABI.hpp"

namespace ZeusRuntimeType {

inline std::string canonicalTypeName(const std::string& typeName) {
    if (typeName.empty()) return {};
    if (typeName.front() == '&') {
        return "&" + canonicalTypeName(typeName.substr(1));
    }

    if (const auto* implementation = ZeusBuiltins::findTypeImplementation(typeName)) {
        switch (implementation->category) {
            case ZeusBuiltins::TypeCategory::String:
                return "string";
            case ZeusBuiltins::TypeCategory::List:
                return "Array";
            case ZeusBuiltins::TypeCategory::Bytes:
                return "bytes";
            case ZeusBuiltins::TypeCategory::Opaque:
                return implementation->canonicalName == "Dict"
                    ? "dict"
                    : std::string(implementation->canonicalName);
        }
    }

    if (const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName)) {
        return abi->canonicalName == "func"
            ? "function"
            : std::string(abi->canonicalName);
    }

    if (typeName == "array" || typeName == "list") return "Array";
    if (typeName == "map" || typeName == "dict") return "dict";
    if (typeName == "str" || typeName == "String") return "string";
    if (typeName == "int32" || typeName == "int64") return "int";
    if (typeName == "float32" || typeName == "float64") return "float";
    if (typeName == "func" || typeName == "closure" ||
        typeName == "native_function") return "function";
    return typeName;
}

inline bool isPrimitiveTypeName(const std::string& typeName) {
    const std::string canonical = canonicalTypeName(typeName);
    return canonical == "nil" || canonical == "int" || canonical == "bigint" ||
        canonical == "float" || canonical == "bool" || canonical == "char" ||
        canonical == "string" || canonical == "bytes" || canonical == "Array" ||
        canonical == "dict" || canonical == "function" || canonical == "class" ||
        canonical == "namespace";
}

inline bool isCompatible(const std::string& expectedType, const std::string& actualType) {
    const bool expectsReference = !expectedType.empty() && expectedType.front() == '&';
    const bool isReference = !actualType.empty() && actualType.front() == '&';
    const std::string expected = canonicalTypeName(
        expectsReference ? expectedType.substr(1) : expectedType);
    const std::string actual = canonicalTypeName(
        isReference ? actualType.substr(1) : actualType);

    if (expectsReference && !isReference) return false;
    if (!expectsReference && isReference &&
        expected != "int" && expected != "bool") return false;

    if (expected == "object") {
        return actual == "object" ||
            (actual != "unknown" && !isPrimitiveTypeName(actual));
    }
    return expected == actual;
}

} // namespace ZeusRuntimeType
