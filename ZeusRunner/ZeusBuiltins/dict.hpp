#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusContainerOps.hpp"

#include <mutex>

namespace ZeusBuiltins {

inline std::string normalizedDictKeyString(
    const ZeusType::UniversalVariant& value,
    const char* operation
) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
        if (const auto* text = std::get_if<std::string>(&literal->data)) {
            return *text;
        }
    }
    throw std::runtime_error(
        std::string(operation) + ": key must be a normalized string");
}

inline std::string getDictKeyString(
    const ZeusType::UniversalVariant& value
) {
    return normalizedDictKeyString(value, "__dict_delete");
}

inline ZeusType::UniversalVariant builtin_dict_clone(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1) {
        throw std::runtime_error("__dict_clone expects 1 argument");
    }

    return ZeusContainerOps::dictClone(args[0]);
}

inline bool dictValueHasToString(
    const ZeusType::UniversalVariant& value,
    const std::string& methodName
) {
    if (const auto* handle = std::get_if<ZeusType::ObjectHandle>(&value)) {
        if (!g_vmObjects || !g_vmClasses || handle->id < 0 ||
            handle->id >= static_cast<int>(g_vmObjects->size())) {
            return false;
        }

        const auto& object = (*g_vmObjects)[handle->id];
        if (object.generation != handle->generation ||
            object.markedForDeletion || object.classId < 0 ||
            object.classId >= static_cast<int>(g_vmClasses->size())) {
            return false;
        }

        const auto& classInfo = (*g_vmClasses)[object.classId];
        return classInfo.instanceMethods.find(methodName) !=
               classInfo.instanceMethods.end();
    }

    return false;
}

inline ZeusType::UniversalVariant builtin_dict_has_to_string(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) {
        throw std::runtime_error("__dict_has_to_string expects 2 arguments");
    }
    return ZeusType::UniversalVariant(
        dictValueHasToString(args[0], getDictKeyString(args[1])));
}

inline ZeusType::UniversalVariant builtin_dict_delete(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) {
        throw std::runtime_error("__dict_delete expects 2 arguments");
    }
    return ZeusContainerOps::dictDelete(args[0], args[1]);
}

// The dict ABI in ZeusABI.hpp answers these two queries for AOT code. The VM
// resolves the same @native declarations through the injection registry, so the
// .zeu layer can drop its using LLVM / using VM split: length() stops walking
// the entries just to count them, and hasKey() becomes one hash lookup instead
// of a linear scan that re-normalized every key.
inline ZeusType::UniversalVariant builtin_dict_length(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1) {
        throw std::runtime_error("__dict_length_i64 expects 1 argument");
    }

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* dictionary = ZeusContainerOps::dictValue(args[0]);
    if (!dictionary || !dictionary->impl) {
        return ZeusType::UniversalVariant(LexerType::Value(static_cast<int64_t>(0)));
    }
    return ZeusType::UniversalVariant(LexerType::Value(static_cast<int64_t>(
        static_cast<int64_t>(dictionary->impl->data.size()))));
}

inline ZeusType::UniversalVariant builtin_dict_has_key(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) {
        throw std::runtime_error("__dict_has_key expects 2 arguments");
    }

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* dictionary = ZeusContainerOps::dictValue(args[0]);
    if (!dictionary || !dictionary->impl) {
        return ZeusType::UniversalVariant(LexerType::Value(static_cast<int64_t>(0)));
    }

    const std::string key = normalizedDictKeyString(args[1], "__dict_has_key");
    const bool present = dictionary->impl->data.find(key) !=
        dictionary->impl->data.end();
    return ZeusType::UniversalVariant(
        LexerType::Value(static_cast<int64_t>(present ? 1 : 0)));
}

inline void registerDictBuiltins() {
    registerBuiltin("__dict_delete", builtin_dict_delete, 2, 2);
    registerBuiltin("__dict_clone", builtin_dict_clone, 1, 1);
    registerBuiltin("__dict_has_to_string", builtin_dict_has_to_string, 2, 2);
}

} // namespace ZeusBuiltins
