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
#include "ZeusObjectLifetime.hpp"
#include "ZeusRuntimeType.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusDynamicBinaryABI.hpp"
#include "ZeusDynamicMemberABI.hpp"

#include "ZeusVariant.hpp"
#include "ZeusSharedHeap.hpp"
#include "ZeusVMUtils.hpp"
#include "ZeusCell.hpp"

#include <cstdio>
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
#include <stdexcept>

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

std::unordered_set<const ZeusValue*>& gcEdgeValueSlots() {
    // Slot owners can be torn down from static runtime objects in other
    // translation units; keep this registry alive until process exit.
    static auto* slots = new std::unordered_set<const ZeusValue*>();
    return *slots;
}

void releaseSlotValue(ZeusValue* slot) {
    if (!slot) dynamicValuePanic("null value slot");
    if (slot->flags == ZEUS_VALUE_FLAG_OWNS_BOX) {
        zeus_value_release_internal(slot);
        return;
    }
    if (slot->flags != 0) dynamicValuePanic("invalid value-slot ownership flags");
    setNilValue(slot);
}

void setSignedPayload(ZeusValue* out, int64_t value) {
    std::memcpy(&out->payload, &value, sizeof(value));
}

int64_t getSignedPayload(const ZeusValue* value) {
    int64_t result = 0;
    std::memcpy(&result, &value->payload, sizeof(result));
    return result;
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

const ZeusType::Bytes* bytesUniversalValue(const ZeusType::UniversalVariant& value) {
    if (const auto* bytes = std::get_if<ZeusType::Bytes>(&value)) return bytes;
    if (const auto* variant = std::get_if<ZeusType::Variant>(&value)) {
        return std::get_if<ZeusType::Bytes>(variant);
    }
    return nullptr;
}

std::mutex& nativeStringPoolMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, ZeusStringView>& nativeStringPool() {
    static std::unordered_map<std::string, ZeusStringView> strings;
    return strings;
}

const ZeusStringView* internNativeString(std::string text) {
    std::lock_guard<std::mutex> lock(nativeStringPoolMutex());
    auto& pool = nativeStringPool();
    auto found = pool.emplace(std::move(text), ZeusStringView{ nullptr, 0 }).first;
    found->second.data = found->first.data();
    found->second.length = found->first.size();
    return &found->second;
}

std::string_view nativeStringContents(const ZeusStringView* view) {
    if (!view || (view->length > 0 && !view->data) ||
        view->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        view->length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        dynamicValuePanic("invalid native string view");
    }
    return std::string_view(
        view->data ? view->data : "",
        static_cast<size_t>(view->length));
}

template <typename Builder>
const ZeusStringView* buildNativeStringView(Builder&& builder) {
    try {
        const std::string result = builder();
        if (result.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            dynamicValuePanic("native string result is too large");
        }
        return internNativeString(std::move(result));
    } catch (...) {
        dynamicValuePanic("failed to build native string result");
    }
}

std::mutex& nativeBytesPoolMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, ZeusBytesView>& nativeBytesPool() {
    static std::unordered_map<std::string, ZeusBytesView> bytes;
    return bytes;
}

const ZeusBytesView* internNativeBytes(const uint8_t* data, size_t length) {
    if (length > 0 && !data) dynamicValuePanic("null native bytes data");
    std::lock_guard<std::mutex> lock(nativeBytesPoolMutex());
    auto& pool = nativeBytesPool();
    const std::string contents(
        length == 0 ? "" : reinterpret_cast<const char*>(data), length);
    auto found = pool.emplace(contents, ZeusBytesView{ nullptr, 0 }).first;
    found->second.data = reinterpret_cast<const uint8_t*>(found->first.data());
    found->second.length = found->first.size();
    return &found->second;
}

bool isReferenceTag(uint32_t tag) {
    return tag == ZEUS_VALUE_TAG_BIGINT ||
           tag == ZEUS_VALUE_TAG_BIGDECIMAL ||
           tag == ZEUS_VALUE_TAG_STRING ||
           tag == ZEUS_VALUE_TAG_BOXED;
}

std::mutex& llvmClassRegistryMutex() {
    static std::mutex mutex;
    return mutex;
}

std::atomic<uint64_t>& nextLLVMClassDebugId() {
    static std::atomic<uint64_t> nextId{ 1 };
    return nextId;
}

std::unordered_map<void*, std::weak_ptr<ZeusType::LLVMClassObjectState>>&
llvmClassRegistry() {
    static std::unordered_map<void*, std::weak_ptr<ZeusType::LLVMClassObjectState>> registry;
    return registry;
}

std::shared_ptr<ZeusType::LLVMClassObjectState> getOrCreateLLVMClassState(
    void* object,
    const char* typeName
) {
    if (!object || !typeName || !*typeName) {
        dynamicValuePanic("class pointer and type name must be non-null");
    }

    std::lock_guard<std::mutex> lock(llvmClassRegistryMutex());
    auto& registry = llvmClassRegistry();
    auto found = registry.find(object);
    if (found != registry.end()) {
        auto existing = found->second.lock();
        if (existing) {
            if (existing->typeName != typeName ||
                existing->object.load(std::memory_order_acquire) != object) {
                dynamicValuePanic("class pointer was registered with a different type");
            }
            return existing;
        }
        registry.erase(found);
    }

    auto state = std::make_shared<ZeusType::LLVMClassObjectState>();
    state->typeName = typeName;
    state->debugId = nextLLVMClassDebugId().fetch_add(1, std::memory_order_relaxed);
    state->object.store(object, std::memory_order_release);
    if (zeus_class_ref_try_retain(object)) {
        state->lifetime = std::shared_ptr<void>(object, [](void* retainedObject) noexcept {
            try {
                zeus_class_ref_release(retainedObject);
            } catch (...) {
                std::abort();
            }
        });
    }
    registry[object] = state;
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
    if (!value || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) return false;
    if (value->tag == ZEUS_VALUE_TAG_STRING) return true;
    if (value->tag != ZEUS_VALUE_TAG_BOXED) return false;
    const ZeusType::UniversalVariant& boxed = valueReference(value)->v;
    const auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&boxed);
    return classHandle && classHandle->state &&
           classHandle->state->typeName == "String";
}

void concatNativeStrings(ZeusValue* out, const ZeusValue* lhs, const ZeusValue* rhs) {
    const ZeusStringView* left = zeus_value_intern_string_view(lhs);
    const ZeusStringView* right = zeus_value_intern_string_view(rhs);
    try {
        std::string joined;
        joined.reserve(static_cast<size_t>(left->length + right->length));
        joined.append(left->data, static_cast<size_t>(left->length));
        joined.append(right->data, static_cast<size_t>(right->length));
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
    if (!value || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) return false;
    const ZeusType::UniversalVariant& boxed = valueReference(value)->v;
    if (std::holds_alternative<ZeusType::LLVMClassHandle>(boxed)) return false;
    const auto* text = stringUniversalValue(boxed);
    if (!text) return false;
    out = *text;
    return true;
}

bool nativeStringEqualsText(const ZeusValue* value, const std::string& text) {
    const ZeusStringView* view = zeus_value_intern_string_view(value);
    return std::string_view(view->data, static_cast<size_t>(view->length)) == text;
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
    if (!value || !isReferenceTag(value->tag) ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX || value->payload == 0) {
        dynamicValuePanic("invalid owned reference value");
    }
    return reinterpret_cast<ZeusBox*>(static_cast<uintptr_t>(value->payload));
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
    ZeusBox* box = new ZeusBox();
    box->v = nullVariant();
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
            const ZeusStringView* rendered =
                zeus_value_intern_string_view(&stringified.value);
            std::cout.write(rendered->data,
                            static_cast<std::streamsize>(rendered->length));
            return;
        }
    }

    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    printUniversalValue(boxed->v);
}

} // namespace

ZeusBox* zeus_box_int(int64_t v) {
    ZeusBox* box = new ZeusBox();
    box->v = LexerType::Value(v); // 小整数路径：零堆分配
    return box;
}

ZeusBox* zeus_box_bool(int32_t v) {
    ZeusBox* box = new ZeusBox();
    box->v = LexerType::Value(v != 0);
    return box;
}

ZeusBox* zeus_box_cstr(const char* s) {
    ZeusBox* box = new ZeusBox();
    box->v = LexerType::Value(s ? std::string(s) : std::string());
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
    setSignedPayload(out, value);
}

void zeus_value_init_bool(ZeusValue* out, int32_t value) {
    if (!out) dynamicValuePanic("null output pointer");
    out->tag = ZEUS_VALUE_TAG_BOOL;
    out->flags = 0;
    out->payload = value != 0 ? 1 : 0;
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
            case ZEUS_VALUE_TAG_BIGINT:
            case ZEUS_VALUE_TAG_BIGDECIMAL:
            case ZEUS_VALUE_TAG_STRING:
            case ZEUS_VALUE_TAG_BOXED: {
                ZeusBox* copy = rooted
                    ? new ZeusBox(*valueReference(value))
                    : new ZeusBox(*valueReference(value), false);
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
    cloneDynamicValue(out, value, true);
}

void zeus_value_slot_register_gc_edge(ZeusValue* slot) {
    if (!slot) dynamicValuePanic("cannot register a null GC-edge value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    gcEdgeValueSlots().insert(slot);
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

    try {
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

        uint32_t tag = ZEUS_VALUE_TAG_BOXED;
        if (isBigIntegerValue(box->v)) tag = ZEUS_VALUE_TAG_BIGINT;
        else if (isBigDecimalValue(box->v)) tag = ZEUS_VALUE_TAG_BIGDECIMAL;
        else if (isStringUniversalValue(box->v)) tag = ZEUS_VALUE_TAG_STRING;

        ZeusBox* copy = new ZeusBox(*box);
        out->tag = tag;
        out->flags = ZEUS_VALUE_FLAG_OWNS_BOX;
        out->payload = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(copy));
    } catch (...) {
        dynamicValuePanic("failed to copy boxed runtime value");
    }
}

void zeus_value_from_class_ptr(ZeusValue* out, void* object, const char* type_name) {
    if (!out) dynamicValuePanic("null class value output pointer");
    setNilValue(out);
    if (!object) return;

    try {
        ZeusBox box;
        box.v = ZeusType::LLVMClassHandle{
            getOrCreateLLVMClassState(object, type_name)
        };
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to box LLVM class pointer");
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

    std::lock_guard<std::mutex> lock(llvmClassRegistryMutex());
    auto& registry = llvmClassRegistry();
    auto found = registry.find(object);
    if (found == registry.end()) return;

    auto state = found->second.lock();
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
    return getSignedPayload(value);
}

int32_t zeus_value_require_bool(const ZeusValue* value) {
    if (value && value->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("read from deleted variable");
    }
    if (!value || value->flags != 0 ||
        (value->tag != ZEUS_VALUE_TAG_BOOL && value->tag != ZEUS_VALUE_TAG_REF_BOOL) ||
        (value->tag == ZEUS_VALUE_TAG_BOOL && value->payload > 1)) {
        dynamicValuePanic("expected an inline bool value");
    }
    if (value->tag == ZEUS_VALUE_TAG_REF_BOOL) {
        if (value->payload == 0) dynamicValuePanic("null bool reference");
        return *reinterpret_cast<const bool*>(static_cast<uintptr_t>(value->payload)) ? 1 : 0;
    }
    return value->payload != 0 ? 1 : 0;
}

const ZeusStringView* zeus_value_intern_string_view(const ZeusValue* value) {
    if (!value || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX ||
        (value->tag != ZEUS_VALUE_TAG_STRING && value->tag != ZEUS_VALUE_TAG_BOXED)) {
        dynamicValuePanic("expected an owned string value for native string-view ABI");
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
        if (rendered.value.tag != ZEUS_VALUE_TAG_STRING &&
            rendered.value.tag != ZEUS_VALUE_TAG_BOXED) {
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
        // Native fields and calls may outlive the temporary ZeusValue used to
        // cross the boundary. The interned view owns stable bytes for process life.
        return internNativeString(*text);
    } catch (...) {
        dynamicValuePanic("failed to intern native string view");
    }
}

const ZeusStringView* zeus_string_view_intern(const ZeusStringView* view) {
    if (!view || (view->length > 0 && !view->data) ||
        view->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicValuePanic("invalid native string view");
    }
    try {
        std::string text(
            view->data ? view->data : "",
            static_cast<size_t>(view->length));
        return internNativeString(std::move(text));
    } catch (...) {
        dynamicValuePanic("failed to intern native string view");
    }
}

const ZeusStringView* zeus_string_view_from_data(const char* data, int64_t length) {
    if (length < 0 || (length > 0 && !data)) {
        dynamicValuePanic("invalid native string data");
    }
    try {
        std::string text(data ? data : "", static_cast<size_t>(length));
        return internNativeString(std::move(text));
    } catch (...) {
        dynamicValuePanic("failed to intern native string data");
    }
}

const ZeusBytesView* zeus_value_intern_bytes_view(const ZeusValue* value) {
    if (!value || value->tag != ZEUS_VALUE_TAG_BOXED ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) {
        dynamicValuePanic("expected an owned Bytes value for native bytes-view ABI");
    }
    const auto* bytes = bytesUniversalValue(valueReference(value)->v);
    if (!bytes) dynamicValuePanic("boxed value is not Bytes");
    try {
        return internNativeBytes(bytes->data.data(), bytes->data.size());
    } catch (...) {
        dynamicValuePanic("failed to intern native bytes view");
    }
}

const ZeusBytesView* zeus_bytes_view_intern(const ZeusBytesView* view) {
    if (!view || (view->length > 0 && !view->data) ||
        view->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicValuePanic("invalid native bytes view");
    }
    try {
        return internNativeBytes(view->data, static_cast<size_t>(view->length));
    } catch (...) {
        dynamicValuePanic("failed to intern native bytes view");
    }
}

const ZeusBytesView* zeus_bytes_view_from_data(const uint8_t* data, int64_t length) {
    if (length < 0 || (length > 0 && !data)) {
        dynamicValuePanic("invalid native bytes data");
    }
    try {
        return internNativeBytes(data, static_cast<size_t>(length));
    } catch (...) {
        dynamicValuePanic("failed to intern native bytes data");
    }
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
            case ZEUS_VALUE_TAG_INT64:
                return zeus_box_int(zeus_value_require_i64(value));
            case ZEUS_VALUE_TAG_REF_INT64:
            case ZEUS_VALUE_TAG_REF_BOOL: {
                if (value->flags != 0 || value->payload == 0) {
                    dynamicValuePanic("invalid non-owning reference value");
                }
                auto* box = new ZeusBox();
                box->v = ZeusType::ReferenceHandle{
                    reinterpret_cast<void*>(static_cast<uintptr_t>(value->payload)),
                    value->tag == ZEUS_VALUE_TAG_REF_INT64 ? "int" : "bool"
                };
                return box;
            }
            case ZEUS_VALUE_TAG_BIGINT:
            case ZEUS_VALUE_TAG_BIGDECIMAL:
            case ZEUS_VALUE_TAG_STRING:
            case ZEUS_VALUE_TAG_BOXED:
                return new ZeusBox(*valueReference(value));
            default:
                dynamicValuePanic("unknown value tag");
        }
    } catch (...) {
        dynamicValuePanic("failed to materialize boxed runtime value");
    }
}

void zeus_value_release_internal(ZeusValue* value) {
    if (!value) return;
    if (value->flags == ZEUS_VALUE_FLAG_OWNS_BOX) {
        delete valueReference(value);
    } else if (value->flags != 0 || isReferenceTag(value->tag)) {
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
        ZeusBox* box = zeus_box_cstr(text);
        zeus_value_from_box(out, box);
        zeus_box_release(box);
    } catch (...) {
        dynamicValuePanic("failed to create string value");
    }
}

void zeus_value_from_string_data(ZeusValue* out, const char* data, int64_t length) {
    if (!out || length < 0 || (length > 0 && !data)) {
        dynamicValuePanic("invalid string input");
    }
    try {
        ZeusBox box;
        box.v = std::string(data ? data : "", static_cast<size_t>(length));
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to create string value");
    }
}

void zeus_value_from_string_view(ZeusValue* out, const ZeusStringView* view) {
    if (!out || !view || (view->length > 0 && !view->data) ||
        view->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicValuePanic("invalid native string view");
    }
    try {
        ZeusBox box;
        box.v = std::string(
            view->data ? view->data : "",
            static_cast<size_t>(view->length));
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to create dynamic string from native view");
    }
}

void zeus_value_from_bytes_view(ZeusValue* out, const ZeusBytesView* view) {
    if (!out || !view || (view->length > 0 && !view->data) ||
        view->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicValuePanic("invalid native bytes view");
    }
    try {
        ZeusBox box;
        ZeusType::Bytes bytes;
        if (view->length > 0) {
            bytes.data.assign(view->data, view->data + static_cast<size_t>(view->length));
        }
        box.v = std::move(bytes);
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to create dynamic Bytes from native view");
    }
}

int64_t zeus_bytes_get(const ZeusBytesView* bytes, int64_t index) {
    if (!bytes || (bytes->length > 0 && !bytes->data) || index < 0 ||
        static_cast<uint64_t>(index) >= bytes->length) {
        dynamicValuePanic("Bytes index out of range");
    }
    return static_cast<int64_t>(bytes->data[index]);
}

int64_t zeus_bytes_len(const ZeusBytesView* bytes) {
    if (!bytes || bytes->length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        dynamicValuePanic("invalid Bytes view length");
    }
    return static_cast<int64_t>(bytes->length);
}

const ZeusBytesView* zeus_bytes_concat(
    const ZeusBytesView* left,
    const ZeusBytesView* right
) {
    if (!left || !right ||
        (left->length > 0 && !left->data) ||
        (right->length > 0 && !right->data) ||
        left->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        right->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        left->length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        right->length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        left->length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - right->length) {
        dynamicValuePanic("invalid Bytes views for concatenation");
    }

    const size_t leftLength = static_cast<size_t>(left->length);
    const size_t rightLength = static_cast<size_t>(right->length);
    std::string contents;
    contents.reserve(leftLength + rightLength);
    if (leftLength > 0) {
        contents.append(reinterpret_cast<const char*>(left->data), leftLength);
    }
    if (rightLength > 0) {
        contents.append(reinterpret_cast<const char*>(right->data), rightLength);
    }
    return internNativeBytes(
        reinterpret_cast<const uint8_t*>(contents.data()), contents.size());
}

const ZeusBytesView* zeus_bytes_slice(
    const ZeusBytesView* bytes,
    int64_t start,
    int64_t end
) {
    if (!bytes || (bytes->length > 0 && !bytes->data) ||
        bytes->length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        dynamicValuePanic("invalid Bytes view for slicing");
    }
    if (end <= 0) return zeus_bytes_view_from_data(nullptr, 0);

    const int64_t length = static_cast<int64_t>(bytes->length);
    const int64_t begin = std::max<int64_t>(0, start);
    const int64_t finish = std::min<int64_t>(length, end);
    const int64_t clampedBegin = std::min(begin, finish);
    const uint8_t* data = bytes->data
        ? bytes->data + clampedBegin
        : nullptr;
    return zeus_bytes_view_from_data(data, finish - clampedBegin);
}

const ZeusStringView* zeus_bytes_decode(
    const ZeusBytesView* bytes,
    const ZeusStringView* encoding
) {
    (void)encoding;
    if (!bytes || (bytes->length > 0 && !bytes->data) ||
        bytes->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicValuePanic("invalid Bytes view for decoding");
    }
    try {
        std::string text(
            bytes->length == 0 ? "" : reinterpret_cast<const char*>(bytes->data),
            static_cast<size_t>(bytes->length));
        return internNativeString(std::move(text));
    } catch (...) {
        dynamicValuePanic("failed to decode Bytes view");
    }
}

void zeus_bytes_to_list(ZeusValue* out, const ZeusBytesView* bytes) {
    if (!out || !bytes || (bytes->length > 0 && !bytes->data) ||
        bytes->length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        dynamicValuePanic("invalid Bytes view for list conversion");
    }
    try {
        ZeusType::Array array;
        array.impl = std::make_shared<ZeusType::ArrayData>();
        array.impl->data.reserve(static_cast<size_t>(bytes->length));
        for (uint64_t i = 0; i < bytes->length; ++i) {
            array.impl->data.emplace_back(
                ZeusType::BigInteger(static_cast<int64_t>(bytes->data[i])));
        }
        ZeusBox box;
        box.v = ZeusType::Variant(std::move(array));
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to convert Bytes to list");
    }
}

int64_t zeus_string_length_view(const ZeusStringView* value) {
    return static_cast<int64_t>(nativeStringContents(value).size());
}

const ZeusStringView* zeus_string_get_view(
    const ZeusStringView* value,
    int64_t index
) {
    const std::string_view text = nativeStringContents(value);
    const int64_t length = static_cast<int64_t>(text.size());
    if (index < 0) index += length;
    if (index < 0 || index >= length) {
        dynamicValuePanic("String index out of range");
    }
    return zeus_string_view_from_data(text.data() + index, 1);
}

const ZeusStringView* zeus_string_slice_view(
    const ZeusStringView* value,
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
    return zeus_string_view_from_data(
        text.data() + start,
        end - start);
}

int64_t zeus_string_index_view(
    const ZeusStringView* value,
    const ZeusStringView* substring
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view needle = nativeStringContents(substring);
    const size_t position = text.find(needle);
    return position == std::string_view::npos
        ? -1
        : static_cast<int64_t>(position);
}

int64_t zeus_string_rindex_view(
    const ZeusStringView* value,
    const ZeusStringView* substring
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view needle = nativeStringContents(substring);
    const size_t position = text.rfind(needle);
    return position == std::string_view::npos
        ? -1
        : static_cast<int64_t>(position);
}

const ZeusStringView* zeus_string_replace_view(
    const ZeusStringView* value,
    const ZeusStringView* old_value,
    const ZeusStringView* replacement
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view oldText = nativeStringContents(old_value);
    const std::string_view replacementText = nativeStringContents(replacement);
    if (oldText.empty()) {
        dynamicValuePanic("String replace pattern cannot be empty");
    }
    return buildNativeStringView([&]() {
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

bool zeus_string_startswith_view(
    const ZeusStringView* value,
    const ZeusStringView* prefix
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view prefixText = nativeStringContents(prefix);
    return text.size() >= prefixText.size() &&
        text.compare(0, prefixText.size(), prefixText) == 0;
}

bool zeus_string_endswith_view(
    const ZeusStringView* value,
    const ZeusStringView* suffix
) {
    const std::string_view text = nativeStringContents(value);
    const std::string_view suffixText = nativeStringContents(suffix);
    return text.size() >= suffixText.size() &&
        text.compare(text.size() - suffixText.size(), suffixText.size(), suffixText) == 0;
}

const ZeusStringView* zeus_string_lower_view(const ZeusStringView* value) {
    const std::string_view text = nativeStringContents(value);
    return buildNativeStringView([&]() {
        std::string result(text.data(), text.size());
        for (char& character : result) {
            character = static_cast<char>(
                std::tolower(static_cast<unsigned char>(character)));
        }
        return result;
    });
}

const ZeusStringView* zeus_string_upper_view(const ZeusStringView* value) {
    const std::string_view text = nativeStringContents(value);
    return buildNativeStringView([&]() {
        std::string result(text.data(), text.size());
        for (char& character : result) {
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        }
        return result;
    });
}

int64_t zeus_string_count_view(
    const ZeusStringView* value,
    const ZeusStringView* substring
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
        ZeusBox box;
        ZeusType::Bytes bytes;
        if (length > 0) {
            bytes.data.assign(data, data + static_cast<size_t>(length));
        }
        box.v = std::move(bytes);
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to create bytes value");
    }
}

void zeus_value_from_bigint_decimal(ZeusValue* out, const char* decimalText) {
    if (!out || !decimalText) dynamicValuePanic("null bigint input");
    try {
        ZeusBox box;
        box.v = ZeusType::BigInteger(std::string(decimalText));
        zeus_value_from_box(out, &box);
    } catch (...) {
        dynamicValuePanic("failed to parse bigint value");
    }
}

void zeus_value_from_bigdecimal_decimal(ZeusValue* out, const char* decimalText) {
    if (!out || !decimalText) dynamicValuePanic("null bigdecimal input");
    try {
        ZeusBox box;
        box.v = ZeusType::BigDecimal(std::string(decimalText));
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

void zeus_value_slot_store(ZeusValue* slot, const ZeusValue* value) {
    if (!slot || !value) dynamicValuePanic("invalid value-slot store");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
    if (slot == value) return;
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
}

void zeus_value_slot_store_i64(ZeusValue* slot, int64_t value) {
    if (!slot) dynamicValuePanic("null int64 value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
    releaseSlotValue(slot);
    zeus_value_init_i64(slot, value);
}

void zeus_value_slot_store_bool(ZeusValue* slot, int32_t value) {
    if (!slot) dynamicValuePanic("null bool value slot");
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    if (slot->tag == ZEUS_VALUE_TAG_DELETED) {
        throw std::runtime_error("write to deleted variable");
    }
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
        ZeusBox* box = new ZeusBox();
        box->v = r;
        return box;
    } catch (...) {
        return nullBox(); // C ABI 边界不抛异常
    }
}

void zeus_print(const ZeusBox* box) {
    if (!box) {
        std::puts("(null)");
        return;
    }

    const auto& u = box->v;

    // 字符串打印原文（不对标调试表示的引号格式，与解释器 print 一致）。
    if (std::holds_alternative<LexerType::Value>(u)) {
        const auto& v = std::get<LexerType::Value>(u);

        if (isStringValue(v)) {
            std::puts(std::get<std::string>(v.data).c_str());
            return;
        }
    }

    if (std::holds_alternative<std::string>(u)) {
        std::puts(std::get<std::string>(u).c_str());
        return;
    }

    std::puts(ZeusVMUtils::valueToDebugString(u).c_str());
}

namespace {

struct GlobalValueSlot {
    ZeusValue value{};
    GlobalValueSlot() { zeus_value_init_nil(&value); }
};

std::vector<GlobalValueSlot*>& abiGlobalValueSlots() {
    static std::vector<GlobalValueSlot*> globals;
    return globals;
}

std::vector<GlobalValueSlot*>& retiredGlobalValueSlots() {
    static std::vector<GlobalValueSlot*> retired;
    return retired;
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

ZeusValue* abiResolveGlobalValueSlot(int idx, bool recreateDeleted) {
    GlobalValueSlot* slot = abiEnsureGlobalValueSlot(idx, recreateDeleted);
    if (!slot) return nullptr;
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
    zeus_value_slot_load(out, slot);
}

void abiClearGlobalValue(int idx) {
    if (idx < 0) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto& globals = abiGlobalValueSlots();
    if (idx >= static_cast<int>(globals.size()) || !globals[idx]) return;
    if (globals[idx]->value.tag == ZEUS_VALUE_TAG_DELETED) return;
    releaseSlotValue(&globals[idx]->value);
}

void abiDeleteGlobalValue(int idx) {
    if (idx < 0) return;
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    auto& globals = abiGlobalValueSlots();
    if (idx >= static_cast<int>(globals.size()) || !globals[idx]) return;
    ZeusValue* value = &globals[idx]->value;
    if (value->tag == ZEUS_VALUE_TAG_DELETED) return;
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
        destroyLLVMClassValue(locals[idx]->value);
        locals[idx]->value = nullVariant();
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
    snapshot.v = cell->read();
    zeus_value_from_box(out, &snapshot);
}

int32_t zeus_shared_value_delete(const char* name) {
    if (!name) return 0;
    auto cell = ZeusShared::getGlobalHeap().find(name);
    if (!cell) return 0;
    cell->lockWrite();
    try {
        const ZeusType::UniversalVariant current = cell->value;
        destroyLLVMClassValue(current);
        auto removed = ZeusShared::getGlobalHeap().erase(name, cell->generation);
        if (!removed) {
            cell->unlockWrite();
            return 0;
        }
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
        ZeusGC::addPermanentRootVariant(&locals[idx]->value);
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

    cell->value = box->v;
    delete box;
}

ZeusBox* zeus_local_load(int idx) {
    if (deletedLocalSlots().find(idx) != deletedLocalSlots().end()) {
        throw std::runtime_error("read from deleted local variable");
    }
    Cell* cell = abiEnsureLocal(idx);

    if (!cell) return nullBox();

    ZeusBox* box = new ZeusBox();
    box->v = cell->value;
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

    ZeusBox* box = new ZeusBox();
    box->v = LexerType::Value(result);
    return box;
}

int32_t zeus_truthy(const ZeusBox* box) {
    if (!box) return 0;

    return truthyImpl(box->v) ? 1 : 0;
}
