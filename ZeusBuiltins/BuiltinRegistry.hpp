#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ZeusBuiltins {

enum class ImportTrigger {
    Always,
    BytesLiteral
};

enum class TypeCategory {
    String,
    List,
    Bytes,
    Opaque
};

enum class BuiltinLiteralKind {
    Array,
    Bytes,
    String
};

struct LiteralWrapper {
    BuiltinLiteralKind kind;
    std::string_view sourceType;
    std::string_view qualifiedClassName;
    std::string_view shortClassName;
};

struct ImplementationModule {
    std::string_view packageName;
    bool autoImportToVM;
    bool autoImportToLLVM;
    bool exposeGlobals;
    ImportTrigger trigger;
    int exposureOrder;
};

struct TypeImplementation {
    std::string_view canonicalName;
    std::vector<std::string_view> aliases;
    std::string_view packageName;
    TypeCategory category;
    bool immutable;
    bool inheritable;
    bool dynamicStringification = false;
};

inline const std::vector<ImplementationModule>& implementationModules() {
    static const std::vector<ImplementationModule> modules = {
        {"ZeusBuiltins.exception", true, true, false, ImportTrigger::Always, -1},
        {"ZeusBuiltins.namespace", true, false, false, ImportTrigger::Always, -1},
        {"ZeusBuiltins.list", true, true, true, ImportTrigger::Always, 1},
        {"ZeusBuiltins.string", true, true, true, ImportTrigger::Always, 0},
        {"ZeusBuiltins.dict", true, true, true, ImportTrigger::Always, 2},
        {"ZeusBuiltins.io", true, false, false, ImportTrigger::Always, -1},
        {"ZeusBuiltins.byte", true, true, true, ImportTrigger::BytesLiteral, 2}
    };
    return modules;
}

inline const std::vector<TypeImplementation>& typeImplementations() {
    static const std::vector<TypeImplementation> types = {
        {"string", {"str", "String"}, "ZeusBuiltins.string", TypeCategory::String, true, false},
        {"Array", {"list", "array"}, "ZeusBuiltins.list", TypeCategory::List, false, true},
        {"Dict", {}, "ZeusBuiltins.dict", TypeCategory::Opaque, false, true, true},
        {"Byte", {}, "ZeusBuiltins.byte", TypeCategory::Opaque, true, true},
        {"bytes", {}, "ZeusBuiltins.byte", TypeCategory::Bytes, true, false}
    };
    return types;
}

inline const std::vector<LiteralWrapper>& literalWrappers() {
    static const std::vector<LiteralWrapper> wrappers = {
        {BuiltinLiteralKind::Array, "list", "ZeusBuiltins.list.Array", "Array"}
    };
    return wrappers;
}

inline const LiteralWrapper* findLiteralWrapper(BuiltinLiteralKind kind) {
    for (const auto& wrapper : literalWrappers()) {
        if (wrapper.kind == kind) return &wrapper;
    }
    return nullptr;
}

inline bool shouldWrapLiteral(BuiltinLiteralKind kind, bool useOriginal) {
    return !useOriginal && findLiteralWrapper(kind) != nullptr;
}

inline const TypeImplementation* findTypeImplementation(const std::string& name) {
    for (const auto& type : typeImplementations()) {
        if (std::string(type.canonicalName) == name) return &type;
        for (const auto alias : type.aliases) {
            if (std::string(alias) == name) return &type;
        }
    }
    return nullptr;
}

inline bool isImplementationModule(const std::string& packageName) {
    for (const auto& module : implementationModules()) {
        if (std::string(module.packageName) == packageName) return true;
    }
    return false;
}

} // namespace ZeusBuiltins
