#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#define private public
#include "../ZeusRunner/ZeusVMSlots.hpp"
#undef private

namespace {
struct MockVM {
    std::string runtimeValueTypeName(
        const ZeusType::UniversalVariant& value) const {
        (void)value;
        return "int";
    }
};

ZeusVMSlots::Registry::Entry staleReveal(void* base, std::uint64_t size) {
    ZeusVMSlots::Registry::Entry entry;
    entry.cell = reinterpret_cast<Cell*>(static_cast<std::uintptr_t>(0x1234));
    entry.cellId = 7;
    entry.reveal = true;
    entry.revealedBase = reinterpret_cast<std::uintptr_t>(base);
    entry.revealedSize = size;
    return entry;
}

bool expect(bool condition, const char* label) {
    if (condition) return true;
    std::fprintf(stderr, "FAIL: %s\n", label);
    return false;
}

ZeusVMSlots::Registry::Entry revealOf(Cell* cell, uint32_t cellId,
                                     void* base, std::uint64_t size) {
    ZeusVMSlots::Registry::Entry entry;
    entry.cell = cell;
    entry.cellId = cellId;
    entry.reveal = true;
    entry.revealedBase = reinterpret_cast<std::uintptr_t>(base);
    entry.revealedSize = size;
    return entry;
}

// A revealed entry must be dropped once the allocator no longer owns the block
// it described, and must survive while that block is live even if the cell it
// came from is gone. Leaving them in place let the registry grow without bound
// and made every @faddr walk the whole history; pruning by cell liveness
// instead would cut aliases whose storage is still perfectly valid.
bool pruneFollowsStorageLifetime() {
    const std::int64_t bits = __zeus_malloc(16);
    if (!expect(bits != 0, "Zeus malloc returned null")) return false;
    void* allocation = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(bits));
    const auto base = reinterpret_cast<std::uintptr_t>(allocation);

    ZeusVMSlots::Registry registry;
    Cell temporary(LexerType::Value(static_cast<std::int64_t>(7)), 0, 0x5150);
    HeapStats::registerCell(&temporary);

    // A live allocation behind a cell that has already been released: the
    // ordinary shape of a method that hands back its own receiver.
    registry.slots[base] = revealOf(&temporary, temporary.debugId, allocation, 16);
    // A second entry describing a block the allocator never handed out.
    registry.slots[base + 0x40] = staleReveal(
        reinterpret_cast<void*>(base + 0x100000), 16);
    HeapStats::unregisterCell(&temporary);

    registry.pruneExpiredSlots();
    if (!expect(registry.slots.size() == 1,
                "pruning must keep exactly the revealed entry over the live block")) {
        return false;
    }
    if (!expect(registry.slots.begin()->first == base,
                "the surviving entry must be the one over the live block")) {
        return false;
    }

    __zeus_free(bits);
    registry.pruneExpiredSlots();
    return expect(registry.slots.empty(),
                  "a revealed entry must not outlive the block it described");
}
}

int main() {
    const std::int64_t allocationBits = __zeus_malloc(16);
    if (!allocationBits) {
        std::fprintf(stderr, "FAIL: Zeus malloc returned null\n");
        return 1;
    }
    void* allocation = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(allocationBits));
    const auto base = reinterpret_cast<std::uintptr_t>(allocation);
    MockVM vm;
    std::string error;

    ZeusVMSlots::Registry overlapping;
    overlapping.slots[base + 0x100] = staleReveal(allocation, 32);
    const bool accepted = overlapping.validateRaw(
        vm, allocation, "int", 1, true, nullptr, error);
    if (!expect(accepted,
                "live Zeus malloc access overlapping a stale reveal must be accepted")) {
        __zeus_free(allocationBits);
        return 2;
    }

    error.clear();
    const bool overflowAccepted = overlapping.validateRaw(
        vm, reinterpret_cast<void*>(base + 15), "int", 2, true, nullptr, error);
    if (!expect(!overflowAccepted &&
                error == "VM @faddr refers to an expired cell slot",
                "the full access range must fit in the live allocation")) {
        __zeus_free(allocationBits);
        return 3;
    }

    ZeusVMSlots::Registry typedSlot;
    typedSlot.slots[base] = ZeusVMSlots::Registry::Entry{};
    error.clear();
    const bool typedAccepted = typedSlot.validateRaw(
        vm, allocation, "int", 1, true, nullptr, error);
    if (!expect(!typedAccepted &&
                error == "VM @faddr cannot access an interior or mismatched typed slot",
                "non-reveal typed slots must keep their existing rejection")) {
        __zeus_free(allocationBits);
        return 4;
    }

    __zeus_free(allocationBits);
    error.clear();
    const bool freedAccepted = overlapping.validateRaw(
        vm, allocation, "int", 1, true, nullptr, error);
    if (!expect(!freedAccepted &&
                error == "VM @faddr refers to an expired cell slot",
                "a stale reveal must still reject after the allocation is freed")) {
        return 5;
    }

    if (!pruneFollowsStorageLifetime()) {
        return 6;
    }

    std::puts("VM_REVEALED_SLOT_REUSE_PASS");
    return 0;
}
