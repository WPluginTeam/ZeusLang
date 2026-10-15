#pragma once

#include "ZeusValueSlot.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ZeusType {

// The VM object table keeps handles and lifetime state. Declared values live
// separately in this malloc-backed, contiguous array of the shared 16-byte
// ZeusValue ABI slots.
class DeclaredFieldBuffer final {
public:
    static std::shared_ptr<DeclaredFieldBuffer> create(std::size_t count) {
        if (count == 0) return {};
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(ZeusValueSlot)) {
            throw std::length_error("class field payload is too large");
        }

        void* memory = std::malloc(count * sizeof(ZeusValueSlot));
        if (!memory) throw std::bad_alloc();

        auto* slots = static_cast<ZeusValueSlot*>(memory);
        std::size_t initialized = 0;
        try {
            for (; initialized < count; ++initialized) {
                ::new (static_cast<void*>(slots + initialized)) ZeusValueSlot(true);
            }
        } catch (...) {
            while (initialized > 0) slots[--initialized].~ZeusValueSlot();
            std::free(memory);
            throw;
        }

        DeclaredFieldBuffer* buffer = nullptr;
        try {
            buffer = new DeclaredFieldBuffer(slots, count);
        } catch (...) {
            while (initialized > 0) slots[--initialized].~ZeusValueSlot();
            std::free(memory);
            throw;
        }
        // shared_ptr deletes buffer if allocating its control block fails.
        return std::shared_ptr<DeclaredFieldBuffer>(buffer);
    }

    DeclaredFieldBuffer(const DeclaredFieldBuffer&) = delete;
    DeclaredFieldBuffer& operator=(const DeclaredFieldBuffer&) = delete;

    ~DeclaredFieldBuffer() noexcept {
        for (std::size_t index = count_; index > 0; --index) {
            slots_[index - 1].~ZeusValueSlot();
        }
        std::free(slots_);
    }

    std::size_t size() const noexcept { return count_; }

    UniversalVariant read(std::size_t index) const {
        checkIndex(index);
        return slots_[index].read();
    }

    void write(std::size_t index, const UniversalVariant& value) {
        checkIndex(index);
        slots_[index].write(value);
    }

    std::shared_ptr<DeclaredFieldBuffer> clone() const {
        auto result = create(count_);
        for (std::size_t index = 0; index < count_; ++index) {
            result->write(index, read(index));
        }
        return result;
    }

private:
    DeclaredFieldBuffer(ZeusValueSlot* slots, std::size_t count) noexcept
        : slots_(slots), count_(count) {}

    void checkIndex(std::size_t index) const {
        if (index >= count_) throw std::out_of_range("class field index out of range");
    }

    ZeusValueSlot* slots_ = nullptr;
    std::size_t count_ = 0;
};

inline void rebuildClassFieldIndices(ClassInfo& info) {
    std::vector<std::string> ordered;
    ordered.reserve(info.fields.size());

    for (const auto& name : info.fieldOrder) {
        if (info.fields.find(name) == info.fields.end() ||
            std::find(ordered.begin(), ordered.end(), name) != ordered.end()) {
            continue;
        }
        ordered.push_back(name);
    }

    std::vector<std::string> missing;
    for (const auto& field : info.fields) {
        if (std::find(ordered.begin(), ordered.end(), field.first) == ordered.end()) {
            missing.push_back(field.first);
        }
    }
    std::sort(missing.begin(), missing.end());
    ordered.insert(ordered.end(), missing.begin(), missing.end());

    info.fieldOrder = std::move(ordered);
    info.fieldIndices.clear();
    info.fieldIndices.reserve(info.fieldOrder.size());
    for (std::size_t index = 0; index < info.fieldOrder.size(); ++index) {
        info.fieldIndices.emplace(info.fieldOrder[index], index);
    }
}

inline void initializeDeclaredFields(ObjectInstance& object, const ClassInfo& info) {
    if (info.fieldOrder.empty()) {
        object.declaredFields.reset();
        return;
    }
    if (info.fieldIndices.size() != info.fieldOrder.size()) {
        throw std::logic_error("class field index metadata is not initialized");
    }
    if (object.declaredFields) {
        if (object.declaredFields->size() != info.fieldOrder.size()) {
            throw std::logic_error("class field payload does not match its class layout");
        }
        return;
    }

    auto payload = DeclaredFieldBuffer::create(info.fieldOrder.size());
    std::vector<std::string> transferred;
    transferred.reserve(info.fieldOrder.size());
    for (std::size_t index = 0; index < info.fieldOrder.size(); ++index) {
        const auto& name = info.fieldOrder[index];
        const auto value = object.extensionValues.find(name);
        if (value == object.extensionValues.end()) continue;
        payload->write(index, value->second);
        transferred.push_back(name);
    }

    object.declaredFields = std::move(payload);
    for (const auto& name : transferred) object.extensionValues.erase(name);
}

inline bool objectHasField(
    const ObjectInstance& object,
    const ClassInfo& info,
    const std::string& name
) {
    return info.fieldIndices.find(name) != info.fieldIndices.end() ||
        object.extensionValues.find(name) != object.extensionValues.end();
}

inline UniversalVariant readObjectField(
    const ObjectInstance& object,
    const ClassInfo& info,
    const std::string& name
) {
    const auto declared = info.fieldIndices.find(name);
    if (declared != info.fieldIndices.end()) {
        if (!object.declaredFields || declared->second >= object.declaredFields->size()) {
            throw std::logic_error("declared class field has no payload slot: " + name);
        }
        return object.declaredFields->read(declared->second);
    }

    const auto extension = object.extensionValues.find(name);
    if (extension == object.extensionValues.end()) {
        throw std::out_of_range("object field not found: " + name);
    }
    return extension->second;
}

inline void writeObjectField(
    ObjectInstance& object,
    const ClassInfo& info,
    const std::string& name,
    const UniversalVariant& value
) {
    const auto declared = info.fieldIndices.find(name);
    if (declared != info.fieldIndices.end()) {
        if (!object.declaredFields || declared->second >= object.declaredFields->size()) {
            throw std::logic_error("declared class field has no payload slot: " + name);
        }
        object.declaredFields->write(declared->second, value);
        return;
    }
    object.extensionValues[name] = value;
}

inline std::vector<std::pair<std::string, UniversalVariant>> snapshotObjectFields(
    const ObjectInstance& object,
    const ClassInfo& info
) {
    std::vector<std::pair<std::string, UniversalVariant>> fields;
    fields.reserve(info.fieldOrder.size() + object.extensionValues.size());
    for (const auto& name : info.fieldOrder) {
        if (!object.declaredFields) continue;
        const auto index = info.fieldIndices.find(name);
        if (index == info.fieldIndices.end()) continue;
        fields.emplace_back(name, object.declaredFields->read(index->second));
    }
    for (const auto& field : object.extensionValues) {
        if (info.fieldIndices.find(field.first) == info.fieldIndices.end()) {
            fields.push_back(field);
        }
    }
    return fields;
}

inline void clearObjectFieldStorage(ObjectInstance& object) noexcept {
    object.declaredFields.reset();
    object.extensionValues.clear();
    object.flags.clear();
}

} // namespace ZeusType
