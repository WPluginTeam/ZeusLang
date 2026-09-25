// ZeusRuntimeLib/ZeusDynamicABI.cpp

#include "ZeusDynamicABI.hpp"
#include "ZeusABIInternal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

ZeusType::UniversalVariant nullVariant() {
    return ZeusType::UniversalVariant(ZeusType::Variant(ZeusType::Null()));
}

ZeusBox* makeBox(const ZeusType::UniversalVariant& value) {
    ZeusBox* box = new ZeusBox();
    box->v = value;
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

    return "<obj>";
}

void consume(ZeusBox* box) {
    delete box;
}

struct ZeusDynamicIteratorImpl {
    enum class Kind { Array, Dict };

    Kind kind;
    size_t index = 0;
    std::shared_ptr<ZeusType::ArrayData> array;
    std::shared_ptr<ZeusType::ObjectData> dict;
};

} // namespace

struct ZeusDynamicIterator {
    ZeusDynamicIteratorImpl impl;
};

ZeusBox* zeus_array_build(int64_t count, ZeusBox* const* values) {
    if (count < 0 || (count > 0 && !values)) return makeNullBox();

    ZeusType::Array array;
    array.impl = std::make_shared<ZeusType::ArrayData>();
    array.impl->data.reserve(static_cast<size_t>(count));

    for (int64_t i = 0; i < count; ++i) {
        ZeusBox* value = values[i];
        array.impl->data.push_back(value ? value->v : nullVariant());
        consume(value);
    }

    return makeBox(ZeusType::UniversalVariant(ZeusType::Variant(array)));
}

ZeusBox* zeus_dict_build(int64_t pairCount, ZeusBox* const* keyValues) {
    if (pairCount < 0 || (pairCount > 0 && !keyValues)) return makeNullBox();

    ZeusType::Object object;
    object.impl = std::make_shared<ZeusType::ObjectData>();

    for (int64_t i = 0; i < pairCount; ++i) {
        ZeusBox* key = keyValues[i * 2];
        ZeusBox* value = keyValues[i * 2 + 1];
        const std::string name = key ? keyString(key->v) : "<obj>";

        if (object.impl->data.find(name) == object.impl->data.end()) {
            object.impl->order.push_back(name);
        }
        object.impl->data[name] = value ? value->v : nullVariant();
        consume(key);
        consume(value);
    }

    return makeBox(ZeusType::UniversalVariant(ZeusType::Variant(object)));
}

int64_t zeus_array_length(const ZeusBox* array) {
    const ZeusType::Array* value = getArray(array);
    if (!value || !value->impl) return 0;
    return static_cast<int64_t>(value->impl->data.size());
}

ZeusBox* zeus_array_load(const ZeusBox* array, int64_t index) {
    const ZeusType::Array* value = getArray(array);
    if (!value || !value->impl || index < 0 ||
        static_cast<size_t>(index) >= value->impl->data.size()) {
        return makeNullBox();
    }

    return makeBox(value->impl->data[static_cast<size_t>(index)]);
}

ZeusBox* zeus_dict_load(const ZeusBox* dict, const ZeusBox* key) {
    const ZeusType::Object* object = getDict(dict);
    if (!object || !object->impl || !key) return makeNullBox();

    const std::string name = keyString(key->v);
    auto it = object->impl->data.find(name);
    if (it == object->impl->data.end()) return makeNullBox();
    return makeBox(it->second);
}

ZeusBox* zeus_dict_keys(const ZeusBox* dict) {
    const ZeusType::Object* object = getDict(dict);
    ZeusType::Array array;
    array.impl = std::make_shared<ZeusType::ArrayData>();

    if (object && object->impl) {
        array.impl->data.reserve(object->impl->order.size());
        for (const auto& key : object->impl->order) {
            array.impl->data.push_back(LexerType::Value(key));
        }
    }

    return makeBox(ZeusType::UniversalVariant(ZeusType::Variant(array)));
}

ZeusBox* zeus_array_store(ZeusBox* array, int64_t index, ZeusBox* value) {
    ZeusType::Array* target = getArray(array);
    ZeusType::UniversalVariant stored = value ? value->v : nullVariant();

    if (target && target->impl && index >= 0 &&
        static_cast<size_t>(index) < target->impl->data.size()) {
        target->impl->data[static_cast<size_t>(index)] = stored;
    }

    consume(value);
    return makeBox(stored);
}

ZeusBox* zeus_dict_store(ZeusBox* dict, ZeusBox* key, ZeusBox* value) {
    ZeusType::Object* target = getDict(dict);
    ZeusType::UniversalVariant stored = value ? value->v : nullVariant();

    if (target && target->impl && key) {
        const std::string name = keyString(key->v);
        if (target->impl->data.find(name) == target->impl->data.end()) {
            target->impl->order.push_back(name);
        }
        target->impl->data[name] = stored;
    }

    consume(key);
    consume(value);
    return makeBox(stored);
}

ZeusDynamicIterator* zeus_array_iter_begin(const ZeusBox* array) {
    const ZeusType::Array* value = getArray(array);
    if (!value || !value->impl) return nullptr;

    auto* iterator = new ZeusDynamicIterator();
    iterator->impl.kind = ZeusDynamicIteratorImpl::Kind::Array;
    iterator->impl.array = value->impl;
    return iterator;
}

int32_t zeus_array_iter_next(ZeusDynamicIterator* iterator, ZeusBox** value) {
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
    const ZeusType::Object* value = getDict(dict);
    if (!value || !value->impl) return nullptr;

    auto* iterator = new ZeusDynamicIterator();
    iterator->impl.kind = ZeusDynamicIteratorImpl::Kind::Dict;
    iterator->impl.dict = value->impl;
    return iterator;
}

int32_t zeus_dict_iter_next(
    ZeusDynamicIterator* iterator,
    ZeusBox** key,
    ZeusBox** value
) {
    if (!iterator || !key || !value ||
        iterator->impl.kind != ZeusDynamicIteratorImpl::Kind::Dict ||
        !iterator->impl.dict ||
        iterator->impl.index >= iterator->impl.dict->order.size()) {
        return 0;
    }

    const std::string& name = iterator->impl.dict->order[iterator->impl.index++];
    auto found = iterator->impl.dict->data.find(name);
    if (found == iterator->impl.dict->data.end()) return 0;

    *key = makeBox(LexerType::Value(name));
    *value = makeBox(found->second);
    return 1;
}

void zeus_dynamic_iter_release(ZeusDynamicIterator* iterator) {
    delete iterator;
}
