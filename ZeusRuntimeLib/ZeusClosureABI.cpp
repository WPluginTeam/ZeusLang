#include "ZeusABI.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusDynamicABI.hpp"
#include "ZeusGC.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void closurePanic(const char* message) {
    throw std::runtime_error(std::string("[ZeusClosure ABI] ") + message);
}

struct CaptureCell {
    ZeusValue value{};
    bool deleted = false;
    std::mutex mutex;

    CaptureCell() {
        zeus_value_init_nil(&value);
        zeus_value_slot_register_gc_edge(&value);
    }
    explicit CaptureCell(const ZeusValue* initial) {
        if (initial) zeus_value_clone_internal(&value, initial);
        else zeus_value_init_nil(&value);
        zeus_value_slot_register_gc_edge(&value);
    }
    ~CaptureCell() {
        zeus_value_slot_unregister_gc_edge(&value);
        zeus_value_release_internal(&value);
    }
};

struct CaptureCellHandle {
    std::shared_ptr<CaptureCell> cell;
};

struct ClosureRecord {
    ZeusClosureInvoke invoke = nullptr;
    int32_t minArity = 0;
    int32_t maxArity = 0;
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
    int32_t minArity,
    int32_t maxArity,
    int32_t captureCount,
    void* const* cells
) {
    auto record = std::make_shared<ClosureRecord>();
    record->invoke = invoke;
    record->minArity = minArity;
    record->maxArity = maxArity;
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
    int32_t minArity,
    int32_t maxArity,
    int32_t captureCount,
    void* const* cells
) {
    if (!invoke || minArity < 0 || maxArity < minArity || captureCount < 0 ||
        (captureCount != 0 && !cells)) {
        closurePanic("invalid closure construction arguments");
    }

    try {
        return ZeusType::NativeClosureHandle{
            makeClosureRecord(invoke, minArity, maxArity, captureCount, cells)
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
        if (cell->cell->deleted) closurePanic("read from deleted capture cell");
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
        cell->cell->deleted = false;
    }
}

ZeusValue* zeus_capture_cell_value_slot(void* handle) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* cell = requireCell(handle);
    std::lock_guard<std::mutex> lock(cell->cell->mutex);
    if (cell->cell->deleted) closurePanic("reference to deleted capture cell");
    return &cell->cell->value;
}

void zeus_capture_cell_delete(void* handle) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto* cellHandle = requireCell(handle);
    ZeusValue previous{};
    zeus_value_init_nil(&previous);
    {
        std::lock_guard<std::mutex> lock(cellHandle->cell->mutex);
        zeus_value_clone_internal(&previous, &cellHandle->cell->value);
    }
    zeus_value_delete_object(&previous);
    zeus_value_release_internal(&previous);
    {
        std::lock_guard<std::mutex> lock(cellHandle->cell->mutex);
        zeus_value_release_internal(&cellHandle->cell->value);
        zeus_value_init_nil(&cellHandle->cell->value);
        cellHandle->cell->value.tag = ZEUS_VALUE_TAG_DELETED;
        cellHandle->cell->deleted = true;
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
        box.v = makeClosureHandle(invoke, arity, arity, capture_count, cells);
        zeus_value_from_box(out, &box);
    } catch (...) {
        closurePanic("failed to create closure value");
    }
}

void zeus_closure_create_range(
    ZeusValue* out,
    ZeusClosureInvoke invoke,
    int32_t min_arity,
    int32_t max_arity,
    int32_t capture_count,
    void* const* cells
) {
    if (!out) closurePanic("null closure output");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusGC::AllocationScope allocation;
    try {
        ZeusBox box;
        box.v = makeClosureHandle(
            invoke, min_arity, max_arity, capture_count, cells);
        zeus_value_from_box(out, &box);
    } catch (...) {
        closurePanic("failed to create closure value");
    }
}

void zeus_closure_call(
    ZeusValue* out,
    const ZeusValue* closure,
    int32_t argc,
    const ZeusValue* const* values,
    ZeusValue* const* referenceSlots
) {
    zeus_closure_call_named(out, closure, argc, values, referenceSlots, nullptr);
}

void zeus_closure_call_named(
    ZeusValue* out,
    const ZeusValue* closure,
    int32_t argc,
    const ZeusValue* const* values,
    ZeusValue* const* referenceSlots,
    const char* const* argument_names
) {
    if (!out || argc < 0 || (argc != 0 && !values)) {
        closurePanic("invalid closure call arguments");
    }
    ZeusBox* box = nullptr;
    ClosureRecord* record = closureRecordFromValue(closure, box);
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> ownedBox(box, &zeus_box_release);
    if (argc < record->minArity || argc > record->maxArity) {
        closurePanic("closure call arity mismatch");
    }
    zeus_value_init_nil(out);
    record->invoke(out, record, argc, values, referenceSlots, argument_names);
}

void zeus_closure_bind_arguments(
    int32_t parameter_count,
    const char* const* parameter_names,
    uint64_t default_parameter_mask,
    int32_t has_variadic_args,
    int32_t has_variadic_kwargs,
    int32_t argc,
    const ZeusValue* const* values,
    ZeusValue* const* referenceSlots,
    const char* const* argument_names,
    const ZeusValue** ordered_values,
    ZeusValue** orderedReferenceSlots,
    uint64_t* provided_mask,
    ZeusValue* variadic_args,
    ZeusValue* variadic_kwargs
) {
    if (parameter_count < 0 || argc < 0 ||
        (parameter_count != 0 && (!parameter_names || !ordered_values)) ||
        (argc != 0 && !values) || !provided_mask ||
        (has_variadic_args && !variadic_args) ||
        (has_variadic_kwargs && !variadic_kwargs)) {
        closurePanic("invalid closure argument binding metadata");
    }

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusGC::AllocationScope allocation;
    *provided_mask = 0;
    for (int32_t i = 0; i < parameter_count; ++i) {
        ordered_values[i] = nullptr;
        if (orderedReferenceSlots) orderedReferenceSlots[i] = nullptr;
    }
    if (has_variadic_args) zeus_value_init_nil(variadic_args);
    if (has_variadic_kwargs) zeus_value_init_nil(variadic_kwargs);

    struct BoundArgument {
        const ZeusValue* value = nullptr;
        ZeusValue* referenceSlot = nullptr;
    };
    std::vector<BoundArgument> positional_arguments;
    std::map<std::string, BoundArgument> named_arguments;
    for (int32_t i = 0; i < argc; ++i) {
        const char* rawName = argument_names ? argument_names[i] : nullptr;
        const BoundArgument argument{
            values[i], referenceSlots ? referenceSlots[i] : nullptr
        };
        if (!rawName || !*rawName) {
            positional_arguments.push_back(argument);
        } else {
            // VM CALL_CLOSURE stores named arguments by name, so a later
            // occurrence replaces the previous value.
            named_arguments[rawName] = argument;
        }
    }

    const size_t positional_count = positional_arguments.size();
    std::set<std::string> consumed_named_arguments;
    for (int32_t i = 0; i < parameter_count; ++i) {
        if (static_cast<size_t>(i) < positional_count) {
            const BoundArgument& argument = positional_arguments[static_cast<size_t>(i)];
            ordered_values[i] = argument.value;
            if (orderedReferenceSlots) orderedReferenceSlots[i] = argument.referenceSlot;
            if (i < 64) *provided_mask |= uint64_t{1} << i;
            continue;
        }

        if (!parameter_names[i]) {
            closurePanic("closure parameter name metadata is null");
        }
        const auto named = named_arguments.find(parameter_names[i]);
        if (named != named_arguments.end()) {
            ordered_values[i] = named->second.value;
            if (orderedReferenceSlots) orderedReferenceSlots[i] = named->second.referenceSlot;
            consumed_named_arguments.insert(named->first);
            if (i < 64) *provided_mask |= uint64_t{1} << i;
            continue;
        }

        const bool hasDefault = i < 64 &&
            ((default_parameter_mask >> i) & uint64_t{1}) != 0;
        if (!hasDefault) closurePanic("missing required closure argument");
    }

    if (!has_variadic_args && positional_count > static_cast<size_t>(parameter_count)) {
        closurePanic("too many positional arguments");
    }
    if (!has_variadic_kwargs) {
        for (const auto& entry : named_arguments) {
            if (consumed_named_arguments.find(entry.first) ==
                consumed_named_arguments.end()) {
                closurePanic("unexpected named argument");
            }
        }
    }

    if (has_variadic_args) {
        const size_t extra_count = positional_count > static_cast<size_t>(parameter_count)
            ? positional_count - static_cast<size_t>(parameter_count)
            : 0;
        std::vector<const ZeusValue*> extra_positional;
        extra_positional.reserve(extra_count);
        for (size_t i = static_cast<size_t>(parameter_count);
             i < positional_count; ++i) {
            extra_positional.push_back(positional_arguments[i].value);
        }
        zeus_value_array_build(
            variadic_args,
            static_cast<int64_t>(extra_positional.size()),
            extra_positional.empty() ? nullptr : extra_positional.data());
    }
    if (has_variadic_kwargs) {
        std::vector<std::pair<std::string, const ZeusValue*>> extra_keywords;
        for (const auto& entry : named_arguments) {
            if (consumed_named_arguments.find(entry.first) ==
                consumed_named_arguments.end()) {
                extra_keywords.emplace_back(entry.first, entry.second.value);
            }
        }

        std::vector<ZeusValue> keywordNames;
        std::vector<const ZeusValue*> keyValues;
        keywordNames.reserve(extra_keywords.size());
        keyValues.reserve(extra_keywords.size() * 2);
        for (const auto& entry : extra_keywords) {
            keywordNames.emplace_back();
            zeus_value_init_nil(&keywordNames.back());
            zeus_value_from_string_data(
                &keywordNames.back(), entry.first.data(),
                static_cast<int64_t>(entry.first.size()));
            keyValues.push_back(&keywordNames.back());
            keyValues.push_back(entry.second);
        }
        zeus_value_dict_build(
            variadic_kwargs,
            static_cast<int64_t>(extra_keywords.size()),
            keyValues.empty() ? nullptr : keyValues.data());
        for (auto& key : keywordNames) zeus_value_release(&key);
    }
}
void zeus_closure_env_load(ZeusValue* out, void* environment, int32_t index) {
    if (!out) closurePanic("null environment load output");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto cell = requireCapture(requireEnvironment(environment), index);
    {
        std::lock_guard<std::mutex> lock(cell->mutex);
        if (cell->deleted) closurePanic("read from deleted closure capture");
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
        cell->deleted = false;
    }
}

ZeusValue* zeus_closure_env_value_slot(void* environment, int32_t index) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto cell = requireCapture(requireEnvironment(environment), index);
    std::lock_guard<std::mutex> lock(cell->mutex);
    if (cell->deleted) closurePanic("reference to deleted closure capture");
    return &cell->value;
}

void zeus_closure_env_delete(void* environment, int32_t index) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto cell = requireCapture(requireEnvironment(environment), index);
    ZeusValue previous{};
    zeus_value_init_nil(&previous);
    {
        std::lock_guard<std::mutex> lock(cell->mutex);
        zeus_value_clone_internal(&previous, &cell->value);
    }
    zeus_value_delete_object(&previous);
    zeus_value_release_internal(&previous);
    {
        std::lock_guard<std::mutex> lock(cell->mutex);
        zeus_value_release_internal(&cell->value);
        zeus_value_init_nil(&cell->value);
        cell->value.tag = ZEUS_VALUE_TAG_DELETED;
        cell->deleted = true;
    }
}
