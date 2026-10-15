#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace ZeusRuntimeDebug {

enum class ResourceKind : uint32_t {
    ZeusBox = 0,
    ManagedClosure,
    CaptureCell,
    ClassObject,
    ThreadRecord,
    SocketRecord,
    DynamicFieldOwner,
    Count
};

struct ResourceCounter {
    uint64_t created = 0;
    uint64_t destroyed = 0;
    uint64_t live = 0;
    uint64_t peak = 0;
    uint64_t unmatchedDestroy = 0;
};

// Counters are always enabled. ZEUS_RUNTIME_DEBUG_DETAILS adds a live-object
// table containing each resource's pointer and creation site.
ResourceCounter counter(ResourceKind kind) noexcept;
bool detailedTrackingEnabled() noexcept;
void trackCreated(
    ResourceKind kind,
    const void* pointer,
    const char* label,
    const char* sourceFile,
    uint32_t sourceLine,
    std::size_t bytes = 0
) noexcept;
void trackDestroyed(ResourceKind kind, const void* pointer) noexcept;
void writeReport(FILE* output = stderr) noexcept;

} // namespace ZeusRuntimeDebug

extern "C" int64_t zeus_runtime_debug_dump(void) noexcept;
extern "C" int64_t zeus_runtime_debug_dump_at_exit(void) noexcept;
