// Included by ZeusABI.cpp so every runtime build exports the scalar ABI.
#include "ZeusFloatingPointABI.hpp"
#include "ZeusNativeDouble.hpp"
#include "ZeusNativeFloat.hpp"
#include "ZeusSignedInteger.hpp"
#include "ZeusUnsignedInteger.hpp"

#include <cmath>
#include <limits>

extern "C" double zeus_value_require_double(const ZeusValue* value) {
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    if (!boxed) throw std::runtime_error("Expected a numeric value for native double");
    return ZeusNativeDouble::require(boxed->v);
}

extern "C" int64_t zeus_double_require_signed_integer(double value, int32_t bits) {
    static_cast<void>(ZeusSignedInteger::minimum(bits));
    if (!std::isfinite(value)) {
        throw std::runtime_error("Native signed integer conversion requires a finite value");
    }
    if (std::trunc(value) != value) {
        throw std::runtime_error("Native signed integer conversion requires an exact integer");
    }

    const double limit = std::ldexp(1.0, bits - 1);
    if (value < -limit || value >= limit) {
        ZeusSignedInteger::outOfRange(std::to_string(value), bits);
    }
    return ZeusSignedInteger::require(static_cast<int64_t>(value), bits);
}

extern "C" uint64_t zeus_checked_unsigned_integer(uint64_t value, int32_t bits) {
    if (value > ZeusUnsignedInteger::maximum(bits)) {
        ZeusUnsignedInteger::outOfRange(
            std::to_string(static_cast<unsigned long long>(value)), bits);
    }
    return value;
}

extern "C" uint64_t zeus_signed_require_unsigned_integer(int64_t value, int32_t bits) {
    if (value < 0) {
        ZeusUnsignedInteger::outOfRange(std::to_string(value), bits);
    }
    return zeus_checked_unsigned_integer(static_cast<uint64_t>(value), bits);
}

extern "C" int64_t zeus_unsigned_require_signed_integer(uint64_t value, int32_t bits) {
    const int64_t maximum = ZeusSignedInteger::maximum(bits);
    if (value > static_cast<uint64_t>(maximum)) {
        ZeusSignedInteger::outOfRange(
            std::to_string(static_cast<unsigned long long>(value)), bits);
    }
    return static_cast<int64_t>(value);
}

extern "C" uint64_t zeus_value_require_unsigned_integer(
    const ZeusValue* value, int32_t bits
) {
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    if (!boxed) throw std::runtime_error("Expected a numeric value for native unsigned integer");
    return ZeusUnsignedInteger::require(boxed->v, bits);
}

extern "C" uint64_t zeus_double_require_unsigned_integer(double value, int32_t bits) {
    ZeusUnsignedInteger::maximum(bits);
    if (!std::isfinite(value)) {
        throw std::runtime_error("Native unsigned integer conversion requires a finite value");
    }
    if (std::trunc(value) != value) {
        throw std::runtime_error("Native unsigned integer conversion requires an exact integer");
    }
    const double limit = std::ldexp(1.0, bits);
    if (value < 0.0 || value >= limit) {
        ZeusUnsignedInteger::outOfRange(std::to_string(value), bits);
    }
    return zeus_checked_unsigned_integer(static_cast<uint64_t>(value), bits);
}

extern "C" float zeus_double_require_float(double value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("Native FLOAT requires a finite value");
    }
    constexpr double maximum = static_cast<double>((std::numeric_limits<float>::max)());
    if (value < -maximum || value > maximum) {
        throw std::runtime_error("Native FLOAT value is out of range");
    }
    const float converted = static_cast<float>(value);
    if (!std::isfinite(converted)) {
        throw std::runtime_error("Native FLOAT requires a finite value");
    }
    return converted;
}

extern "C" float zeus_value_require_float(const ZeusValue* value) {
    std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
        zeus_value_to_box(value), &zeus_box_release);
    if (!boxed) throw std::runtime_error("Expected a numeric value for native FLOAT");
    return ZeusNativeFloat::require(boxed->v);
}

extern "C" void zeus_value_init_double(ZeusValue* out, double value) {
    ZeusBox boxed;
    boxed.assignVariant(ZeusNativeDouble::boxed(value));
    zeus_value_from_box(out, &boxed);
}

extern "C" void zeus_value_init_unsigned_integer(
    ZeusValue* out, uint64_t value, int32_t bits
) {
    ZeusBox boxed;
    boxed.assignVariant(ZeusUnsignedInteger::normalize(
        ZeusType::BigInteger(std::to_string(static_cast<unsigned long long>(value))), bits));
    zeus_value_from_box(out, &boxed);
}

extern "C" void zeus_value_init_float(ZeusValue* out, float value) {
    ZeusBox boxed;
    boxed.assignVariant(ZeusNativeFloat::boxed(value));
    zeus_value_from_box(out, &boxed);
}

extern "C" double zeus_global_load_double(int index) {
    ZeusValue value{};
    zeus_value_global_load(&value, index);
    try {
        const auto result = zeus_value_require_double(&value);
        zeus_value_release(&value);
        return result;
    } catch (...) {
        zeus_value_release(&value);
        throw;
    }
}

extern "C" void zeus_global_store_double(int index, double value) {
    ZeusValue dynamicValue{};
    zeus_value_init_double(&dynamicValue, value);
    zeus_value_global_store(index, &dynamicValue);
}

extern "C" uint64_t zeus_global_load_unsigned_integer(int index, int32_t bits) {
    ZeusValue value{};
    zeus_value_global_load(&value, index);
    try {
        const auto result = zeus_value_require_unsigned_integer(&value, bits);
        zeus_value_release(&value);
        return result;
    } catch (...) {
        zeus_value_release(&value);
        throw;
    }
}

extern "C" void zeus_global_store_unsigned_integer(
    int index, uint64_t value, int32_t bits
) {
    ZeusValue dynamicValue{};
    zeus_value_init_unsigned_integer(&dynamicValue, value, bits);
    zeus_value_global_store(index, &dynamicValue);
}

extern "C" float zeus_global_load_float(int index) {
    ZeusValue value{};
    zeus_value_global_load(&value, index);
    try {
        const auto result = zeus_value_require_float(&value);
        zeus_value_release(&value);
        return result;
    } catch (...) {
        zeus_value_release(&value);
        throw;
    }
}

extern "C" void zeus_global_store_float(int index, float value) {
    ZeusValue dynamicValue{};
    zeus_value_init_float(&dynamicValue, value);
    zeus_value_global_store(index, &dynamicValue);
}
