#pragma once

#include "ZeusVariant.hpp"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

namespace ZeusNativeDouble {

inline double require(const ZeusType::UniversalVariant& value) {
    if (const auto* number = std::get_if<ZeusType::BigDecimal>(&value)) {
        return number->toDouble();
    }
    if (const auto* integer = std::get_if<ZeusType::BigInteger>(&value)) {
        return std::stod(integer->toString());
    }
    if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
        if (const auto* number = std::get_if<ZeusType::BigDecimal>(&literal->data)) {
            return number->toDouble();
        }
        if (const auto* integer = std::get_if<ZeusType::BigInteger>(&literal->data)) {
            return std::stod(integer->toString());
        }
    }
    throw std::runtime_error("Native DOUBLE requires a numeric value");
}

// BigDecimal's double constructor uses stream default precision. Build an
// explicit round-trip decimal instead, including exponents outside fixed
// notation's useful range. BigDecimal itself accepts ordinary decimal text.
inline ZeusType::UniversalVariant boxed(double value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("Non-finite native DOUBLE cannot be stored in a Zeus numeric value");
    }
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::scientific
           << std::setprecision(std::numeric_limits<double>::max_digits10 - 1)
           << value;
    const std::string scientific = stream.str();
    const auto exponentAt = scientific.find('e');
    const int exponent = std::stoi(scientific.substr(exponentAt + 1));
    const bool negative = scientific.front() == '-';
    std::string digits = scientific.substr(negative ? 1 : 0,
        exponentAt - (negative ? 1 : 0));
    digits.erase(digits.find('.'), 1);
    const int point = exponent + 1;
    std::string decimal;
    if (point <= 0) {
        decimal = "0." + std::string(-point, '0') + digits;
    } else if (point >= static_cast<int>(digits.size())) {
        decimal = digits + std::string(point - digits.size(), '0');
    } else {
        decimal = digits.substr(0, point) + "." + digits.substr(point);
    }
    if (negative) decimal.insert(decimal.begin(), '-');
    return ZeusType::BigDecimal(decimal);
}

inline ZeusType::UniversalVariant normalize(const ZeusType::UniversalVariant& value) {
    return boxed(require(value));
}

} // namespace ZeusNativeDouble
