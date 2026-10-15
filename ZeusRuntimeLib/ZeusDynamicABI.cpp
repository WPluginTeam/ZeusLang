// ZeusRuntimeLib/ZeusDynamicABI.cpp

#include "ZeusDynamicABI.hpp"
#include "ZeusABIInternal.hpp"
#include "ZeusContainerOps.hpp"
#include "ZeusVMUtils.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

ZeusType::UniversalVariant nullVariant() {
    return ZeusType::UniversalVariant(ZeusType::Variant(ZeusType::Null()));
}

ZeusBox* makeBox(const ZeusType::UniversalVariant& value) {
    ZeusBox* box = ZEUS_NEW_BOX();
    box->assignVariant(value);
    return box;
}

ZeusBox* makeNullBox() {
    return makeBox(nullVariant());
}

bool isArray(const ZeusBox* box) {
    if (!box || !std::holds_alternative<ZeusType::Variant>(box->v)) return false;
    return std::holds_alternative<ZeusType::Array>(
        std::get<ZeusType::Variant>(box->v)
    );
}

bool isDict(const ZeusBox* box) {
    if (!box || !std::holds_alternative<ZeusType::Variant>(box->v)) return false;
    return std::holds_alternative<ZeusType::Object>(
        std::get<ZeusType::Variant>(box->v)
    );
}

ZeusType::Array* getArray(ZeusBox* box) {
    if (!isArray(box)) return nullptr;
    return &std::get<ZeusType::Array>(std::get<ZeusType::Variant>(box->v));
}

const ZeusType::Array* getArray(const ZeusBox* box) {
    if (!isArray(box)) return nullptr;
    return &std::get<ZeusType::Array>(std::get<ZeusType::Variant>(box->v));
}

ZeusType::Object* getDict(ZeusBox* box) {
    if (!isDict(box)) return nullptr;
    return &std::get<ZeusType::Object>(std::get<ZeusType::Variant>(box->v));
}

const ZeusType::Object* getDict(const ZeusBox* box) {
    if (!isDict(box)) return nullptr;
    return &std::get<ZeusType::Object>(std::get<ZeusType::Variant>(box->v));
}

std::string keyString(const ZeusType::UniversalVariant& value) {
    if (std::holds_alternative<LexerType::Value>(value)) {
        const auto& literal = std::get<LexerType::Value>(value);
        if (std::holds_alternative<std::string>(literal.data)) {
            return std::get<std::string>(literal.data);
        }
        if (std::holds_alternative<ZeusType::BigInteger>(literal.data)) {
            return std::get<ZeusType::BigInteger>(literal.data).toString();
        }
        if (std::holds_alternative<ZeusType::BigDecimal>(literal.data)) {
            return std::get<ZeusType::BigDecimal>(literal.data).toString();
        }
        if (std::holds_alternative<bool>(literal.data)) {
            return std::get<bool>(literal.data) ? "true" : "false";
        }
    }

    if (std::holds_alternative<std::string>(value)) {
        return std::get<std::string>(value);
    }
    if (std::holds_alternative<ZeusType::BigInteger>(value)) {
        return std::get<ZeusType::BigInteger>(value).toString();
    }
    if (std::holds_alternative<ZeusType::BigDecimal>(value)) {
        return std::get<ZeusType::BigDecimal>(value).toString();
    }
    if (std::holds_alternative<bool>(value)) {
        return std::get<bool>(value) ? "true" : "false";
    }
    if (const auto* character = std::get_if<char>(&value)) {
        return std::string(1, *character);
    }

    if (const auto* handle =
            std::get_if<ZeusType::LLVMClassHandle>(&value)) {
        if (handle->state && handle->state->typeName == "String") {
            ZeusBox boxedString;
            boxedString.assignVariant(value);

            ZeusValue dynamicString{};
            zeus_value_init_nil(&dynamicString);
            std::unique_ptr<ZeusValue, decltype(&zeus_value_release)> ownedString(
                &dynamicString, &zeus_value_release);
            zeus_value_from_box(ownedString.get(), &boxedString);

            const ZeusNativeString view =
                zeus_value_intern_string(ownedString.get());
            uint64_t length = 0;
            if (view) std::memcpy(&length, view - sizeof(uint64_t), sizeof(length));
            if (!view || length >
                    static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
                length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
                throw std::length_error("String dictionary key is too large");
            }
            return std::string(view, static_cast<size_t>(length));
        }
    }

    return "<obj>";
}

int arrayIndexFromI64(int64_t integer) {
    if (integer < std::numeric_limits<int>::min() ||
        integer > std::numeric_limits<int>::max()) {
        throw std::overflow_error("Value too large for array index");
    }
    return static_cast<int>(integer);
}

int arrayIndex(const ZeusType::UniversalVariant& value) {
    int64_t integer = 0;
    if (!ZeusVMUtils::tryGetInt64(value, integer)) return 0;
    return arrayIndexFromI64(integer);
}

using OwnedZeusBox = std::unique_ptr<ZeusBox, decltype(&zeus_box_release)>;

class OwnedZeusBoxRange {
public:
    OwnedZeusBoxRange(ZeusBox* const* boxes, size_t count) noexcept
        : boxes_(boxes), count_(count) {}

    OwnedZeusBoxRange(const OwnedZeusBoxRange&) = delete;
    OwnedZeusBoxRange& operator=(const OwnedZeusBoxRange&) = delete;

    ~OwnedZeusBoxRange() noexcept {
        releaseNext(count_ - next_);
    }

    void releaseNext(size_t count) noexcept {
        while (count > 0 && next_ < count_) {
            zeus_box_release(boxes_[next_]);
            ++next_;
            --count;
        }
    }

private:
    ZeusBox* const* boxes_;
    size_t count_;
    size_t next_ = 0;
};

struct DeferredContainerEdgeRelease {
    const ZeusType::UniversalVariant* value = nullptr;

    ~DeferredContainerEdgeRelease() noexcept {
        if (value) ZeusGC::releaseContainerClassEdgeNoexcept(*value);
    }
};

[[noreturn]] void valueContainerPanic(const char* message) {
    throw std::runtime_error(std::string("[ZeusValue container ABI] ") + message);
}

OwnedZeusBox boxFromValue(const ZeusValue* value) {
    if (!value) valueContainerPanic("null dynamic value argument");
    return OwnedZeusBox(zeus_value_to_box(value), &zeus_box_release);
}

void writeValueFromBox(ZeusValue* out, ZeusBox* box) {
    if (!out) valueContainerPanic("null dynamic value result");
    OwnedZeusBox owned(box, &zeus_box_release);
    zeus_value_init_nil(out);
    zeus_value_from_box(out, owned.get());
}

struct ZeusDynamicIteratorImpl {
    enum class Kind { Array, Dict };

    Kind kind;
    size_t index = 0;
    std::shared_ptr<ZeusType::ArrayData> array;
    std::shared_ptr<ZeusType::ObjectData> dict;
    std::shared_ptr<void> rootedNode;
};

} // namespace

struct ZeusDynamicIterator {
    ZeusDynamicIteratorImpl impl;
};

ZeusBox* zeus_array_build(int64_t count, ZeusBox* const* values) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (count < 0 || (count > 0 && !values)) return makeNullBox();
    OwnedZeusBoxRange ownedValues(values, static_cast<size_t>(count));

    ZeusType::Array array;
    array.impl = ZeusGC::makeArrayData();
    array.impl->data.reserve(static_cast<size_t>(count));

    for (int64_t i = 0; i < count; ++i) {
        ZeusBox* value = values[i];
        ZeusGC::appendContainerValue(
            array.impl->data, value ? value->v : nullVariant());
        ownedValues.releaseNext(1);
    }

    return makeBox(ZeusType::UniversalVariant(ZeusType::Variant(array)));
}

ZeusBox* zeus_dict_build(int64_t pairCount, ZeusBox* const* keyValues) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (pairCount < 0 ||
        static_cast<uint64_t>(pairCount) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max() / 2) ||
        (pairCount > 0 && !keyValues)) {
        return makeNullBox();
    }
    const size_t valueCount = static_cast<size_t>(pairCount) * 2;
    OwnedZeusBoxRange ownedValues(keyValues, valueCount);

    ZeusType::Object object;
    object.impl = ZeusGC::makeObjectData();

    for (int64_t i = 0; i < pairCount; ++i) {
        const size_t valueOffset = static_cast<size_t>(i) * 2;
        ZeusBox* key = keyValues[valueOffset];
        ZeusBox* value = keyValues[valueOffset + 1];
        const std::string name = key ? keyString(key->v) : "<obj>";

        ZeusGC::storeObjectContainerValue(
            *object.impl, name, value ? value->v : nullVariant());
        ownedValues.releaseNext(2);
    }

    return makeBox(ZeusType::UniversalVariant(ZeusType::Variant(object)));
}

int64_t zeus_array_length(const ZeusBox* array) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const ZeusType::Array* value = getArray(array);
    if (!value || !value->impl) return 0;
    return static_cast<int64_t>(value->impl->data.size());
}

ZeusBox* zeus_array_load(const ZeusBox* array, int64_t index) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const ZeusType::Array* value = getArray(array);
    if (!value || !value->impl || index < 0 ||
        static_cast<size_t>(index) >= value->impl->data.size()) {
        return makeNullBox();
    }

    return makeBox(value->impl->data[static_cast<size_t>(index)]);
}

ZeusBox* zeus_dict_load(const ZeusBox* dict, const ZeusBox* key) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (getArray(dict) && key) {
        return zeus_array_load(dict, arrayIndex(key->v));
    }
    const ZeusType::Object* object = getDict(dict);
    if (!object || !key) return makeNullBox();

    const std::string name = keyString(key->v);
    if (!object->impl) {
        throw std::runtime_error("KeyError: missing key '" + name + "'");
    }
    auto it = object->impl->data.find(name);
    if (it == object->impl->data.end()) {
        throw std::runtime_error("KeyError: missing key '" + name + "'");
    }
    return makeBox(it->second);
}

ZeusBox* zeus_dict_keys(const ZeusBox* dict) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const ZeusType::Object* object = getDict(dict);
    ZeusType::Array array;
    array.impl = ZeusGC::makeArrayData();

    if (object && object->impl) {
        array.impl->data.reserve(object->impl->data.size());
        for (std::size_t i = 0; i < object->impl->order.size(); ++i) {
            if (!object->impl->isLiveOrderEntry(i)) continue;
            array.impl->data.push_back(
                LexerType::Value(object->impl->order[i]));
        }
    }

    return makeBox(ZeusType::UniversalVariant(ZeusType::Variant(array)));
}

ZeusBox* zeus_array_store(ZeusBox* array, int64_t index, ZeusBox* value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusType::Array* target = getArray(array);
    OwnedZeusBox ownedValue(value, &zeus_box_release);
    ZeusType::UniversalVariant stored = ownedValue ? ownedValue->v : nullVariant();
    OwnedZeusBox result(makeBox(stored), &zeus_box_release);

    if (target && target->impl && index >= 0 &&
        static_cast<size_t>(index) < target->impl->data.size()) {
        ZeusGC::replaceContainerValue(
            target->impl->data[static_cast<size_t>(index)], stored);
    }

    return result.release();
}

ZeusBox* zeus_dict_store(ZeusBox* dict, ZeusBox* key, ZeusBox* value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    OwnedZeusBox ownedKey(key, &zeus_box_release);
    OwnedZeusBox ownedValue(value, &zeus_box_release);
    if (getArray(dict) && key) {
        const int index = arrayIndex(ownedKey->v);
        return zeus_array_store(dict, index, ownedValue.release());
    }
    ZeusType::Object* target = getDict(dict);
    ZeusType::UniversalVariant stored = ownedValue ? ownedValue->v : nullVariant();
    OwnedZeusBox result(makeBox(stored), &zeus_box_release);

    if (target && target->impl && ownedKey) {
        const std::string name = keyString(ownedKey->v);
        ZeusGC::storeObjectContainerValue(*target->impl, name, stored);
    }

    return result.release();
}

ZeusDynamicIterator* zeus_array_iter_begin(const ZeusBox* array) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const ZeusType::Array* value = getArray(array);
    if (!value || !value->impl) return nullptr;

    auto* iterator = new ZeusDynamicIterator();
    iterator->impl.kind = ZeusDynamicIteratorImpl::Kind::Array;
    iterator->impl.array = value->impl;
    iterator->impl.rootedNode = std::static_pointer_cast<void>(value->impl);
    ZeusGC::rootNode(iterator->impl.rootedNode);
    return iterator;
}

int32_t zeus_array_iter_next(ZeusDynamicIterator* iterator, ZeusBox** value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!iterator || !value ||
        iterator->impl.kind != ZeusDynamicIteratorImpl::Kind::Array ||
        !iterator->impl.array ||
        iterator->impl.index >= iterator->impl.array->data.size()) {
        return 0;
    }

    *value = makeBox(iterator->impl.array->data[iterator->impl.index++]);
    return 1;
}

ZeusDynamicIterator* zeus_dict_iter_begin(const ZeusBox* dict) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const ZeusType::Object* value = getDict(dict);
    if (!value || !value->impl) return nullptr;

    auto* iterator = new ZeusDynamicIterator();
    iterator->impl.kind = ZeusDynamicIteratorImpl::Kind::Dict;
    iterator->impl.dict = value->impl;
    iterator->impl.rootedNode = std::static_pointer_cast<void>(value->impl);
    ZeusGC::rootNode(iterator->impl.rootedNode);
    return iterator;
}

int32_t zeus_dict_iter_next(
    ZeusDynamicIterator* iterator,
    ZeusBox** key,
    ZeusBox** value
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!iterator || !key || !value ||
        iterator->impl.kind != ZeusDynamicIteratorImpl::Kind::Dict ||
        !iterator->impl.dict) {
        return 0;
    }

    while (iterator->impl.index < iterator->impl.dict->order.size()) {
        const std::size_t position = iterator->impl.index++;
        if (!iterator->impl.dict->isLiveOrderEntry(position)) continue;

        const std::string& name = iterator->impl.dict->order[position];
        auto found = iterator->impl.dict->data.find(name);
        if (found == iterator->impl.dict->data.end()) continue;

        *key = makeBox(LexerType::Value(name));
        *value = makeBox(found->second);
        return 1;
    }
    return 0;
}

void zeus_dynamic_iter_release(ZeusDynamicIterator* iterator) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (iterator) ZeusGC::unrootNode(iterator->impl.rootedNode);
    delete iterator;
}

void zeus_value_array_build(
    ZeusValue* out,
    int64_t count,
    const ZeusValue* const* values
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array build output");
    zeus_value_init_nil(out);
    if (count < 0 || (count > 0 && !values)) {
        valueContainerPanic("invalid array build arguments");
    }

    try {
        std::vector<OwnedZeusBox> ownedValues;
        std::vector<ZeusBox*> rawValues;
        ownedValues.reserve(static_cast<size_t>(count));
        rawValues.reserve(static_cast<size_t>(count));
        for (int64_t i = 0; i < count; ++i) {
            ownedValues.push_back(boxFromValue(values[i]));
            rawValues.push_back(ownedValues.back().get());
        }
        for (auto& value : ownedValues) value.release();
        writeValueFromBox(out, zeus_array_build(count, rawValues.data()));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to build dynamic array");
    }
}

void zeus_value_dict_build(
    ZeusValue* out,
    int64_t pairCount,
    const ZeusValue* const* keyValues
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null dictionary build output");
    zeus_value_init_nil(out);
    if (pairCount < 0 ||
        static_cast<uint64_t>(pairCount) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max() / 2) ||
        (pairCount > 0 && !keyValues)) {
        valueContainerPanic("invalid dictionary build arguments");
    }

    try {
        const size_t valueCount = static_cast<size_t>(pairCount) * 2;
        std::vector<OwnedZeusBox> ownedValues;
        std::vector<ZeusBox*> rawValues;
        ownedValues.reserve(valueCount);
        rawValues.reserve(valueCount);
        for (size_t i = 0; i < valueCount; ++i) {
            ownedValues.push_back(boxFromValue(keyValues[i]));
            rawValues.push_back(ownedValues.back().get());
        }
        for (auto& value : ownedValues) value.release();
        writeValueFromBox(out, zeus_dict_build(pairCount, rawValues.data()));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to build dynamic dictionary");
    }
}

int32_t zeus_value_is_array(const ZeusValue* value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!value) return 0;
    try {
        OwnedZeusBox boxedValue = boxFromValue(value);
        return getArray(boxedValue.get()) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

void zeus_value_array_length(ZeusValue* out, const ZeusValue* array) {
    if (!out) valueContainerPanic("null array length output");
    zeus_value_init_nil(out);
    try {
        zeus_value_init_i64(out, zeus_value_array_length_i64(array));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to read dynamic array length");
    }
}

void zeus_value_array_load(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* index
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out || !index) valueContainerPanic("null array load argument");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedIndex = boxFromValue(index);
        const ZeusType::Array* target = getArray(boxedArray.get());
        if (!target || !target->impl) {
            zeus_value_init_i64(out, 0);
            return;
        }
        const int arrayElement = arrayIndex(boxedIndex->v);
        if (arrayElement < 0 ||
            static_cast<size_t>(arrayElement) >= target->impl->data.size()) {
            // LIST_LOAD_ELEM shares ZeusVMUtils::arrayRead semantics: an
            // invalid element read produces numeric zero.
            zeus_value_init_i64(out, 0);
            return;
        }
        writeValueFromBox(out, zeus_array_load(boxedArray.get(), arrayElement));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to read dynamic array element");
    }
}

void zeus_value_array_load_checked(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* index
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out || !index) valueContainerPanic("null checked array load argument");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedIndex = boxFromValue(index);
        const ZeusType::Array* target = getArray(boxedArray.get());
        if (!target) {
            throw std::runtime_error("TypeError: attempted subscript on non-container");
        }

        const int arrayElement = arrayIndex(boxedIndex->v);
        const size_t arraySize = target->impl ? target->impl->data.size() : 0;
        if (!target->impl || arrayElement < 0 ||
            static_cast<size_t>(arrayElement) >= arraySize) {
            throw std::runtime_error(
                "Array index out of bounds: index=" + std::to_string(arrayElement) +
                ", size=" + std::to_string(arraySize));
        }
        writeValueFromBox(out, zeus_array_load(boxedArray.get(), arrayElement));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to read checked dynamic array element");
    }
}

void zeus_value_array_load_i64(
    ZeusValue* out,
    const ZeusValue* array,
    int64_t index
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array load output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        const ZeusType::Array* target = getArray(boxedArray.get());
        if (!target || !target->impl) {
            zeus_value_init_i64(out, 0);
            return;
        }
        const int arrayElement = arrayIndexFromI64(index);
        if (arrayElement < 0 ||
            static_cast<size_t>(arrayElement) >= target->impl->data.size()) {
            zeus_value_init_i64(out, 0);
            return;
        }
        writeValueFromBox(out, zeus_array_load(boxedArray.get(), arrayElement));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to read dynamic array element");
    }
}

void zeus_value_array_load_i64_checked(
    ZeusValue* out,
    const ZeusValue* array,
    int64_t index
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array load output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        const ZeusType::Array* target = getArray(boxedArray.get());
        if (!target) {
            throw std::runtime_error("TypeError: attempted subscript on non-container");
        }
        const int arrayElement = arrayIndexFromI64(index);
        const size_t arraySize = target->impl ? target->impl->data.size() : 0;
        if (!target->impl || arrayElement < 0 ||
            static_cast<size_t>(arrayElement) >= arraySize) {
            throw std::runtime_error(
                "Array index out of bounds: index=" + std::to_string(arrayElement) +
                ", size=" + std::to_string(arraySize));
        }
        writeValueFromBox(out, zeus_array_load(boxedArray.get(), arrayElement));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to read dynamic array element");
    }
}

void zeus_value_array_store(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* index,
    const ZeusValue* value
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out || !index) valueContainerPanic("null array store argument");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedIndex = boxFromValue(index);
        OwnedZeusBox boxedValue = boxFromValue(value);
        writeValueFromBox(
            out, zeus_array_store(boxedArray.get(),
                                  arrayIndex(boxedIndex->v),
                                  boxedValue.release())
        );
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to write dynamic array element");
    }
}

void zeus_value_array_store_i64(
    ZeusValue* out,
    const ZeusValue* array,
    int64_t index,
    const ZeusValue* value
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array store output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedValue = boxFromValue(value);
        writeValueFromBox(
            out,
            zeus_array_store(
                boxedArray.get(), arrayIndexFromI64(index), boxedValue.release())
        );
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to write dynamic array element");
    }
}

void zeus_value_dict_keys(ZeusValue* out, const ZeusValue* dict) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null dictionary keys output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedDict = boxFromValue(dict);
        writeValueFromBox(out, zeus_dict_keys(boxedDict.get()));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to build dynamic dictionary keys");
    }
}

void zeus_value_dict_load(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* key
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null dictionary load output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedDict = boxFromValue(dict);
        OwnedZeusBox boxedKey = boxFromValue(key);
        if (getArray(boxedDict.get())) {
            int64_t rawIndex = 0;
            if (!ZeusVMUtils::tryGetInt64(boxedKey->v, rawIndex)) {
                valueContainerPanic("DICT_LOAD_VALUE array index must be an integer");
            }
            if (rawIndex < std::numeric_limits<int>::min() ||
                rawIndex > std::numeric_limits<int>::max()) {
                valueContainerPanic("DICT_LOAD_VALUE array index is outside int range");
            }
            const int index = static_cast<int>(rawIndex);
            const ZeusType::Array* target = getArray(boxedDict.get());
            if (!target->impl || index < 0 ||
                static_cast<size_t>(index) >= target->impl->data.size()) {
                valueContainerPanic("DICT_LOAD_VALUE array index is out of bounds");
            }
        }
        writeValueFromBox(out, zeus_dict_load(boxedDict.get(), boxedKey.get()));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to read dynamic dictionary value");
    }
}

void zeus_value_dict_store(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* key,
    const ZeusValue* value
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null dictionary store output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedDict = boxFromValue(dict);
        OwnedZeusBox boxedKey = boxFromValue(key);
        OwnedZeusBox boxedValue = boxFromValue(value);
        writeValueFromBox(
            out,
            zeus_dict_store(
                boxedDict.get(), boxedKey.release(), boxedValue.release())
        );
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to write dynamic dictionary value");
    }
}

void zeus_value_array_push(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* value
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array push output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedValue = boxFromValue(value);
        writeValueFromBox(
            out,
            makeBox(ZeusContainerOps::arrayPush(boxedArray->v, boxedValue->v)));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to append dynamic array value");
    }
}

void zeus_value_array_pop(ZeusValue* out, const ZeusValue* array) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array pop output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        ZeusType::UniversalVariant removed =
            ZeusContainerOps::arrayPop(boxedArray->v, true);
        DeferredContainerEdgeRelease releaseEdge{ &removed };
        writeValueFromBox(out, makeBox(removed));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to remove dynamic array value");
    }
}

void zeus_value_array_get(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* index
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array get output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedIndex = boxFromValue(index);
        writeValueFromBox(
            out,
            makeBox(ZeusContainerOps::arrayGet(boxedArray->v, boxedIndex->v)));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to read dynamic array value");
    }
}

void zeus_value_array_set(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* index,
    const ZeusValue* value
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array set output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedIndex = boxFromValue(index);
        OwnedZeusBox boxedValue = boxFromValue(value);
        writeValueFromBox(
            out,
            makeBox(ZeusContainerOps::arraySet(
                boxedArray->v, boxedIndex->v, boxedValue->v)));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to write dynamic array value");
    }
}

void zeus_value_array_clear(ZeusValue* out, const ZeusValue* array) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array clear output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        writeValueFromBox(out, makeBox(ZeusContainerOps::arrayClear(boxedArray->v)));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to clear dynamic array");
    }
}

void zeus_value_array_slice(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* start,
    const ZeusValue* end
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null array slice output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedArray = boxFromValue(array);
        OwnedZeusBox boxedStart = boxFromValue(start);
        OwnedZeusBox boxedEnd = boxFromValue(end);
        writeValueFromBox(
            out,
            makeBox(ZeusContainerOps::arraySlice(
                boxedArray->v, boxedStart->v, boxedEnd->v)));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to slice dynamic array");
    }
}

void zeus_value_dict_delete(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* key
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null dictionary delete output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedDict = boxFromValue(dict);
        OwnedZeusBox boxedKey = boxFromValue(key);
        writeValueFromBox(
            out,
            [&]() {
                ZeusType::UniversalVariant removed =
                    ZeusContainerOps::dictDelete(
                        boxedDict->v, boxedKey->v, true);
                DeferredContainerEdgeRelease releaseEdge{ &removed };
                return makeBox(removed);
            }());
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to delete dynamic dictionary value");
    }
}

void zeus_value_dict_clone(ZeusValue* out, const ZeusValue* dict) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) valueContainerPanic("null dictionary clone output");
    zeus_value_init_nil(out);
    try {
        OwnedZeusBox boxedDict = boxFromValue(dict);
        writeValueFromBox(
            out, makeBox(ZeusContainerOps::dictClone(boxedDict->v)));
    } catch (const std::exception&) {
        throw;
    } catch (...) {
        valueContainerPanic("failed to clone dynamic dictionary");
    }
}
