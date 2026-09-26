// ZeusZTest/gc_collector_test.cpp
// Unit test for the shared mark/break-cycles collector in ZeusGCABI.cpp.
//
// These cases cannot be written in Zeus today: the type checker rejects a local
// that is nil before a closure is assigned into it, so a self-referential
// closure is only constructible through the ABI.
//
// Build and run (from the repository root):
//   ZeusZTest\run_gc_collector_test.bat

#include "ZeusABI.hpp"
#include "ZeusABIInternal.hpp"
#include "ZeusGC.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* name) {
    ++g_checks;
    if (condition) {
        std::printf("[PASS] %s\n", name);
        return;
    }
    ++g_failures;
    std::printf("[FAIL] %s\n", name);
}

void noop(ZeusValue* out, void*, int32_t, const ZeusValue* const*) {
    zeus_value_init_nil(out);
}

ZeusValue makeClosure(void* cell) {
    ZeusValue closure{};
    zeus_value_init_nil(&closure);
    void* cells[1] = { cell };
    zeus_closure_create(&closure, noop, 0, 1, cells);
    return closure;
}

// The environment pointer is the closure record carried by the value. Reading it
// here is the point of the test: it is the same handle the generated code passes
// to zeus_closure_env_load.
void* closureEnvironment(const ZeusValue* value) {
    if (!value || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX || value->payload == 0) {
        return nullptr;
    }
    const auto* box = reinterpret_cast<const ZeusBox*>(
        static_cast<uintptr_t>(value->payload));
    const auto* handle = std::get_if<ZeusType::NativeClosureHandle>(&box->v);
    return handle ? handle->state.get() : nullptr;
}

// A capture cell that stores the closure owning it: the record and the cell
// reference each other, so only cycle breaking can reclaim them.
void testSelfCycle() {
    void* cell = zeus_capture_cell_create(nullptr);
    ZeusValue closure = makeClosure(cell);
    zeus_capture_cell_store(cell, &closure);
    zeus_value_release(&closure);
    zeus_capture_cell_release(cell);

    check(ZeusGC::liveNodeCount() == 2, "self cycle registers two nodes");
    zeus_gc_collect();
    check(ZeusGC::liveNodeCount() == 0, "self cycle is reclaimed");
}

// Two closures whose capture cells hold each other.
void testMutualCycle() {
    void* cellA = zeus_capture_cell_create(nullptr);
    void* cellB = zeus_capture_cell_create(nullptr);
    ZeusValue a = makeClosure(cellA);
    ZeusValue b = makeClosure(cellB);
    zeus_capture_cell_store(cellA, &b);
    zeus_capture_cell_store(cellB, &a);
    zeus_value_release(&a);
    zeus_value_release(&b);
    zeus_capture_cell_release(cellA);
    zeus_capture_cell_release(cellB);

    check(ZeusGC::liveNodeCount() == 4, "mutual cycle registers four nodes");
    zeus_gc_collect();
    check(ZeusGC::liveNodeCount() == 0, "mutual cycle is reclaimed");
}

// A rooted closure must survive collection with its captures intact. This is
// the cross-boundary invariant: the box owning the value is the root, so the
// record stays marked and its cells are never broken.
void testLiveClosureSurvives() {
    ZeusValue start{};
    zeus_value_init_i64(&start, 7);
    void* cell = zeus_capture_cell_create(&start);
    zeus_value_release(&start);
    ZeusValue closure = makeClosure(cell);
    zeus_capture_cell_release(cell);

    zeus_gc_collect();
    check(ZeusGC::liveNodeCount() == 2, "live closure survives collection");

    ZeusValue loaded{};
    zeus_value_init_nil(&loaded);
    zeus_closure_env_load(&loaded, closureEnvironment(&closure), 0);
    check(zeus_value_require_i64(&loaded) == 7, "live closure keeps its capture");
    zeus_value_release(&loaded);

    zeus_value_release(&closure);
    zeus_gc_collect();
    check(ZeusGC::liveNodeCount() == 0, "released closure is reclaimed");
}

// Allocation must trigger collection on its own, because AOT code never calls
// zeus_gc_collect explicitly.
void testAllocationTriggeredCollection() {
    const int kIterations = 20000;
    for (int i = 0; i < kIterations; ++i) {
        void* cell = zeus_capture_cell_create(nullptr);
        ZeusValue closure = makeClosure(cell);
        zeus_capture_cell_store(cell, &closure);
        zeus_value_release(&closure);
        zeus_capture_cell_release(cell);
    }
    const std::size_t live = ZeusGC::liveNodeCount();
    check(live > 0 && live < 1024,
        "allocation-triggered collection bounds the node table");
    std::printf("       live nodes after %d cycles: %llu\n", kIterations,
        static_cast<unsigned long long>(live));
}

// A collection running concurrently with graph mutation must not reclaim a
// value that other threads can still observe.
void testConcurrentStoreAndCollect() {
    void* shared = zeus_capture_cell_create(nullptr);
    std::atomic<bool> stop{ false };
    std::atomic<long> iterations{ 0 };
    std::atomic<long> collections{ 0 };

    std::thread collector([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            zeus_gc_collect();
            collections.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) {
        workers.emplace_back([&] {
            for (int n = 0; n < 20000; ++n) {
                ZeusValue start{};
                zeus_value_init_i64(&start, n);
                void* cell = zeus_capture_cell_create(&start);
                zeus_value_release(&start);
                ZeusValue closure = makeClosure(cell);
                zeus_capture_cell_store(shared, &closure);
                ZeusValue loaded{};
                zeus_value_init_nil(&loaded);
                zeus_capture_cell_load(&loaded, shared);
                if (loaded.flags == ZEUS_VALUE_FLAG_OWNS_BOX) zeus_value_release(&loaded);
                zeus_value_release(&closure);
                zeus_capture_cell_release(cell);
                iterations.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    stop.store(true);
    collector.join();

    ZeusValue loaded{};
    zeus_value_init_nil(&loaded);
    zeus_capture_cell_load(&loaded, shared);
    check(loaded.flags == ZEUS_VALUE_FLAG_OWNS_BOX,
        "concurrent collection keeps a published closure alive");
    if (loaded.flags == ZEUS_VALUE_FLAG_OWNS_BOX) zeus_value_release(&loaded);
    zeus_capture_cell_release(shared);

    std::printf("       %ld mutations, %ld collections, %llu live nodes\n",
        iterations.load(), collections.load(),
        static_cast<unsigned long long>(ZeusGC::liveNodeCount()));
}

} // namespace

int main() {
    check(ZeusGC::liveNodeCount() == 0, "collector starts empty");
    testSelfCycle();
    testMutualCycle();
    testLiveClosureSurvives();
    testAllocationTriggeredCollection();
    testConcurrentStoreAndCollect();

    std::printf("\nPASS = %d FAIL = %d TOTAL = %d\n",
        g_checks - g_failures, g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
