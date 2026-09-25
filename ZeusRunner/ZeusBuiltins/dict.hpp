#pragma once

#include "core.hpp"

#include <algorithm>

namespace ZeusBuiltins {

inline ZeusType::Object* getMutableDictData(
    const ZeusType::UniversalVariant& value
) {
    const auto* variant = std::get_if<ZeusType::Variant>(&value);
    if (!variant) return nullptr;
    return const_cast<ZeusType::Object*>(std::get_if<ZeusType::Object>(variant));
}

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

    const ZeusType::Object* source = getMutableDictData(args[0]);
    if (!source) {
        throw std::runtime_error("__dict_clone: argument must be a dictionary");
    }

    ZeusType::Object result;
    result.impl = std::make_shared<ZeusType::ObjectData>();
    if (source->impl) {
        result.impl->data = source->impl->data;
        result.impl->order = source->impl->order;
    }
    return ZeusType::UniversalVariant(ZeusType::Variant(std::move(result)));
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

inline bool takeDictValue(
    const ZeusType::UniversalVariant& value,
    const std::string& key,
    ZeusType::UniversalVariant& removed
) {
    ZeusType::Object* dict = getMutableDictData(value);
    if (!dict || !dict->impl) return false;

    auto found = dict->impl->data.find(key);
    if (found == dict->impl->data.end()) return false;

    removed = std::move(found->second);
    dict->impl->data.erase(found);
    auto& order = dict->impl->order;
    order.erase(std::remove(order.begin(), order.end(), key), order.end());
    return true;
}

inline ZeusType::UniversalVariant builtin_dict_delete(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) {
        throw std::runtime_error("__dict_delete expects 2 arguments");
    }
    if (!getMutableDictData(args[0])) {
        throw std::runtime_error("__dict_delete: first argument must be a dictionary");
    }

    ZeusType::UniversalVariant removed;
    if (!takeDictValue(args[0], getDictKeyString(args[1]), removed)) {
        return ZeusType::UniversalVariant(ZeusType::Variant(ZeusType::Null()));
    }
    return removed;
}

inline void registerDictBuiltins() {
    registerBuiltin("__dict_delete", builtin_dict_delete, 2, 2);
    registerBuiltin("__dict_clone", builtin_dict_clone, 1, 1);
    registerBuiltin("__dict_has_to_string", builtin_dict_has_to_string, 2, 2);
}

} // namespace ZeusBuiltins
