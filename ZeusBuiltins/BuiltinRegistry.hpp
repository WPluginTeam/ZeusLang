#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ZeusBuiltins {

// How many parameters an @native(from=...) declaration may take. Win32 entry
// points routinely need more than a hand-rolled helper does, for example
// CreateProcessW has ten. The runtime rebuilds the callee signature by
// recursion, so the practical limit is only how deep that recursion may go
// before template instantiation becomes untenable. The parser enforces the
// same number so a declaration is rejected before it reaches the runtime.
constexpr size_t kDynamicNativeMaxParameters = 16;

// A pointer is a 64-bit address on every target Zeus supports, so an int
// parameter already carries one. Naming it explicitly keeps declarations
// readable without adding a type the dispatch table has to know about: raw
// addresses from zeus.mem or @faddr pass straight to the callee.
inline bool isNativePointerTypeName(const std::string& type) {
    return type == "ptr" || type == "void*" || type == "pointer";
}

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
    std::string_view llvmSourceFile = {};
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

struct BuiltinFunctionType {
    std::string_view name;
    std::string_view returnType;
};

inline const BuiltinFunctionType* findBuiltinFunctionType(
    std::string_view name
) {
    static const BuiltinFunctionType functions[] = {
        {"bytes", "bytes"},
        {"__bytes_create", "bytes"}
    };
    for (const auto& function : functions) {
        if (function.name == name) return &function;
    }
    return nullptr;
}

inline const std::vector<ImplementationModule>& implementationModules() {
    static const std::vector<ImplementationModule> modules = {
        {"zeus.exception", true, true, false, ImportTrigger::Always, -1},
        {"zeus.namespace", true, false, false, ImportTrigger::Always, -1},
        {"zeus.list", true, true, true, ImportTrigger::Always, 1},
        {"zeus.string", true, true, true, ImportTrigger::Always, 0},
        {"zeus.dict", true, true, true, ImportTrigger::Always, 2},
        {"zeus.io", true, false, false, ImportTrigger::Always, -1},
        {"zeus.byte", true, true, true, ImportTrigger::BytesLiteral, 2},
        {"zeus.native_runtime", true, true, true, ImportTrigger::Always, 3}
    };
    return modules;
}

inline const std::vector<TypeImplementation>& typeImplementations() {
    static const std::vector<TypeImplementation> types = {
        {"string", {"str", "String"}, "zeus.string", TypeCategory::String, true, false},
        {"Array", {"list", "array"}, "zeus.list", TypeCategory::List, false, true},
        {"Dict", {}, "zeus.dict", TypeCategory::Opaque, false, true, true},
        {"Byte", {}, "zeus.byte", TypeCategory::Opaque, true, true},
        {"bytes", {}, "zeus.byte", TypeCategory::Bytes, true, false}
    };
    return types;
}

inline const std::vector<LiteralWrapper>& literalWrappers() {
    static const std::vector<LiteralWrapper> wrappers = {};
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

inline const ImplementationModule* findImplementationModule(const std::string& packageName) {
    for (const auto& module : implementationModules()) {
        if (std::string(module.packageName) == packageName) return &module;
    }
    return nullptr;
}

inline bool isImplementationModule(const std::string& packageName) {
    return findImplementationModule(packageName) != nullptr;
}

} // namespace ZeusBuiltins
