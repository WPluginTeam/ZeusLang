#pragma once

#include "ZeusVMUtils.hpp"
#include "ZeusGC.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

namespace ZeusContainerOps {

using Value = ZeusType::UniversalVariant;

inline const ZeusType::Array* arrayValue(const Value& value) {
    const auto* variant = std::get_if<ZeusType::Variant>(&value);
    return variant ? std::get_if<ZeusType::Array>(variant) : nullptr;
}

inline const ZeusType::Object* dictValue(const Value& value) {
    const auto* variant = std::get_if<ZeusType::Variant>(&value);
    return variant ? std::get_if<ZeusType::Object>(variant) : nullptr;
}

inline Value arrayPush(const Value& arrayValue, const Value& value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* array = ZeusContainerOps::arrayValue(arrayValue);
    if (!array) {
        throw std::runtime_error("__array_push: first argument must be an array");
    }
    if (!array->impl) {
        throw std::runtime_error("__array_push: array has no internal data");
    }
    ZeusGC::appendContainerValue(array->impl->data, value);
    return ZeusType::BigInteger(static_cast<int>(array->impl->data.size()));
}

inline Value arrayPop(const Value& arrayValue, bool deferClassEdgeRelease = false) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* array = ZeusContainerOps::arrayValue(arrayValue);
    if (!array || !array->impl || array->impl->data.empty()) {
        throw std::runtime_error("__array_pop: cannot pop from empty array");
    }
    Value result = array->impl->data.back();
    array->impl->data.pop_back();
    if (!deferClassEdgeRelease) {
        ZeusGC::releaseContainerClassEdgeNoexcept(result);
    }
    return result;
}

inline Value arrayGet(const Value& arrayValue, const Value& indexValue) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* array = ZeusContainerOps::arrayValue(arrayValue);
    if (!array || !array->impl) {
        throw std::runtime_error("__array_get: first argument must be an array");
    }
    int index = 0;
    ZeusVMUtils::tryGetInt(indexValue, index);
    if (index < 0 || static_cast<size_t>(index) >= array->impl->data.size()) {
        throw std::runtime_error("__array_get: index out of range");
    }
    return array->impl->data[static_cast<size_t>(index)];
}

inline Value arraySet(
    const Value& arrayValue,
    const Value& indexValue,
    const Value& value
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* array = ZeusContainerOps::arrayValue(arrayValue);
    if (!array || !array->impl) {
        throw std::runtime_error("__array_set: first argument must be an array");
    }
    int index = 0;
    ZeusVMUtils::tryGetInt(indexValue, index);
    if (index < 0 || static_cast<size_t>(index) >= array->impl->data.size()) {
        throw std::runtime_error("__array_set: index out of range");
    }
    ZeusGC::replaceContainerValue(
        array->impl->data[static_cast<size_t>(index)], value);
    return LexerType::Value(true);
}

inline Value arrayClear(const Value& arrayValue) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* array = ZeusContainerOps::arrayValue(arrayValue);
    if (!array || !array->impl) {
        throw std::runtime_error("__array_clear: argument must be an array");
    }
    std::vector<Value> removed;
    removed.swap(array->impl->data);
    for (const auto& value : removed) {
        ZeusGC::releaseContainerClassEdgeNoexcept(value);
    }
    return LexerType::Value(true);
}

inline Value arraySlice(
    const Value& arrayValue,
    const Value& startValue,
    const Value& endValue
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* array = ZeusContainerOps::arrayValue(arrayValue);
    if (!array || !array->impl) {
        throw std::runtime_error("__array_slice: first argument must be an array");
    }

    int start = 0;
    int end = static_cast<int>(array->impl->data.size());
    ZeusVMUtils::tryGetInt(startValue, start);
    ZeusVMUtils::tryGetInt(endValue, end);
    if (start < 0) start = 0;
    if (end > static_cast<int>(array->impl->data.size())) {
        end = static_cast<int>(array->impl->data.size());
    }
    if (start > end) start = end;

    ZeusType::Array result;
    result.impl = ZeusGC::makeArrayData();
    result.impl->valueRefCount.store(0, std::memory_order_relaxed);
    for (int i = start; i < end; ++i) {
        ZeusGC::appendContainerValue(
            result.impl->data,
            array->impl->data[static_cast<size_t>(i)]);
    }
    return ZeusType::Variant(std::move(result));
}

inline std::string normalizedDictKey(const Value& value) {
    if (const auto* text = std::get_if<std::string>(&value)) return *text;
    if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
        if (const auto* text = std::get_if<std::string>(&literal->data)) {
            return *text;
        }
    }
    throw std::runtime_error("__dict_delete: key must be a normalized string");
}

// The caller holds graphMutex so map membership, order, and GC edges change
// as one operation.
inline Value dictDeleteFromObject(
    const ZeusType::Object& dict,
    const std::string& key,
    bool deferClassEdgeRelease
) {
    if (!dict.impl) return ZeusType::Variant(ZeusType::Null());

    auto found = dict.impl->data.find(key);
    if (found == dict.impl->data.end()) {
        return ZeusType::Variant(ZeusType::Null());
    }

    if (!dict.impl->hasOrderKey(key)) {
        throw std::runtime_error(
            "__dict_delete: dictionary insertion-order index is inconsistent");
    }

    Value removed = std::move(found->second);
    dict.impl->eraseOrderKey(key);
    dict.impl->data.erase(found);
    if (!deferClassEdgeRelease) {
        ZeusGC::releaseContainerClassEdgeNoexcept(removed);
    }
    return removed;
}

inline Value dictDelete(
    const Value& dictValue,
    const Value& keyValue,
    bool deferClassEdgeRelease = false
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* dict = ZeusContainerOps::dictValue(dictValue);
    if (!dict) {
        throw std::runtime_error(
            "__dict_delete: first argument must be a dictionary");
    }
    const std::string key = normalizedDictKey(keyValue);
    return dictDeleteFromObject(*dict, key, deferClassEdgeRelease);
}

inline Value dictClone(const Value& dictValue) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* source = ZeusContainerOps::dictValue(dictValue);
    if (!source) {
        throw std::runtime_error("__dict_clone: argument must be a dictionary");
    }

    ZeusType::Object result;
    result.impl = ZeusGC::makeObjectData();
    if (source->impl) {
        for (const auto& item : source->impl->data) {
            ZeusGC::retainContainerClassEdge(item.second);
            try {
                result.impl->data.emplace(item.first, item.second);
            } catch (...) {
                ZeusGC::releaseContainerClassEdgeNoexcept(item.second);
                throw;
            }
        }
        result.impl->order = source->impl->order;
        result.impl->deletedOrderCount = source->impl->deletedOrderCount;
        result.impl->orderPositions.reserve(source->impl->orderPositions.size());
        for (const auto& position : source->impl->orderPositions) {
            const auto copied = result.impl->data.find(std::string(position.first));
            if (copied == result.impl->data.end()) {
                throw std::logic_error(
                    "__dict_clone: insertion-order index is inconsistent");
            }
            const auto inserted = result.impl->orderPositions.emplace(
                std::string_view(copied->first), position.second);
            if (!inserted.second) {
                throw std::logic_error(
                    "__dict_clone: duplicate insertion-order key");
            }
        }
    }
    return ZeusType::Variant(std::move(result));
}

} // namespace ZeusContainerOps
