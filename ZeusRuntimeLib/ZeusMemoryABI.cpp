#include "ZeusMemoryABI.hpp"

#include "ZeusObjectLifetime.hpp"
#include "ZeusLValueABI.hpp"


#include <cstddef>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace {

struct AllocationRecord {
    std::size_t size = 0;
    bool freeing = false;
};

std::mutex& allocationMutex() {
    // Shared/static Zeus slots can release native buffers during process
    // teardown, after ordinary function-local statics would be destroyed.
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::unordered_map<void*, AllocationRecord>& allocations() {
    // Keep allocation metadata alive until the OS tears down the process so
    // late ZeusValueSlot destruction can still unregister and free buffers.
    static auto* records = new std::unordered_map<void*, AllocationRecord>();
    return *records;
}

} // namespace

extern "C" std::int64_t __zeus_malloc(std::int64_t size) noexcept {
    if (size < 0 || static_cast<std::uint64_t>(size) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return 0;
    }

    void* allocation = std::malloc(static_cast<std::size_t>(size));
    if (!allocation) return 0;
    try {
        std::lock_guard<std::mutex> lock(allocationMutex());
        allocations().emplace(
            allocation, AllocationRecord{static_cast<std::size_t>(size), false});
    } catch (...) {
        std::free(allocation);
        return 0;
    }
    return static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(allocation));
}

extern "C" std::int64_t __zeus_free(std::int64_t address) {
    void* allocation = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    if (!allocation) return 0;

    bool tracked = false;
    uint64_t allocationSize = 0;
    {
        std::lock_guard<std::mutex> lock(allocationMutex());
        const auto found = allocations().find(allocation);
        if (found != allocations().end()) {
            if (found->second.freeing) return 0;
            allocationSize = found->second.size;
            found->second.freeing = true;
            tracked = true;
        }
    }

    if (tracked) {
        try {
            zeus_class_ref_destroy_embedded_in_allocation(allocation);
        } catch (...) {
            std::lock_guard<std::mutex> lock(allocationMutex());
            const auto found = allocations().find(allocation);
            if (found != allocations().end()) found->second.freeing = false;
            throw;
        }
        std::lock_guard<std::mutex> lock(allocationMutex());
        allocations().erase(allocation);
        zeus_lvalue_unregister_span(allocation, allocationSize);
    }

    std::free(allocation);
    return 0;
}

extern "C" void* zeus_memory_find_allocation(
    void* address,
    std::uint64_t size
) noexcept {
    if (!address) return nullptr;
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    std::lock_guard<std::mutex> lock(allocationMutex());
    for (const auto& entry : allocations()) {
        if (entry.second.freeing) continue;
        const auto base = reinterpret_cast<std::uintptr_t>(entry.first);
        if (target < base) continue;
        const std::uintptr_t offset = target - base;
        if (offset > entry.second.size) continue;
        const std::uint64_t remaining = static_cast<std::uint64_t>(
            entry.second.size - static_cast<std::size_t>(offset));
        if (size <= remaining) return entry.first;
    }
    return nullptr;
}
