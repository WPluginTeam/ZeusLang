#pragma once

#include "ZeusVariant.hpp"
#include <variant>

namespace ZeusGC {

// Helpers to retain/release ObjectHandle inside a UniversalVariant.
// VMType is expected to provide retainObject(int) and releaseObject(int).
template <typename VMType>
inline void retainVariantIfObject(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.retainObject(std::get<ZeusType::ObjectHandle>(v).id);
    }
}

template <typename VMType>
inline void releaseVariantIfObject(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.releaseObject(std::get<ZeusType::ObjectHandle>(v).id);
    }
}

// Deep retain/release for UniversalVariant, including nested values inside Variant(Object/Array)
// and NamedArg wrappers. VMType is expected to provide:
//  - retainObject(int), releaseObject(int)
//  - retainClosure(int), releaseClosure(int)
template <typename VMType>
inline void retainVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v);

template <typename VMType>
inline void releaseVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v);

template <typename VMType>
inline void retainVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.retainObject(std::get<ZeusType::ObjectHandle>(v).id);
        return;
    }
    if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
        vm.retainClosure(std::get<ZeusType::ClosureHandle>(v).id);
        return;
    }
    if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(v)) {
        auto ptr = std::get<std::shared_ptr<ZeusType::NamedArg>>(v);
        if (ptr) retainVariantDeep(vm, ptr->value);
        return;
    }
    if (!std::holds_alternative<ZeusType::Variant>(v)) return;

    const auto& var = std::get<ZeusType::Variant>(v);
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ZeusType::Array>) {
            if (!x.impl) return;
            for (const auto& it : x.impl->data) retainVariantDeep(vm, it);
        } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
            if (!x.impl) return;
            for (const auto& kv : x.impl->data) retainVariantDeep(vm, kv.second);
        } else {
            // primitives / Bytes / Function: no-op
        }
    }, var);
}

template <typename VMType>
inline void releaseVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.releaseObject(std::get<ZeusType::ObjectHandle>(v).id);
        return;
    }
    if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
        vm.releaseClosure(std::get<ZeusType::ClosureHandle>(v).id);
        return;
    }
    if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(v)) {
        auto ptr = std::get<std::shared_ptr<ZeusType::NamedArg>>(v);
        if (ptr) releaseVariantDeep(vm, ptr->value);
        return;
    }
    if (!std::holds_alternative<ZeusType::Variant>(v)) return;

    const auto& var = std::get<ZeusType::Variant>(v);
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ZeusType::Array>) {
            if (!x.impl) return;
            for (const auto& it : x.impl->data) releaseVariantDeep(vm, it);
        } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
            if (!x.impl) return;
            for (const auto& kv : x.impl->data) releaseVariantDeep(vm, kv.second);
        } else {
            // primitives / Bytes / Function: no-op
        }
    }, var);
}

} // namespace ZeusGC
