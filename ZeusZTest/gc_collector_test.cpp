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
#include "ZeusDynamicABI.hpp"
#include "ZeusGC.hpp"
#include "ZeusValueSlot.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

static_assert(std::is_standard_layout_v<ZeusValueSlot>,
    "ZeusValueSlot must keep ZeusValue as its first ABI-compatible field");

const ZeusValue& rawSlotValue(const ZeusValueSlot& slot) {
    return *reinterpret_cast<const ZeusValue*>(&slot);
}

void check(bool condition, const char* name) {
    ++g_checks;
    if (condition) {
        std::printf("[PASS] %s\n", name);
        return;
    }
    ++g_failures;
    std::printf("[FAIL] %s\n", name);
}

void noop(
    ZeusValue* out,
    void*,
    int32_t,
    const ZeusValue* const*,
    ZeusValue* const*,
    const char* const*
) {
    zeus_value_init_nil(out);
}

bool readInt64(const ZeusType::UniversalVariant& value, int64_t& out) {
    const auto* literal = std::get_if<LexerType::Value>(&value);
    if (!literal) return false;
    const auto* integer = std::get_if<ZeusType::BigInteger>(&literal->data);
    return integer && integer->tryToInt64(out);
}

void testDirectValueSlots() {
    ZeusValueSlot nil;
    check(rawSlotValue(nil).tag == ZEUS_VALUE_TAG_NIL && nil.nativeAddress() != nullptr,
        "nil slot exposes its inline payload address");

    ZeusValueSlot integer(ZeusType::UniversalVariant(
        LexerType::Value(static_cast<int64_t>(17))));
    int64_t* integerAddress = static_cast<int64_t*>(integer.nativeAddress());
    check(rawSlotValue(integer).tag == ZEUS_VALUE_TAG_INT64 &&
          integerAddress && *integerAddress == 17,
        "int slot exposes its inline payload address");
    if (integerAddress) *integerAddress = 23;
    int64_t integerValue = 0;
    check(readInt64(integer.read(), integerValue) && integerValue == 23,
        "native int slot writes are visible to VM reads");
    integer.write(ZeusType::UniversalVariant(
        LexerType::Value(static_cast<int64_t>(31))));
    check(integer.nativeAddress() == integerAddress && integerAddress &&
          *integerAddress == 31,
        "int slot address stays stable when the binding is written");
    integer.withVariant([](ZeusType::UniversalVariant& value) {
        value = LexerType::Value(static_cast<int64_t>(37));
    });
    check(rawSlotValue(integer).tag == ZEUS_VALUE_TAG_INT64 &&
          readInt64(integer.read(), integerValue) && integerValue == 37,
        "mutable UniversalVariant access is encoded back into the slot");

    ZeusValueSlot boolean(ZeusType::UniversalVariant(LexerType::Value(true)));
    bool* boolAddress = static_cast<bool*>(boolean.nativeAddress());
    check(rawSlotValue(boolean).tag == ZEUS_VALUE_TAG_BOOL &&
          boolAddress && *boolAddress,
        "bool slot exposes its inline payload address");
    if (boolAddress) *boolAddress = false;
    const auto boolValue = boolean.read();
    const auto* boolLiteral = std::get_if<LexerType::Value>(&boolValue);
    const auto* boolPayload = boolLiteral
        ? std::get_if<bool>(&boolLiteral->data) : nullptr;
    check(boolPayload && !*boolPayload,
        "native bool slot writes are visible to VM reads");

    ZeusValueSlot character(ZeusType::UniversalVariant('A'));
    auto* characterAddress = static_cast<char*>(character.nativeAddress());
    check(rawSlotValue(character).tag == ZEUS_VALUE_TAG_CHAR &&
          rawSlotValue(character).flags == 0 &&
          characterAddress && *characterAddress == 'A',
        "char slot stores and exposes its inline native payload");
    if (characterAddress) *characterAddress = 'Z';
    const auto characterValue = character.read();
    const auto* decodedCharacter = std::get_if<char>(&characterValue);
    check(decodedCharacter && *decodedCharacter == 'Z',
        "native char slot writes are visible to VM reads");

    const ZeusType::BigInteger largeInteger("9223372036854775808");
    ZeusValueSlot bigInteger{ZeusType::UniversalVariant(
        LexerType::Value(largeInteger))};
    check(rawSlotValue(bigInteger).tag == ZEUS_VALUE_TAG_BIGINT &&
          rawSlotValue(bigInteger).flags ==
              (ZEUS_VALUE_FLAG_OWNS_NATIVE_PAYLOAD | ZEUS_VALUE_FLAG_LEXER_VALUE),
        "large integer slot owns a typed payload without a ZeusBox");
    const auto* nativeInteger = static_cast<const ZeusType::BigInteger*>(
        bigInteger.nativeAddress());
    const auto decodedInteger = bigInteger.read();
    const auto* decodedIntegerLiteral = std::get_if<LexerType::Value>(&decodedInteger);
    const auto* decodedIntegerPayload = decodedIntegerLiteral
        ? std::get_if<ZeusType::BigInteger>(&decodedIntegerLiteral->data) : nullptr;
    check(nativeInteger && nativeInteger->toString() == largeInteger.toString() &&
          decodedIntegerPayload &&
          decodedIntegerPayload->toString() == largeInteger.toString(),
        "large integer native address and VM reads preserve the numeric value");

    ZeusValue clonedInteger{};
    zeus_value_init_nil(&clonedInteger);
    zeus_value_clone(&clonedInteger, &rawSlotValue(bigInteger));
    const auto* clonedIntegerPayload = static_cast<const ZeusType::BigInteger*>(
        zeus_value_native_address(&clonedInteger, 1));
    check(clonedIntegerPayload && clonedIntegerPayload != nativeInteger &&
          clonedIntegerPayload->toString() == largeInteger.toString(),
        "large integer ZeusValue clone deep-copies its typed payload");
    ZeusBox* materializedInteger = zeus_value_to_box(&rawSlotValue(bigInteger));
    const auto* materializedLiteral = std::get_if<LexerType::Value>(
        &materializedInteger->v);
    const auto* materializedIntegerPayload = materializedLiteral
        ? std::get_if<ZeusType::BigInteger>(&materializedLiteral->data) : nullptr;
    check(materializedIntegerPayload &&
          materializedIntegerPayload->toString() == largeInteger.toString(),
        "dynamic VM bridge materializes the original numeric wrapper");
    zeus_box_release(materializedInteger);
    zeus_value_release(&clonedInteger);

    const ZeusType::BigDecimal decimal("1234.5678");
    ZeusValueSlot bigDecimal{ZeusType::UniversalVariant(decimal)};
    const auto* nativeDecimal = static_cast<const ZeusType::BigDecimal*>(
        bigDecimal.nativeAddress());
    const auto decodedDecimal = bigDecimal.read();
    const auto* decodedDecimalPayload = std::get_if<ZeusType::BigDecimal>(
        &decodedDecimal);
    check(rawSlotValue(bigDecimal).tag == ZEUS_VALUE_TAG_BIGDECIMAL &&
          rawSlotValue(bigDecimal).flags == ZEUS_VALUE_FLAG_OWNS_NATIVE_PAYLOAD &&
          nativeDecimal && nativeDecimal->toString() == decimal.toString() &&
          decodedDecimalPayload &&
          decodedDecimalPayload->toString() == decimal.toString(),
        "decimal slot owns a typed native payload and decodes directly");

    int64_t referenced = 41;
    ZeusValueSlot reference(ZeusType::UniversalVariant(
        ZeusType::ReferenceHandle{ &referenced, "int" }));
    check(rawSlotValue(reference).tag == ZEUS_VALUE_TAG_REF_INT64 &&
          reference.nativeAddress() == &referenced,
        "scalar reference slot points at its referent");

    std::weak_ptr<ZeusType::ArrayData> arrayWeak;
    {
        auto arrayData = ZeusGC::makeArrayData();
        ZeusType::Array array;
        array.impl = arrayData;
        arrayWeak = arrayData;
        ZeusValueSlot arraySlot{ ZeusType::UniversalVariant(
            ZeusType::Variant(array)) };
        check(rawSlotValue(arraySlot).tag == ZEUS_VALUE_TAG_BOXED &&
              rawSlotValue(arraySlot).flags == ZEUS_VALUE_FLAG_OWNS_BOX,
            "managed value slot stores an owned ZeusBox reference");
        array.impl.reset();
        arrayData.reset();
        ZeusGC::collect();
        check(!arrayWeak.expired(),
            "boxed value slot roots managed values");
    }
    ZeusGC::collect();
    check(arrayWeak.expired(),
        "boxed value slot releases managed roots on destruction");
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

void testContainerCycles() {
    const std::size_t baseline = ZeusGC::liveNodeCount();
    std::weak_ptr<ZeusType::ArrayData> selfArrayWeak;
    std::weak_ptr<ZeusType::ObjectData> selfDictWeak;
    {
        auto data = ZeusGC::makeArrayData();
        ZeusType::Array self;
        self.impl = data;
        data->data.emplace_back(ZeusType::Variant(self));
        selfArrayWeak = data;

        ZeusType::UniversalVariant root = ZeusType::Variant(self);
        ZeusGC::collect();
        check(!selfArrayWeak.expired(), "rooted self-array cycle survives collection");
        root = LexerType::Value(0);
        self.impl.reset();
        data.reset();
    }
    ZeusGC::collect();
    check(selfArrayWeak.expired(), "unrooted self-array cycle is reclaimed");

    {
        auto data = ZeusGC::makeObjectData();
        ZeusType::Object self;
        self.impl = data;
        data->setValue("self", ZeusType::Variant(self));
        selfDictWeak = data;

        ZeusType::UniversalVariant root = ZeusType::Variant(self);
        ZeusGC::collect();
        check(!selfDictWeak.expired(), "rooted self-dictionary cycle survives collection");
        root = LexerType::Value(0);
        self.impl.reset();
        data.reset();
    }
    ZeusGC::collect();
    check(selfDictWeak.expired(), "unrooted self-dictionary cycle is reclaimed");

    std::weak_ptr<ZeusType::ArrayData> arrayWeak;
    std::weak_ptr<ZeusType::ObjectData> dictWeak;
    {
        auto arrayData = ZeusGC::makeArrayData();
        auto dictData = ZeusGC::makeObjectData();
        ZeusType::Array array;
        ZeusType::Object dict;
        array.impl = arrayData;
        dict.impl = dictData;
        arrayData->data.emplace_back(ZeusType::Variant(dict));
        dictData->setValue("array", ZeusType::Variant(array));
        arrayWeak = arrayData;
        dictWeak = dictData;

        ZeusType::UniversalVariant root = ZeusType::Variant(array);
        ZeusGC::collect();
        check(!arrayWeak.expired() && !dictWeak.expired(),
            "rooted mixed array-dictionary cycle survives collection");
        root = LexerType::Value(0);
        array.impl.reset();
        dict.impl.reset();
        arrayData.reset();
        dictData.reset();
    }
    ZeusGC::collect();
    check(arrayWeak.expired() && dictWeak.expired(),
        "unrooted mixed array-dictionary cycle is reclaimed");
    check(ZeusGC::liveNodeCount() == baseline,
        "container cycle collection returns to the prior node count");
}

void testOrdinaryContainersRelease() {
    std::weak_ptr<ZeusType::ArrayData> arrayWeak;
    std::weak_ptr<ZeusType::ObjectData> dictWeak;
    {
        auto array = ZeusGC::makeArrayData();
        auto dict = ZeusGC::makeObjectData();
        ZeusType::Array child;
        child.impl = array;
        dict->setValue("child", ZeusType::Variant(child));
        arrayWeak = array;
        dictWeak = dict;
    }
    check(arrayWeak.expired() && dictWeak.expired(),
        "acyclic nested array and dictionary release normally");
    check(ZeusGC::liveNodeCount() == 0,
        "expired acyclic container nodes are removed from the registry");
}

void testValueAbiContainerCycles() {
    const std::size_t baseline = ZeusGC::liveNodeCount();
    ZeusValue array{};
    ZeusValue result{};
    ZeusValue key{};
    ZeusValue dict{};
    ZeusValue dictResult{};
    zeus_value_init_nil(&array);
    zeus_value_init_nil(&result);
    zeus_value_init_nil(&key);
    zeus_value_init_nil(&dict);
    zeus_value_init_nil(&dictResult);

    zeus_value_array_build(&array, 0, nullptr);
    zeus_value_array_push(&result, &array, &array);
    zeus_value_release(&result);
    zeus_value_release(&array);
    zeus_gc_collect();
    check(ZeusGC::liveNodeCount() == baseline,
        "ZeusValue ABI collects a self-referential array");

    zeus_value_from_cstr(&key, "self");
    zeus_value_dict_build(&dict, 0, nullptr);
    zeus_value_dict_store(&dictResult, &dict, &key, &dict);
    zeus_value_release(&dictResult);
    zeus_value_release(&key);
    zeus_value_release(&dict);
    zeus_gc_collect();
    check(ZeusGC::liveNodeCount() == baseline,
        "ZeusValue ABI collects a self-referential dictionary");
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
    check(live < 1024,
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
    testDirectValueSlots();
    testSelfCycle();
    testMutualCycle();
    testContainerCycles();
    testOrdinaryContainersRelease();
    testValueAbiContainerCycles();
    testLiveClosureSurvives();
    testAllocationTriggeredCollection();
    testConcurrentStoreAndCollect();

    std::printf("\nPASS = %d FAIL = %d TOTAL = %d\n",
        g_checks - g_failures, g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
