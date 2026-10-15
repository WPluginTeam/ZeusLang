#include "ZeusObjectLifetime.hpp"
#include "ZeusLValueABI.hpp"

#include "ZeusABI.hpp"
#include "ZeusDynamicMemberABI.hpp"
#include "ZeusGC.hpp"
#include "ZeusMemoryABI.hpp"
#include "ZeusRuntimeDebug.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct ObjectRecord {
    void* object = nullptr;
    ZeusClassDestroyCallback destroy = nullptr;
    std::string typeName;
    void* allocationOwner = nullptr;
    uint64_t references = 1;
    uint64_t graphReferences = 0;
    uint64_t leases = 0;
    bool ownsStorage = true;
    bool embedded = false;
    bool embeddedInitialized = true;
    bool embeddedOwnerReleased = false;
    bool live = true;
    bool destroying = false;
};

struct EmbeddedSlotRecord {
    void* allocationOwner = nullptr;
    uint64_t storageSize = 0;
    std::string typeName;
    ZeusClassDestroyCallback destroy = nullptr;
    void* currentObject = nullptr;
};

std::mutex& recordsMutex() {
    // Allocation release can inspect these records from late static-cell
    // destructors, so the registry must outlive the heap's static teardown.
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::unordered_map<void*, std::shared_ptr<ObjectRecord>>& records() {
    static auto* values = new std::unordered_map<void*, std::shared_ptr<ObjectRecord>>();
    return *values;
}

std::unordered_map<void*, EmbeddedSlotRecord>& embeddedSlots() {
    static auto* values = new std::unordered_map<void*, EmbeddedSlotRecord>();
    return *values;
}

void validateEmbeddedSlot(
    const EmbeddedSlotRecord& record,
    void* allocationOwner,
    const char* typeName,
    uint64_t storageSize
) {
    if (record.allocationOwner != allocationOwner ||
        record.storageSize != storageSize || record.typeName != typeName) {
        throw std::runtime_error(
            "embedded class slot is occupied by an incompatible object");
    }
}

void finishDestroy(const std::shared_ptr<ObjectRecord>& record) {
    std::exception_ptr failure;
    try {
        if (record->destroy &&
            (!record->embedded || record->embeddedInitialized)) {
            record->destroy(record->object);
        }
    } catch (...) {
        failure = std::current_exception();
    }

    try {
        zeus_value_dynamic_field_object_destroy(record->object);
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }

    zeus_lvalue_unregister(record->object);
    zeus_class_pointer_invalidate(record->object);

    bool freeObject = false;
    bool ownsStorage = false;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        record->live = false;
        record->destroying = false;
        if (record->references == 0 && record->leases == 0) {
            records().erase(record->object);
            freeObject = true;
            ownsStorage = record->ownsStorage;
        }
    }

    if (freeObject && ownsStorage) {
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::ClassObject, record->object);
        std::free(record->object);
    } else if (freeObject) {
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::ClassObject, record->object);
    }
    if (failure) std::rethrow_exception(failure);
}

void releaseEmbeddedOwner(void* object) {
    if (!object) return;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found != records().end() && found->second->embedded) {
            if (found->second->embeddedOwnerReleased) return;
            found->second->embeddedOwnerReleased = true;
        }
    }
    zeus_class_ref_release(object);
}

} // namespace

extern "C" void zeus_class_ref_register(
    void* object,
    ZeusClassDestroyCallback destroy,
    const char* debug_type
) noexcept {
    if (!object || !destroy) return;
    try {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found != records().end()) return;
        auto record = std::make_shared<ObjectRecord>();
        record->object = object;
        record->destroy = destroy;
        record->typeName = debug_type && *debug_type ? debug_type : "class-instance";
        const auto inserted = records().emplace(object, std::move(record)).second;
        if (inserted) {
            ZeusRuntimeDebug::trackCreated(
                ZeusRuntimeDebug::ResourceKind::ClassObject,
                object,
                debug_type && *debug_type ? debug_type : "class-instance",
                __FILE__,
                __LINE__
            );
        }
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("registering class owner", debug_type);
    } catch (...) {
        std::abort();
    }
}

extern "C" void zeus_class_ref_register_embedded(
    void* object,
    ZeusClassDestroyCallback destroy,
    const char* type_name,
    uint64_t storage_size
) noexcept(false) {
    try {
        if (!object || !destroy || !type_name || !*type_name || storage_size == 0) {
            throw std::runtime_error("invalid embedded class registration");
        }
        void* owner = zeus_memory_find_allocation(object, storage_size);
        if (!owner) {
            throw std::runtime_error(
                "embedded class payload must fit inside a live Zeus malloc allocation");
        }

        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found != records().end()) {
            const auto& existing = found->second;
            if (existing->embedded && existing->live &&
                existing->typeName == type_name &&
                existing->allocationOwner == owner) {
                return;
            }
            throw std::runtime_error(
                "embedded class storage address is already owned by another object");
        }

        auto record = std::make_shared<ObjectRecord>();
        record->object = object;
        record->destroy = destroy;
        record->typeName = type_name;
        record->allocationOwner = owner;
        record->ownsStorage = false;
        record->embedded = true;
        const auto inserted = records().emplace(object, record).second;
        if (!inserted) {
            throw std::runtime_error("failed to register embedded class storage");
        }
        ZeusRuntimeDebug::trackCreated(
            ZeusRuntimeDebug::ResourceKind::ClassObject,
            object,
            type_name,
            __FILE__,
            __LINE__
        );
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("registering embedded class owner", type_name);
    }
}

extern "C" int32_t zeus_class_ref_embedded_slot_state(
    void* slot,
    const char* type_name,
    uint64_t storage_size
) noexcept(false) {
    if (!slot || !type_name || !*type_name || storage_size == 0) {
        throw std::runtime_error("invalid embedded class slot query");
    }
    void* owner = zeus_memory_find_allocation(slot, storage_size);
    if (!owner) {
        throw std::runtime_error(
            "embedded class slot is outside a live Zeus malloc allocation");
    }

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = embeddedSlots().find(slot);
    if (found == embeddedSlots().end()) return 0;
    validateEmbeddedSlot(found->second, owner, type_name, storage_size);
    if (!found->second.currentObject) return 2;
    const auto objectFound = records().find(found->second.currentObject);
    if (objectFound == records().end()) {
        throw std::runtime_error("embedded class slot lost its payload record");
    }
    const auto& objectRecord = objectFound->second;
    return objectRecord->live && !objectRecord->destroying &&
        objectRecord->embeddedInitialized ? 1 : 2;
}

extern "C" void* zeus_class_ref_embedded_slot_object(
    void* slot,
    const char* type_name,
    uint64_t storage_size
) noexcept(false) {
    if (!slot || !type_name || !*type_name || storage_size == 0) {
        throw std::runtime_error("invalid embedded class slot lookup");
    }
    void* owner = zeus_memory_find_allocation(slot, storage_size);
    if (!owner) {
        throw std::runtime_error(
            "embedded class slot is outside a live Zeus malloc allocation");
    }

    void* object = nullptr;
    {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = embeddedSlots().find(slot);
        if (found == embeddedSlots().end()) {
            throw std::runtime_error("read from an empty embedded class slot");
        }
        validateEmbeddedSlot(found->second, owner, type_name, storage_size);
        object = found->second.currentObject;
    }
    if (!object) {
        throw std::runtime_error("read from an empty embedded class slot");
    }
    zeus_class_ref_require_live(object);
    return object;
}

extern "C" void zeus_class_ref_embedded_slot_assign(
    void* slot,
    void* value,
    ZeusClassDestroyCallback destroy,
    const char* type_name,
    uint64_t storage_size
) noexcept(false) {
    try {
        if (!slot || !destroy || !type_name || !*type_name || storage_size == 0) {
            throw std::runtime_error("invalid embedded class slot assignment");
        }
        void* owner = zeus_memory_find_allocation(slot, storage_size);
        if (!owner) {
            throw std::runtime_error(
                "embedded class slot is outside a live Zeus malloc allocation");
        }

        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        void* previous = nullptr;
        {
            std::lock_guard<std::mutex> lock(recordsMutex());
            auto slotFound = embeddedSlots().find(slot);
            if (slotFound != embeddedSlots().end()) {
                validateEmbeddedSlot(slotFound->second, owner, type_name, storage_size);
                if (slotFound->second.destroy != destroy) {
                    throw std::runtime_error(
                        "embedded class slot was registered with a different finalizer");
                }
                previous = slotFound->second.currentObject;
                if (previous == value) return;
            } else if (!value) {
                return;
            }

            auto valueFound = records().end();
            if (value) {
                valueFound = records().find(value);
                if (valueFound == records().end()) {
                    throw std::runtime_error(
                        "class slot assignment requires a managed class value");
                }
                const auto& record = valueFound->second;
                if (record->typeName != type_name) {
                    throw std::runtime_error(
                        "class slot assignment type does not match its managed value");
                }
                if (!record->live || record->destroying ||
                    (record->embedded && !record->embeddedInitialized)) {
                    throw std::runtime_error("use of deleted class object");
                }
            }

            if (slotFound == embeddedSlots().end()) {
                slotFound = embeddedSlots().emplace(
                    slot,
                    EmbeddedSlotRecord{owner, storage_size, type_name, destroy, nullptr}
                ).first;
            }
            if (value) ++valueFound->second->references;
            slotFound->second.currentObject = value;
        }

        if (previous) releaseEmbeddedOwner(previous);
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("registering embedded class slot", type_name);
    }
}

extern "C" void* zeus_class_ref_embedded_slot_create(
    void* slot,
    ZeusClassDestroyCallback destroy,
    const char* type_name,
    uint64_t storage_size
) noexcept(false) {
    try {
        if (!slot || !destroy || !type_name || !*type_name || storage_size == 0) {
            throw std::runtime_error("invalid embedded class slot creation");
        }
        void* owner = zeus_memory_find_allocation(slot, storage_size);
        if (!owner) {
            throw std::runtime_error(
                "embedded class slot is outside a live Zeus malloc allocation");
        }

        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        void* abandonedObject = nullptr;
        {
            std::lock_guard<std::mutex> lock(recordsMutex());
            auto slotFound = embeddedSlots().find(slot);
            if (slotFound == embeddedSlots().end()) {
                slotFound = embeddedSlots().emplace(
                    slot,
                    EmbeddedSlotRecord{owner, storage_size, type_name, destroy, nullptr}
                ).first;
            } else {
                validateEmbeddedSlot(slotFound->second, owner, type_name, storage_size);
                if (slotFound->second.destroy != destroy) {
                    throw std::runtime_error(
                        "embedded class slot was registered with a different finalizer");
                }
                abandonedObject = slotFound->second.currentObject;
                if (abandonedObject) {
                    const auto objectFound = records().find(abandonedObject);
                    if (objectFound == records().end()) {
                        throw std::runtime_error(
                            "embedded class slot lost its payload record");
                    }
                    const auto& objectRecord = objectFound->second;
                    if (objectRecord->live && !objectRecord->destroying &&
                        objectRecord->embeddedInitialized) {
                        throw std::runtime_error(
                            "cannot create an object in an active embedded class slot");
                    }
                    slotFound->second.currentObject = nullptr;
                }
            }
        }
        if (abandonedObject) releaseEmbeddedOwner(abandonedObject);

        std::lock_guard<std::mutex> lock(recordsMutex());
        auto slotFound = embeddedSlots().find(slot);
        if (slotFound == embeddedSlots().end() || slotFound->second.currentObject) {
            throw std::runtime_error("embedded class slot changed during creation");
        }

        void* object = std::calloc(1, static_cast<std::size_t>(storage_size));
        if (!object) throw std::bad_alloc();

        std::shared_ptr<ObjectRecord> record;
        try {
            record = std::make_shared<ObjectRecord>();
            record->object = object;
            record->destroy = destroy;
            record->typeName = type_name;
            record->allocationOwner = owner;
            record->ownsStorage = true;
            record->embedded = true;
            record->embeddedInitialized = false;
            const auto inserted = records().emplace(object, record).second;
            if (!inserted) {
                std::free(object);
                throw std::runtime_error(
                    "new embedded class slot payload address is already registered");
            }
        } catch (...) {
            if (!record || records().find(object) == records().end()) {
                std::free(object);
            }
            throw;
        }

        slotFound->second.currentObject = object;
        ZeusRuntimeDebug::trackCreated(
            ZeusRuntimeDebug::ResourceKind::ClassObject,
            object,
            type_name,
            __FILE__,
            __LINE__
        );
        return object;
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("creating embedded class slot object", type_name);
    }
}

extern "C" void zeus_class_ref_embedded_slot_clear(
    void* slot,
    const char* type_name,
    uint64_t storage_size
) noexcept(false) {
    if (!slot || !type_name || !*type_name || storage_size == 0) {
        throw std::runtime_error("invalid embedded class slot clear");
    }
    void* owner = zeus_memory_find_allocation(slot, storage_size);
    if (!owner) {
        throw std::runtime_error(
            "embedded class slot is outside a live Zeus malloc allocation");
    }

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    void* object = nullptr;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = embeddedSlots().find(slot);
        if (found == embeddedSlots().end()) return;
        validateEmbeddedSlot(found->second, owner, type_name, storage_size);
        object = found->second.currentObject;
        found->second.currentObject = nullptr;
    }
    if (!object) return;
    releaseEmbeddedOwner(object);
}

extern "C" void zeus_class_ref_embedded_set_initialized(
    void* object,
    int32_t initialized
) noexcept(false) {
    if (!object) throw std::runtime_error("null embedded class slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found == records().end() || !found->second->embedded ||
        !found->second->live) {
        throw std::runtime_error("embedded class slot is not live");
    }
    found->second->embeddedInitialized = initialized != 0;
}

extern "C" void zeus_class_ref_require_embedded_live(
    void* object,
    const char* type_name,
    uint64_t storage_size
) noexcept(false) {
    if (zeus_class_ref_embedded_slot_state(object, type_name, storage_size) != 1) {
        throw std::runtime_error("read from an empty or inactive embedded class slot");
    }
}

extern "C" void zeus_class_ref_destroy_embedded_in_allocation(
    void* allocation
) noexcept(false) {
    if (!allocation) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::vector<void*> objects;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        for (auto slot = embeddedSlots().begin();
             slot != embeddedSlots().end();) {
            if (slot->second.allocationOwner == allocation) {
                if (slot->second.currentObject) {
                    objects.push_back(slot->second.currentObject);
                }
                slot = embeddedSlots().erase(slot);
            } else {
                ++slot;
            }
        }
        for (const auto& entry : records()) {
            const auto& record = entry.second;
            if (record->embedded && record->allocationOwner == allocation &&
                !record->embeddedOwnerReleased) {
                if (std::find(objects.begin(), objects.end(), record->object) ==
                    objects.end()) {
                    objects.push_back(record->object);
                }
            }
        }
    }

    std::exception_ptr failure;
    for (void* object : objects) {
        try {
            releaseEmbeddedOwner(object);
        } catch (...) {
            if (!failure) failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
}

extern "C" int32_t zeus_class_ref_try_retain(void* object) noexcept {
    if (!object) return 0;
    try {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found == records().end() || !found->second->live ||
            (found->second->embedded &&
             !found->second->embeddedInitialized)) return 0;
        ++found->second->leases;
        return 1;
    } catch (...) {
        std::abort();
    }
}

extern "C" void zeus_class_ref_release_lease(void* object) noexcept {
    if (!object) return;
    try {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::shared_ptr<ObjectRecord> record;
        bool freeObject = false;
        bool ownsStorage = false;
        {
            std::lock_guard<std::mutex> lock(recordsMutex());
            const auto found = records().find(object);
            if (found == records().end()) return;
            record = found->second;
            if (record->leases == 0) return;
            --record->leases;
            if (record->leases == 0 && record->references == 0 && !record->live &&
                !record->destroying) {
                records().erase(found);
                freeObject = true;
                ownsStorage = record->ownsStorage;
            }
        }
        if (freeObject) {
            ZeusRuntimeDebug::trackDestroyed(
                ZeusRuntimeDebug::ResourceKind::ClassObject, record->object);
            if (ownsStorage) std::free(record->object);
        }
    } catch (...) {
        std::abort();
    }
}

extern "C" uint64_t zeus_class_ref_external_reference_count(void* object) noexcept {
    if (!object) return 0;
    try {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found == records().end() || !found->second->live ||
            found->second->destroying ||
            (found->second->embedded &&
             !found->second->embeddedInitialized)) return 0;
        return found->second->references - found->second->graphReferences;
    } catch (...) {
        return 0;
    }
}

extern "C" void zeus_class_ref_retain(void* object) noexcept(false) {
    if (!object) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found == records().end()) return;
    if ((!found->second->live && !found->second->destroying) ||
        (found->second->embedded && !found->second->embeddedInitialized)) {
        throw std::runtime_error("use of deleted class object");
    }
    ++found->second->references;
}

extern "C" void zeus_class_ref_require_managed_live(void* object) noexcept(false) {
    if (!object) throw std::runtime_error("class result is null");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found == records().end()) {
        throw std::runtime_error(
            "class result is not registered with the Zeus lifetime manager");
    }
    const auto& record = *found->second;
    if (!record.live || record.destroying ||
        (record.embedded && !record.embeddedInitialized)) {
        throw std::runtime_error("use of deleted class object");
    }
}

extern "C" void zeus_class_ref_retain_owned(void* object) noexcept(false) {
    if (!object) throw std::runtime_error("owning class result is null");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found == records().end()) {
        throw std::runtime_error(
            "owning class result is not registered with the Zeus lifetime manager");
    }
    const auto& record = *found->second;
    if (!record.live || record.destroying ||
        (record.embedded && !record.embeddedInitialized)) {
        throw std::runtime_error("use of deleted class object");
    }
    ++found->second->references;
}

extern "C" void zeus_class_ref_release(void* object) noexcept(false) {
    if (!object) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::shared_ptr<ObjectRecord> record;
    bool shouldDestroy = false;
    bool freeObject = false;
    bool ownsStorage = false;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found == records().end()) return;
        record = found->second;
        if (record->references == 0) {
            throw std::runtime_error("class reference count underflow");
        }
        --record->references;
        if (record->references != 0 || record->destroying) return;
        if (record->live) {
            record->destroying = true;
            shouldDestroy = true;
        } else {
            if (record->leases != 0) return;
            records().erase(found);
            freeObject = true;
            ownsStorage = record->ownsStorage;
        }
    }

    if (shouldDestroy) {
        finishDestroy(record);
    } else if (freeObject) {
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::ClassObject, record->object);
        if (ownsStorage) std::free(record->object);
    }
}

extern "C" void zeus_class_ref_retain_edge(void* object) noexcept(false) {
    if (!object) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found == records().end()) return;
    if ((!found->second->live && !found->second->destroying) ||
        (found->second->embedded && !found->second->embeddedInitialized)) {
        throw std::runtime_error("use of deleted class object");
    }
    ++found->second->references;
    ++found->second->graphReferences;
}

extern "C" void zeus_class_ref_release_edge(void* object) noexcept(false) {
    if (!object) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::shared_ptr<ObjectRecord> record;
    bool shouldDestroy = false;
    bool freeObject = false;
    bool ownsStorage = false;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found == records().end()) return;
        record = found->second;
        if (record->references == 0 || record->graphReferences == 0) {
            throw std::runtime_error("class graph reference count underflow");
        }
        --record->references;
        --record->graphReferences;
        if (record->references != 0 || record->destroying) return;
        if (record->live) {
            record->destroying = true;
            shouldDestroy = true;
        } else {
            if (record->leases != 0) return;
            records().erase(found);
            freeObject = true;
            ownsStorage = record->ownsStorage;
        }
    }

    if (shouldDestroy) {
        finishDestroy(record);
    } else if (freeObject) {
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::ClassObject, record->object);
        if (ownsStorage) std::free(record->object);
    }
}

extern "C" int32_t zeus_class_ref_destroy_now(void* object) noexcept(false) {
    if (!object) return 1;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::shared_ptr<ObjectRecord> record;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found == records().end()) return 0;
        record = found->second;
        if (!record->live || record->destroying) return 1;
        record->destroying = true;
    }
    finishDestroy(record);
    return 1;
}

extern "C" void zeus_class_ref_require_live(void* object) noexcept(false) {
    if (!object) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found != records().end() &&
        ((!found->second->live && !found->second->destroying) ||
         (found->second->embedded &&
          !found->second->embeddedInitialized))) {
        throw std::runtime_error("use of deleted class object");
    }
}

namespace {
struct ClassCopyContext {
    std::unordered_map<void*, void*> objects;
    std::vector<void*> roots;
    std::vector<void*> allocated;
};
std::mutex& classCopyRegistryMutex() {
    static std::mutex value;
    return value;
}
std::unordered_map<std::string, ZeusClassCopyCallback>& classCopyDefaultRegistry() {
    static std::unordered_map<std::string, ZeusClassCopyCallback> value;
    return value;
}
std::unordered_map<std::string, ZeusClassCopyCallback>& classCopyRegistry() {
    static std::unordered_map<std::string, ZeusClassCopyCallback> value;
    return value;
}
}
extern "C" void* zeus_class_copy_begin() noexcept(false) {
    return new ClassCopyContext();
}
extern "C" void* zeus_class_copy_find(void* context, void* source) noexcept(false) {
    if (!context || !source) return nullptr;
    const auto& objects = static_cast<ClassCopyContext*>(context)->objects;
    const auto found = objects.find(source);
    return found == objects.end() ? nullptr : found->second;
}
extern "C" void zeus_class_copy_remember(void* context, void* source, void* target) noexcept(false) {
    if (!context || !source || !target) throw std::runtime_error("invalid class copy graph entry");
    auto& objects = static_cast<ClassCopyContext*>(context)->objects;
    const auto inserted = objects.emplace(source, target);
    if (!inserted.second && inserted.first->second != target)
        throw std::runtime_error("class copy source already mapped to another object");
    if (inserted.second) {
        try { static_cast<ClassCopyContext*>(context)->roots.push_back(target); }
        catch (...) { objects.erase(source); throw; }
        zeus_class_ref_retain(target);
    }
}
extern "C" void zeus_class_copy_end(void* context) noexcept(false) {
    auto* transaction = static_cast<ClassCopyContext*>(context);
    if (!transaction) return;
    for (void* root : transaction->roots) zeus_class_ref_release(root);
    delete transaction;
}
extern "C" void zeus_class_copy_register(const char* type_name, ZeusClassCopyCallback callback) noexcept(false) {
    if (!type_name || !*type_name || !callback) throw std::runtime_error("invalid class copy registration");
    std::lock_guard<std::mutex> lock(classCopyRegistryMutex());
    classCopyRegistry()[type_name] = callback;
}
extern "C" void* zeus_class_copy_object(void* source, void* context) noexcept(false) {
    if (!source) return nullptr;
    zeus_class_ref_require_live(source);
    if (void* existing = zeus_class_copy_find(context, source)) {
        zeus_class_ref_retain(existing);
        return existing;
    }
    std::string typeName;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(source);
        if (found == records().end()) throw std::runtime_error("class copy source is not a registered object");
        typeName = found->second->typeName;
    }
    ZeusClassCopyCallback callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(classCopyRegistryMutex());
        const auto found = classCopyRegistry().find(typeName);
        if (found != classCopyRegistry().end()) callback = found->second;
    }
    if (!callback) throw std::runtime_error("class copy callback is missing for '" + typeName + "'");
    try { return callback(source, context); }
    catch (...) {
        if (context) {
            auto* transaction = static_cast<ClassCopyContext*>(context);
            void* partial = zeus_class_copy_find(context, source);
            if (partial && std::find(transaction->allocated.begin(), transaction->allocated.end(), partial) != transaction->allocated.end()) {
                zeus_class_ref_release(partial);
            }
        }
        throw;
    }
}

extern "C" void zeus_class_copy_register_default(const char* type_name, ZeusClassCopyCallback callback) noexcept(false) {
    if (!type_name || !*type_name || !callback) throw std::runtime_error("invalid default class copy registration");
    std::lock_guard<std::mutex> lock(classCopyRegistryMutex());
    classCopyDefaultRegistry()[type_name] = callback;
}
extern "C" void* zeus_class_copy_default_object(void* source, void* context) noexcept(false) {
    if (!source) return nullptr;
    zeus_class_ref_require_live(source);
    if (void* existing = zeus_class_copy_find(context, source)) {
        zeus_class_ref_retain(existing);
        return existing;
    }
    std::string typeName;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(source);
        if (found == records().end()) throw std::runtime_error("default class copy source is not registered");
        typeName = found->second->typeName;
    }
    ZeusClassCopyCallback callback = nullptr;
    {
        std::lock_guard<std::mutex> lock(classCopyRegistryMutex());
        const auto found = classCopyDefaultRegistry().find(typeName);
        if (found != classCopyDefaultRegistry().end()) callback = found->second;
    }
    if (!callback) throw std::runtime_error("default class copy callback missing for '" + typeName + "'");
    try { return callback(source, context); }
    catch (...) {
        if (context) {
            auto* transaction = static_cast<ClassCopyContext*>(context);
            void* partial = zeus_class_copy_find(context, source);
            if (partial && std::find(transaction->allocated.begin(), transaction->allocated.end(), partial) != transaction->allocated.end()) {
                zeus_class_ref_release(partial);
            }
        }
        throw;
    }
}

extern "C" void zeus_class_copy_remember_allocated(void* context, void* source, void* target) noexcept(false) {
    zeus_class_copy_remember(context, source, target);
    static_cast<ClassCopyContext*>(context)->allocated.push_back(target);
}
namespace {
void rollbackClassCopy(ClassCopyContext* context) noexcept {
    // Explicitly destroy partial graphs before their transaction roots release.
    for (void* object : context->allocated) {
        try { zeus_class_ref_destroy_now(object); } catch (...) {}
    }
}
}
extern "C" void* zeus_class_copy_owned(void* source, int32_t skip_hook) noexcept(false) {
    auto* context = static_cast<ClassCopyContext*>(zeus_class_copy_begin());
    void* result = nullptr;
    try {
        result = skip_hook ? zeus_class_copy_default_object(source, context)
                           : zeus_class_copy_object(source, context);
    } catch (...) {
        rollbackClassCopy(context);
        zeus_class_copy_end(context);
        throw;
    }
    zeus_class_copy_end(context);
    return result;
}
