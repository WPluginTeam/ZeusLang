#include "ZeusObjectLifetime.hpp"

#include "ZeusABI.hpp"

#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace {

struct ObjectRecord {
    void* object = nullptr;
    ZeusClassDestroyCallback destroy = nullptr;
    uint64_t references = 1;
    bool live = true;
    bool destroying = false;
};

std::mutex& recordsMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<void*, std::shared_ptr<ObjectRecord>>& records() {
    static std::unordered_map<void*, std::shared_ptr<ObjectRecord>> values;
    return values;
}

void finishDestroy(const std::shared_ptr<ObjectRecord>& record) {
    std::exception_ptr failure;
    try {
        if (record->destroy) record->destroy(record->object);
    } catch (...) {
        failure = std::current_exception();
    }

    zeus_class_pointer_invalidate(record->object);

    bool freeObject = false;
    {
        std::lock_guard<std::mutex> lock(recordsMutex());
        record->live = false;
        record->destroying = false;
        if (record->references == 0) {
            records().erase(record->object);
            freeObject = true;
        }
    }

    if (freeObject) std::free(record->object);
    if (failure) std::rethrow_exception(failure);
}

} // namespace

extern "C" void zeus_class_ref_register(
    void* object,
    ZeusClassDestroyCallback destroy
) noexcept {
    if (!object || !destroy) return;
    try {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found != records().end()) return;
        auto record = std::make_shared<ObjectRecord>();
        record->object = object;
        record->destroy = destroy;
        records().emplace(object, std::move(record));
    } catch (...) {
        std::abort();
    }
}

extern "C" int32_t zeus_class_ref_try_retain(void* object) noexcept {
    if (!object) return 0;
    try {
        std::lock_guard<std::mutex> lock(recordsMutex());
        const auto found = records().find(object);
        if (found == records().end() || !found->second->live) return 0;
        ++found->second->references;
        return 1;
    } catch (...) {
        std::abort();
    }
}

extern "C" void zeus_class_ref_retain(void* object) {
    if (!object) return;
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found == records().end()) return;
    if (!found->second->live && !found->second->destroying) {
        throw std::runtime_error("use of deleted class object");
    }
    ++found->second->references;
}

extern "C" void zeus_class_ref_release(void* object) {
    if (!object) return;
    std::shared_ptr<ObjectRecord> record;
    bool shouldDestroy = false;
    bool freeObject = false;
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
            records().erase(found);
            freeObject = true;
        }
    }

    if (shouldDestroy) {
        finishDestroy(record);
    } else if (freeObject) {
        std::free(record->object);
    }
}

extern "C" int32_t zeus_class_ref_destroy_now(void* object) {
    if (!object) return 1;
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

extern "C" void zeus_class_ref_require_live(void* object) {
    if (!object) return;
    std::lock_guard<std::mutex> lock(recordsMutex());
    const auto found = records().find(object);
    if (found != records().end() && !found->second->live &&
        !found->second->destroying) {
        throw std::runtime_error("use of deleted class object");
    }
}
