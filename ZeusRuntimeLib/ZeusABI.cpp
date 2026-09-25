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

#include "ZeusABIInternal.hpp"
#include "ZeusDynamicBinaryABI.hpp"
#include "ZeusDynamicMemberABI.hpp"

#include "ZeusVariant.hpp"
#include "ZeusVMUtils.hpp"
#include "ZeusCell.hpp"

#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <cstdint>
#include <limits>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

static_assert(sizeof(ZeusValue) == 16, "ZeusValue ABI size must remain 16 bytes");
static_assert(offsetof(ZeusValue, payload) == 8, "ZeusValue payload offset must remain 8");
static_assert(sizeof(uintptr_t) <= sizeof(uint64_t), "ZeusValue payload cannot hold a pointer");

[[noreturn]] void dynamicValuePanic(const char* message) {
    std::fprintf(stderr, "[ZeusValue ABI] %s\n", message);
    std::abort();
}

void setNilValue(ZeusValue* out) {
    if (!out) dynamicValuePanic("null output pointer");
    out->tag = ZEUS_VALUE_TAG_NIL;
    out->flags = 0;
    out->payload = 0;
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

std::mutex& nativeStringPoolMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<std::string, ZeusStringView>& nativeStringPool() {
    static std::unordered_map<std::string, ZeusStringView> strings;
    return strings;
}

const ZeusStringView* internNativeString(const std::string& text) {
    std::lock_guard<std::mutex> lock(nativeStringPoolMutex());
    auto& pool = nativeStringPool();
    auto found = pool.emplace(text, ZeusStringView{ nullptr, 0 }).first;
    found->second.data = found->first.data();
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

ZeusBox* valueReference(const ZeusValue* value) {
    if (!value || !isReferenceTag(value->tag) ||
        value->flags != ZEUS_VALUE_FLAG_OWNS_BOX || value->payload == 0) {
        dynamicValuePanic("invalid owned reference value");
    }
    return reinterpret_cast<ZeusBox*>(static_cast<uintptr_t>(value->payload));
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

void zeus_value_clone(ZeusValue* out, const ZeusValue* value) {
    if (!out || !value) dynamicValuePanic("null clone pointer");
    if (out == value) return;
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
                ZeusBox* copy = new ZeusBox(*valueReference(value));
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

int64_t zeus_value_require_i64(const ZeusValue* value) {
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
        const std::string text(
            view->data ? view->data : "",
            static_cast<size_t>(view->length));
        return internNativeString(text);
    } catch (...) {
        dynamicValuePanic("failed to intern native string view");
    }
}

const ZeusStringView* zeus_string_view_from_data(const char* data, int64_t length) {
    if (length < 0 || (length > 0 && !data)) {
        dynamicValuePanic("invalid native string data");
    }
    try {
        const std::string text(data ? data : "", static_cast<size_t>(length));
        return internNativeString(text);
    } catch (...) {
        dynamicValuePanic("failed to intern native string data");
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

void zeus_value_release(ZeusValue* value) {
    if (!value) return;
    if (value->flags == ZEUS_VALUE_FLAG_OWNS_BOX) {
        delete valueReference(value);
    } else if (value->flags != 0 || isReferenceTag(value->tag)) {
        dynamicValuePanic("invalid value ownership flags");
    }
    setNilValue(value);
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
        ZeusBox* box = zeus_value_to_box(value);
        zeus_value_release(value);
        zeus_global_store(idx, box);
    } catch (...) {
        dynamicValuePanic("failed to store dynamic global");
    }
}

void zeus_value_global_load(ZeusValue* out, int idx) {
    if (!out) dynamicValuePanic("null output pointer");
    setNilValue(out);
    try {
        ZeusBox* box = zeus_global_load(idx);
        zeus_value_from_box(out, box);
        zeus_box_release(box);
    } catch (...) {
        dynamicValuePanic("failed to load dynamic global");
    }
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
    try {
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> left(
            zeus_value_to_box(lhs), &zeus_box_release);
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> right(
            zeus_value_to_box(rhs), &zeus_box_release);
        if (isLLVMClassValue(left.get()) || isLLVMClassValue(right.get())) {
            dynamicValuePanic("LLVM class pointers do not support dynamic operators");
        }
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> result(
            zeus_dynamic_binary(
                static_cast<ZeusRuntimeContext*>(context), opcode, left.get(), right.get()),
            &zeus_box_release);
        zeus_value_from_box(out, result.get());
    } catch (...) {
        dynamicValuePanic("failed to evaluate dynamic binary operation");
    }
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

// ---- 全局量槽（Phase A 极简版） ----

namespace {

std::vector<Cell*>& abiGlobals() {
    static std::vector<Cell*> globals;
    return globals;
}

Cell* abiEnsureGlobal(int idx) {
    if (idx < 0) return nullptr;

    auto& globals = abiGlobals();

    if (idx >= (int)globals.size()) {
        globals.resize(idx + 1, nullptr);
    }

    if (!globals[idx]) {
        globals[idx] = new Cell{nullVariant(), 0, __nextCellId()};
        globals[idx]->createdBy = "zeus_abi_global";
        HeapStats::recordCreate();
        HeapStats::registerCell(globals[idx]);
    }

    return globals[idx];
}

} // namespace

void zeus_global_store(int idx, ZeusBox* box) {
    if (!box) return;

    Cell* cell = abiEnsureGlobal(idx);

    if (!cell) {
        delete box;
        return;
    }

    // variant 赋值自动释放旧字符串；对象/句柄随 Phase B 到来。
    cell->value = box->v;
    delete box;
}

ZeusBox* zeus_global_load(int idx) {
    Cell* cell = abiEnsureGlobal(idx);

    if (!cell) return nullBox();

    ZeusBox* box = new ZeusBox();
    box->v = cell->value; // 拷贝（小整数零分配；字符串一次拷贝）
    return box;
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

    Cell* cell = abiEnsureLocal(idx);

    if (!cell) {
        delete box;
        return;
    }

    cell->value = box->v;
    delete box;
}

ZeusBox* zeus_local_load(int idx) {
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
