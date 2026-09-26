#include "ZeusABI.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusGC.hpp"

#include <memory>
#include <mutex>
#include <vector>

namespace {

[[noreturn]] void closurePanic(const char* message) {
    std::fprintf(stderr, "[ZeusClosure ABI] %s\n", message);
    std::abort();
}

struct CaptureCell {
    ZeusValue value{};
    std::mutex mutex;

    CaptureCell() = default;
    explicit CaptureCell(const ZeusValue* initial) {
        if (initial) zeus_value_clone_internal(&value, initial);
        else zeus_value_init_nil(&value);
    }
    ~CaptureCell() { zeus_value_release_internal(&value); }
};

struct CaptureCellHandle {
    std::shared_ptr<CaptureCell> cell;
};

struct ClosureRecord {
    ZeusClosureInvoke invoke = nullptr;
    int32_t arity = 0;
    std::mutex mutex;
    std::vector<std::shared_ptr<CaptureCell>> captures;
};

void traceCaptureCell(void* payload, const ZeusGC::Visitor& visitor) {
    auto* cell = static_cast<CaptureCell*>(payload);
    std::lock_guard<std::mutex> lock(cell->mutex);
    visitor.visitValue(&cell->value);
}

void breakCaptureCellCycles(void* payload) {
    auto* cell = static_cast<CaptureCell*>(payload);
    std::lock_guard<std::mutex> lock(cell->mutex);
    zeus_value_release_internal(&cell->value);
}

void traceClosureRecord(void* payload, const ZeusGC::Visitor& visitor) {
    auto* record = static_cast<ClosureRecord*>(payload);
    std::lock_guard<std::mutex> lock(record->mutex);
    for (const auto& cell : record->captures) {
        visitor.visitNode(std::static_pointer_cast<void>(cell));
    }
}

void breakClosureRecordCycles(void* payload) {
    auto* record = static_cast<ClosureRecord*>(payload);
    std::lock_guard<std::mutex> lock(record->mutex);
    record->captures.clear();
}

std::shared_ptr<CaptureCell> makeCaptureCell(const ZeusValue* initial) {
    auto cell = std::make_shared<CaptureCell>(initial);
    ZeusGC::registerSharedNode(
        std::static_pointer_cast<void>(cell),
        traceCaptureCell,
        breakCaptureCellCycles
    );
    return cell;
}

CaptureCellHandle* requireCell(void* handle);

// The record is published to the collector only once it is fully populated, so
// a collection triggered by the registration cannot see a half-built node.
std::shared_ptr<ClosureRecord> makeClosureRecord(
    ZeusClosureInvoke invoke,
    int32_t arity,
    int32_t captureCount,
    void* const* cells
) {
    auto record = std::make_shared<ClosureRecord>();
    record->invoke = invoke;
    record->arity = arity;
    record->captures.reserve(static_cast<size_t>(captureCount));
    for (int32_t i = 0; i < captureCount; ++i) {
        record->captures.push_back(requireCell(cells[i])->cell);
    }
    ZeusGC::registerSharedNode(
        std::static_pointer_cast<void>(record),
        traceClosureRecord,
        breakClosureRecordCycles
    );
    return record;
}

CaptureCellHandle* makeCellHandle(std::shared_ptr<CaptureCell> cell) {
    auto rooted = std::static_pointer_cast<void>(cell);
    ZeusGC::rootNode(rooted);
    try {
        return new CaptureCellHandle{ std::move(cell) };
    } catch (...) {
        ZeusGC::unrootNode(rooted);
        throw;
    }
}

[[noreturn]] void rethrowAsClosurePanic(const char* message) {
    closurePanic(message);
}

CaptureCellHandle* requireCell(void* handle) {
    auto* cell = static_cast<CaptureCellHandle*>(handle);
    if (!cell || !cell->cell) closurePanic("invalid capture cell");
    return cell;
}

ClosureRecord* requireEnvironment(void* environment) {
    if (!environment) closurePanic("null closure environment");
    return static_cast<ClosureRecord*>(environment);
}

std::shared_ptr<CaptureCell> requireCapture(ClosureRecord* environment, int32_t index) {
    std::lock_guard<std::mutex> lock(environment->mutex);
    if (index < 0 || static_cast<size_t>(index) >= environment->captures.size()) {
        closurePanic("capture index out of range");
    }
    return environment->captures[static_cast<size_t>(index)];
}

ZeusType::NativeClosureHandle makeClosureHandle(
    ZeusClosureInvoke invoke,
    int32_t arity,
    int32_t captureCount,
    void* const* cells
) {
    if (!invoke || arity < 0 || captureCount < 0 ||
        (captureCount != 0 && !cells)) {
        closurePanic("invalid closure construction arguments");
    }

    try {
        return ZeusType::NativeClosureHandle{
            makeClosureRecord(invoke, arity, captureCount, cells)
        };
    } catch (...) {
        rethrowAsClosurePanic("failed to register closure environment");
    }
}

ClosureRecord* closureRecordFromValue(const ZeusValue* value, ZeusBox*& ownedBox) {
    if (!value) closurePanic("null closure value");
    ownedBox = zeus_value_to_box(value);
    if (!ownedBox) closurePanic("failed to read closure value");
    const auto* closure = std::get_if<ZeusType::NativeClosureHandle>(&ownedBox->v);
    if (!closure || !closure->state) {
        zeus_box_release(ownedBox);
        ownedBox = nullptr;
        closurePanic("attempted to call a non-closure value");
    }
    return static_cast<ClosureRecord*>(closure->state.get());
}

} // namespace

void* zeus_capture_cell_create(const ZeusValue* initial) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusGC::AllocationScope allocation;
    try {
        auto handle = makeCellHandle(makeCaptureCell(initial));
        return handle;
    } catch (...) {
        closurePanic("failed to allocate capture cell");
    }
}

void* zeus_capture_cell_clone(void* handle) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* source = requireCell(handle);
    ZeusValue copy{};
    zeus_value_init_nil(&copy);
    {
        std::lock_guard<std::mutex> lock(source->cell->mutex);
        zeus_value_clone_internal(&copy, &source->cell->value);
    }
    try {
        ZeusGC::AllocationScope allocation;
        auto clone = makeCellHandle(makeCaptureCell(&copy));
        zeus_value_release_internal(&copy);
        return clone;
    } catch (...) {
        zeus_value_release_internal(&copy);
        closurePanic("failed to copy capture cell");
    }
}

void* zeus_capture_cell_retain(void* handle) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* source = requireCell(handle);
    try {
        auto retained = makeCellHandle(source->cell);
        return retained;
    } catch (...) {
        closurePanic("failed to retain capture cell");
    }
}

void zeus_capture_cell_release(void* handle) {
    if (!handle) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* cell = static_cast<CaptureCellHandle*>(handle);
    if (cell->cell) ZeusGC::unrootNode(std::static_pointer_cast<void>(cell->cell));
    delete cell;
}

void zeus_capture_cell_load(ZeusValue* out, void* handle) {
    if (!out) closurePanic("null capture load output");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* cell = requireCell(handle);
    {
        std::lock_guard<std::mutex> lock(cell->cell->mutex);
        zeus_value_clone(out, &cell->cell->value);
    }
}

void zeus_capture_cell_store(void* handle, const ZeusValue* value) {
    if (!value) closurePanic("null capture store value");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* cell = requireCell(handle);
    ZeusValue copy{};
    zeus_value_init_nil(&copy);
    zeus_value_clone_internal(&copy, value);
    {
        std::lock_guard<std::mutex> lock(cell->cell->mutex);
        zeus_value_release_internal(&cell->cell->value);
        cell->cell->value = copy;
    }
}

void* zeus_closure_capture_ref(void* environment, int32_t index) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* record = requireEnvironment(environment);
    try {
        return makeCellHandle(requireCapture(record, index));
    } catch (...) {
        closurePanic("failed to capture reference cell");
    }
}

void* zeus_closure_capture_copy(void* environment, int32_t index) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* record = requireEnvironment(environment);
    const auto source = requireCapture(record, index);
    ZeusValue copy{};
    zeus_value_init_nil(&copy);
    {
        std::lock_guard<std::mutex> lock(source->mutex);
        zeus_value_clone_internal(&copy, &source->value);
    }
    try {
        ZeusGC::AllocationScope allocation;
        auto copied = makeCellHandle(makeCaptureCell(&copy));
        zeus_value_release_internal(&copy);
        return copied;
    } catch (...) {
        zeus_value_release_internal(&copy);
        closurePanic("failed to copy closure capture");
    }
}

void zeus_closure_create(
    ZeusValue* out,
    ZeusClosureInvoke invoke,
    int32_t arity,
    int32_t capture_count,
    void* const* cells
) {
    if (!out) closurePanic("null closure output");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusGC::AllocationScope allocation;
    try {
        // The local box is a root, so the record stays reachable if the scope
        // exit triggers a collection.
        ZeusBox box;
        box.v = makeClosureHandle(invoke, arity, capture_count, cells);
        zeus_value_from_box(out, &box);
    } catch (...) {
        closurePanic("failed to create closure value");
    }
}

void zeus_closure_call(
    ZeusValue* out,
    const ZeusValue* closure,
    int32_t argc,
    const ZeusValue* const* values
) {
    if (!out || argc < 0 || (argc != 0 && !values)) {
        closurePanic("invalid closure call arguments");
    }
    ZeusBox* box = nullptr;
    ClosureRecord* record = closureRecordFromValue(closure, box);
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> ownedBox(box, &zeus_box_release);
    if (argc != record->arity) closurePanic("closure call arity mismatch");
    zeus_value_init_nil(out);
    record->invoke(out, record, argc, values);
}

void zeus_closure_env_load(ZeusValue* out, void* environment, int32_t index) {
    if (!out) closurePanic("null environment load output");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto cell = requireCapture(requireEnvironment(environment), index);
    {
        std::lock_guard<std::mutex> lock(cell->mutex);
        zeus_value_clone(out, &cell->value);
    }
}

void zeus_closure_env_store(
    void* environment,
    int32_t index,
    const ZeusValue* value
) {
    if (!value) closurePanic("null environment store value");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto cell = requireCapture(requireEnvironment(environment), index);
    ZeusValue copy{};
    zeus_value_init_nil(&copy);
    zeus_value_clone_internal(&copy, value);
    {
        std::lock_guard<std::mutex> lock(cell->mutex);
        zeus_value_release_internal(&cell->value);
        cell->value = copy;
    }
}
