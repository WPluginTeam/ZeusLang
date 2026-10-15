// ZeusRuntimeLib/ZeusABI.cpp
//
// Phase A：extern "C" ABI 的第一批实现。全部基于本目录已有的语义构件
// （UniversalVariant / BigInteger 小整数快路径 / ZeusVMUtils 转换工具），
// 不依赖解释器调度（ZeusRunner/ZeusVM）。
//
// 所有权规则：
// - 所有返回 ZeusBox* 的函数都返回新装箱，调用方拥有（zeus_box_release）。
// - zeus_global_store 拷贝值入槽并吃掉传入的 box。
// - VM ObjectHandle/闭包句柄仍不跨 ABI 传递；LLVM class 指针只通过非拥有的
//   typed handle 在动态 ZeusValue 与原生 class ptr 之间往返。

#include "ZeusABI.hpp"
#include "ZeusLValueABI.hpp"
#include "ZeusObjectLifetime.hpp"
#include "ZeusRuntimeType.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusDynamicBinaryABI.hpp"
#include "ZeusDynamicMemberABI.hpp"

#include "ZeusVariant.hpp"
#include "ZeusSharedHeap.hpp"
#include "ZeusVMUtils.hpp"
#include "ZeusContainerOps.hpp"
#include "ZeusCell.hpp"
#include "ZeusNativeAddress.hpp"
#include "ZeusMemoryABI.hpp"

#include <cstdio>
#include <cerrno>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <exception>
#include <functional>
#include <stdexcept>
#include <new>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
struct ZeusRaisedValue {
    ZeusValue value;
    explicit ZeusRaisedValue(const ZeusValue* source) {
        zeus_value_clone(&value, source);
    }
    ZeusRaisedValue(const ZeusRaisedValue& other) {
        zeus_value_clone(&value, &other.value);
    }
    ~ZeusRaisedValue() { zeus_value_release(&value); }
};

class ZeusTypeError : public std::runtime_error {
public:
    explicit ZeusTypeError(const std::string& message)
        : std::runtime_error(message) {}
};

ZeusValue* abiResolveGlobalValueSlot(int idx, bool recreateDeleted);
void abiStoreGlobalValue(int idx, ZeusValue* value);
void abiLoadGlobalValue(ZeusValue* out, int idx);
void abiClearGlobalValue(int idx);
void abiDeleteGlobalValue(int idx);
void abiValidateNativeStringGlobalStore(const ZeusValue* slot, const ZeusValue* value);
void abiSyncNativeStringGlobalValueSlot(const ZeusValue* slot);
void abiValidateNativeBytesGlobalStore(const ZeusValue* slot, const ZeusValue* value);
void abiSyncNativeBytesGlobalValueSlot(const ZeusValue* slot);
}

namespace {
void writeFatalBytes(const char* bytes, std::size_t length) noexcept {
    while (length != 0) {
#ifdef _WIN32
        const int written = ::_write(2, bytes, static_cast<unsigned int>(length));
#else
        const auto written = ::write(STDERR_FILENO, bytes, length);
#endif
        if (written <= 0) {
            if (written < 0 && errno == EINTR) continue;
            return;
        }
        bytes += written;
        length -= static_cast<std::size_t>(written);
    }
}

void writeFatalLabel(const char* value) noexcept {
    if (!value || !*value) return;
    constexpr std::size_t maxLabelLength = 128;
    std::size_t length = 0;
    while (length < maxLabelLength && value[length] != '\0') ++length;
    writeFatalBytes(value, length);
}
}

extern "C" [[noreturn]] void zeus_oom_abort(
    const char* operation,
    const char* type_name
) noexcept {
    static constexpr char message[] = "Zeus fatal: out of memory";
    static constexpr char during[] = " during ";
    static constexpr char typePrefix[] = " (type: ";
    static constexpr char closing[] = ")\n";
    writeFatalBytes(message, sizeof(message) - 1);
    if (operation && *operation) {
        writeFatalBytes(during, sizeof(during) - 1);
        writeFatalLabel(operation);
    }
    if (type_name && *type_name) {
        writeFatalBytes(typePrefix, sizeof(typePrefix) - 1);
        writeFatalLabel(type_name);
        writeFatalBytes(closing, sizeof(closing) - 1);
    } else {
        static constexpr char newline[] = "\n";
        writeFatalBytes(newline, sizeof(newline) - 1);
    }
    std::abort();
}

extern "C" void zeus_exception_raise(const ZeusValue* value) noexcept(false) {
    if (!value) throw std::runtime_error("throw requires a value");
    throw ZeusRaisedValue(value);
}

extern "C" void zeus_exception_raise_owned(ZeusValue* value) noexcept(false) {
    if (!value) throw std::runtime_error("throw requires a value");
    ZeusRaisedValue raised(value);
    zeus_value_release(value);
    throw raised;
}

extern "C" void zeus_exception_capture(
    ZeusValue* out,
    ZeusRuntimeErrorFactory runtimeFactory,
    ZeusRuntimeErrorFactory typeErrorFactory
) noexcept(false) {
    if (!out) return;
    const std::exception_ptr current = std::current_exception();
    if (!current) throw std::runtime_error("catch has no active exception");
    try {
        std::rethrow_exception(current);
    } catch (const ZeusRaisedValue& raised) {
        zeus_value_clone(out, &raised.value);
    } catch (const ZeusTypeError& error) {
        const char* message = error.what();
        if (!typeErrorFactory) throw std::runtime_error("TypeError factory is unavailable");
        typeErrorFactory(out, message, static_cast<int64_t>(std::strlen(message)));
    } catch (const std::exception& error) {
        const char* message = error.what();
        if (!runtimeFactory) throw std::runtime_error("RuntimeError factory is unavailable");
        runtimeFactory(out, message, static_cast<int64_t>(std::strlen(message)));
    } catch (...) {
        static constexpr char message[] = "Unknown runtime error";
        if (!runtimeFactory) throw std::runtime_error("RuntimeError factory is unavailable");
        runtimeFactory(out, message, sizeof(message) - 1);
    }
}

extern "C" void zeus_exception_report_current() noexcept {
    try {
        const std::exception_ptr current = std::current_exception();
        if (!current) return;
        std::rethrow_exception(current);
    } catch (const ZeusRaisedValue& raised) {
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
            zeus_value_to_box(&raised.value), &zeus_box_release);
        std::fprintf(stderr, "Unhandled Zeus exception: %s\n",
            ZeusVMUtils::valueToDebugString(boxed->v).c_str());
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Unhandled Zeus exception: %s\n", error.what());
    } catch (...) {
        std::fprintf(stderr, "Unhandled Zeus exception of unknown type\n");
    }
}

extern "C" void zeus_i64_check_division(
    int64_t left, int64_t right, int32_t line
) noexcept(false) {
    if (right == 0) {
        throw std::runtime_error("division by zero at line " + std::to_string(line));
    }
    if (left == std::numeric_limits<int64_t>::min() && right == -1) {
        throw std::runtime_error("integer division overflow at line " +
            std::to_string(line));
    }
}

extern "C" void zeus_class_field_require_initialized(
    int32_t initialized, const char* class_name, const char* field_name, int32_t line
) noexcept(false) {
    if (initialized) return;
    throw std::runtime_error("Zeus runtime error at line " +
        std::to_string(line) + ": class " +
        (class_name ? class_name : "<unknown>") + " has no field '" +
        (field_name ? field_name : "<unknown>") + "'");
}

namespace {

static_assert(sizeof(ZeusValue) == 16, "ZeusValue ABI size must remain 16 bytes");
static_assert(offsetof(ZeusValue, payload) == 8, "ZeusValue payload offset must remain 8");
static_assert(offsetof(ZeusValue, int64_payload) == 8, "ZeusValue int64 payload offset must remain 8");
static_assert(offsetof(ZeusValue, bool_payload) == 8, "ZeusValue bool payload offset must remain 8");
static_assert(offsetof(ZeusValue, char_payload) == 8, "ZeusValue char payload offset must remain 8");
static_assert(sizeof(uintptr_t) <= sizeof(uint64_t), "ZeusValue payload cannot hold a pointer");

[[noreturn]] void dynamicValuePanic(const char* message) {
    throw std::runtime_error(std::string("[ZeusValue ABI] ") + message);
}

void setNilValue(ZeusValue* out) {
    if (!out) dynamicValuePanic("null output pointer");
    out->tag = ZEUS_VALUE_TAG_NIL;
    out->flags = 0;
    out->payload = 0;
}

bool isOwnedBufferTag(uint32_t tag) noexcept {
    return tag == ZEUS_VALUE_TAG_STRING || tag == ZEUS_VALUE_TAG_BYTES;
}

uint8_t* requireOwnedBufferData(
    const ZeusValue* value, uint32_t expectedTag, uint64_t* outLength = nullptr
) {
    if (!value || value->tag != expectedTag ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BUFFER || value->payload == 0) {
        dynamicValuePanic("invalid owned string/bytes buffer value");
    }
    const uintptr_t dataAddress = static_cast<uintptr_t>(value->payload);
    if (dataAddress < sizeof(uint64_t)) {
        dynamicValuePanic("owned string/bytes buffer header address underflows");
    }
    auto* data = reinterpret_cast<uint8_t*>(dataAddress);
    auto* allocation = reinterpret_cast<void*>(dataAddress - sizeof(uint64_t));
    if (!zeus_memory_find_allocation(allocation, sizeof(uint64_t))) {
        dynamicValuePanic("owned string/bytes buffer header is not a live allocation");
    }
    uint64_t length = 0;
    std::memcpy(&length, allocation, sizeof(length));
    const uint64_t terminator = expectedTag == ZEUS_VALUE_TAG_STRING ? 1 : 0;
    if (length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        length > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t) - terminator) {
        dynamicValuePanic("owned string/bytes buffer length is invalid");
    }
    const uint64_t allocationSize = sizeof(uint64_t) + length + terminator;
    if (zeus_memory_find_allocation(allocation, allocationSize) != allocation) {
        dynamicValuePanic("owned string/bytes buffer is not a live allocation");
    }
    if (terminator != 0 && data[length] != 0) {
        dynamicValuePanic("owned string buffer is missing its terminator");
    }
    if (outLength) *outLength = length;
    return data;
}

void initializeOwnedBuffer(
    ZeusValue* out, uint32_t tag, const uint8_t* source, uint64_t length
) {
    if (!out || !isOwnedBufferTag(tag) || (length != 0 && !source)) {
        dynamicValuePanic("invalid owned string/bytes buffer input");
    }
    const uint64_t terminator = tag == ZEUS_VALUE_TAG_STRING ? 1 : 0;
    if (length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        length > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t) - terminator) {
        dynamicValuePanic("owned string/bytes buffer is too large");
    }
    const uint64_t allocationSize = sizeof(uint64_t) + length + terminator;
    const int64_t rawAllocation = __zeus_malloc(static_cast<int64_t>(allocationSize));
    if (rawAllocation == 0) {
        zeus_oom_abort("allocating dynamic string/bytes", tag == ZEUS_VALUE_TAG_STRING
            ? "string" : "bytes");
    }
    auto* allocation = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(rawAllocation));
    std::memcpy(allocation, &length, sizeof(length));
    uint8_t* data = allocation + sizeof(uint64_t);
    if (length != 0) std::memcpy(data, source, static_cast<size_t>(length));
    if (terminator != 0) data[length] = 0;
    out->tag = tag;
    out->flags = ZEUS_VALUE_FLAG_OWNS_BUFFER;
    out->payload = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data));
}

void cloneOwnedBuffer(ZeusValue* out, const ZeusValue* value) {
    uint64_t length = 0;
    const uint8_t* data = requireOwnedBufferData(value, value->tag, &length);
    initializeOwnedBuffer(out, value->tag, data, length);
}

template <typename T>
T* requireOwnedNativePayload(const ZeusValue* value, uint32_t expectedTag) {
    const uint32_t allowedFlags = ZEUS_VALUE_FLAG_OWNS_NATIVE_PAYLOAD |
        ZEUS_VALUE_FLAG_LEXER_VALUE;
    if (!value || value->tag != expectedTag ||
        (value->flags & ZEUS_VALUE_FLAG_OWNS_NATIVE_PAYLOAD) == 0 ||
        (value->flags & ~allowedFlags) != 0 || value->payload == 0) {
        dynamicValuePanic("invalid typed native value payload");
    }
    void* payload = reinterpret_cast<void*>(
        static_cast<uintptr_t>(value->payload));
    if (zeus_memory_find_allocation(payload, sizeof(T)) != payload) {
        dynamicValuePanic("typed native value payload is not a live allocation");
    }
    return static_cast<T*>(payload);
}

template <typename T>
void initializeOwnedNativePayload(
    ZeusValue* out,
    uint32_t tag,
    const T& source,
    bool cameFromLexerValue
) {
    if (!out || (tag != ZEUS_VALUE_TAG_BIGINT &&
                 tag != ZEUS_VALUE_TAG_BIGDECIMAL)) {
        dynamicValuePanic("invalid typed native value payload input");
    }
    const int64_t rawAllocation = __zeus_malloc(static_cast<int64_t>(sizeof(T)));
    if (rawAllocation == 0) {
        zeus_oom_abort("allocating dynamic numeric payload",
            tag == ZEUS_VALUE_TAG_BIGINT ? "BigInteger" : "BigDecimal");
    }
    void* allocation = reinterpret_cast<void*>(
        static_cast<uintptr_t>(rawAllocation));
    try {
        new (allocation) T(source);
    } catch (const std::bad_alloc&) {
        (void)__zeus_free(rawAllocation);
        zeus_oom_abort("copying dynamic numeric payload",
            tag == ZEUS_VALUE_TAG_BIGINT ? "BigInteger" : "BigDecimal");
    } catch (...) {
        (void)__zeus_free(rawAllocation);
        throw;
    }
    out->tag = tag;
    out->flags = ZEUS_VALUE_FLAG_OWNS_NATIVE_PAYLOAD |
        (cameFromLexerValue ? ZEUS_VALUE_FLAG_LEXER_VALUE : 0u);
    out->payload = static_cast<uint64_t>(rawAllocation);
}

template <typename T>
void cloneOwnedNativePayload(ZeusValue* out, const ZeusValue* value) {
    const T* source = requireOwnedNativePayload<T>(value, value->tag);
    initializeOwnedNativePayload(out, value->tag, *source,
        (value->flags & ZEUS_VALUE_FLAG_LEXER_VALUE) != 0);
}

std::unordered_set<const ZeusValue*>& gcEdgeValueSlots() {
    // Slot owners can be torn down from static runtime objects in other
    // translation units; keep this registry alive until process exit.
    static auto* slots = new std::unordered_set<const ZeusValue*>();
    return *slots;
}

void releaseSlotValue(ZeusValue* slot) {
    if (!slot) dynamicValuePanic("null value slot");
    zeus_value_release_internal(slot);
}

const LexerType::Value* lexerValue(const ZeusType::UniversalVariant& value) {
    if (!std::holds_alternative<LexerType::Value>(value)) return nullptr;
    return &std::get<LexerType::Value>(value);
}

bool isBooleanValue(const ZeusType::UniversalVariant& value) {
    if (std::holds_alternative<bool>(value)) return true;
    const auto* literal = lexerValue(value);
    return literal && std::holds_alternative<bool>(literal->data);
}

bool isBigIntegerValue(const ZeusType::UniversalVariant& value) {
    if (std::holds_alternative<ZeusType::BigInteger>(value)) return true;
    const auto* literal = lexerValue(value);
    return literal && std::holds_alternative<ZeusType::BigInteger>(literal->data);
}

bool isBigDecimalValue(const ZeusType::UniversalVariant& value) {
    if (std::holds_alternative<ZeusType::BigDecimal>(value)) return true;
    const auto* literal = lexerValue(value);
    return literal && std::holds_alternative<ZeusType::BigDecimal>(literal->data);
}

const ZeusType::BigInteger* bigIntegerUniversalValue(
    const ZeusType::UniversalVariant& value
) {
    if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value)) {
        return integer;
    }
    const auto* literal = lexerValue(value);
    return literal ? std::get_if<ZeusType::BigInteger>(&literal->data) : nullptr;
}

const ZeusType::BigDecimal* bigDecimalUniversalValue(
    const ZeusType::UniversalVariant& value
) {
    if (const auto* decimal = std::get_if<ZeusType::BigDecimal>(&value)) {
        return decimal;
    }
    const auto* literal = lexerValue(value);
    return literal ? std::get_if<ZeusType::BigDecimal>(&literal->data) : nullptr;
}

bool isStringUniversalValue(const ZeusType::UniversalVariant& value) {
    if (std::holds_alternative<std::string>(value)) return true;
    const auto* literal = lexerValue(value);
    return literal && std::holds_alternative<std::string>(literal->data);
}

const std::string* stringUniversalValue(const ZeusType::UniversalVariant& value) {
    if (std::holds_alternative<std::string>(value)) {
        return &std::get<std::string>(value);
    }
    const auto* literal = lexerValue(value);
    if (literal && std::holds_alternative<std::string>(literal->data)) {
        return &std::get<std::string>(literal->data);
    }
    return nullptr;
}

std::string* mutableStringUniversalValue(ZeusType::UniversalVariant& value) {
    if (auto* text = std::get_if<std::string>(&value)) return text;
    auto* literal = std::get_if<LexerType::Value>(&value);
    return literal ? std::get_if<std::string>(&literal->data) : nullptr;
}

const ZeusType::Bytes* bytesUniversalValue(const ZeusType::UniversalVariant& value) {
    if (const auto* bytes = std::get_if<ZeusType::Bytes>(&value)) return bytes;
    if (const auto* variant = std::get_if<ZeusType::Variant>(&value)) {
        return std::get_if<ZeusType::Bytes>(variant);
    }
    return nullptr;
}

const ZeusType::RawBytesRange* rawBytesRangeUniversalValue(
    const ZeusType::UniversalVariant& value
) {
    return std::get_if<ZeusType::RawBytesRange>(&value);
}

std::mutex& nativeStringPoolMutex() {
    static std::mutex mutex;
    return mutex;
}

struct NativeStringPoolEntry {
    std::string originalBytes;
    std::vector<uint64_t> allocation;

    ZeusNativeString address() const noexcept {
        if (allocation.empty()) return nullptr;
        return reinterpret_cast<const char*>(allocation.data()) + sizeof(uint64_t);
    }

    std::string_view contents() const noexcept {
        const char* data = address();
        if (!data) return {};
        uint64_t length = 0;
        std::memcpy(&length, data - sizeof(uint64_t), sizeof(length));
        return std::string_view(data, static_cast<size_t>(length));
    }

    void assign(std::string text) {
        originalBytes = std::move(text);
        if (originalBytes.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            dynamicValuePanic("native string result is too large");
        }
        const size_t byteCount = sizeof(uint64_t) + originalBytes.size() + 1;
        allocation.assign((byteCount + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
        const uint64_t length = static_cast<uint64_t>(originalBytes.size());
        std::memcpy(allocation.data(), &length, sizeof(length));
        char* characters = reinterpret_cast<char*>(allocation.data()) + sizeof(uint64_t);
        if (!originalBytes.empty()) {
            std::memcpy(characters, originalBytes.data(), originalBytes.size());
        }
        characters[originalBytes.size()] = '\0';
    }
};

std::unordered_map<size_t, std::vector<std::unique_ptr<NativeStringPoolEntry>>>&
nativeStringPool() {
    static std::unordered_map<
        size_t, std::vector<std::unique_ptr<NativeStringPoolEntry>>> strings;
    return strings;
}

std::vector<std::unique_ptr<NativeStringPoolEntry>>& nativeStringCopies() {
    static std::vector<std::unique_ptr<NativeStringPoolEntry>> strings;
    return strings;
}

std::unordered_set<const char*>& nativeStringAddresses() {
    static std::unordered_set<const char*> addresses;
    return addresses;
}

std::string_view nativeStringContents(ZeusNativeString value) {
    if (!value) dynamicValuePanic("null native string pointer");
    uint64_t length = 0;
    std::memcpy(&length, value - sizeof(uint64_t), sizeof(length));
    if (length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        dynamicValuePanic("invalid native string length");
    }
    return std::string_view(value, static_cast<size_t>(length));
}

void nativeStringValidateImpl(ZeusNativeString value) {
    if (!value) dynamicValuePanic("null native string pointer");
    std::lock_guard<std::mutex> lock(nativeStringPoolMutex());
    if (nativeStringAddresses().find(value) == nativeStringAddresses().end()) {
        dynamicValuePanic("native string pointer is not owned by the string pool");
    }
}

ZeusNativeString internNativeString(std::string text) {
    std::lock_guard<std::mutex> lock(nativeStringPoolMutex());
    auto& pool = nativeStringPool();
    const size_t hash = std::hash<std::string_view>{}(text);
    auto& candidates = pool[hash];
    for (const auto& candidate : candidates) {
        if (candidate->contents() == text) return candidate->address();
    }
    auto entry = std::make_unique<NativeStringPoolEntry>();
    entry->assign(std::move(text));
    const ZeusNativeString value = entry->address();
    nativeStringAddresses().insert(value);
    candidates.push_back(std::move(entry));
    return value;
}

ZeusNativeString copyNativeString(std::string text) {
    std::lock_guard<std::mutex> lock(nativeStringPoolMutex());
    auto entry = std::make_unique<NativeStringPoolEntry>();
    entry->assign(std::move(text));
    const ZeusNativeString value = entry->address();
    nativeStringAddresses().insert(value);
    nativeStringCopies().push_back(std::move(entry));
    return value;
}

template <typename Builder>
ZeusNativeString buildNativeString(Builder&& builder) {
    try {
        const std::string result = builder();
        if (result.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            dynamicValuePanic("native string result is too large");
        }
        return internNativeString(result);
    } catch (...) {
        dynamicValuePanic("failed to build native string result");
    }
}

// Native Bytes blocks are self-describing: [uint64 length][payload...].
// The header replaces the old pointer->allocation registry: length lookups
// and release are plain memory operations without a global map or lock.
uint64_t nativeBytesLength(ZeusNativeBytes data) {
    if (!data) dynamicValuePanic("null native Bytes data pointer");
    uint64_t headerLength = 0;
    std::memcpy(&headerLength, data - sizeof(uint64_t), sizeof(headerLength));
    return headerLength;
}

ZeusNativeBytes allocateNativeBytes(const uint8_t* data, uint64_t length) {
    if (length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - sizeof(uint64_t)) ||
        (length > 0 && !data)) {
        dynamicValuePanic("invalid native Bytes allocation size or source");
    }
    uint8_t* storage = new uint8_t[
        sizeof(uint64_t) + static_cast<size_t>(length)];
    std::memcpy(storage, &length, sizeof(length));
    uint8_t* result = storage + sizeof(uint64_t);
    if (length > 0) {
        std::memcpy(result, data, static_cast<size_t>(length));
    }
    return result;
}

void releaseNativeBytes(ZeusNativeBytes data) {
    if (!data) return;
    zeus_lvalue_unregister_revealed(const_cast<uint8_t*>(data));
    delete[] const_cast<uint8_t*>(data - sizeof(uint64_t));
}

bool isBoxTag(uint32_t tag) {
    return tag == ZEUS_VALUE_TAG_BOXED;
}

std::mutex& llvmClassRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::atomic<uint64_t>& nextLLVMClassDebugId() {
    static std::atomic<uint64_t> nextId{ 1 };
    return nextId;
}

std::unordered_map<void*, std::shared_ptr<ZeusType::LLVMClassObjectState>>&
llvmClassRegistry() {
    static std::unordered_map<void*, std::shared_ptr<ZeusType::LLVMClassObjectState>> registry;
    return registry;
}

void traceLLVMClassObjectNode(void* payload, const ZeusGC::Visitor& visitor) {
    auto* state = static_cast<ZeusType::LLVMClassObjectState*>(payload);
    void* object = state->object.load(std::memory_order_acquire);
    if (!object) return;
    zeus_value_dynamic_field_trace(
        object,
          state->typeName.c_str(),
        [](void* context, const ZeusValue* value) {
            if (!value) return;
            static_cast<const ZeusGC::Visitor*>(context)->visitValue(value);
        },
        const_cast<ZeusGC::Visitor*>(&visitor)
    );
}

void breakLLVMClassObjectCycles(void* payload) {
    auto* state = static_cast<ZeusType::LLVMClassObjectState*>(payload);
    void* object = state->object.load(std::memory_order_acquire);
    if (!object) return;
    try {
        // Run the same finalizer as an ordinary last-reference release, which
        // clears its field sidecar and severs edges back into managed containers.
        zeus_class_ref_destroy_now(object);
    } catch (...) {
        zeus_exception_report_current();
    }
}

bool hasExternalRootsLLVMClassObject(void* payload, std::size_t internalReferences) {
    (void)internalReferences;
    auto* state = static_cast<ZeusType::LLVMClassObjectState*>(payload);
    void* object = state->object.load(std::memory_order_acquire);
    (void)internalReferences;
    return object && zeus_class_ref_external_reference_count(object) > 0;
}

std::shared_ptr<ZeusType::LLVMClassObjectState> getOrCreateLLVMClassState(
    void* object,
    const char* typeName
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!object || !typeName || !*typeName) {
        dynamicValuePanic("class pointer and type name must be non-null");
    }

    std::lock_guard<std::mutex> lock(llvmClassRegistryMutex());
    auto& registry = llvmClassRegistry();
    auto found = registry.find(object);
    if (found != registry.end()) {
        auto existing = found->second;
        if (existing) {
            if (existing->typeName != typeName ||
                existing->object.load(std::memory_order_acquire) != object) {
                dynamicValuePanic("class pointer was registered with a different type");
            }
            return existing;
        }
    }

    auto state = std::make_shared<ZeusType::LLVMClassObjectState>();
    state->typeName = typeName;
    state->debugId = nextLLVMClassDebugId().fetch_add(1, std::memory_order_relaxed);
    state->object.store(object, std::memory_order_release);
    zeus_value_dynamic_field_prepare(object, typeName);
    if (zeus_class_ref_try_retain(object)) {
        state->lifetime = std::shared_ptr<void>(object, [](void* retainedObject) noexcept {
            try {
                zeus_class_ref_release_lease(retainedObject);
            } catch (...) {
                std::abort();
            }
        });
    }
    registry[object] = state;
    ZeusGC::registerSharedNode(
        std::static_pointer_cast<void>(state),
        traceLLVMClassObjectNode,
        breakLLVMClassObjectCycles,
        hasExternalRootsLLVMClassObject,
        1
    );
    return state;
}

void* resolveLLVMClassPointer(
    const ZeusType::LLVMClassHandle& handle,
    const char* expectedType
) {
    if (!handle.state || !expectedType || !*expectedType) {
        dynamicValuePanic("invalid LLVM class handle");
    }
    if (handle.state->typeName != expectedType) {
        dynamicValuePanic("LLVM class handle type mismatch");
    }
    void* object = handle.state->object.load(std::memory_order_acquire);
    if (!object) dynamicValuePanic("LLVM class object has already been deleted");
    return object;
}

bool isLiveLLVMClassHandle(const ZeusType::LLVMClassHandle& handle) {
    if (!handle.state) {
        dynamicValuePanic("invalid LLVM class handle");
    }
    if (!handle.state->object.load(std::memory_order_acquire)) {
        dynamicValuePanic("LLVM class object has already been deleted");
    }
    return true;
}

bool isLLVMClassValue(const ZeusBox* box) {
    return box && std::holds_alternative<ZeusType::LLVMClassHandle>(box->v);
}

void destroyLLVMClassValue(const ZeusType::UniversalVariant& value) {
    const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&value);
    if (!handle || !handle->state) return;
    void* object = handle->state->object.load(std::memory_order_acquire);
    if (object) zeus_class_ref_destroy_now(object);
}

ZeusBox* valueReference(const ZeusValue* value);

// A native String instance and a boxed string are both usable as text, but the
// native one only exposes its bytes through the registered toString method, so
// it has to travel the string ABI instead of the numeric fast paths.
bool isNativeStringValue(const ZeusValue* value) {
    if (!value) return false;
    if (value->tag == ZEUS_VALUE_TAG_STRING) {
        return value->flags == ZEUS_VALUE_FLAG_OWNS_BUFFER && value->payload != 0;
    }
    if (value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) return false;
    if (value->tag != ZEUS_VALUE_TAG_BOXED) return false;
    const ZeusType::UniversalVariant& boxed = valueReference(value)->v;
    const auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&boxed);
    return classHandle && classHandle->state &&
           classHandle->state->typeName == "String";
}

void concatNativeStrings(ZeusValue* out, const ZeusValue* lhs, const ZeusValue* rhs) {
    const std::string_view left = nativeStringContents(zeus_value_intern_string(lhs));
    const std::string_view right = nativeStringContents(zeus_value_intern_string(rhs));
    try {
        std::string joined;
        joined.reserve(left.size() + right.size());
        joined.append(left.data(), left.size());
        joined.append(right.data(), right.size());
        zeus_value_from_string_data(out, joined.data(),
            static_cast<int64_t>(joined.size()));
    } catch (...) {
        dynamicValuePanic("failed to concatenate native strings");
    }
}

// Returns true when the operands were handled as text, so the caller only
// rejects class values that really have no dynamic operator.
bool tryNativeStringConcat(ZeusValue* out, const ZeusValue* lhs, const ZeusValue* rhs) {
    if (!isNativeStringValue(lhs) || !isNativeStringValue(rhs)) return false;
    concatNativeStrings(out, lhs, rhs);
    return true;
}

// A native class handle is a non-owning typed pointer, so equality is identity
// on the underlying object. A native String still compares equal to text, so a
// String instance and a boxed string with the same bytes are equal.
const void* nativeClassObject(const ZeusValue* value) {
    if (!value || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX ||
        value->tag != ZEUS_VALUE_TAG_BOXED) {
        return nullptr;
    }
    const ZeusType::UniversalVariant& boxed = valueReference(value)->v;
    const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&boxed);
    if (!handle || !handle->state) return nullptr;
    return handle->state->object.load(std::memory_order_acquire);
}

bool boxedStringText(const ZeusValue* value, std::string& out) {
    if (!value) return false;
    if (value->tag == ZEUS_VALUE_TAG_STRING) {
        uint64_t length = 0;
        const uint8_t* data = requireOwnedBufferData(
            value, ZEUS_VALUE_TAG_STRING, &length);
        out.assign(reinterpret_cast<const char*>(data), static_cast<size_t>(length));
        return true;
    }
    if (value->flags != ZEUS_VALUE_FLAG_OWNS_BOX ||
        value->tag != ZEUS_VALUE_TAG_BOXED) return false;
    const ZeusType::UniversalVariant& boxed = valueReference(value)->v;
    if (std::holds_alternative<ZeusType::LLVMClassHandle>(boxed)) return false;
    const auto* text = stringUniversalValue(boxed);
    if (!text) return false;
    out = *text;
    return true;
}

bool nativeStringEqualsText(const ZeusValue* value, const std::string& text) {
    return nativeStringContents(zeus_value_intern_string(value)) == text;
}

bool tryNativeClassEquality(ZeusValue* out, int32_t opcode,
                            const ZeusValue* lhs, const ZeusValue* rhs) {
    const void* left = nativeClassObject(lhs);
    const void* right = nativeClassObject(rhs);
    if (!left && !right) return false;

    bool equal = left != nullptr && left == right;
    if (!equal) {
        std::string text;
        if (left && !right && boxedStringText(rhs, text)) {
            equal = nativeStringEqualsText(lhs, text);
        } else if (right && !left && boxedStringText(lhs, text)) {
            equal = nativeStringEqualsText(rhs, text);
        }
    }
    zeus_value_init_bool(out, (opcode == ZEUS_BINARY_EQUAL ? equal : !equal) ? 1 : 0);
    return true;
}

ZeusBox* valueReference(const ZeusValue* value) {
    if (!value || !isBoxTag(value->tag) ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX || value->payload == 0) {
        dynamicValuePanic("invalid owned reference value");
    }
    return reinterpret_cast<ZeusBox*>(static_cast<uintptr_t>(value->payload));
}

const ZeusType::Object* nativeDictionaryFromValue(const ZeusValue* value) {
    if (!value) dynamicValuePanic("null dictionary lookup input");
    if (value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (value->tag != ZEUS_VALUE_TAG_BOXED) {
        dynamicValuePanic("dictionary lookup input must be an object");
    }

    const auto* dynamic = std::get_if<ZeusType::Variant>(&valueReference(value)->v);
    const auto* dictionary = dynamic
        ? std::get_if<ZeusType::Object>(dynamic)
        : nullptr;
    if (!dictionary) {
        dynamicValuePanic("dictionary lookup input must be an object");
    }
    return dictionary;
}

std::string nativeNormalizedDictionaryKey(const ZeusValue* value) {
    if (!value) dynamicValuePanic("null normalized dictionary key");
    if (value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (value->tag == ZEUS_VALUE_TAG_STRING) {
        uint64_t length = 0;
        const uint8_t* data = requireOwnedBufferData(
            value, ZEUS_VALUE_TAG_STRING, &length);
        return std::string(reinterpret_cast<const char*>(data),
                           static_cast<size_t>(length));
    }
    if (value->tag != ZEUS_VALUE_TAG_BOXED ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
        dynamicValuePanic("dictionary lookup key must be a normalized string");
    }
    const auto* text = stringUniversalValue(valueReference(value)->v);
    if (!text) {
        dynamicValuePanic("dictionary lookup key must be a normalized string");
    }
    return *text;
}

const ZeusType::UniversalVariant* findNativeDictionaryEntry(
    const ZeusType::Object* dictionary,
    const std::string& key
) {
    if (!dictionary || !dictionary->impl) return nullptr;
    const auto found = dictionary->impl->data.find(key);
    return found == dictionary->impl->data.end() ? nullptr : &found->second;
}

std::string universalValueTypeName(const ZeusType::UniversalVariant& value) {
    if (isStringUniversalValue(value)) return "string";
    if (isBigIntegerValue(value)) return "bigint";
    if (isBigDecimalValue(value)) return "float";
    if (std::holds_alternative<bool>(value)) return "bool";
    if (std::holds_alternative<char>(value)) return "char";
    if (std::holds_alternative<ZeusType::BigInteger>(value)) return "bigint";
    if (std::holds_alternative<ZeusType::BigDecimal>(value)) return "float";
    if (std::holds_alternative<ZeusType::Bytes>(value)) return "bytes";
    if (std::holds_alternative<ZeusType::RawBytesRange>(value)) return "&bytes";
    if (std::holds_alternative<ZeusType::ReferenceHandle>(value)) {
        return "&" + std::get<ZeusType::ReferenceHandle>(value).typeName;
    }
    if (const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&value)) {
        return handle->state && !handle->state->typeName.empty()
            ? handle->state->typeName
            : "object";
    }
    if (std::holds_alternative<ZeusType::Variant>(value)) {
        const auto& variant = std::get<ZeusType::Variant>(value);
        if (std::holds_alternative<ZeusType::Null>(variant)) return "nil";
        if (std::holds_alternative<ZeusType::Array>(variant)) return "Array";
        if (std::holds_alternative<ZeusType::Bytes>(variant)) return "bytes";
        if (std::holds_alternative<ZeusType::Object>(variant)) return "object";
    }
    if (std::holds_alternative<ZeusType::ClosureHandle>(value) ||
        std::holds_alternative<ZeusType::NativeClosureHandle>(value) ||
        std::holds_alternative<ZeusType::NativeFuncHandle>(value)) {
        return "function";
    }
    if (std::holds_alternative<ZeusType::ClassHandle>(value) ||
        std::holds_alternative<ZeusType::SharedClassHandle>(value)) {
        return "class";
    }
    if (std::holds_alternative<ZeusType::NamespaceHandle>(value)) return "namespace";
    if (std::holds_alternative<ZeusType::ObjectHandle>(value) ||
        std::holds_alternative<ZeusType::SharedObjectHandle>(value)) {
        return "object";
    }
    if (std::holds_alternative<LexerType::Value>(value)) {
        const auto& literal = std::get<LexerType::Value>(value);
        if (std::holds_alternative<ZeusType::BigInteger>(literal.data)) return "int";
        if (std::holds_alternative<ZeusType::BigDecimal>(literal.data)) return "float";
        if (std::holds_alternative<bool>(literal.data)) return "bool";
        if (std::holds_alternative<std::string>(literal.data)) return "string";
    }
    return "unknown";
}

std::string dynamicValueTypeName(const ZeusValue* value) {
    if (!value) return "unknown";
    switch (value->tag) {
        case ZEUS_VALUE_TAG_NIL:
            return "nil";
        case ZEUS_VALUE_TAG_BOOL:
        case ZEUS_VALUE_TAG_REF_BOOL:
            return "bool";
        case ZEUS_VALUE_TAG_INT64:
            return "int";
        case ZEUS_VALUE_TAG_CHAR:
            return "char";
        case ZEUS_VALUE_TAG_REF_INT64:
            return "&int";
        case ZEUS_VALUE_TAG_DELETED:
            return "deleted";
        case ZEUS_VALUE_TAG_BIGINT:
            return "bigint";
        case ZEUS_VALUE_TAG_BIGDECIMAL:
            return "float";
        case ZEUS_VALUE_TAG_STRING:
            return "string";
        case ZEUS_VALUE_TAG_BYTES:
            return "bytes";
        case ZEUS_VALUE_TAG_BOXED:
            if (value->flags == ZEUS_VALUE_FLAG_OWNS_BOX) {
                return universalValueTypeName(valueReference(value)->v);
            }
            return "unknown";
        default:
            return "unknown";
    }
}

// 空值装箱（解释器内对应 Variant(Null)，打印为 nil）。
ZeusType::UniversalVariant nullVariant() {
    return ZeusType::UniversalVariant(ZeusType::Variant(ZeusType::Null()));
}

ZeusBox* nullBox() {
    ZeusBox* box = ZEUS_NEW_BOX();
    box->assignVariant(nullVariant());
    return box;
}

bool isStringValue(const LexerType::Value& v) {
    return std::holds_alternative<std::string>(v.data);
}

void printUniversalValue(const ZeusType::UniversalVariant& value) {
    if (std::holds_alternative<std::string>(value)) {
        std::cout << std::get<std::string>(value);
    } else if (std::holds_alternative<LexerType::Value>(value)) {
        const auto& literal = std::get<LexerType::Value>(value);
        if (std::holds_alternative<std::string>(literal.data)) {
            std::cout << std::get<std::string>(literal.data);
        } else {
            std::cout << literal.toString();
        }
    } else {
        std::cout << ZeusVMUtils::valueToDebugString(value);
    }
}

struct OwnedZeusValue {
    ZeusValue value{};

    OwnedZeusValue() { zeus_value_init_nil(&value); }
    ~OwnedZeusValue() { zeus_value_release(&value); }
};

void printDynamicValue(const ZeusValue* value) {
    if (!value) dynamicValuePanic("null value pointer while printing");

    const bool isLLVMClass = value->tag == ZEUS_VALUE_TAG_BOXED &&
        value->flags == ZEUS_VALUE_FLAG_OWNS_BOX &&
        isLLVMClassValue(valueReference(value));
    if (isLLVMClass) {
        OwnedZeusValue stringified;
        if (zeus_value_try_dynamic_member_call(
                &stringified.value, value, "toString", 0, nullptr)) {
            const std::string_view rendered =
                nativeStringContents(zeus_value_intern_string(&stringified.value));
            std::cout.write(rendered.data(),
                            static_cast<std::streamsize>(rendered.size()));
            return;
        }
    }

    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    printUniversalValue(boxed->v);
}

} // namespace

extern "C" void zeus_native_string_validate(ZeusNativeString value) {
    nativeStringValidateImpl(value);
}

ZeusBox* zeus_box_int(int64_t v) {
    ZeusBox* box = ZEUS_NEW_BOX();
    box->assignVariant(LexerType::Value(v)); // 小整数路径：零堆分配
    return box;
}

ZeusBox* zeus_box_bool(int32_t v) {
    ZeusBox* box = ZEUS_NEW_BOX();
    box->assignVariant(LexerType::Value(v != 0));
    return box;
}

ZeusBox* zeus_box_cstr(const char* s) {
    ZeusBox* box = ZEUS_NEW_BOX();
    box->assignVariant(LexerType::Value(s ? std::string(s) : std::string()));
    return box;
}

ZeusBox* zeus_null_box(void) {
    return nullBox();
}

void zeus_box_release(ZeusBox* box) {
    delete box; // UniversalVariant 析构负责释放 string/shared_ptr 等
}

int64_t zeus_unbox_int(const ZeusBox* box) {
    if (!box) return 0;

    int64_t value = 0;
    if (ZeusVMUtils::tryGetInt64(box->v, value)) return value;
    return 0;
}

int32_t zeus_unbox_bool(const ZeusBox* box) {
    if (!box) return 0;

    if (std::holds_alternative<LexerType::Value>(box->v)) {
        const auto& value = std::get<LexerType::Value>(box->v);
        if (std::holds_alternative<bool>(value.data)) {
            return std::get<bool>(value.data) ? 1 : 0;
        }
    }

    return 0;
}

void zeus_value_init_nil(ZeusValue* out) {
    setNilValue(out);
}

void zeus_value_init_i64(ZeusValue* out, int64_t value) {
    if (!out) dynamicValuePanic("null output pointer");
    out->tag = ZEUS_VALUE_TAG_INT64;
    out->flags = 0;
    out->int64_payload = value;
}

void zeus_value_init_bool(ZeusValue* out, int32_t value) {
    if (!out) dynamicValuePanic("null output pointer");
    out->tag = ZEUS_VALUE_TAG_BOOL;
    out->flags = 0;
    out->payload = 0;
    out->bool_payload = value != 0;
}

void zeus_value_from_i64_ptr(ZeusValue* out, void* value) {
    if (!out) dynamicValuePanic("null int reference output pointer");
    setNilValue(out);
    if (!value) return;
    out->tag = ZEUS_VALUE_TAG_REF_INT64;
    out->flags = 0;
    out->payload = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
}

void zeus_value_from_bool_ptr(ZeusValue* out, void* value) {
    if (!out) dynamicValuePanic("null bool reference output pointer");
    setNilValue(out);
    if (!value) return;
    out->tag = ZEUS_VALUE_TAG_REF_BOOL;
    out->flags = 0;
    out->payload = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(value));
}

// `rooted` boxes are caller-owned values and act as GC roots; unrooted boxes
// are graph edges. The distinction must survive every clone, otherwise a
// closure that captures itself would keep its own subgraph alive.
static void cloneDynamicValue(ZeusValue* out, const ZeusValue* value, bool rooted) {
    if (!out || !value) dynamicValuePanic("null clone pointer");
    if (out == value) return;
    if (value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    setNilValue(out);

    try {
        switch (value->tag) {
            case ZEUS_VALUE_TAG_NIL:
                if (value->flags != 0 || value->payload != 0) {
                    dynamicValuePanic("invalid nil value");
                }
                return;
            case ZEUS_VALUE_TAG_BOOL:
                zeus_value_init_bool(out, zeus_value_require_bool(value));
                return;
            case ZEUS_VALUE_TAG_CHAR:
                if (value->flags != 0) dynamicValuePanic("invalid inline char value");
                zeus_value_from_char(out, value->char_payload);
                return;
            case ZEUS_VALUE_TAG_INT64:
                zeus_value_init_i64(out, zeus_value_require_i64(value));
                return;
            case ZEUS_VALUE_TAG_REF_INT64:
            case ZEUS_VALUE_TAG_REF_BOOL:
                if (value->flags != 0 || value->payload == 0) {
                    dynamicValuePanic("invalid non-owning reference value");
                }
                *out = *value;
                return;
            case ZEUS_VALUE_TAG_STRING:
            case ZEUS_VALUE_TAG_BYTES:
                cloneOwnedBuffer(out, value);
                return;
            case ZEUS_VALUE_TAG_BIGINT:
                cloneOwnedNativePayload<ZeusType::BigInteger>(out, value);
                return;
            case ZEUS_VALUE_TAG_BIGDECIMAL:
                cloneOwnedNativePayload<ZeusType::BigDecimal>(out, value);
                return;
            case ZEUS_VALUE_TAG_BOXED: {
#ifdef ZEUS_RUNTIME_DEBUG_DETAILS
                const char* cloneLabel = rooted
                    ? "rooted-value-clone:boxed"
                    : "gc-edge-value-clone:boxed";
                ZeusBox* copy = rooted
                    ? ZEUS_NEW_BOX_NAMED(cloneLabel, *valueReference(value))
                    : ZEUS_NEW_BOX_NAMED(cloneLabel, *valueReference(value), false);
#else
                ZeusBox* copy = rooted
                    ? ZEUS_NEW_BOX_NAMED("rooted-value-clone", *valueReference(value))
                    : ZEUS_NEW_BOX_NAMED("gc-edge-value-clone", *valueReference(value), false);
#endif
                out->tag = value->tag;
                out->flags = ZEUS_VALUE_FLAG_OWNS_BOX;
                out->payload = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(copy));
                return;
            }
            default:
                dynamicValuePanic("unknown value tag");
        }
    } catch (...) {
        dynamicValuePanic("failed to clone dynamic value");
    }
}

void zeus_value_clone_internal(ZeusValue* out, const ZeusValue* value) {
    cloneDynamicValue(out, value, false);
}

void zeus_value_clone(ZeusValue* out, const ZeusValue* value) {
    cloneDynamicValue(out, value, !zeus_value_slot_is_gc_edge(out));
}

void zeus_value_slot_register_gc_edge(ZeusValue* slot) {
    if (!slot) dynamicValuePanic("cannot register a null GC-edge value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    gcEdgeValueSlots().insert(slot);
    if (isBoxTag(slot->tag) && slot->flags == ZEUS_VALUE_FLAG_OWNS_BOX) {
        valueReference(slot)->gcRoot.retract();
    } else if (slot->flags == ZEUS_VALUE_FLAG_OWNS_BOX) {
        dynamicValuePanic("invalid boxed GC-edge value slot");
    }
}

void zeus_value_slot_unregister_gc_edge(ZeusValue* slot) {
    if (!slot) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    gcEdgeValueSlots().erase(slot);
}

bool zeus_value_slot_is_gc_edge(const ZeusValue* slot) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    return gcEdgeValueSlots().find(slot) != gcEdgeValueSlots().end();
}

void zeus_value_from_box(ZeusValue* out, const ZeusBox* box) {
    if (!out) dynamicValuePanic("null output pointer");
    setNilValue(out);
    if (!box || ZeusVMUtils::isNull(box->v)) return;
    const bool rooted = !zeus_value_slot_is_gc_edge(out);

    try {
        if (const auto* character = std::get_if<char>(&box->v)) {
            zeus_value_from_char(out, static_cast<uint8_t>(*character));
            return;
        }
        if (const auto* reference = std::get_if<ZeusType::ReferenceHandle>(&box->v)) {
            if (!reference->address) return;
            if (reference->typeName == "int") {
                zeus_value_from_i64_ptr(out, reference->address);
                return;
            }
            if (reference->typeName == "bool") {
                zeus_value_from_bool_ptr(out, reference->address);
                return;
            }
        }
        if (isBooleanValue(box->v)) {
            bool value = false;
            if (std::holds_alternative<bool>(box->v)) {
                value = std::get<bool>(box->v);
            } else {
                value = std::get<bool>(std::get<LexerType::Value>(box->v).data);
            }
            zeus_value_init_bool(out, value ? 1 : 0);
            return;
        }

        int64_t integer = 0;
        if (ZeusVMUtils::tryGetInt64(box->v, integer)) {
            zeus_value_init_i64(out, integer);
            return;
        }

        if (const auto* text = stringUniversalValue(box->v)) {
            initializeOwnedBuffer(out, ZEUS_VALUE_TAG_STRING,
                reinterpret_cast<const uint8_t*>(text->data()), text->size());
            return;
        }
        if (const auto* bytes = bytesUniversalValue(box->v)) {
            initializeOwnedBuffer(out, ZEUS_VALUE_TAG_BYTES,
                bytes->data.data(), bytes->data.size());
            return;
        }
        if (const auto* integer = bigIntegerUniversalValue(box->v)) {
            initializeOwnedNativePayload(out, ZEUS_VALUE_TAG_BIGINT,
                *integer, lexerValue(box->v) != nullptr);
            return;
        }
        if (const auto* decimal = bigDecimalUniversalValue(box->v)) {
            initializeOwnedNativePayload(out, ZEUS_VALUE_TAG_BIGDECIMAL,
                *decimal, lexerValue(box->v) != nullptr);
            return;
        }

        uint32_t tag = ZEUS_VALUE_TAG_BOXED;

#ifdef ZEUS_RUNTIME_DEBUG_DETAILS
        ZeusBox* copy = ZEUS_NEW_BOX_NAMED("box-from-value:boxed", *box, rooted);
#else
        ZeusBox* copy = ZEUS_NEW_BOX(*box, rooted);
#endif
        out->tag = tag;
        out->flags = ZEUS_VALUE_FLAG_OWNS_BOX;
        out->payload = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(copy));
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("boxing runtime value", nullptr);
    } catch (...) {
        dynamicValuePanic("failed to copy boxed runtime value");
    }
}

void zeus_value_from_class_handle(ZeusValue* out, int32_t class_id) {
    if (!out) dynamicValuePanic("null class handle output pointer");
    if (class_id < 0) dynamicValuePanic("invalid class handle id");

    try {
        ZeusBox box;
        box.assignVariant(ZeusType::ClassHandle{ class_id });
        zeus_value_from_box(out, &box);
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("boxing class handle", nullptr);
    } catch (...) {
        dynamicValuePanic("failed to box class handle");
    }
}

int32_t zeus_value_require_class_handle(
    const ZeusValue* value, int32_t class_count
) {
    if (class_count <= 0) dynamicValuePanic("no AOT class layouts are available");
    if (!value || value->tag != ZEUS_VALUE_TAG_BOXED ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
        dynamicValuePanic("expected a boxed class handle");
    }

    const ZeusBox* box = valueReference(value);
    const auto* handle = std::get_if<ZeusType::ClassHandle>(&box->v);
    if (!handle) dynamicValuePanic("boxed value is not a class handle");
    if (handle->id < 0 || handle->id >= class_count) {
        dynamicValuePanic("class handle is outside the AOT class table");
    }
    return handle->id;
}

void zeus_value_from_class_ptr(ZeusValue* out, void* object, const char* type_name) {
    if (!out) dynamicValuePanic("null class value output pointer");
    setNilValue(out);
    if (!object) return;

    try {
        ZeusBox box;
        box.assignVariant(ZeusType::LLVMClassHandle{
            getOrCreateLLVMClassState(object, type_name)
        });
        zeus_value_from_box(out, &box);
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("boxing class value", type_name);
    } catch (...) {
        dynamicValuePanic("failed to box LLVM class pointer");
    }
}

void zeus_class_gc_register(void* object, const char* type_name) {
    if (!object) return;
    try {
        getOrCreateLLVMClassState(object, type_name);
    } catch (const std::bad_alloc&) {
        zeus_oom_abort("registering class GC owner", type_name);
    }
}

void* zeus_value_require_class_ptr(const ZeusValue* value, const char* type_name) {
    if (!value || value->tag != ZEUS_VALUE_TAG_BOXED) {
        dynamicValuePanic("expected a boxed LLVM class pointer");
    }
    const ZeusBox* box = valueReference(value);
    const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&box->v);
    if (!handle) dynamicValuePanic("boxed value is not an LLVM class pointer");
    return resolveLLVMClassPointer(*handle, type_name);
}

// Same as zeus_value_require_class_ptr, except that a nil value yields nullptr
// instead of aborting. Callers use it to clear or delete a slot whose content
// is not known statically. A non-nil value of the wrong class still aborts.
void* zeus_value_try_class_ptr(const ZeusValue* value, const char* type_name) {
    if (!value) return nullptr;
    if (value->tag == ZEUS_VALUE_TAG_NIL) return nullptr;
    if (value->tag != ZEUS_VALUE_TAG_BOXED || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
        dynamicValuePanic("expected a boxed LLVM class pointer");
    }
    const ZeusBox* box = valueReference(value);
    const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&box->v);
    if (!handle) {
        if (ZeusVMUtils::isNull(box->v)) return nullptr;
        dynamicValuePanic("boxed value is not an LLVM class pointer");
    }
    return resolveLLVMClassPointer(*handle, type_name);
}

void zeus_class_pointer_invalidate(void* object) {
    if (!object) return;

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    std::lock_guard<std::mutex> lock(llvmClassRegistryMutex());
    auto& registry = llvmClassRegistry();
    auto found = registry.find(object);
    if (found == registry.end()) return;

    auto state = found->second;
    if (state) state->object.store(nullptr, std::memory_order_release);
    registry.erase(found);
}

void zeus_value_validate_argument_type(
    const ZeusValue* value,
    const char* functionName,
    int32_t argumentIndex,
    const char* expectedType
) {
    if (!value || !functionName || !expectedType || argumentIndex < 0) {
        dynamicValuePanic("invalid typed-call argument validation input");
    }
    const std::string expected = ZeusRuntimeType::canonicalTypeName(expectedType);
    const std::string actual = ZeusRuntimeType::canonicalTypeName(
        dynamicValueTypeName(value));
    if (ZeusRuntimeType::isCompatible(expected, actual)) return;

    throw ZeusTypeError(
        std::string(functionName) + "(): argument " +
        std::to_string(argumentIndex + 1) + " must " + expected + ", not " + actual);
}

void zeus_value_slot_validate_argument_type(
    const ZeusValue* slot,
    const ZeusValue* value,
    const char* functionName,
    int32_t argumentIndex,
    const char* expectedType
) {
    if (!value || !functionName || !expectedType || argumentIndex < 0) {
        dynamicValuePanic("invalid reference-call argument validation input");
    }

    if (!slot) {
        const std::string expected = ZeusRuntimeType::canonicalTypeName(expectedType);
        const std::string actual = ZeusRuntimeType::canonicalTypeName(
            dynamicValueTypeName(value));
        throw ZeusTypeError(
            std::string(functionName) + "(): argument " +
            std::to_string(argumentIndex + 1) + " must variable reference to " +
            expected + ", not " + actual);
    }

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    zeus_value_validate_argument_type(slot, functionName, argumentIndex, expectedType);
}

int64_t zeus_value_require_i64(const ZeusValue* value) {
    if (value && value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (!value || value->flags != 0 ||
        (value->tag != ZEUS_VALUE_TAG_INT64 && value->tag != ZEUS_VALUE_TAG_REF_INT64)) {
        dynamicValuePanic("expected an inline int64 value");
    }
    if (value->tag == ZEUS_VALUE_TAG_REF_INT64) {
        if (value->payload == 0) dynamicValuePanic("null int reference");
        return *reinterpret_cast<const int64_t*>(static_cast<uintptr_t>(value->payload));
    }
    return value->int64_payload;
}

int32_t zeus_value_require_bool(const ZeusValue* value) {
    if (value && value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (!value || value->flags != 0 ||
        (value->tag != ZEUS_VALUE_TAG_BOOL && value->tag != ZEUS_VALUE_TAG_REF_BOOL)) {
        dynamicValuePanic("expected an inline bool value");
    }
    if (value->tag == ZEUS_VALUE_TAG_REF_BOOL) {
        if (value->payload == 0) dynamicValuePanic("null bool reference");
        return *reinterpret_cast<const bool*>(static_cast<uintptr_t>(value->payload)) ? 1 : 0;
    }
    return value->bool_payload ? 1 : 0;
}

int64_t zeus_value_require_argument_i64(
    const ZeusValue* value,
    const char* function_name,
    int32_t argument_index,
    const char* expected_type
) {
    const bool expectsInteger = expected_type &&
        (std::strcmp(expected_type, "int") == 0 ||
         std::strcmp(expected_type, "int32") == 0 ||
         std::strcmp(expected_type, "int64") == 0);
    if (value && function_name && expectsInteger && argument_index >= 0 &&
        value->flags == 0) {
        if (value->tag == ZEUS_VALUE_TAG_INT64) {
            return value->int64_payload;
        }
        if (value->tag == ZEUS_VALUE_TAG_REF_INT64 && value->payload != 0) {
            return *reinterpret_cast<const int64_t*>(
                static_cast<uintptr_t>(value->payload));
        }
    }

    zeus_value_validate_argument_type(
        value, function_name, argument_index, expected_type);
    return zeus_value_require_i64(value);
}

int32_t zeus_value_require_argument_bool(
    const ZeusValue* value,
    const char* function_name,
    int32_t argument_index,
    const char* expected_type
) {
    const bool expectsBool = expected_type &&
        std::strcmp(expected_type, "bool") == 0;
    if (value && function_name && expectsBool && argument_index >= 0 &&
        value->flags == 0) {
        if (value->tag == ZEUS_VALUE_TAG_BOOL) {
            return value->bool_payload ? 1 : 0;
        }
        if (value->tag == ZEUS_VALUE_TAG_REF_BOOL && value->payload != 0) {
            return *reinterpret_cast<const bool*>(
                static_cast<uintptr_t>(value->payload)) ? 1 : 0;
        }
    }

    zeus_value_validate_argument_type(
        value, function_name, argument_index, expected_type);
    return zeus_value_require_bool(value);
}

int32_t zeus_value_require_char(const ZeusValue* value) {
    if (value && value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (value && value->tag == ZEUS_VALUE_TAG_CHAR && value->flags == 0) {
        return value->char_payload;
    }
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    if (!boxed) dynamicValuePanic("expected a char value");
    if (const auto* character = std::get_if<char>(&boxed->v)) {
        return static_cast<uint8_t>(*character);
    }
    dynamicValuePanic("expected a char value");
}

void zeus_value_from_char(ZeusValue* out, int32_t value) {
    if (!out) dynamicValuePanic("null char output pointer");
    setNilValue(out);
    out->tag = ZEUS_VALUE_TAG_CHAR;
    out->flags = 0;
    out->char_payload = static_cast<uint8_t>(value);
}

ZeusNativeString zeus_value_intern_string(const ZeusValue* value) {
    if (!value) {
        dynamicValuePanic("expected an owned string value for native string ABI");
    }

    if (value->tag == ZEUS_VALUE_TAG_STRING) {
        uint64_t length = 0;
        const uint8_t* data = requireOwnedBufferData(
            value, ZEUS_VALUE_TAG_STRING, &length);
        try {
            return internNativeString(std::string(
                reinterpret_cast<const char*>(data), static_cast<size_t>(length)));
        } catch (...) {
            dynamicValuePanic("failed to intern native string");
        }
    }
    if (value->tag != ZEUS_VALUE_TAG_BOXED ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
        dynamicValuePanic("expected an owned string value for native string ABI");
    }

    const auto* box = valueReference(value);
    const auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&box->v);
    if (classHandle) {
        if (!classHandle->state || classHandle->state->typeName != "String") {
            dynamicValuePanic("boxed class is not a native string or String object");
        }

        OwnedZeusValue rendered;
        if (!zeus_value_try_dynamic_member_call(
                &rendered.value, value, "toString", 0, nullptr)) {
            dynamicValuePanic("String object has no registered toString method");
        }
        if (rendered.value.tag == ZEUS_VALUE_TAG_STRING) {
            return zeus_value_intern_string(&rendered.value);
        }
        if (rendered.value.tag != ZEUS_VALUE_TAG_BOXED ||
            rendered.value.flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
            dynamicValuePanic("String.toString must return a string value");
        }

        const auto* renderedText = stringUniversalValue(
            valueReference(&rendered.value)->v);
        if (!renderedText) {
            dynamicValuePanic("String.toString returned a non-string value");
        }
        try {
            return internNativeString(*renderedText);
        } catch (...) {
            dynamicValuePanic("failed to intern String object contents");
        }
    }

    const auto* text = stringUniversalValue(box->v);
    if (!text) dynamicValuePanic("boxed value is not a string");

    try {
        // Native pointers use the same data pointer and prefix as string literals.
        return internNativeString(*text);
    } catch (...) {
        dynamicValuePanic("failed to intern native string");
    }
}

void* zeus_value_native_address(const ZeusValue* value, int32_t reveal) {
    if (!value) dynamicValuePanic("null value address request");
    (void)reveal;
    if (value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("address of deleted variable");
    }

    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    switch (value->tag) {
        case ZEUS_VALUE_TAG_INT64:
            if (value->flags != 0) dynamicValuePanic("invalid inline int value");
            return const_cast<int64_t*>(&value->int64_payload);
        case ZEUS_VALUE_TAG_BOOL:
            if (value->flags != 0) dynamicValuePanic("invalid inline bool value");
            return const_cast<bool*>(&value->bool_payload);
        case ZEUS_VALUE_TAG_CHAR:
            if (value->flags != 0) dynamicValuePanic("invalid inline char value");
            return const_cast<uint8_t*>(&value->char_payload);
        case ZEUS_VALUE_TAG_REF_INT64:
        case ZEUS_VALUE_TAG_REF_BOOL:
            if (value->flags != 0 || value->payload == 0) {
                dynamicValuePanic("invalid scalar reference value");
            }
            return reinterpret_cast<void*>(static_cast<uintptr_t>(value->payload));
        case ZEUS_VALUE_TAG_NIL:
            if (value->flags != 0) dynamicValuePanic("invalid nil value");
            return const_cast<uint64_t*>(&value->payload);
        case ZEUS_VALUE_TAG_STRING:
        case ZEUS_VALUE_TAG_BYTES:
            return requireOwnedBufferData(value, value->tag);
        case ZEUS_VALUE_TAG_BIGINT:
            return requireOwnedNativePayload<ZeusType::BigInteger>(
                value, ZEUS_VALUE_TAG_BIGINT);
        case ZEUS_VALUE_TAG_BIGDECIMAL:
            return requireOwnedNativePayload<ZeusType::BigDecimal>(
                value, ZEUS_VALUE_TAG_BIGDECIMAL);
        default:
            break;
    }

    if (value->tag != ZEUS_VALUE_TAG_BOXED ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
        dynamicValuePanic("value has no addressable native representation");
    }

    ZeusBox* box = valueReference(value);
    auto& nativeValue = box->v;
    if (auto* character = std::get_if<char>(&nativeValue)) {
        return character;
    }
    if (isStringUniversalValue(nativeValue) || bytesUniversalValue(nativeValue)) {
        dynamicValuePanic("string/bytes must use their direct ZeusValue buffer tag");
    }
    if (auto* reference = std::get_if<ZeusType::ReferenceHandle>(&nativeValue)) {
        if (reference->address) return reference->address;
    }
    if (auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&nativeValue)) {
        if (classHandle->state) {
            if (void* object = classHandle->state->object.load(std::memory_order_acquire)) {
                return object;
            }
        }
    }
    if (auto* variant = std::get_if<ZeusType::Variant>(&nativeValue)) {
        if (auto* object = std::get_if<ZeusType::Object>(variant)) {
            if (object->impl) return object->impl.get();
        }
        if (auto* array = std::get_if<ZeusType::Array>(variant)) {
            if (array->impl) return array->impl.get();
        }
    }
    if (auto* literal = std::get_if<LexerType::Value>(&nativeValue)) {
        return std::visit([](auto& item) -> void* { return &item; }, literal->data);
    }
    if (auto* closure = std::get_if<ZeusType::NativeClosureHandle>(&nativeValue)) {
        if (closure->state) return closure->state.get();
    }
    return std::visit([](auto& item) -> void* { return &item; }, nativeValue);
}

ZeusNativeString zeus_string_intern(ZeusNativeString value) {
    const std::string_view text = nativeStringContents(value);
    try {
        return internNativeString(std::string(text.data(), text.size()));
    } catch (...) {
        dynamicValuePanic("failed to intern native string");
    }
}

ZeusNativeString zeus_string_from_data(const char* data, int64_t length) {
    if (length < 0 ||
        static_cast<uint64_t>(length) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        (length > 0 && !data)) {
        dynamicValuePanic("invalid native string data");
    }
    try {
        std::string text(data ? data : "", static_cast<size_t>(length));
        return internNativeString(std::move(text));
    } catch (...) {
        dynamicValuePanic("failed to intern native string data");
    }
}

ZeusNativeString zeus_string_from_address(int64_t dataAddress, int64_t length) {
    if (dataAddress <= 0 || length < 0) {
        dynamicValuePanic("invalid native string data address");
    }
    const auto* data = reinterpret_cast<const char*>(
        static_cast<uintptr_t>(dataAddress));
    if (!zeus_memory_find_allocation(
            const_cast<char*>(data), static_cast<uint64_t>(length))) {
        dynamicValuePanic("native string data address is outside a live allocation");
    }
    return zeus_string_from_data(data, length);
}

ZeusNativeBytes zeus_value_copy_bytes(const ZeusValue* value) {
    if (!value || value->tag != ZEUS_VALUE_TAG_BYTES ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BUFFER) {
        dynamicValuePanic("expected an owned Bytes value for native Bytes ABI");
    }
    uint64_t length = 0;
    const uint8_t* data = requireOwnedBufferData(
        value, ZEUS_VALUE_TAG_BYTES, &length);
    try {
        return allocateNativeBytes(data, length);
    } catch (...) {
        dynamicValuePanic("failed to copy dynamic Bytes value to native storage");
    }
}

ZeusNativeBytes zeus_bytes_from_data(const uint8_t* data, int64_t length) {
    if (length < 0 ||
        static_cast<uint64_t>(length) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        (length > 0 && !data)) {
        dynamicValuePanic("invalid native Bytes data");
    }
    try {
        return allocateNativeBytes(data, static_cast<uint64_t>(length));
    } catch (...) {
        dynamicValuePanic("failed to allocate native Bytes data");
    }
}

ZeusNativeBytes zeus_bytes_copy_from_address(
    int64_t dataAddress, int64_t length
) {
    if (length < 0 || static_cast<uint64_t>(length) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        (length > 0 && dataAddress <= 0)) {
        dynamicValuePanic("invalid native Bytes address or length");
    }
    const auto* data = reinterpret_cast<const uint8_t*>(
        static_cast<uintptr_t>(dataAddress));
    try {
        if (length > 0) {
            zeus_lvalue_require_raw(
                const_cast<uint8_t*>(data), static_cast<uint64_t>(length), 0);
        }
        return allocateNativeBytes(data, static_cast<uint64_t>(length));
    } catch (...) {
        dynamicValuePanic("failed to copy native Bytes address range");
    }
}

ZeusNativeBytes zeus_bytes_clone(ZeusNativeBytes bytes) {
    if (!bytes) return nullptr;
    try {
        const uint64_t length = nativeBytesLength(bytes);
        return allocateNativeBytes(bytes, length);
    } catch (...) {
        dynamicValuePanic("failed to clone native Bytes value");
    }
}

void zeus_bytes_release(ZeusNativeBytes bytes) {
    releaseNativeBytes(bytes);
}

void zeus_bytes_store_slot(
    ZeusNativeBytes* slot, ZeusNativeBytes value, int32_t valueOwned
) {
    if (!slot) dynamicValuePanic("null native Bytes slot");
    ZeusNativeBytes replacement = nullptr;
    if (value) replacement = zeus_bytes_clone(value);
    const ZeusNativeBytes previous = *slot;
    *slot = replacement;
    if (previous && (previous != value || valueOwned == 0)) {
        zeus_bytes_release(previous);
    }
    if (valueOwned != 0 && value) zeus_bytes_release(value);
    zeus_global_sync_native_bytes_slot(slot);
}

void zeus_bytes_copy_to_address(
    ZeusNativeBytes bytes, int64_t destinationAddress, int64_t length
) {
    if (length < 0) dynamicValuePanic("native Bytes copy length cannot be negative");
    const uint64_t sourceLength = nativeBytesLength(bytes);
    if (sourceLength != static_cast<uint64_t>(length)) {
        dynamicValuePanic("native Bytes write length must equal the source length");
    }
    if (length == 0) return;
    if (destinationAddress <= 0) dynamicValuePanic("native Bytes destination address is invalid");
    auto* destination = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(destinationAddress));
    try {
        std::vector<uint8_t> snapshot(bytes, bytes + sourceLength);
        zeus_lvalue_require_raw(destination, sourceLength, 1);
        std::memmove(destination, snapshot.data(), snapshot.size());
    } catch (...) {
        dynamicValuePanic("failed to copy native Bytes to address range");
    }
}

void zeus_bytes_store_raw_range(
    ZeusNativeBytes bytes, int64_t destinationAddress, int64_t length
) {
    if (length < 0) dynamicValuePanic("raw Bytes range length cannot be negative");
    const uint64_t sourceLength = nativeBytesLength(bytes);
    if (sourceLength != static_cast<uint64_t>(length)) {
        dynamicValuePanic("raw Bytes range write length must equal the source length");
    }
    if (length == 0) return;
    if (destinationAddress <= 0 || static_cast<uint64_t>(length) >
            std::numeric_limits<uintptr_t>::max() -
                static_cast<uintptr_t>(destinationAddress)) {
        dynamicValuePanic("raw Bytes range destination is invalid");
    }
    auto* destination = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(destinationAddress));
    try {
        std::vector<uint8_t> snapshot(bytes, bytes + sourceLength);
        std::memmove(destination, snapshot.data(), snapshot.size());
    } catch (...) {
        dynamicValuePanic("failed to write owned Bytes into a raw range");
    }
}

void zeus_raw_bytes_range_validate(int64_t address, int64_t length) {
    if (length < 0 || (length > 0 && address <= 0) ||
        (length > 0 && static_cast<uint64_t>(length) >
            std::numeric_limits<uintptr_t>::max() -
                static_cast<uintptr_t>(address))) {
        dynamicValuePanic("invalid raw Bytes range address or length");
    }
}

void zeus_raw_bytes_range_copy(
    int64_t destinationAddress, int64_t sourceAddress, int64_t length
) {
    zeus_raw_bytes_range_validate(destinationAddress, length);
    zeus_raw_bytes_range_validate(sourceAddress, length);
    if (length == 0) return;
    auto* destination = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(destinationAddress));
    const auto* source = reinterpret_cast<const uint8_t*>(
        static_cast<uintptr_t>(sourceAddress));
    try {
        std::vector<uint8_t> snapshot(
            source, source + static_cast<size_t>(length));
        std::memmove(destination, snapshot.data(), snapshot.size());
    } catch (...) {
        dynamicValuePanic("failed to copy a raw Bytes range");
    }
}

void zeus_raw_bytes_range_store(
    int64_t destinationAddress, int64_t destinationLength,
    int64_t sourceAddress, int64_t sourceLength
) {
    zeus_raw_bytes_range_validate(destinationAddress, destinationLength);
    zeus_raw_bytes_range_validate(sourceAddress, sourceLength);
    if (destinationLength != sourceLength) {
        dynamicValuePanic("raw Bytes range write length must equal the source length");
    }
    zeus_raw_bytes_range_copy(
        destinationAddress, sourceAddress, destinationLength);
}

int64_t zeus_raw_bytes_range_get(
    int64_t address, int64_t length, int64_t index
) {
    if (length < 0 || index < 0 || index >= length || address <= 0 ||
        static_cast<uint64_t>(index) >
            std::numeric_limits<uintptr_t>::max() -
                static_cast<uintptr_t>(address)) {
        dynamicValuePanic("raw Bytes range index is out of bounds");
    }
    auto* byte = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(address) + static_cast<uintptr_t>(index));
    return static_cast<int64_t>(*byte);
}

void zeus_raw_bytes_range_set(
    int64_t address, int64_t length, int64_t index, int64_t value
) {
    if (length < 0 || index < 0 || index >= length || address <= 0 ||
        value < 0 || value > 255 ||
        static_cast<uint64_t>(index) >
            std::numeric_limits<uintptr_t>::max() -
                static_cast<uintptr_t>(address)) {
        dynamicValuePanic("raw Bytes range assignment is out of bounds");
    }
    auto* byte = reinterpret_cast<uint8_t*>(
        static_cast<uintptr_t>(address) + static_cast<uintptr_t>(index));
    *byte = static_cast<uint8_t>(value);
}

void* zeus_value_require_i64_ptr(const ZeusValue* value) {
    if (!value || value->tag != ZEUS_VALUE_TAG_REF_INT64 ||
        value->flags != 0 || value->payload == 0) {
        dynamicValuePanic("expected a non-owning int64 reference");
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value->payload));
}

void* zeus_value_require_bool_ptr(const ZeusValue* value) {
    if (!value || value->tag != ZEUS_VALUE_TAG_REF_BOOL ||
        value->flags != 0 || value->payload == 0) {
        dynamicValuePanic("expected a non-owning bool reference");
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value->payload));
}

int64_t zeus_value_array_length_i64(const ZeusValue* value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!value) dynamicValuePanic("null array length input");
    if (value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (value->tag != ZEUS_VALUE_TAG_BOXED) return 0;

    return static_cast<int64_t>(
        ZeusVMUtils::arrayLengthUnlocked(valueReference(value)->v));
}

int64_t zeus_value_dict_length_i64(const ZeusValue* value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!value) dynamicValuePanic("null dictionary length input");
    if (value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (value->tag != ZEUS_VALUE_TAG_BOXED) return 0;

    const auto* dynamic = std::get_if<ZeusType::Variant>(
        &valueReference(value)->v);
    if (!dynamic) return 0;
    const auto* dictionary = std::get_if<ZeusType::Object>(dynamic);
    if (!dictionary || !dictionary->impl) return 0;
    return static_cast<int64_t>(dictionary->impl->data.size());
}

int64_t zeus_value_dict_contains_key_i64(
    const ZeusValue* dictionaryValue,
    const ZeusValue* normalizedKey
) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto* dictionary = nativeDictionaryFromValue(dictionaryValue);
    if (!dictionary->impl) return 0;
    const std::string keyText = nativeNormalizedDictionaryKey(normalizedKey);
    return findNativeDictionaryEntry(dictionary, keyText) ? 1 : 0;
}

ZeusNativeString zeus_string_copy_from_data(
    const char* data, int64_t length
) {
    if (length < 0 ||
        static_cast<uint64_t>(length) >
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        (length > 0 && !data)) {
        dynamicValuePanic("invalid native string copy data");
    }
    try {
        std::string text(data ? data : "", static_cast<size_t>(length));
        return copyNativeString(std::move(text));
    } catch (...) {
        dynamicValuePanic("failed to copy native string data");
    }
}

void zeus_value_dict_get_normalized(
    ZeusValue* out,
    const ZeusValue* dictionaryValue,
    const ZeusValue* normalizedKey
) {
    if (!out) dynamicValuePanic("null dictionary lookup output");
    zeus_value_init_nil(out);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());

    const auto* dictionary = nativeDictionaryFromValue(dictionaryValue);
    const std::string keyText = nativeNormalizedDictionaryKey(normalizedKey);
    const auto* value = findNativeDictionaryEntry(dictionary, keyText);
    if (!value) {
        throw std::runtime_error("KeyError: missing key '" + keyText + "'");
    }

    // Copy the stored value into a normal dynamic result while the dictionary is locked.
    ZeusBox result;
    result.assignVariant(*value);
    zeus_value_from_box(out, &result);
}

void zeus_value_dict_store_normalized(
    ZeusValue* out,
    const ZeusValue* dictionaryValue,
    const ZeusValue* normalizedKey,
    const ZeusValue* storedValue
) {
    if (!out) dynamicValuePanic("null dictionary store output");
    zeus_value_init_nil(out);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());

    const auto* dictionary = nativeDictionaryFromValue(dictionaryValue);
    const std::string keyText = nativeNormalizedDictionaryKey(normalizedKey);
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> valueBox(
        zeus_value_to_box(storedValue), &zeus_box_release);
    if (dictionary->impl) {
        ZeusGC::storeObjectContainerValue(
            *dictionary->impl, keyText, valueBox->v);
    }
}

void zeus_value_dict_delete_normalized(
    ZeusValue* out,
    const ZeusValue* dictionaryValue,
    const ZeusValue* normalizedKey
) {
    if (!out) dynamicValuePanic("null dictionary delete output");
    zeus_value_init_nil(out);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());

    const auto* dictionary = nativeDictionaryFromValue(dictionaryValue);
    const std::string keyText = nativeNormalizedDictionaryKey(normalizedKey);
    (void)ZeusContainerOps::dictDeleteFromObject(
        *dictionary, keyText, false);
}

ZeusBox* zeus_value_to_box(const ZeusValue* value) {
    if (!value) dynamicValuePanic("null input pointer");
    if (value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }

    try {
        switch (value->tag) {
            case ZEUS_VALUE_TAG_NIL:
                if (value->flags != 0 || value->payload != 0) {
                    dynamicValuePanic("invalid nil value");
                }
                return zeus_null_box();
            case ZEUS_VALUE_TAG_BOOL:
                return zeus_box_bool(zeus_value_require_bool(value));
            case ZEUS_VALUE_TAG_CHAR: {
                if (value->flags != 0) dynamicValuePanic("invalid inline char value");
                std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                    ZEUS_NEW_BOX(), &zeus_box_release);
                box->assignVariant(static_cast<char>(value->char_payload));
                return box.release();
            }
            case ZEUS_VALUE_TAG_INT64:
                return zeus_box_int(zeus_value_require_i64(value));
            case ZEUS_VALUE_TAG_REF_INT64:
            case ZEUS_VALUE_TAG_REF_BOOL: {
                if (value->flags != 0 || value->payload == 0) {
                    dynamicValuePanic("invalid non-owning reference value");
                }
                auto* box = ZEUS_NEW_BOX();
                box->assignVariant(ZeusType::ReferenceHandle{
                    reinterpret_cast<void*>(static_cast<uintptr_t>(value->payload)),
                    value->tag == ZEUS_VALUE_TAG_REF_INT64 ? "int" : "bool"
                });
                return box;
            }
            case ZEUS_VALUE_TAG_STRING: {
                uint64_t length = 0;
                const uint8_t* data = requireOwnedBufferData(
                    value, ZEUS_VALUE_TAG_STRING, &length);
                std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                    ZEUS_NEW_BOX(), &zeus_box_release);
                box->assignVariant(std::string(
                    reinterpret_cast<const char*>(data), static_cast<size_t>(length)));
                return box.release();
            }
            case ZEUS_VALUE_TAG_BYTES: {
                uint64_t length = 0;
                const uint8_t* data = requireOwnedBufferData(
                    value, ZEUS_VALUE_TAG_BYTES, &length);
                ZeusType::Bytes bytes;
                if (length != 0) bytes.data.assign(data, data + static_cast<size_t>(length));
                std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                    ZEUS_NEW_BOX(), &zeus_box_release);
                box->assignVariant(std::move(bytes));
                return box.release();
            }
            case ZEUS_VALUE_TAG_BIGINT: {
                const auto* integer = requireOwnedNativePayload<ZeusType::BigInteger>(
                    value, ZEUS_VALUE_TAG_BIGINT);
                std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                    ZEUS_NEW_BOX(), &zeus_box_release);
                if ((value->flags & ZEUS_VALUE_FLAG_LEXER_VALUE) != 0) {
                    box->assignVariant(LexerType::Value(*integer));
                } else {
                    box->assignVariant(*integer);
                }
                return box.release();
            }
            case ZEUS_VALUE_TAG_BIGDECIMAL: {
                const auto* decimal = requireOwnedNativePayload<ZeusType::BigDecimal>(
                    value, ZEUS_VALUE_TAG_BIGDECIMAL);
                std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                    ZEUS_NEW_BOX(), &zeus_box_release);
                if ((value->flags & ZEUS_VALUE_FLAG_LEXER_VALUE) != 0) {
                    box->assignVariant(LexerType::Value(*decimal));
                } else {
                    box->assignVariant(*decimal);
                }
                return box.release();
            }
            case ZEUS_VALUE_TAG_BOXED:
                return ZEUS_NEW_BOX(*valueReference(value));
            default:
                dynamicValuePanic("unknown value tag");
        }
    } catch (...) {
        dynamicValuePanic("failed to materialize boxed runtime value");
    }
}

void zeus_value_release_internal(ZeusValue* value) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!value) return;
    if (isBoxTag(value->tag)) {
        if (value->flags != ZEUS_VALUE_FLAG_OWNS_BOX || value->payload == 0) {
            dynamicValuePanic("invalid owned ZeusBox value");
        }
        delete valueReference(value);
    } else if (isOwnedBufferTag(value->tag)) {
        uint8_t* data = requireOwnedBufferData(value, value->tag);
        const auto base = reinterpret_cast<intptr_t>(data - sizeof(uint64_t));
        (void)__zeus_free(static_cast<int64_t>(base));
    } else if (value->tag == ZEUS_VALUE_TAG_BIGINT) {
        auto* integer = requireOwnedNativePayload<ZeusType::BigInteger>(
            value, ZEUS_VALUE_TAG_BIGINT);
        std::destroy_at(integer);
        (void)__zeus_free(static_cast<int64_t>(
            reinterpret_cast<intptr_t>(integer)));
    } else if (value->tag == ZEUS_VALUE_TAG_BIGDECIMAL) {
        auto* decimal = requireOwnedNativePayload<ZeusType::BigDecimal>(
            value, ZEUS_VALUE_TAG_BIGDECIMAL);
        std::destroy_at(decimal);
        (void)__zeus_free(static_cast<int64_t>(
            reinterpret_cast<intptr_t>(decimal)));
    } else if (value->tag == ZEUS_VALUE_TAG_REF_INT64 ||
               value->tag == ZEUS_VALUE_TAG_REF_BOOL) {
        if (value->flags != 0 || value->payload == 0) {
            dynamicValuePanic("invalid non-owning reference value");
        }
    } else if (value->flags != 0) {
        dynamicValuePanic("invalid value ownership flags");
    }
    setNilValue(value);
}

void zeus_value_release(ZeusValue* value) {
    // The owning box unlinks its own root on destruction, so dropping the value
    // is enough; there is no separate root entry to retire.
    zeus_value_release_internal(value);
}

void zeus_value_from_cstr(ZeusValue* out, const char* text) {
    if (!out) dynamicValuePanic("null output pointer");
    try {
        const char* source = text ? text : "";
        initializeOwnedBuffer(out, ZEUS_VALUE_TAG_STRING,
            reinterpret_cast<const uint8_t*>(source), std::strlen(source));
    } catch (...) {
        dynamicValuePanic("failed to create string value");
    }
}

void zeus_value_from_string_data(ZeusValue* out, const char* data, int64_t length) {
    if (!out || length < 0 || (length > 0 && !data)) {
        dynamicValuePanic("invalid string input");
    }
    try {
        initializeOwnedBuffer(out, ZEUS_VALUE_TAG_STRING,
            reinterpret_cast<const uint8_t*>(data), static_cast<uint64_t>(length));
    } catch (...) {
        dynamicValuePanic("failed to create string value");
    }
}

void zeus_value_from_native_string(ZeusValue* out, ZeusNativeString value) {
    if (!out) dynamicValuePanic("null dynamic string output pointer");
    const std::string_view text = nativeStringContents(value);
    try {
        initializeOwnedBuffer(out, ZEUS_VALUE_TAG_STRING,
            reinterpret_cast<const uint8_t*>(text.data()), text.size());
    } catch (...) {
        dynamicValuePanic("failed to create dynamic string from native pointer");
    }
}

void zeus_value_from_native_bytes(ZeusValue* out, ZeusNativeBytes data) {
    if (!out) dynamicValuePanic("null dynamic Bytes output pointer");
    try {
        const uint64_t length = nativeBytesLength(data);
        initializeOwnedBuffer(out, ZEUS_VALUE_TAG_BYTES, data, length);
    } catch (const std::exception& error) {
        const std::string message =
            std::string("failed to copy native Bytes into a dynamic value: ") + error.what();
        dynamicValuePanic(message.c_str());
    } catch (...) {
        dynamicValuePanic("failed to copy native Bytes into a dynamic value");
    }
}

void zeus_value_from_raw_bytes_range(
    ZeusValue* out, int64_t address, int64_t length
) {
    if (!out) dynamicValuePanic("null raw Bytes range output pointer");
    if (length < 0 || (length > 0 && address <= 0) ||
        (length > 0 && static_cast<uint64_t>(length) >
            std::numeric_limits<uintptr_t>::max() -
                static_cast<uintptr_t>(address))) {
        dynamicValuePanic("invalid raw Bytes range address or length");
    }
    try {
        ZeusType::RawBytesRange range{
            static_cast<uintptr_t>(address), static_cast<uint64_t>(length)};
        ZeusBox box;
        box.assignVariant(range);
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to box raw Bytes range");
    }
}

void zeus_value_require_raw_bytes_range(
    ZeusRawBytesRange* out, const ZeusValue* value
) {
    if (!out || !value) dynamicValuePanic("invalid raw Bytes range conversion");
    try {
        const auto* range = rawBytesRangeUniversalValue(valueReference(value)->v);
        if (!range) dynamicValuePanic("expected a raw Bytes range value");
        out->address = static_cast<uint64_t>(range->address);
        out->length = range->length;
    } catch (...) {
        dynamicValuePanic("failed to unbox raw Bytes range");
    }
}

int64_t zeus_bytes_get(ZeusNativeBytes bytes, int64_t index) {
    const uint64_t length = nativeBytesLength(bytes);
    if (index < 0 || static_cast<uint64_t>(index) >= length) {
        dynamicValuePanic("Bytes index out of range");
    }
    return static_cast<int64_t>(bytes[index]);
}

int64_t zeus_bytes_len(ZeusNativeBytes bytes) {
    const uint64_t length = nativeBytesLength(bytes);
    if (length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        dynamicValuePanic("invalid native Bytes length");
    }
    return static_cast<int64_t>(length);
}

ZeusNativeBytes zeus_bytes_concat(
    ZeusNativeBytes left,
    ZeusNativeBytes right
) {
    const uint64_t leftLength = nativeBytesLength(left);
    const uint64_t rightLength = nativeBytesLength(right);
    if (leftLength > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - rightLength ||
        leftLength + rightLength > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicValuePanic("native Bytes concatenation is too large");
    }
    try {
        std::vector<uint8_t> contents;
        contents.reserve(static_cast<size_t>(leftLength + rightLength));
        contents.insert(contents.end(), left, left + leftLength);
        contents.insert(contents.end(), right, right + rightLength);
        return allocateNativeBytes(contents.data(), contents.size());
    } catch (...) {
        dynamicValuePanic("failed to concatenate native Bytes values");
    }
}

ZeusNativeBytes zeus_bytes_slice(
    ZeusNativeBytes bytes,
    int64_t start,
    int64_t end
) {
    const uint64_t nativeLength = nativeBytesLength(bytes);
    if (nativeLength > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        dynamicValuePanic("native Bytes length is too large for slicing");
    const int64_t length = static_cast<int64_t>(nativeLength);
    if (end <= 0) return allocateNativeBytes(nullptr, 0);
    const int64_t begin = std::max<int64_t>(0, start);
    const int64_t finish = std::min<int64_t>(length, end);
    const int64_t clampedBegin = std::min(begin, finish);
    const uint8_t* data = finish == clampedBegin ? nullptr : bytes + clampedBegin;
    return allocateNativeBytes(data, static_cast<uint64_t>(finish - clampedBegin));
}

ZeusNativeString zeus_bytes_decode(
    ZeusNativeBytes bytes,
    ZeusNativeString encoding
) {
    (void)encoding;
    try {
        const uint64_t length = nativeBytesLength(bytes);
        std::string text(
            length == 0 ? "" : reinterpret_cast<const char*>(bytes),
            static_cast<size_t>(length));
        return internNativeString(std::move(text));
    } catch (...) {
        dynamicValuePanic("failed to decode native Bytes");
    }
}

void zeus_bytes_to_list(ZeusValue* out, ZeusNativeBytes bytes) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!out) dynamicValuePanic("null Bytes list output pointer");
    try {
        const uint64_t length = nativeBytesLength(bytes);
        ZeusType::Array array;
        array.impl = ZeusGC::makeArrayData();
        array.impl->data.reserve(static_cast<size_t>(length));
        for (uint64_t i = 0; i < length; ++i) {
            array.impl->data.emplace_back(
                ZeusType::BigInteger(static_cast<int64_t>(bytes[i])));
        }
        ZeusBox box;
        box.assignVariant(ZeusType::Variant(std::move(array)));
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to convert Bytes to list");
    }
}

int64_t zeus_string_length(ZeusNativeString value) {
    return static_cast<int64_t>(nativeStringContents(value).size());
}

ZeusNativeString zeus_native_string_concat(
    ZeusNativeString left,
    ZeusNativeString right
) {
    const std::string_view leftText = nativeStringContents(left);
    const std::string_view rightText = nativeStringContents(right);
    const size_t maxLength = static_cast<size_t>(std::numeric_limits<int64_t>::max());
    if (leftText.size() > maxLength || rightText.size() > maxLength - leftText.size()) {
        dynamicValuePanic("string concatenation result is too large");
    }
    return buildNativeString([&]() {
        std::string result;
        result.reserve(leftText.size() + rightText.size());
        result.append(leftText.data(), leftText.size());
        result.append(rightText.data(), rightText.size());
        return result;
    });
}

bool zeus_native_string_equal(
    ZeusNativeString left,
    ZeusNativeString right
) {
    return nativeStringContents(left) == nativeStringContents(right);
}

ZeusNativeString zeus_native_string_get(
    ZeusNativeString value,
    int64_t index
) {
    const std::string_view text = nativeStringContents(value);
    const int64_t length = static_cast<int64_t>(text.size());
    if (index < 0) index += length;
    if (index < 0 || index >= length) {
        dynamicValuePanic("String index out of range");
    }
    return zeus_string_from_data(text.data() + index, 1);
}

ZeusNativeString zeus_native_string_slice(
    ZeusNativeString value,
    int64_t start,
    int64_t end
) {
    const std::string_view text = nativeStringContents(value);
    const int64_t length = static_cast<int64_t>(text.size());
    if (start < 0) start += length;
    if (end < 0) end += length;
    if (start < 0) start = 0;
    if (end > length) end = length;
    if (start > end) start = end;
    if (start < 0 || end < 0) {
        dynamicValuePanic("String slice range out of bounds");
    }
    return zeus_string_from_data(
        text.data() + start,
        end - start);
}

int64_t zeus_native_string_index(
    ZeusNativeString value,
    ZeusNativeString substring
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view needle = nativeStringContents(substring);
    const size_t position = text.find(needle);
    return position == std::string_view::npos
        ? -1
        : static_cast<int64_t>(position);
}

int64_t zeus_native_string_rindex(
    ZeusNativeString value,
    ZeusNativeString substring
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view needle = nativeStringContents(substring);
    const size_t position = text.rfind(needle);
    return position == std::string_view::npos
        ? -1
        : static_cast<int64_t>(position);
}

ZeusNativeString zeus_native_string_replace(
    ZeusNativeString value,
    ZeusNativeString old_value,
    ZeusNativeString replacement
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view oldText = nativeStringContents(old_value);
    const std::string_view replacementText = nativeStringContents(replacement);
    if (oldText.empty()) {
        dynamicValuePanic("String replace pattern cannot be empty");
    }
    return buildNativeString([&]() {
        std::string result;
        result.reserve(text.size());
        size_t copiedThrough = 0;
        size_t position = 0;
        while ((position = text.find(oldText, copiedThrough)) !=
               std::string_view::npos) {
            result.append(text.data() + copiedThrough, position - copiedThrough);
            result.append(replacementText.data(), replacementText.size());
            copiedThrough = position + oldText.size();
        }
        result.append(text.data() + copiedThrough, text.size() - copiedThrough);
        return result;
    });
}

bool zeus_native_string_startswith(
    ZeusNativeString value,
    ZeusNativeString prefix
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view prefixText = nativeStringContents(prefix);
    return text.size() >= prefixText.size() &&
        text.compare(0, prefixText.size(), prefixText) == 0;
}

bool zeus_native_string_endswith(
    ZeusNativeString value,
    ZeusNativeString suffix
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view suffixText = nativeStringContents(suffix);
    return text.size() >= suffixText.size() &&
        text.compare(text.size() - suffixText.size(), suffixText.size(), suffixText) == 0;
}

ZeusNativeString zeus_native_string_lower(ZeusNativeString value) {
    const std::string_view text = nativeStringContents(value);
    return buildNativeString([&]() {
        std::string result(text.data(), text.size());
        for (char& character : result) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        return result;
    });
}

ZeusNativeString zeus_native_string_upper(ZeusNativeString value) {
    const std::string_view text = nativeStringContents(value);
    return buildNativeString([&]() {
        std::string result(text.data(), text.size());
        for (char& character : result) {
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        }
        return result;
    });
}

int64_t zeus_native_string_count(
    ZeusNativeString value,
    ZeusNativeString substring
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view needle = nativeStringContents(substring);
    if (needle.empty()) return 0;

    int64_t count = 0;
    size_t position = 0;
    while ((position = text.find(needle, position)) != std::string_view::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

void zeus_value_from_bytes(ZeusValue* out, const uint8_t* data, int64_t length) {
    if (!out || length < 0 || (length > 0 && !data)) {
        dynamicValuePanic("invalid bytes input");
    }
    try {
        initializeOwnedBuffer(out, ZEUS_VALUE_TAG_BYTES, data,
                              static_cast<uint64_t>(length));
    } catch (...) {
        dynamicValuePanic("failed to create bytes value");
    }
}

void zeus_value_from_bigint_decimal(ZeusValue* out, const char* decimalText) {
    if (!out || !decimalText) dynamicValuePanic("null bigint input");
    try {
        ZeusBox box;
        box.assignVariant(ZeusType::BigInteger(std::string(decimalText)));
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to parse bigint value");
    }
}

void zeus_value_from_bigdecimal_decimal(ZeusValue* out, const char* decimalText) {
    if (!out || !decimalText) dynamicValuePanic("null bigdecimal input");
    try {
        ZeusBox box;
        box.assignVariant(ZeusType::BigDecimal(std::string(decimalText)));
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to parse bigdecimal value");
    }
}

void zeus_value_push(ZeusValue* value) {
    if (!value) dynamicValuePanic("null value pointer");
    try {
        ZeusBox* box = zeus_value_to_box(value);
        zeus_value_release(value);
        zeus_push(box);
    } catch (...) {
        dynamicValuePanic("failed to push dynamic value");
    }
}

void zeus_value_pop(ZeusValue* out) {
    if (!out) dynamicValuePanic("null output pointer");
    setNilValue(out);
    try {
        ZeusBox* box = zeus_pop();
        zeus_value_from_box(out, box);
        zeus_box_release(box);
    } catch (...) {
        dynamicValuePanic("failed to pop dynamic value");
    }
}

void zeus_value_print(const ZeusValue* value) {
    try {
        printDynamicValue(value);
        std::cout << '\n';
        // Unchecked raw addresses crash by design; keep diagnostics from being
        // swallowed by stream buffering when the process dies.
        std::cout.flush();
    } catch (...) {
        dynamicValuePanic("failed to print dynamic value");
    }
}

void zeus_value_print_many(int32_t argc, const ZeusValue* const* values) {
    if (argc < 0 || (argc > 0 && !values)) {
        dynamicValuePanic("invalid print argument array");
    }

    try {
        for (int32_t i = 0; i < argc; ++i) {
            if (i > 0) std::cout << ' ';
            printDynamicValue(values[i]);
        }
        std::cout << '\n';
        std::cout.flush();
    } catch (...) {
        dynamicValuePanic("failed to print dynamic argument array");
    }
}

void zeus_value_print_call(
    ZeusValue* out,
    int32_t argc,
    const ZeusValue* const* values
) {
    if (!out) dynamicValuePanic("null variadic return pointer");
    setNilValue(out);
    zeus_value_print_many(argc, values);
}

void zeus_value_global_store(int idx, ZeusValue* value) {
    if (!value) dynamicValuePanic("null value pointer");
    try {
        abiStoreGlobalValue(idx, value);
    } catch (...) {
        dynamicValuePanic("failed to store dynamic global");
    }
}

void zeus_value_global_load(ZeusValue* out, int idx) {
    if (!out) dynamicValuePanic("null output pointer");
    try {
        abiLoadGlobalValue(out, idx);
    } catch (...) {
        dynamicValuePanic("failed to load dynamic global");
    }
}

ZeusValue* zeus_value_global_slot(int idx) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusValue* slot = abiResolveGlobalValueSlot(idx, false);
    if (!slot) dynamicValuePanic("invalid global reference slot");
    return slot;
}

void zeus_value_slot_load(ZeusValue* out, const ZeusValue* slot) {
    if (!out || !slot) dynamicValuePanic("invalid value-slot load");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (out == slot) return;
    setNilValue(out);
    zeus_value_clone(out, slot);
}

int64_t zeus_value_slot_load_i64(const ZeusValue* slot) {
    if (!slot) dynamicValuePanic("null int64 value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    return zeus_value_require_i64(slot);
}

int32_t zeus_value_slot_load_bool(const ZeusValue* slot) {
    if (!slot) dynamicValuePanic("null bool value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    return zeus_value_require_bool(slot);
}

int64_t* zeus_value_slot_i64_pointer(ZeusValue* slot) {
    if (!slot) dynamicValuePanic("null int64 reference slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
    if (slot->flags != 0) dynamicValuePanic("invalid int64 reference slot flags");
    if (slot->tag == ZEUS_VALUE_TAG_REF_INT64) {
        if (slot->payload == 0) dynamicValuePanic("null int64 reference slot");
        return reinterpret_cast<int64_t*>(static_cast<uintptr_t>(slot->payload));
    }
    if (slot->tag != ZEUS_VALUE_TAG_INT64) {
        dynamicValuePanic("expected a mutable int64 variable reference");
    }
    return &slot->int64_payload;
}

bool* zeus_value_slot_bool_pointer(ZeusValue* slot) {
    if (!slot) dynamicValuePanic("null bool reference slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
    if (slot->flags != 0) dynamicValuePanic("invalid bool reference slot flags");
    if (slot->tag == ZEUS_VALUE_TAG_REF_BOOL) {
        if (slot->payload == 0) dynamicValuePanic("null bool reference slot");
        return reinterpret_cast<bool*>(static_cast<uintptr_t>(slot->payload));
    }
    if (slot->tag != ZEUS_VALUE_TAG_BOOL) {
        dynamicValuePanic("expected a mutable bool variable reference");
    }
    return &slot->bool_payload;
}

void zeus_value_slot_store(ZeusValue* slot, const ZeusValue* value) {
    if (!slot || !value) dynamicValuePanic("invalid value-slot store");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
    if (slot == value) return;
    abiValidateNativeStringGlobalStore(slot, value);
    abiValidateNativeBytesGlobalStore(slot, value);
    zeus_lvalue_unregister_revealed(slot);
    ZeusValue replacement{};
    zeus_value_init_nil(&replacement);
    if (zeus_value_slot_is_gc_edge(slot)) {
        zeus_value_clone_internal(&replacement, value);
    } else {
        zeus_value_clone(&replacement, value);
    }
    releaseSlotValue(slot);
    *slot = replacement;
    setNilValue(&replacement);
    abiSyncNativeStringGlobalValueSlot(slot);
    abiSyncNativeBytesGlobalValueSlot(slot);
}

void zeus_value_slot_store_i64(ZeusValue* slot, int64_t value) {
    if (!slot) dynamicValuePanic("null int64 value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
    zeus_lvalue_unregister_revealed(slot);
    releaseSlotValue(slot);
    zeus_value_init_i64(slot, value);
}

void zeus_value_slot_store_bool(ZeusValue* slot, int32_t value) {
    if (!slot) dynamicValuePanic("null bool value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
    zeus_lvalue_unregister_revealed(slot);
    releaseSlotValue(slot);
    zeus_value_init_bool(slot, value);
}

void zeus_value_local_store(int idx, ZeusValue* value) {
    if (!value) dynamicValuePanic("null value pointer");
    try {
        ZeusBox* box = zeus_value_to_box(value);
        zeus_value_release(value);
        zeus_local_store(idx, box);
    } catch (...) {
        dynamicValuePanic("failed to store dynamic local");
    }
}

void zeus_value_local_load(ZeusValue* out, int idx) {
    if (!out) dynamicValuePanic("null output pointer");
    setNilValue(out);
    try {
        ZeusBox* box = zeus_local_load(idx);
        zeus_value_from_box(out, box);
        zeus_box_release(box);
    } catch (...) {
        dynamicValuePanic("failed to load dynamic local");
    }
}

void zeus_value_int_add(ZeusValue* out, const ZeusValue* lhs, const ZeusValue* rhs) {
    if (!out) dynamicValuePanic("null output pointer");
    setNilValue(out);
    try {
        if (tryNativeStringConcat(out, lhs, rhs)) return;
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> left(
            zeus_value_to_box(lhs), &zeus_box_release);
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> right(
            zeus_value_to_box(rhs), &zeus_box_release);
        if (isLLVMClassValue(left.get()) || isLLVMClassValue(right.get())) {
            dynamicValuePanic("LLVM class pointers do not support dynamic arithmetic");
        }
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> result(
            zeus_int_add(left.get(), right.get()), &zeus_box_release);
        zeus_value_from_box(out, result.get());
    } catch (...) {
        dynamicValuePanic("failed to add dynamic values");
    }
}

void zeus_value_less_than(ZeusValue* out, const ZeusValue* lhs, const ZeusValue* rhs) {
    if (!out) dynamicValuePanic("null output pointer");
    setNilValue(out);
    try {
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> left(
            zeus_value_to_box(lhs), &zeus_box_release);
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> right(
            zeus_value_to_box(rhs), &zeus_box_release);
        if (isLLVMClassValue(left.get()) || isLLVMClassValue(right.get())) {
            dynamicValuePanic("LLVM class pointers do not support dynamic comparison");
        }
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> result(
            zeus_less_than(left.get(), right.get()), &zeus_box_release);
        zeus_value_from_box(out, result.get());
    } catch (...) {
        dynamicValuePanic("failed to compare dynamic values");
    }
}

int32_t zeus_value_truthy(const ZeusValue* value) {
    try {
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
            zeus_value_to_box(value), &zeus_box_release);
        return zeus_truthy(box.get());
    } catch (...) {
        dynamicValuePanic("failed to test dynamic value truthiness");
    }
}

void zeus_value_dynamic_binary(
    ZeusValue* out,
    void* context,
    int32_t opcode,
    const ZeusValue* lhs,
    const ZeusValue* rhs
) {
    if (!out) dynamicValuePanic("null output pointer");
    setNilValue(out);
    if (opcode == ZEUS_BINARY_ADD && tryNativeStringConcat(out, lhs, rhs)) return;
    if ((opcode == ZEUS_BINARY_EQUAL || opcode == ZEUS_BINARY_NOT_EQUAL) &&
        tryNativeClassEquality(out, opcode, lhs, rhs)) {
        return;
    }
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> left(
        zeus_value_to_box(lhs), &zeus_box_release);
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> right(
        zeus_value_to_box(rhs), &zeus_box_release);
    const bool logicalOperator = opcode == ZEUS_BINARY_LOGICAL_AND ||
        opcode == ZEUS_BINARY_LOGICAL_OR;
    if (!logicalOperator &&
        (isLLVMClassValue(left.get()) || isLLVMClassValue(right.get()))) {
        dynamicValuePanic("LLVM class pointers do not support dynamic operators");
    }
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> result(
        zeus_dynamic_binary(
            static_cast<ZeusRuntimeContext*>(context), opcode, left.get(), right.get()),
        &zeus_box_release);
    zeus_value_from_box(out, result.get());
}

ZeusBox* zeus_int_add(const ZeusBox* a, const ZeusBox* b) {
    if (!a || !b) return nullBox();

    // 快路径：双 int64（复用 P0 的 tryGetInt64，与 VM 内同一套逻辑）。
    int64_t x = 0, y = 0;

    if (ZeusVMUtils::tryGetInt64(a->v, x) && ZeusVMUtils::tryGetInt64(b->v, y)) {
        bool overflow = false;

        if (y > 0) overflow = (x > INT64_MAX - y);
        else if (y < 0) overflow = (x < INT64_MIN - y);

        if (!overflow) return zeus_box_int(x + y);
        // 溢出则继续走慢路径（大整数加法）。
    }

    // 慢路径：与 VM ADD 同语义（数字+数字、字符串+字符串）。
    const ZeusType::Bytes* leftBytes = bytesUniversalValue(a->v);
    const ZeusType::Bytes* rightBytes = bytesUniversalValue(b->v);
    if (leftBytes && rightBytes) {
        try {
            if (rightBytes->data.size() >
                leftBytes->data.max_size() - leftBytes->data.size()) {
                return nullBox();
            }

            ZeusType::Bytes combined(leftBytes->data);
            combined.data.reserve(leftBytes->data.size() + rightBytes->data.size());
            combined.data.insert(
                combined.data.end(), rightBytes->data.begin(), rightBytes->data.end());
            std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> result(
                ZEUS_NEW_BOX(), &zeus_box_release);
            result->assignVariant(std::move(combined));
            return result.release();
        } catch (const std::exception&) {
            return nullBox();
        } catch (...) {
            return nullBox();
        }
    }

    LexerType::Value lv(int64_t(0));
    LexerType::Value rv(int64_t(0));

    if (!ZeusVMUtils::convertToValue(a->v, lv) || !ZeusVMUtils::convertToValue(b->v, rv)) {
        return nullBox();
    }

    bool numPair = ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv);
    bool strPair = isStringValue(lv) && isStringValue(rv);

    if (!numPair && !strPair) return nullBox();

    try {
        LexerType::Value r = lv + rv;
        ZeusBox* box = ZEUS_NEW_BOX();
        box->assignVariant(r);
        return box;
    } catch (...) {
        return nullBox(); // C ABI 边界不抛异常
    }
}

void zeus_print(const ZeusBox* box) {
    if (!box) {
        std::puts("(null)");
        std::fflush(stdout);
        return;
    }

    const auto& u = box->v;

    // 字符串打印原文（不对标调试表示的引号格式，与解释器 print 一致）。
    if (std::holds_alternative<LexerType::Value>(u)) {
        const auto& v = std::get<LexerType::Value>(u);

        if (isStringValue(v)) {
            std::puts(std::get<std::string>(v.data).c_str());
            // Unchecked raw addresses crash by design; keep diagnostics from
            // being swallowed by block buffering when the process dies.
            std::fflush(stdout);
            return;
        }
    }

    if (std::holds_alternative<std::string>(u)) {
        std::puts(std::get<std::string>(u).c_str());
        std::fflush(stdout);
        return;
    }

    std::puts(ZeusVMUtils::valueToDebugString(u).c_str());
    std::fflush(stdout);
}

namespace {

struct GlobalValueSlot {
    ZeusValue value{};
    GlobalValueSlot() { zeus_value_init_nil(&value); }
};

struct NativeStringGlobalSlot {
    ZeusNativeString value = nullptr;
};

struct NativeBytesGlobalSlot {
    ZeusNativeBytes value = nullptr;
    bool initialized = false;
};

std::vector<GlobalValueSlot*>& abiGlobalValueSlots() {
    static std::vector<GlobalValueSlot*> globals;
    return globals;
}

std::vector<GlobalValueSlot*>& retiredGlobalValueSlots() {
    static std::vector<GlobalValueSlot*> retired;
    return retired;
}

std::vector<NativeStringGlobalSlot*>& nativeStringGlobalSlots() {
    static std::vector<NativeStringGlobalSlot*> slots;
    return slots;
}

std::vector<NativeStringGlobalSlot*>& retiredNativeStringGlobalSlots() {
    static std::vector<NativeStringGlobalSlot*> retired;
    return retired;
}

std::vector<NativeBytesGlobalSlot*>& nativeBytesGlobalSlots() {
    static std::vector<NativeBytesGlobalSlot*> slots;
    return slots;
}

std::vector<NativeBytesGlobalSlot*>& retiredNativeBytesGlobalSlots() {
    static std::vector<NativeBytesGlobalSlot*> retired;
    return retired;
}

std::unordered_map<const ZeusValue*, int>& nativeStringGlobalIndexByValueSlot() {
    static std::unordered_map<const ZeusValue*, int> indices;
    return indices;
}

std::unordered_map<void*, int>& nativeStringGlobalIndexByNativeSlot() {
    static std::unordered_map<void*, int> indices;
    return indices;
}

std::unordered_map<const ZeusValue*, int>& nativeBytesGlobalIndexByValueSlot() {
    static std::unordered_map<const ZeusValue*, int> indices;
    return indices;
}

std::unordered_map<void*, int>& nativeBytesGlobalIndexByNativeSlot() {
    static std::unordered_map<void*, int> indices;
    return indices;
}

std::unordered_set<int>& deletedLocalSlots() {
    static std::unordered_set<int> deleted;
    return deleted;
}

GlobalValueSlot* abiEnsureGlobalValueSlot(int idx, bool recreateDeleted) {
    if (idx < 0) return nullptr;

    auto& globals = abiGlobalValueSlots();

    if (idx >= (int)globals.size()) {
        globals.resize(idx + 1, nullptr);
    }

    if (globals[idx] && globals[idx]->value.tag == ZEUS_VALUE_TAG_DELETED) {
        if (!recreateDeleted) {
            throw std::runtime_error("read from deleted global variable");
        }
        retiredGlobalValueSlots().push_back(globals[idx]);
        globals[idx] = nullptr;
    }

    if (!globals[idx]) globals[idx] = new GlobalValueSlot();
    return globals[idx];
}

NativeStringGlobalSlot* abiFindNativeStringGlobalSlot(int idx) {
    auto& slots = nativeStringGlobalSlots();
    if (idx < 0 || idx >= static_cast<int>(slots.size())) return nullptr;
    return slots[static_cast<size_t>(idx)];
}

NativeStringGlobalSlot* abiEnsureNativeStringGlobalSlot(int idx) {
    if (idx < 0) return nullptr;
    auto& slots = nativeStringGlobalSlots();
    if (idx >= static_cast<int>(slots.size())) slots.resize(static_cast<size_t>(idx) + 1, nullptr);
    auto*& slot = slots[static_cast<size_t>(idx)];
    if (!slot) slot = new NativeStringGlobalSlot();
    nativeStringGlobalIndexByNativeSlot()[&slot->value] = idx;
    return slot;
}

NativeBytesGlobalSlot* abiFindNativeBytesGlobalSlot(int idx) {
    auto& slots = nativeBytesGlobalSlots();
    if (idx < 0 || idx >= static_cast<int>(slots.size())) return nullptr;
    return slots[static_cast<size_t>(idx)];
}

NativeBytesGlobalSlot* abiEnsureNativeBytesGlobalSlot(int idx) {
    if (idx < 0) return nullptr;
    auto& slots = nativeBytesGlobalSlots();
    if (idx >= static_cast<int>(slots.size())) {
        slots.resize(static_cast<size_t>(idx) + 1, nullptr);
    }
    auto*& slot = slots[static_cast<size_t>(idx)];
    if (!slot) slot = new NativeBytesGlobalSlot();
    nativeBytesGlobalIndexByNativeSlot()[&slot->value] = idx;
    return slot;
}

void abiRetireNativeStringGlobalSlot(int idx) {
    NativeStringGlobalSlot* slot = abiFindNativeStringGlobalSlot(idx);
    if (!slot) return;
    void* address = &slot->value;
    zeus_lvalue_unregister(address);
    nativeStringGlobalIndexByNativeSlot().erase(address);
    nativeStringGlobalSlots()[static_cast<size_t>(idx)] = nullptr;
    retiredNativeStringGlobalSlots().push_back(slot);
}

void abiRetireNativeBytesGlobalSlot(int idx) {
    NativeBytesGlobalSlot* slot = abiFindNativeBytesGlobalSlot(idx);
    if (!slot) return;
    void* address = &slot->value;
    zeus_lvalue_unregister(address);
    nativeBytesGlobalIndexByNativeSlot().erase(address);
    nativeBytesGlobalSlots()[static_cast<size_t>(idx)] = nullptr;
    if (slot->value) {
        zeus_bytes_release(slot->value);
        slot->value = nullptr;
    }
    slot->initialized = false;
    retiredNativeBytesGlobalSlots().push_back(slot);
}

ZeusNativeString nativeStringFromGlobalValue(const ZeusValue* value) {
    if (!value) throw std::runtime_error("null string global value");
    if (value->tag == ZEUS_VALUE_TAG_NIL && value->flags == 0) return nullptr;
    if (value->tag == ZEUS_VALUE_TAG_STRING &&
        value->flags == ZEUS_VALUE_FLAG_OWNS_BUFFER) {
        return zeus_value_intern_string(value);
    }
    if (value->tag != ZEUS_VALUE_TAG_BOXED ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
        throw std::runtime_error("typed string global received a non-string value");
    }
    const ZeusBox* box = valueReference(value);
    if (stringUniversalValue(box->v)) return zeus_value_intern_string(value);
    const auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&box->v);
    if (classHandle && classHandle->state && classHandle->state->typeName == "String") {
        return zeus_value_intern_string(value);
    }
    throw std::runtime_error("typed string global received a non-string value");
}

void abiSyncNativeStringGlobalSlotFromValue(int idx, const ZeusValue* value) {
    NativeStringGlobalSlot* nativeSlot = abiFindNativeStringGlobalSlot(idx);
    if (!nativeSlot) return;
    const ZeusNativeString nextValue = nativeStringFromGlobalValue(value);
    if (nativeSlot->value != nextValue) {
        zeus_lvalue_unregister_revealed(&nativeSlot->value);
        nativeSlot->value = nextValue;
    }
}

void abiSyncNativeBytesGlobalSlotFromValue(int idx, const ZeusValue* value) {
    NativeBytesGlobalSlot* nativeSlot = abiFindNativeBytesGlobalSlot(idx);
    if (!nativeSlot || !value) return;
    ZeusNativeBytes replacement = nullptr;
    if (value->tag != ZEUS_VALUE_TAG_NIL || value->flags != 0) {
        replacement = zeus_value_copy_bytes(value);
    }
    const ZeusNativeBytes previous = nativeSlot->value;
    nativeSlot->value = replacement;
    nativeSlot->initialized = true;
    if (previous) zeus_bytes_release(previous);
}

void abiValidateNativeStringGlobalStore(const ZeusValue* slot, const ZeusValue* value) {
    const auto found = nativeStringGlobalIndexByValueSlot().find(slot);
    if (found == nativeStringGlobalIndexByValueSlot().end()) return;
    (void)nativeStringFromGlobalValue(value);
}

void abiSyncNativeStringGlobalValueSlot(const ZeusValue* slot) {
    const auto found = nativeStringGlobalIndexByValueSlot().find(slot);
    if (found == nativeStringGlobalIndexByValueSlot().end()) return;
    abiSyncNativeStringGlobalSlotFromValue(found->second, slot);
}

void abiValidateNativeBytesGlobalStore(const ZeusValue* slot, const ZeusValue* value) {
    const auto found = nativeBytesGlobalIndexByValueSlot().find(slot);
    if (found == nativeBytesGlobalIndexByValueSlot().end()) return;
    if (value && value->tag == ZEUS_VALUE_TAG_NIL && value->flags == 0) return;
    const ZeusNativeBytes checked = zeus_value_copy_bytes(value);
    if (checked) zeus_bytes_release(checked);
}

void abiSyncNativeBytesGlobalValueSlot(const ZeusValue* slot) {
    const auto found = nativeBytesGlobalIndexByValueSlot().find(slot);
    if (found == nativeBytesGlobalIndexByValueSlot().end()) return;
    abiSyncNativeBytesGlobalSlotFromValue(found->second, slot);
}

ZeusValue* abiResolveGlobalValueSlot(int idx, bool recreateDeleted) {
    GlobalValueSlot* slot = abiEnsureGlobalValueSlot(idx, recreateDeleted);
    if (!slot) return nullptr;
    if (abiFindNativeStringGlobalSlot(idx)) {
        nativeStringGlobalIndexByValueSlot()[&slot->value] = idx;
    }
    if (abiFindNativeBytesGlobalSlot(idx)) {
        nativeBytesGlobalIndexByValueSlot()[&slot->value] = idx;
    }
    return &slot->value;
}

void abiStoreGlobalValue(int idx, ZeusValue* value) {
    if (!value) dynamicValuePanic("null global value");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusValue* slot = abiResolveGlobalValueSlot(idx, true);
    if (!slot) {
        zeus_value_release(value);
        return;
    }
    zeus_value_slot_store(slot, value);
    if (value != slot) zeus_value_release(value);
}

void abiLoadGlobalValue(ZeusValue* out, int idx) {
    if (!out) dynamicValuePanic("null global value output");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    ZeusValue* slot = abiResolveGlobalValueSlot(idx, false);
    if (!slot) {
        setNilValue(out);
        return;
    }
    NativeStringGlobalSlot* nativeString = abiFindNativeStringGlobalSlot(idx);
    if (nativeString && nativeString->value && out != slot) {
        setNilValue(out);
        zeus_value_from_native_string(out, nativeString->value);
        return;
    }
    NativeBytesGlobalSlot* nativeBytes = abiFindNativeBytesGlobalSlot(idx);
    if (nativeBytes && nativeBytes->initialized && out != slot) {
        setNilValue(out);
        if (nativeBytes->value) {
            zeus_value_from_native_bytes(out, nativeBytes->value);
        }
        return;
    }
    zeus_value_slot_load(out, slot);
}

void abiClearGlobalValue(int idx) {
    if (idx < 0) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto& globals = abiGlobalValueSlots();
    if (idx >= static_cast<int>(globals.size()) || !globals[idx]) return;
    if (globals[idx]->value.tag == ZEUS_VALUE_TAG_DELETED) return;
    if (NativeStringGlobalSlot* nativeString = abiFindNativeStringGlobalSlot(idx)) {
        zeus_lvalue_unregister_revealed(&nativeString->value);
        nativeString->value = nullptr;
    }
    if (NativeBytesGlobalSlot* nativeBytes = abiFindNativeBytesGlobalSlot(idx)) {
        zeus_lvalue_unregister_revealed(&nativeBytes->value);
        if (nativeBytes->value) zeus_bytes_release(nativeBytes->value);
        nativeBytes->value = nullptr;
        nativeBytes->initialized = false;
    }
    zeus_lvalue_unregister_revealed(&globals[idx]->value);
    releaseSlotValue(&globals[idx]->value);
}

void abiDeleteGlobalValue(int idx) {
    if (idx < 0) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto& globals = abiGlobalValueSlots();
    if (idx >= static_cast<int>(globals.size()) || !globals[idx]) return;
    ZeusValue* value = &globals[idx]->value;
    if (value->tag == ZEUS_VALUE_TAG_DELETED) return;
    nativeStringGlobalIndexByValueSlot().erase(value);
    nativeBytesGlobalIndexByValueSlot().erase(value);
    abiRetireNativeStringGlobalSlot(idx);
    abiRetireNativeBytesGlobalSlot(idx);
    zeus_lvalue_unregister(value);
    zeus_value_delete_object(value);
    releaseSlotValue(value);
    value->tag = ZEUS_VALUE_TAG_DELETED;
    value->flags = 0;
    value->payload = 0;
}

} // namespace

void zeus_global_store(int idx, ZeusBox* box) {
    if (!box) return;
    ZeusValue value{};
    zeus_value_init_nil(&value);
    zeus_value_from_box(&value, box);
    delete box;
    abiStoreGlobalValue(idx, &value);
}

ZeusBox* zeus_global_load(int idx) {
    ZeusValue value{};
    zeus_value_init_nil(&value);
    abiLoadGlobalValue(&value, idx);
    ZeusBox* box = zeus_value_to_box(&value);
    zeus_value_release(&value);
    return box;
}

void zeus_global_clear(int idx) {
    abiClearGlobalValue(idx);
}

void zeus_value_global_delete(int idx) {
    abiDeleteGlobalValue(idx);
}

namespace {
std::vector<Cell*>& abiLocals();
}

void zeus_value_local_delete(int idx) {
    if (idx < 0) return;
    auto& locals = abiLocals();
    if (idx < static_cast<int>(locals.size()) && locals[idx]) {
        const ZeusType::UniversalVariant current = locals[idx]->readValue();
        destroyLLVMClassValue(current);
        locals[idx]->writeValue(nullVariant());
    }
    deletedLocalSlots().insert(idx);
}

void zeus_value_delete_object(ZeusValue* value) {
    if (!value) return;
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    if (boxed) destroyLLVMClassValue(boxed->v);
}

void zeus_shared_value_store(const char* name, const ZeusValue* value) {
    if (!name || !value) dynamicValuePanic("invalid shared store arguments");
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    auto cell = ZeusShared::getGlobalHeap().getOrCreate(name, boxed->v);
    zeus_lvalue_unregister_revealed(cell.get());
    cell->write(boxed->v);
}

void zeus_shared_value_assign(const char* name, const ZeusValue* value) {
    if (!name || !value) dynamicValuePanic("invalid shared assign arguments");
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) {
        throw std::runtime_error(std::string("undefined shared variable '") + name + "'");
    }
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    zeus_lvalue_unregister_revealed(cell.get());
    cell->write(boxed->v);
}

void zeus_shared_value_load(ZeusValue* out, const char* name) {
    if (!out || !name) dynamicValuePanic("invalid shared load arguments");
    setNilValue(out);
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) {
        throw std::runtime_error(std::string("undefined shared variable '") + name + "'");
    }
    ZeusBox snapshot;
    snapshot.assignVariant(cell->read());
    zeus_value_from_box(out, &snapshot);
}

void zeus_shared_value_store_i64(const char* name, int64_t value) {
    if (!name) dynamicValuePanic("invalid shared int64 store name");
    ZeusType::UniversalVariant stored = ZeusType::BigInteger(value);
    auto cell = ZeusShared::getGlobalHeap().getOrCreate(name, stored);
    zeus_lvalue_unregister_revealed(cell.get());
    cell->write(stored);
}

void zeus_shared_value_assign_i64(const char* name, int64_t value) {
    if (!name) dynamicValuePanic("invalid shared int64 assign name");
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) {
        throw std::runtime_error(std::string("undefined shared variable '") + name + "'");
    }
    zeus_lvalue_unregister_revealed(cell.get());
    cell->write(ZeusType::BigInteger(value));
}

int64_t zeus_shared_value_load_i64(const char* name) {
    if (!name) dynamicValuePanic("invalid shared int64 load name");
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) {
        throw std::runtime_error(std::string("undefined shared variable '") + name + "'");
    }
    int64_t value = 0;
    if (!ZeusVMUtils::tryGetInt64(cell->read(), value)) {
        dynamicValuePanic("shared variable is not an int64 value");
    }
    return value;
}

void zeus_shared_value_store_bool(const char* name, int32_t value) {
    if (!name) dynamicValuePanic("invalid shared bool store name");
    ZeusType::UniversalVariant stored = value != 0;
    auto cell = ZeusShared::getGlobalHeap().getOrCreate(name, stored);
    zeus_lvalue_unregister_revealed(cell.get());
    cell->write(stored);
}

void zeus_shared_value_assign_bool(const char* name, int32_t value) {
    if (!name) dynamicValuePanic("invalid shared bool assign name");
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) {
        throw std::runtime_error(std::string("undefined shared variable '") + name + "'");
    }
    zeus_lvalue_unregister_revealed(cell.get());
    cell->write(value != 0);
}

int32_t zeus_shared_value_load_bool(const char* name) {
    if (!name) dynamicValuePanic("invalid shared bool load name");
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) {
        throw std::runtime_error(std::string("undefined shared variable '") + name + "'");
    }
    bool value = false;
    const ZeusType::UniversalVariant snapshot = cell->read();
    if (!ZeusNativeAddress::boolPayload(snapshot, value)) {
        dynamicValuePanic("shared variable is not a bool value");
    }
    return value ? 1 : 0;
}

void* zeus_shared_value_native_address(const char* name, int32_t revealView) {
    if (!name) dynamicValuePanic("invalid shared address name");
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) {
        throw std::runtime_error(std::string("undefined shared variable '") + name + "'");
    }

    void* address = nullptr;
    std::string error;
    if (!ZeusNativeAddress::addressShared(
            cell.get(), revealView != 0, address, error)) {
        throw std::runtime_error(error);
    }
    if (revealView) {
        ZeusBox snapshot; snapshot.assignVariant(cell->read());
        ZeusValue boxed{}; zeus_value_from_box(&boxed,&snapshot);
        try { zeus_lvalue_register_revealed(cell.get(),&boxed,address); }
        catch (...) { zeus_value_release(&boxed); throw; }
        zeus_value_release(&boxed);
    }
    return address;
}

int32_t zeus_shared_value_delete(const char* name) {
    if (!name) return 0;
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) return 0;
    cell->lockWrite();
    try {
        const ZeusType::UniversalVariant current = cell->read();
        destroyLLVMClassValue(current);
        auto removed = ZeusShared::getGlobalHeap().erase(name, cell->generation);
        if (!removed) {
            cell->unlockWrite();
            return 0;
        }
        zeus_lvalue_unregister(cell.get());
        cell->write(nullVariant());
        cell->unlockWrite();
        return 1;
    } catch (...) {
        cell->unlockWrite();
        throw;
    }
}

// ---- 标量全局量快路径 ----
//
// Globals live in the same slot table whether they are touched by a top-level
// statement or by a native function, so a scalar global would otherwise pay for
// a ZeusValue box on every access. These accessors read and write the cell
// directly. A type mismatch aborts instead of silently reinterpreting a value.

void zeus_global_store_i64(int idx, int64_t value) {
    ZeusValue* slot = abiResolveGlobalValueSlot(idx, true);
    if (!slot) dynamicValuePanic("global slot is unavailable");
    zeus_value_slot_store_i64(slot, value);
}

int64_t zeus_global_load_i64(int idx) {
    ZeusValue* slot = abiResolveGlobalValueSlot(idx, false);
    if (!slot) dynamicValuePanic("global slot is unavailable");
    return zeus_value_slot_load_i64(slot);
}

void zeus_global_store_bool(int idx, int32_t value) {
    ZeusValue* slot = abiResolveGlobalValueSlot(idx, true);
    if (!slot) dynamicValuePanic("global slot is unavailable");
    zeus_value_slot_store_bool(slot, value);
}

int32_t zeus_global_load_bool(int idx) {
    ZeusValue* slot = abiResolveGlobalValueSlot(idx, false);
    if (!slot) dynamicValuePanic("global slot is unavailable");
    return zeus_value_slot_load_bool(slot);
}

void zeus_global_store_string(int idx, ZeusNativeString value) {
    if (idx < 0) dynamicValuePanic("invalid native string global index");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const ZeusNativeString interned = zeus_string_intern(value);
    NativeStringGlobalSlot* slot = abiEnsureNativeStringGlobalSlot(idx);
    if (!slot) dynamicValuePanic("native string global slot is unavailable");
    if (slot->value != interned) {
        zeus_lvalue_unregister_revealed(&slot->value);
        slot->value = interned;
    }

    ZeusValue dynamicValue{};
    zeus_value_init_nil(&dynamicValue);
    zeus_value_from_native_string(&dynamicValue, interned);
    abiStoreGlobalValue(idx, &dynamicValue);
}

ZeusNativeString zeus_global_load_string(int idx) {
    if (idx < 0) dynamicValuePanic("invalid native string global index");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    NativeStringGlobalSlot* slot = abiFindNativeStringGlobalSlot(idx);
    if (slot && slot->value) return slot->value;

    OwnedZeusValue dynamicValue;
    abiLoadGlobalValue(&dynamicValue.value, idx);
    const ZeusNativeString interned = zeus_value_intern_string(&dynamicValue.value);
    slot = abiEnsureNativeStringGlobalSlot(idx);
    if (!slot) dynamicValuePanic("native string global slot is unavailable");
    slot->value = interned;
    return interned;
}

ZeusNativeString* zeus_global_string_slot(int idx) {
    (void)zeus_global_load_string(idx);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    NativeStringGlobalSlot* slot = abiFindNativeStringGlobalSlot(idx);
    if (!slot) dynamicValuePanic("native string global slot is unavailable");
    return &slot->value;
}

void zeus_global_sync_native_string_slot(void* address) {
    if (!address) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto index = nativeStringGlobalIndexByNativeSlot().find(address);
    if (index == nativeStringGlobalIndexByNativeSlot().end()) return;
    NativeStringGlobalSlot* nativeSlot = abiFindNativeStringGlobalSlot(index->second);
    if (!nativeSlot) return;

    ZeusValue* dynamicSlot = abiResolveGlobalValueSlot(index->second, true);
    if (!dynamicSlot) dynamicValuePanic("native string global mirror is unavailable");
    OwnedZeusValue mirror;
    if (nativeSlot->value) {
        zeus_value_from_native_string(&mirror.value, nativeSlot->value);
    }
    zeus_value_slot_store(dynamicSlot, &mirror.value);
}

void zeus_global_store_bytes(int idx, ZeusNativeBytes value) {
    if (idx < 0) dynamicValuePanic("invalid native Bytes global index");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (!abiEnsureNativeBytesGlobalSlot(idx)) {
        dynamicValuePanic("native Bytes global slot is unavailable");
    }
    OwnedZeusValue dynamicValue;
    zeus_value_from_native_bytes(&dynamicValue.value, value);
    abiStoreGlobalValue(idx, &dynamicValue.value);
}

ZeusNativeBytes zeus_global_load_bytes(int idx) {
    if (idx < 0) dynamicValuePanic("invalid native Bytes global index");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    NativeBytesGlobalSlot* slot = abiFindNativeBytesGlobalSlot(idx);
    if (slot && slot->initialized) return zeus_bytes_clone(slot->value);

    OwnedZeusValue dynamicValue;
    abiLoadGlobalValue(&dynamicValue.value, idx);
    const ZeusNativeBytes copied = zeus_value_copy_bytes(&dynamicValue.value);
    slot = abiEnsureNativeBytesGlobalSlot(idx);
    if (!slot) dynamicValuePanic("native Bytes global slot is unavailable");
    if (slot->value) zeus_bytes_release(slot->value);
    slot->value = copied;
    slot->initialized = true;
    return zeus_bytes_clone(slot->value);
}

ZeusNativeBytes* zeus_global_bytes_slot(int idx) {
    const ZeusNativeBytes initializedValue = zeus_global_load_bytes(idx);
    if (initializedValue) zeus_bytes_release(initializedValue);
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    NativeBytesGlobalSlot* slot = abiFindNativeBytesGlobalSlot(idx);
    if (!slot) dynamicValuePanic("native Bytes global slot is unavailable");
    return &slot->value;
}

void zeus_global_sync_native_bytes_slot(void* address) {
    if (!address) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    const auto index = nativeBytesGlobalIndexByNativeSlot().find(address);
    if (index == nativeBytesGlobalIndexByNativeSlot().end()) return;
    NativeBytesGlobalSlot* nativeSlot = abiFindNativeBytesGlobalSlot(index->second);
    if (!nativeSlot) return;

    ZeusValue* dynamicSlot = abiResolveGlobalValueSlot(index->second, true);
    if (!dynamicSlot) dynamicValuePanic("native Bytes global mirror is unavailable");
    OwnedZeusValue mirror;
    zeus_value_from_native_bytes(&mirror.value, nativeSlot->value);
    zeus_value_slot_store(dynamicSlot, &mirror.value);
}



// ---- Emitter v0 栈机 ----

namespace {

std::vector<ZeusBox*>& abiOpStack() {
    static std::vector<ZeusBox*> stack;
    return stack;
}

std::vector<Cell*>& abiLocals() {
    static std::vector<Cell*> locals;
    return locals;
}

Cell* abiEnsureLocal(int idx) {
    if (idx < 0) return nullptr;

    auto& locals = abiLocals();

    if (idx >= (int)locals.size()) {
        locals.resize(idx + 1, nullptr);
    }

    if (!locals[idx]) {
        locals[idx] = new Cell{nullVariant(), 0, __nextCellId()};
        locals[idx]->createdBy = "zeus_abi_local";
        HeapStats::recordCreate();
        HeapStats::registerCell(locals[idx]);
    }

    return locals[idx];
}

// 与 VM::truthy 同语义的子集（v0：解释器不参与，比较/分支全走这里）。
bool truthyImpl(const ZeusType::UniversalVariant& u) {
    if (std::holds_alternative<ZeusType::LLVMClassHandle>(u)) {
        return isLiveLLVMClassHandle(std::get<ZeusType::LLVMClassHandle>(u));
    }

    if (std::holds_alternative<LexerType::Value>(u)) {
        const auto& v = std::get<LexerType::Value>(u);

        if (std::holds_alternative<bool>(v.data)) return std::get<bool>(v.data);

        if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
            return std::get<ZeusType::BigInteger>(v.data) != ZeusType::BigInteger(0);
        }

        if (std::holds_alternative<ZeusType::BigDecimal>(v.data)) {
            return std::get<ZeusType::BigDecimal>(v.data) != ZeusType::BigDecimal(0);
        }

        if (std::holds_alternative<std::string>(v.data)) {
            return !std::get<std::string>(v.data).empty();
        }

        return true;
    }

    if (std::holds_alternative<ZeusType::BigInteger>(u)) {
        return std::get<ZeusType::BigInteger>(u) != ZeusType::BigInteger(0);
    }

    if (std::holds_alternative<ZeusType::BigDecimal>(u)) {
        return std::get<ZeusType::BigDecimal>(u) != ZeusType::BigDecimal(0);
    }

    if (std::holds_alternative<bool>(u)) return std::get<bool>(u);

    if (std::holds_alternative<std::string>(u)) {
        return !std::get<std::string>(u).empty();
    }

    if (std::holds_alternative<ZeusType::Variant>(u)) {
        const auto& var = std::get<ZeusType::Variant>(u);

        if (std::holds_alternative<ZeusType::Null>(var)) return false;
    }

    return true;
}

} // namespace

void zeus_push(ZeusBox* box) {
    abiOpStack().push_back(box ? box : nullBox());
}

ZeusBox* zeus_pop(void) {
    auto& stack = abiOpStack();

    if (stack.empty()) return nullBox();

    ZeusBox* top = stack.back();
    stack.pop_back();
    return top ? top : nullBox();
}

void zeus_local_store(int idx, ZeusBox* box) {
    if (!box) return;
    deletedLocalSlots().erase(idx);

    Cell* cell = abiEnsureLocal(idx);

    if (!cell) {
        delete box;
        return;
    }

    cell->writeValue(box->v);
    delete box;
}

ZeusBox* zeus_local_load(int idx) {
    if (deletedLocalSlots().find(idx) != deletedLocalSlots().end()) {
        throw std::runtime_error("read from deleted local variable");
    }
    Cell* cell = abiEnsureLocal(idx);

    if (!cell) return nullBox();

    ZeusBox* box = ZEUS_NEW_BOX();
    box->assignVariant(cell->readValue());
    return box;
}

ZeusBox* zeus_less_than(const ZeusBox* a, const ZeusBox* b) {
    bool result = false;

    if (a && b) {
        // 快路径：双 int64。
        int64_t x = 0, y = 0;

        if (ZeusVMUtils::tryGetInt64(a->v, x) && ZeusVMUtils::tryGetInt64(b->v, y)) {
            result = (x < y);
        } else {
            // 慢路径：与 VM LESS_THAN 一致，转 BigDecimal 比较。
            ZeusType::BigDecimal dx(0);
            ZeusType::BigDecimal dy(0);

            if (ZeusVMUtils::extractNumber(a->v, dx) && ZeusVMUtils::extractNumber(b->v, dy)) {
                result = (dx < dy);
            }
        }
    }

    ZeusBox* box = ZEUS_NEW_BOX();
    box->assignVariant(LexerType::Value(result));
    return box;
}

int32_t zeus_truthy(const ZeusBox* box) {
    if (!box) return 0;

    return truthyImpl(box->v) ? 1 : 0;
}
