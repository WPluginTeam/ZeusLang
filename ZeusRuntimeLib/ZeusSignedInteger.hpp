#pragma once

#include "ZeusVariant.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace ZeusSignedInteger {

inline int64_t minimum(int32_t bits) {
    if (bits < 1 || bits > 64) {
        throw std::runtime_error("Invalid signed integer width: " + std::to_string(bits));
    }
    return bits == 64 ? (std::numeric_limits<int64_t>::min)()
                      : -(int64_t{1} << (bits - 1));
}

inline int64_t maximum(int32_t bits) {
    return bits == 64 ? (std::numeric_limits<int64_t>::max)() : -minimum(bits) - 1;
}

[[noreturn]] inline void outOfRange(const std::string& value, int32_t bits) {
    throw std::overflow_error("Integer " + value + " is out of range for native signed integer i" +
        std::to_string(bits) + " [" + std::to_string(minimum(bits)) + ", " +
        std::to_string(maximum(bits)) + "]");
}

inline int64_t require(int64_t value, int32_t bits) {
    if (value < minimum(bits) || value > maximum(bits)) {
        outOfRange(std::to_string(value), bits);
    }
    return value;
}

inline int64_t require(const ZeusType::UniversalVariant& value, int32_t bits) {
    const ZeusType::BigInteger* integer = std::get_if<ZeusType::BigInteger>(&value);
    ZeusType::BigInteger converted;
    if (!integer) {
        if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
            integer = std::get_if<ZeusType::BigInteger>(&literal->data);
        }
    }
    if (!integer) {
        const ZeusType::BigDecimal* decimal = std::get_if<ZeusType::BigDecimal>(&value);
        if (!decimal) {
            if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
                decimal = std::get_if<ZeusType::BigDecimal>(&literal->data);
            }
        }
        if (decimal) {
            const ZeusType::BigDecimal floored = decimal->floor();
            if (*decimal != floored) {
                throw std::runtime_error(
                    "Native signed integer i" + std::to_string(bits) +
                    " requires an exact integer; fractional value " +
                    decimal->toString() + " is not allowed");
            }
            converted = ZeusType::BigInteger(floored.toString());
            integer = &converted;
        }
    }
    if (!integer) {
        throw std::runtime_error(
            "Expected an integer or exact-integer decimal for native signed integer i" +
            std::to_string(bits));
    }
    if (*integer < ZeusType::BigInteger(minimum(bits)) ||
        *integer > ZeusType::BigInteger(maximum(bits))) {
        outOfRange(integer->toString(), bits);
    }
    return integer->toInt64();
}

inline ZeusType::UniversalVariant normalize(
    const ZeusType::UniversalVariant& value,
    int32_t bits
) {
    return ZeusType::BigInteger(require(value, bits));
}

} // namespace ZeusSignedInteger
