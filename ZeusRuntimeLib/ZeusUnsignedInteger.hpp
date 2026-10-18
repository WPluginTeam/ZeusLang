#pragma once

#include "ZeusVariant.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace ZeusUnsignedInteger {

inline void validateBits(int32_t bits) {
    if (bits < 1 || bits > 64) {
        throw std::runtime_error("Invalid unsigned integer width: " + std::to_string(bits));
    }
}

inline uint64_t minimum(int32_t bits) {
    validateBits(bits);
    return 0;
}

inline uint64_t maximum(int32_t bits) {
    validateBits(bits);
    return bits == 64
        ? (std::numeric_limits<uint64_t>::max)()
        : (uint64_t{1} << bits) - 1;
}

[[noreturn]] inline void outOfRange(const std::string& value, int32_t bits) {
    throw std::overflow_error(
        "Integer " + value + " is out of range for native unsigned integer u" +
        std::to_string(bits) + " [0, " + std::to_string(maximum(bits)) + "]");
}

inline uint64_t toUInt64(const ZeusType::BigInteger& integer) {
    const std::string decimal = integer.toString();
    uint64_t result = 0;
    const uint64_t maxValue = (std::numeric_limits<uint64_t>::max)();
    for (const char digit : decimal) {
        if (digit < '0' || digit > '9') {
            throw std::runtime_error("Cannot convert a negative integer to uint64_t");
        }
        const uint64_t value = static_cast<uint64_t>(digit - '0');
        if (result > (maxValue - value) / 10) {
            throw std::overflow_error("BigInteger too large for uint64_t");
        }
        result = result * 10 + value;
    }
    return result;
}

inline uint64_t require(const ZeusType::UniversalVariant& value, int32_t bits) {
    const uint64_t maxValue = maximum(bits);
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
                    "Native unsigned integer u" + std::to_string(bits) +
                    " requires an exact integer; fractional value " +
                    decimal->toString() + " is not allowed");
            }
            converted = ZeusType::BigInteger(floored.toString());
            integer = &converted;
        }
    }

    if (!integer) {
        throw std::runtime_error(
            "Expected an integer or exact-integer decimal for native unsigned integer u" +
            std::to_string(bits));
    }

    if (*integer < ZeusType::BigInteger(0) ||
        *integer > ZeusType::BigInteger(std::to_string(
            static_cast<unsigned long long>(maxValue)))) {
        outOfRange(integer->toString(), bits);
    }
    return toUInt64(*integer);
}

inline ZeusType::UniversalVariant normalize(
    const ZeusType::UniversalVariant& value,
    int32_t bits
) {
    return ZeusType::BigInteger(std::to_string(
        static_cast<unsigned long long>(require(value, bits))));
}

} // namespace ZeusUnsignedInteger
