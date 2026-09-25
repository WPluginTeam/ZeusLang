#pragma once

#include <string>
#include "../ZeusBuiltins/BuiltinRegistry.hpp"

namespace ZeusTypeInfer {

    enum class InferredType {
        Unknown,

        Int,
        BigInt,
        Float,
        Bool,
        Char,
        String,
        Bytes,
        Nil,

        List,
        Dict,
        Object,
        Function
    };

    inline bool isNumeric(InferredType type) {
        return type == InferredType::Int ||
               type == InferredType::BigInt ||
               type == InferredType::Float;
    }

    inline const char* typeName(InferredType type) {
        switch (type) {
            case InferredType::Int:      return "int";
            case InferredType::BigInt:   return "bigint";
            case InferredType::Float:    return "float";
            case InferredType::Bool:     return "bool";
            case InferredType::Char:     return "char";
            case InferredType::String:   return "string";
            case InferredType::Bytes:    return "bytes";
            case InferredType::Nil:      return "nil";
            case InferredType::List:     return "list";
            case InferredType::Dict:     return "dict";
            case InferredType::Object:   return "object";
            case InferredType::Function: return "function";
            case InferredType::Unknown:  return "unknown";
        }

        return "unknown";
    }

    inline InferredType parseTypeName(const std::string& name) {
        if (!name.empty() && name.front() == '&') {
            return parseTypeName(name.substr(1));
        }
        if (const auto* builtin = ZeusBuiltins::findTypeImplementation(name)) {
            if (builtin->category == ZeusBuiltins::TypeCategory::String) return InferredType::String;
            if (builtin->category == ZeusBuiltins::TypeCategory::List) return InferredType::List;
            if (builtin->category == ZeusBuiltins::TypeCategory::Bytes) return InferredType::Bytes;
            if (builtin->category == ZeusBuiltins::TypeCategory::Opaque &&
                builtin->inheritable) return InferredType::Object;
        }
        if (name == "int" || name == "int32" || name == "int64") return InferredType::Int;
        if (name == "bigint") return InferredType::BigInt;
        if (name == "float" || name == "float32" || name == "float64") return InferredType::Float;
        if (name == "bool") return InferredType::Bool;
        if (name == "char") return InferredType::Char;
        if (name == "string" || name == "str") return InferredType::String;
        if (name == "bytes") return InferredType::Bytes;
        if (name == "nil") return InferredType::Nil;
        if (name == "list" || name == "array") return InferredType::List;
        if (name == "dict" || name == "map") return InferredType::Dict;
        if (name == "object") return InferredType::Object;
        if (name == "func" || name == "function") return InferredType::Function;
        return InferredType::Unknown;
    }

}
