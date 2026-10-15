#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusContainerOps.hpp"

namespace ZeusBuiltins {

inline std::string getDictKeyString(
    const ZeusType::UniversalVariant& value
) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
        if (const auto* text = std::get_if<std::string>(&literal->data)) {
            return *text;
        }
    }
    throw std::runtime_error("__dict_delete: key must be a normalized string");
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

inline void registerDictBuiltins() {
    registerBuiltin("__dict_delete", builtin_dict_delete, 2, 2);
    registerBuiltin("__dict_clone", builtin_dict_clone, 1, 1);
    registerBuiltin("__dict_has_to_string", builtin_dict_has_to_string, 2, 2);
}

} // namespace ZeusBuiltins
