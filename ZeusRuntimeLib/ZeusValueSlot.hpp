#pragma once

#include "ZeusABIInternal.hpp"

#include <exception>
#include <functional>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

// VM cells keep the same 16-byte representation used by the LLVM ABI. Dynamic
// VM operations decode a temporary UniversalVariant at this boundary; strings
// and bytes own direct [u64 length][data] buffers, BigInteger/BigDecimal own
// typed native payloads, and graph-managed values use a ZeusBox whose root or
// graph-edge status follows the slot registration.
class ZeusValueSlot final {
    using UniversalVariant = ZeusType::UniversalVariant;

    class OwnedValue final {
    public:
        explicit OwnedValue(bool gcEdge = false) : gcEdge_(gcEdge) {
            zeus_value_init_nil(&value);
            if (gcEdge_) zeus_value_slot_register_gc_edge(&value);
        }
        OwnedValue(const OwnedValue&) = delete;
        OwnedValue& operator=(const OwnedValue&) = delete;

        ~OwnedValue() noexcept {
            if (gcEdge_) zeus_value_slot_unregister_gc_edge(&value);
            if (gcEdge_) ZeusValueSlot::releaseInternalNoexcept(value);
            else ZeusValueSlot::releaseNoexcept(value);
        }

        ZeusValue take() noexcept {
            if (gcEdge_) {
                zeus_value_slot_unregister_gc_edge(&value);
                gcEdge_ = false;
            }
            ZeusValue result = value;
            zeus_value_init_nil(&value);
            return result;
        }

        ZeusValue value{};

    private:
        bool gcEdge_ = false;
    };

public:
    ZeusValueSlot() : value_{} {
        zeus_value_init_nil(&value_);
    }

    explicit ZeusValueSlot(bool gcEdge) : value_{} {
        zeus_value_init_nil(&value_);
        if (gcEdge) zeus_value_slot_register_gc_edge(&value_);
    }

    explicit ZeusValueSlot(const UniversalVariant& value) : value_{} {
        zeus_value_init_nil(&value_);
        encodeVariant(&value_, value);
    }

    ZeusValueSlot(const ZeusValueSlot&) = delete;
    ZeusValueSlot& operator=(const ZeusValueSlot&) = delete;
    ZeusValueSlot(ZeusValueSlot&&) = delete;
    ZeusValueSlot& operator=(ZeusValueSlot&&) = delete;

    ~ZeusValueSlot() noexcept {
        const bool gcEdge = zeus_value_slot_is_gc_edge(&value_);
        if (gcEdge) zeus_value_slot_unregister_gc_edge(&value_);
        if (gcEdge) releaseInternalNoexcept(value_);
        else releaseNoexcept(value_);
    }

    UniversalVariant read() const {
        std::lock_guard<std::recursive_mutex> lock(ZeusGC::graphMutex());
        return decode(value_);
    }

    template <typename Function>
    void withVariant(Function&& function) {
        using Result = std::invoke_result_t<Function, UniversalVariant&>;
        static_assert(std::is_void_v<Result>,
            "ZeusValueSlot callbacks must not return references or pointers");

        std::lock_guard<std::recursive_mutex> lock(ZeusGC::graphMutex());
        if (hasOwnedBoxTag(value_)) {
            std::invoke(std::forward<Function>(function), checkedBox().v);
            return;
        }

        UniversalVariant current = decode(value_);
        try {
            std::invoke(std::forward<Function>(function), current);
        } catch (...) {
            const std::exception_ptr failure = std::current_exception();
            storeVariant(current);
            std::rethrow_exception(failure);
        }
        storeVariant(current);
    }

    template <typename Function>
    void withVariant(Function&& function) const {
        using Result = std::invoke_result_t<Function, const UniversalVariant&>;
        static_assert(std::is_void_v<Result>,
            "ZeusValueSlot callbacks must not return references or pointers");

        std::lock_guard<std::recursive_mutex> lock(ZeusGC::graphMutex());
        if (hasOwnedBoxTag(value_)) {
            std::invoke(std::forward<Function>(function), checkedBox().v);
            return;
        }

        const UniversalVariant current = decode(value_);
        std::invoke(std::forward<Function>(function), current);
    }

    void write(const UniversalVariant& value) {
        std::lock_guard<std::recursive_mutex> lock(ZeusGC::graphMutex());
        storeVariant(value);
    }

    // Return the stable address for inline values, scalar references, direct
    // buffers, and typed numeric payloads. Graph-managed values are accessed
    // through withVariant so callers can find their native payload without
    // exposing the ZeusValue header.
    void* nativeAddress() {
        std::lock_guard<std::recursive_mutex> lock(ZeusGC::graphMutex());
        switch (value_.tag) {
        case ZEUS_VALUE_TAG_NIL:
            validateInline(value_);
            return &value_.payload;
        case ZEUS_VALUE_TAG_BOOL:
            validateInline(value_);
            return &value_.bool_payload;
        case ZEUS_VALUE_TAG_CHAR:
            validateInline(value_);
            return &value_.char_payload;
        case ZEUS_VALUE_TAG_INT64:
            validateInline(value_);
            return &value_.int64_payload;
        case ZEUS_VALUE_TAG_REF_INT64:
        case ZEUS_VALUE_TAG_REF_BOOL:
            validateReference(value_);
            return reinterpret_cast<void*>(static_cast<uintptr_t>(value_.payload));
        case ZEUS_VALUE_TAG_STRING:
        case ZEUS_VALUE_TAG_BYTES:
        case ZEUS_VALUE_TAG_BIGINT:
        case ZEUS_VALUE_TAG_BIGDECIMAL:
            return zeus_value_native_address(&value_, 1);
        case ZEUS_VALUE_TAG_BOXED:
            validateBox(value_);
            return nullptr;
        case ZEUS_VALUE_TAG_DELETED:
            throw std::runtime_error("address of deleted variable");
        default:
            throw std::logic_error("invalid ZeusValueSlot tag");
        }
    }

private:
    static bool isBoxTag(uint32_t tag) noexcept {
        return tag == ZEUS_VALUE_TAG_BOXED;
    }

    static bool hasOwnedBoxTag(const ZeusValue& value) noexcept {
        return isBoxTag(value.tag) &&
            value.flags == ZEUS_VALUE_FLAG_OWNS_BOX && value.payload != 0;
    }

    static void validateInline(const ZeusValue& value) {
        if (value.flags != 0) {
            throw std::logic_error("invalid inline ZeusValueSlot ownership flags");
        }
        if (value.tag == ZEUS_VALUE_TAG_NIL && value.payload != 0) {
            throw std::logic_error("invalid nil ZeusValueSlot payload");
        }
    }

    static void validateReference(const ZeusValue& value) {
        if (value.flags != 0 || value.payload == 0) {
            throw std::logic_error("invalid ZeusValueSlot reference");
        }
    }

    static void validateBox(const ZeusValue& value) {
        if (!hasOwnedBoxTag(value)) {
            throw std::logic_error("invalid ZeusValueSlot box tag or ownership flags");
        }
    }

    static std::pair<const uint8_t*, size_t> bufferContents(
        const ZeusValue& value, uint32_t expectedTag
    ) {
        if (value.tag != expectedTag ||
            value.flags != ZEUS_VALUE_FLAG_OWNS_BUFFER || value.payload == 0) {
            throw std::logic_error("invalid ZeusValueSlot buffer tag or ownership flags");
        }
        const auto* data = static_cast<const uint8_t*>(
            zeus_value_native_address(&value, 1));
        uint64_t length = 0;
        std::memcpy(&length, data - sizeof(length), sizeof(length));
        if (length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::logic_error("ZeusValueSlot buffer length exceeds the platform limit");
        }
        return {data, static_cast<size_t>(length)};
    }

    static ZeusBox* boxFrom(const ZeusValue& value) noexcept {
        return reinterpret_cast<ZeusBox*>(
            static_cast<uintptr_t>(value.payload));
    }

    ZeusBox& checkedBox() {
        validateBox(value_);
        return *boxFrom(value_);
    }

    const ZeusBox& checkedBox() const {
        validateBox(value_);
        return *boxFrom(value_);
    }

    static UniversalVariant decode(const ZeusValue& value) {
        switch (value.tag) {
        case ZEUS_VALUE_TAG_NIL:
            validateInline(value);
            return UniversalVariant(ZeusType::Variant(ZeusType::Null()));
        case ZEUS_VALUE_TAG_BOOL:
            validateInline(value);
            return UniversalVariant(LexerType::Value(
                zeus_value_require_bool(&value) != 0));
        case ZEUS_VALUE_TAG_CHAR:
            validateInline(value);
            return UniversalVariant(static_cast<char>(value.char_payload));
        case ZEUS_VALUE_TAG_INT64:
            validateInline(value);
            return UniversalVariant(LexerType::Value(
                zeus_value_require_i64(&value)));
        case ZEUS_VALUE_TAG_REF_INT64:
        case ZEUS_VALUE_TAG_REF_BOOL:
            validateReference(value);
            return UniversalVariant(ZeusType::ReferenceHandle{
                reinterpret_cast<void*>(static_cast<uintptr_t>(value.payload)),
                value.tag == ZEUS_VALUE_TAG_REF_INT64 ? "int" : "bool"
            });
        case ZEUS_VALUE_TAG_STRING: {
            const auto [data, length] = bufferContents(value, ZEUS_VALUE_TAG_STRING);
            return UniversalVariant(std::string(
                reinterpret_cast<const char*>(data), length));
        }
        case ZEUS_VALUE_TAG_BIGINT: {
            const auto* integer = static_cast<const ZeusType::BigInteger*>(
                zeus_value_native_address(&value, 1));
            if (!integer) throw std::logic_error("missing native BigInteger payload");
            if ((value.flags & ZEUS_VALUE_FLAG_LEXER_VALUE) != 0) {
                return UniversalVariant(LexerType::Value(*integer));
            }
            return UniversalVariant(*integer);
        }
        case ZEUS_VALUE_TAG_BIGDECIMAL: {
            const auto* decimal = static_cast<const ZeusType::BigDecimal*>(
                zeus_value_native_address(&value, 1));
            if (!decimal) throw std::logic_error("missing native BigDecimal payload");
            if ((value.flags & ZEUS_VALUE_FLAG_LEXER_VALUE) != 0) {
                return UniversalVariant(LexerType::Value(*decimal));
            }
            return UniversalVariant(*decimal);
        }
        case ZEUS_VALUE_TAG_BYTES: {
            const auto [data, length] = bufferContents(value, ZEUS_VALUE_TAG_BYTES);
            ZeusType::Bytes bytes;
            if (length != 0) bytes.data.assign(data, data + length);
            return UniversalVariant(std::move(bytes));
        }
        case ZEUS_VALUE_TAG_BOXED:
            validateBox(value);
            return boxFrom(value)->v;
        case ZEUS_VALUE_TAG_DELETED:
            throw std::runtime_error("read from deleted variable");
        default:
            throw std::logic_error("invalid ZeusValueSlot tag");
        }
    }

    static void encodeVariant(ZeusValue* out, const UniversalVariant& value) {
        ZeusBox source;
        source.assignVariant(value);
        zeus_value_from_box(out, &source);
    }

    void storeVariant(const UniversalVariant& value) {
        OwnedValue replacement(zeus_value_slot_is_gc_edge(&value_));
        encodeVariant(&replacement.value, value);
        zeus_value_release(&value_);
        value_ = replacement.take();
    }

    static void releaseNoexcept(ZeusValue& value) noexcept {
        try {
            zeus_value_release(&value);
        } catch (...) {
            std::terminate();
        }
    }

    static void releaseInternalNoexcept(ZeusValue& value) noexcept {
        try {
            zeus_value_release_internal(&value);
        } catch (...) {
            std::terminate();
        }
    }

    ZeusValue value_;
};

static_assert(sizeof(ZeusValueSlot) == sizeof(ZeusValue),
    "ZeusValueSlot must preserve the 16-byte ZeusValue layout");
static_assert(sizeof(ZeusValueSlot) == 16,
    "ZeusValueSlot ABI size must remain 16 bytes");
