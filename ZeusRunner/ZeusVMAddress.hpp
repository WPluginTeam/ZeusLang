#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "../ZeusRuntimeLib/ZeusCell.hpp"
#include "../ZeusRuntimeLib/ZeusRuntimeType.hpp"
#include "../ZeusRuntimeLib/ZeusSharedHeap.hpp"
#include "../ZeusRuntimeLib/ZeusNativeAddress.hpp"
#include "../ZeusRuntimeLib/ZeusVMUtils.hpp"
#include "../ZeusRuntimeLib/ZeusMemoryABI.hpp"

namespace ZeusVMAddress {

using ZeusNativeAddress::boolPayload;
using ZeusNativeAddress::bytesPayload;
using ZeusNativeAddress::decimalPayload;
using ZeusNativeAddress::integerPayload;
using ZeusNativeAddress::stringPayload;

inline std::string runtimeNativeTypeName(
    const ZeusType::UniversalVariant& value
) {
    auto integerType = [](const ZeusType::BigInteger& integer) {
        int64_t native = 0;
        return integer.tryToInt64(native) ? "int" : "bigint";
    };
    if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value)) {
        return integerType(*integer);
    }
    if (std::holds_alternative<ZeusType::BigDecimal>(value)) return "float";
    if (std::holds_alternative<bool>(value)) return "bool";
    if (std::holds_alternative<std::string>(value)) return "string";
    if (std::holds_alternative<ZeusType::Bytes>(value)) return "bytes";
    if (std::holds_alternative<ZeusType::RawBytesRange>(value)) return "&bytes";
    if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
        if (const auto* integer = std::get_if<ZeusType::BigInteger>(&literal->data)) {
            return integerType(*integer);
        }
        if (std::holds_alternative<ZeusType::BigDecimal>(literal->data)) return "float";
        if (std::holds_alternative<bool>(literal->data)) return "bool";
        if (std::holds_alternative<std::string>(literal->data)) return "string";
    }
    if (const auto* variant = std::get_if<ZeusType::Variant>(&value)) {
        if (std::holds_alternative<ZeusType::String>(*variant)) return "string";
        if (std::holds_alternative<ZeusType::Integer>(*variant)) return "int";
        if (std::holds_alternative<ZeusType::Float>(*variant)) return "float";
        if (std::holds_alternative<ZeusType::Boolean>(*variant)) return "bool";
        if (std::holds_alternative<ZeusType::Bytes>(*variant)) return "bytes";
        if (std::holds_alternative<ZeusType::Array>(*variant)) return "Array";
        if (std::holds_alternative<ZeusType::Null>(*variant)) return "nil";
        if (std::holds_alternative<ZeusType::Object>(*variant)) return "object";
    }
    return {};
}

inline bool address(Cell* cell, bool reveal, void*& out, std::string& error) {
    return ZeusNativeAddress::addressCell(cell, reveal, out, error);
}

inline bool address(ZeusShared::SharedCell* cell, bool reveal,
                    void*& out, std::string& error) {
    return ZeusNativeAddress::addressShared(cell, reveal, out, error);
}

struct NativeBufferSpan {
    const uint8_t* data = nullptr;
    const uint8_t* header = nullptr;
    uint64_t length = 0;
    uint64_t allocationSize = 0;
};

inline bool nativeBufferSpan(void* dataAddress, const std::string& type,
                             NativeBufferSpan& out, std::string& error) {
    if (!dataAddress) {
        error = "native string/bytes data pointer is null";
        return false;
    }
    const uintptr_t data = reinterpret_cast<uintptr_t>(dataAddress);
    if (data < sizeof(uint64_t)) {
        error = "native string/bytes header address underflows";
        return false;
    }
    const auto* header = reinterpret_cast<const uint8_t*>(data - sizeof(uint64_t));
    if (!zeus_memory_find_allocation(const_cast<uint8_t*>(header), sizeof(uint64_t))) {
        error = "native string/bytes length is outside a live allocation";
        return false;
    }
    uint64_t length = 0;
    std::memcpy(&length, header, sizeof(length));
    const uint64_t terminator = type == "string" ? 1 : 0;
    if (length > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
        length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        length > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t) - terminator) {
        error = "native string/bytes length is invalid";
        return false;
    }
    const uint64_t allocationSize = sizeof(uint64_t) + length + terminator;
    if (!zeus_memory_find_allocation(
            const_cast<uint8_t*>(header), allocationSize)) {
        error = "native string/bytes contents exceed their live allocation";
        return false;
    }
    const auto* dataBytes = reinterpret_cast<const uint8_t*>(dataAddress);
    if (terminator != 0 && dataBytes[length] != 0) {
        error = "native string is missing its terminator";
        return false;
    }
    out = NativeBufferSpan{dataBytes, header, length, allocationSize};
    return true;
}

inline bool fetch(void* address, const std::string& sourceType,
                  ZeusType::UniversalVariant& out, std::string& error,
                  int64_t accessSize = -1) {
    const std::string type = ZeusRuntimeType::canonicalTypeName(sourceType);
    if (!address && !((type == "string" || type == "bytes" || type == "&bytes") &&
                      accessSize == 0)) {
        error = "cannot read through a null address";
        return false;
    }

    if (type == "int") {
        const int64_t width = accessSize < 0
            ? static_cast<int64_t>(sizeof(int64_t)) : accessSize;
        if (width != 1 && width != 2 && width != 4 && width != 8) {
            error = "VM @faddr int width must be 1, 2, 4, or 8 bytes";
            return false;
        }
        uint64_t raw = 0;
        std::memcpy(&raw, address, static_cast<size_t>(width));
        out = LexerType::Value(static_cast<int64_t>(raw));
        return true;
    }
    if (type == "bool") {
        uint8_t value = 0;
        std::memcpy(&value, address, sizeof(value));
        out = LexerType::Value(value != 0);
        return true;
    }
    if (type == "bigint") {
        out = *static_cast<const ZeusType::BigInteger*>(address);
        return true;
    }
    if (type == "float") {
        out = LexerType::Value(*static_cast<const ZeusType::BigDecimal*>(address));
        return true;
    }
    if (type == "&bytes") {
        if (accessSize < 0) {
            error = "VM @faddr raw bytes range requires an explicit length";
            return false;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(address);
        if (static_cast<uint64_t>(accessSize) >
            static_cast<uint64_t>(std::numeric_limits<uintptr_t>::max() - start)) {
            error = "VM @faddr raw bytes range overflows the address space";
            return false;
        }
        out = ZeusType::RawBytesRange{
            reinterpret_cast<uintptr_t>(address),
            static_cast<uint64_t>(accessSize)
        };
        return true;
    }
    if (type == "string" || type == "bytes") {
        if (accessSize >= 0) {
            if (static_cast<uint64_t>(accessSize) >
                static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                error = "VM @faddr byte length exceeds the platform limit";
                return false;
            }
            try {
                if (accessSize == 0) {
                    if (type == "string") out = LexerType::Value(std::string());
                    else out = ZeusType::Variant(ZeusType::Bytes{});
                } else {
                    if (type == "string") {
                        out = LexerType::Value(std::string(
                            static_cast<const char*>(address),
                            static_cast<size_t>(accessSize)));
                    } else {
                        ZeusType::Bytes bytes;
                        const auto* data = static_cast<const uint8_t*>(address);
                        bytes.data.assign(data, data + static_cast<size_t>(accessSize));
                        out = ZeusType::Variant(std::move(bytes));
                    }
                }
            } catch (...) {
                error = "VM @faddr failed to copy the requested byte range";
                return false;
            }
            return true;
        }
        NativeBufferSpan span;
        if (!nativeBufferSpan(address, type, span, error)) return false;
        const size_t length = static_cast<size_t>(span.length);
        if (type == "string") {
            out = LexerType::Value(std::string(
                reinterpret_cast<const char*>(span.data), length));
        } else {
            ZeusType::Bytes bytes;
            if (length != 0) bytes.data.assign(span.data, span.data + length);
            out = ZeusType::Variant(std::move(bytes));
        }
        return true;
    }

    error = "VM @faddr has no native memory mapping for type '" + sourceType + "'";
    return false;
}

inline bool store(void* address, const std::string& targetType,
                  const ZeusType::UniversalVariant& value, std::string& error,
                  int64_t accessSize = -1) {
    if (!address && !(accessSize == 0 &&
                      (ZeusRuntimeType::canonicalTypeName(targetType) == "bytes" ||
                       ZeusRuntimeType::canonicalTypeName(targetType) == "&bytes"))) {
        error = "cannot write through a null address";
        return false;
    }

    std::string resolvedTargetType = targetType;
    constexpr const char* typeVariablePrefix = "__zeus_typevar:";
    if (resolvedTargetType.rfind(typeVariablePrefix, 0) == 0) {
        resolvedTargetType = runtimeNativeTypeName(value);
        if (resolvedTargetType.empty()) {
            error = "VM cannot infer a native @faddr type from this generic value";
            return false;
        }
    }
    const std::string type = ZeusRuntimeType::canonicalTypeName(resolvedTargetType);
    if (type == "int") {
        int64_t integer = 0;
        if (!ZeusVMUtils::tryGetInt64(value, integer)) {
            error = "@faddr target int requires an int64 value";
            return false;
        }
        const int64_t width = accessSize < 0
            ? static_cast<int64_t>(sizeof(integer)) : accessSize;
        if (width != 1 && width != 2 && width != 4 && width != 8) {
            error = "VM @faddr int width must be 1, 2, 4, or 8 bytes";
            return false;
        }
        const uint64_t raw = static_cast<uint64_t>(integer);
        std::memcpy(address, &raw, static_cast<size_t>(width));
        return true;
    }
    if (type == "bool") {
        bool boolean = false;
        if (!boolPayload(value, boolean)) {
            error = "@faddr target bool requires a bool value";
            return false;
        }
        const uint8_t raw = boolean ? 1 : 0;
        std::memcpy(address, &raw, sizeof(raw));
        return true;
    }
    if (type == "bigint") {
        const ZeusType::BigInteger* integer = nullptr;
        if (const auto* direct = std::get_if<ZeusType::BigInteger>(&value)) {
            integer = direct;
        } else if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
            integer = std::get_if<ZeusType::BigInteger>(&literal->data);
        }
        if (!integer) {
            error = "@faddr target bigint requires an integer value";
            return false;
        }
        *static_cast<ZeusType::BigInteger*>(address) = *integer;
        return true;
    }
    if (type == "float") {
        const ZeusType::BigDecimal* decimal = nullptr;
        if (const auto* direct = std::get_if<ZeusType::BigDecimal>(&value)) {
            decimal = direct;
        } else if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
            decimal = std::get_if<ZeusType::BigDecimal>(&literal->data);
        }
        if (!decimal) {
            error = "@faddr target float requires a float value";
            return false;
        }
        *static_cast<ZeusType::BigDecimal*>(address) = *decimal;
        return true;
    }
    if (type == "string") {
        const std::string* text = nullptr;
        if (const auto* direct = std::get_if<std::string>(&value)) {
            text = direct;
        } else if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
            text = std::get_if<std::string>(&literal->data);
        }
        if (!text) {
            error = "@faddr target string requires a string value";
            return false;
        }
        if (!text->empty()) std::memmove(address, text->data(), text->size());
        return true;
    }
    if (type == "bytes") {
        const ZeusType::Bytes* bytes = nullptr;
        if (const auto* direct = std::get_if<ZeusType::Bytes>(&value)) {
            bytes = direct;
        } else if (const auto* variant = std::get_if<ZeusType::Variant>(&value)) {
            bytes = std::get_if<ZeusType::Bytes>(variant);
        }
        if (!bytes) {
            error = "@faddr target bytes requires a bytes value";
            return false;
        }
        if (accessSize >= 0 && static_cast<uint64_t>(accessSize) != bytes->data.size()) {
            error = "VM @faddr bytes write length must equal the source length";
            return false;
        }
        if (!bytes->data.empty()) {
            std::vector<uint8_t> snapshot(bytes->data);
            std::memmove(address, snapshot.data(), snapshot.size());
        }
        return true;
    }
    if (type == "&bytes") {
        if (accessSize < 0) {
            error = "VM @faddr raw bytes range assignment requires an explicit length";
            return false;
        }
        const uint64_t length = static_cast<uint64_t>(accessSize);
        const uintptr_t destination = reinterpret_cast<uintptr_t>(address);
        if (length > static_cast<uint64_t>(
                std::numeric_limits<uintptr_t>::max() - destination)) {
            error = "VM @faddr raw bytes range destination overflows the address space";
            return false;
        }
        std::vector<uint8_t> snapshot;
        if (const auto* range = std::get_if<ZeusType::RawBytesRange>(&value)) {
            if (range->length != length || (length != 0 && range->address == 0)) {
                error = "VM @faddr raw bytes range write length must equal the source length";
                return false;
            }
            const auto* source = reinterpret_cast<const uint8_t*>(range->address);
            if (length != 0) snapshot.assign(source, source + static_cast<size_t>(length));
        } else {
            const ZeusType::Bytes* bytes = nullptr;
            if (const auto* direct = std::get_if<ZeusType::Bytes>(&value)) {
                bytes = direct;
            } else if (const auto* variant = std::get_if<ZeusType::Variant>(&value)) {
                bytes = std::get_if<ZeusType::Bytes>(variant);
            }
            if (!bytes || bytes->data.size() != length) {
                error = "VM @faddr raw bytes range write length must equal the source length";
                return false;
            }
            snapshot = bytes->data;
        }
        if (!snapshot.empty()) std::memmove(address, snapshot.data(), snapshot.size());
        return true;
    }

    error = "VM @faddr has no native memory mapping for type '" + targetType + "'";
    return false;
}

} // namespace ZeusVMAddress
