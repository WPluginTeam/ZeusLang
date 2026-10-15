#include "ZeusRuntimeDebug.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ZeusRuntimeDebug {
namespace {

constexpr std::size_t kindCount = static_cast<std::size_t>(ResourceKind::Count);

struct AtomicCounter {
    std::atomic<uint64_t> created{0};
    std::atomic<uint64_t> destroyed{0};
    std::atomic<uint64_t> live{0};
    std::atomic<uint64_t> peak{0};
    std::atomic<uint64_t> unmatchedDestroy{0};
};

std::array<AtomicCounter, kindCount>& counters() noexcept {
    static std::array<AtomicCounter, kindCount> value{};
    return value;
}

const char* kindName(ResourceKind kind) noexcept {
    switch (kind) {
    case ResourceKind::ZeusBox: return "ZeusBox(heap)";
    case ResourceKind::ManagedClosure: return "managed-closure-record";
    case ResourceKind::CaptureCell: return "capture-cell";
    case ResourceKind::ClassObject: return "native-class-object";
    case ResourceKind::ThreadRecord: return "runtime-thread-record";
    case ResourceKind::SocketRecord: return "runtime-socket-record";
    case ResourceKind::DynamicFieldOwner: return "dynamic-field-owner";
    case ResourceKind::Count: break;
    }
    return "unknown";
}

bool validKind(ResourceKind kind) noexcept {
    return static_cast<std::size_t>(kind) < kindCount;
}

void updatePeak(std::atomic<uint64_t>& peak, uint64_t live) noexcept {
    uint64_t current = peak.load(std::memory_order_relaxed);
    while (current < live && !peak.compare_exchange_weak(
        current, live, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

#ifdef _WIN32
struct ProcessHeapUsage {
    DWORD heaps = 0;
    SIZE_T allocated = 0;
    SIZE_T committed = 0;
    SIZE_T reserved = 0;
    DWORD failed = 0;
};

ProcessHeapUsage processHeapUsage() noexcept {
    ProcessHeapUsage result;
    constexpr DWORD maxHeaps = 128;
    HANDLE heaps[maxHeaps]{};
    const DWORD required = GetProcessHeaps(0, nullptr);
    if (required == 0 || required > maxHeaps) {
        result.failed = 1;
        return result;
    }

    result.heaps = GetProcessHeaps(maxHeaps, heaps);
    if (result.heaps == 0 || result.heaps > maxHeaps) {
        result.failed = 1;
        return result;
    }

    for (DWORD i = 0; i < result.heaps; ++i) {
        HEAP_SUMMARY summary{};
        summary.cb = sizeof(summary);
        if (!HeapSummary(heaps[i], 0, &summary)) {
            ++result.failed;
            continue;
        }
        result.allocated += summary.cbAllocated;
        result.committed += summary.cbCommitted;
        result.reserved += summary.cbReserved;
    }
    return result;
}
#endif

#ifdef ZEUS_RUNTIME_DEBUG_DETAILS
struct ResourceKey {
    ResourceKind kind;
    const void* pointer;

    bool operator==(const ResourceKey& other) const noexcept {
        return kind == other.kind && pointer == other.pointer;
    }
};

struct ResourceKeyHash {
    std::size_t operator()(const ResourceKey& key) const noexcept {
        const std::size_t kind = static_cast<std::size_t>(key.kind);
        const std::size_t pointer = std::hash<const void*>{}(key.pointer);
        return pointer ^ (kind + 0x9e3779b9u + (pointer << 6) + (pointer >> 2));
    }
};

struct ResourceDetail {
    std::string label;
    std::string sourceFile;
    uint32_t sourceLine = 0;
    std::size_t bytes = 0;
};

std::mutex& detailMutex() noexcept {
    static auto* mutex = new std::mutex();
    return *mutex;
}

std::unordered_map<ResourceKey, ResourceDetail, ResourceKeyHash>& details() noexcept {
    // Keep this process-wide table alive through static runtime teardown.
    static auto* table = new std::unordered_map<ResourceKey, ResourceDetail, ResourceKeyHash>();
    return *table;
}

std::atomic<uint64_t>& detailFailures() noexcept {
    static std::atomic<uint64_t> value{0};
    return value;
}
#endif

} // namespace

ResourceCounter counter(ResourceKind kind) noexcept {
    if (!validKind(kind)) return {};
    const auto& source = counters()[static_cast<std::size_t>(kind)];
    return ResourceCounter{
        source.created.load(std::memory_order_relaxed),
        source.destroyed.load(std::memory_order_relaxed),
        source.live.load(std::memory_order_relaxed),
        source.peak.load(std::memory_order_relaxed),
        source.unmatchedDestroy.load(std::memory_order_relaxed)
    };
}

bool detailedTrackingEnabled() noexcept {
#ifdef ZEUS_RUNTIME_DEBUG_DETAILS
    return true;
#else
    return false;
#endif
}

void trackCreated(
    ResourceKind kind,
    const void* pointer,
    const char* label,
    const char* sourceFile,
    uint32_t sourceLine,
    std::size_t bytes
) noexcept {
    if (!validKind(kind) || !pointer) return;

    auto& value = counters()[static_cast<std::size_t>(kind)];
    value.created.fetch_add(1, std::memory_order_relaxed);
    const uint64_t live = value.live.fetch_add(1, std::memory_order_relaxed) + 1;
    updatePeak(value.peak, live);

#ifdef ZEUS_RUNTIME_DEBUG_DETAILS
    try {
        std::lock_guard<std::mutex> lock(detailMutex());
        const ResourceKey key{kind, pointer};
        const auto inserted = details().emplace(key, ResourceDetail{
            label ? label : kindName(kind),
            sourceFile ? sourceFile : "<unknown>",
            sourceLine,
            bytes
        });
        if (!inserted.second) {
            inserted.first->second = ResourceDetail{
                label ? label : kindName(kind),
                sourceFile ? sourceFile : "<unknown>",
                sourceLine,
                bytes
            };
            detailFailures().fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        detailFailures().fetch_add(1, std::memory_order_relaxed);
    }
#else
    (void)label;
    (void)sourceFile;
    (void)sourceLine;
    (void)bytes;
#endif
}

void trackDestroyed(ResourceKind kind, const void* pointer) noexcept {
    if (!validKind(kind) || !pointer) return;

    auto& value = counters()[static_cast<std::size_t>(kind)];
    value.destroyed.fetch_add(1, std::memory_order_relaxed);
    uint64_t live = value.live.load(std::memory_order_relaxed);
    while (live != 0 && !value.live.compare_exchange_weak(
        live, live - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    if (live == 0) {
        value.unmatchedDestroy.fetch_add(1, std::memory_order_relaxed);
    }

#ifdef ZEUS_RUNTIME_DEBUG_DETAILS
    try {
        std::lock_guard<std::mutex> lock(detailMutex());
        if (details().erase(ResourceKey{kind, pointer}) == 0) {
            detailFailures().fetch_add(1, std::memory_order_relaxed);
        }
    } catch (...) {
        detailFailures().fetch_add(1, std::memory_order_relaxed);
    }
#endif
}

void writeReport(FILE* output) noexcept {
    if (!output) output = stderr;
    std::fprintf(output, "[ZeusRuntimeDebug] details=%s\n",
        detailedTrackingEnabled() ? "on" : "off");
#ifdef _WIN32
    const auto heapUsage = processHeapUsage();
    std::fprintf(output,
        "[ZeusRuntimeDebug] process_heaps count=%lu allocated=%llu committed=%llu reserved=%llu failed=%lu\n",
        static_cast<unsigned long>(heapUsage.heaps),
        static_cast<unsigned long long>(heapUsage.allocated),
        static_cast<unsigned long long>(heapUsage.committed),
        static_cast<unsigned long long>(heapUsage.reserved),
        static_cast<unsigned long>(heapUsage.failed));
#endif
    for (std::size_t i = 0; i < kindCount; ++i) {
        const auto kind = static_cast<ResourceKind>(i);
        const auto value = counter(kind);
        std::fprintf(output,
            "[ZeusRuntimeDebug] %-26s created=%llu destroyed=%llu live=%llu peak=%llu unmatched_destroy=%llu\n",
            kindName(kind),
            static_cast<unsigned long long>(value.created),
            static_cast<unsigned long long>(value.destroyed),
            static_cast<unsigned long long>(value.live),
            static_cast<unsigned long long>(value.peak),
            static_cast<unsigned long long>(value.unmatchedDestroy));
    }

#ifdef ZEUS_RUNTIME_DEBUG_DETAILS
    try {
        std::lock_guard<std::mutex> lock(detailMutex());
        for (const auto& entry : details()) {
            const auto& detail = entry.second;
            std::fprintf(output,
                "[ZeusRuntimeDebug] live_object kind=%s ptr=%p label=%s created_at=%s:%u bytes=%llu\n",
                kindName(entry.first.kind), entry.first.pointer,
                detail.label.c_str(), detail.sourceFile.c_str(), detail.sourceLine,
                static_cast<unsigned long long>(detail.bytes));
        }
        std::fprintf(output, "[ZeusRuntimeDebug] detail_failures=%llu\n",
            static_cast<unsigned long long>(detailFailures().load(std::memory_order_relaxed)));
    } catch (...) {
        std::fprintf(output, "[ZeusRuntimeDebug] failed to print object details\n");
    }
#endif
    std::fflush(output);
}

} // namespace ZeusRuntimeDebug

namespace {

void writeRuntimeDebugReportAtExit() noexcept {
    ZeusRuntimeDebug::writeReport(stderr);
}

} // namespace

extern "C" int64_t zeus_runtime_debug_dump(void) noexcept {
    ZeusRuntimeDebug::writeReport(stderr);
    return 0;
}

extern "C" int64_t zeus_runtime_debug_dump_at_exit(void) noexcept {
    static std::once_flag registered;
    static int registrationResult = -1;
    std::call_once(registered, [] {
        registrationResult = std::atexit(writeRuntimeDebugReportAtExit);
    });
    return registrationResult == 0 ? 0 : -1;
}
