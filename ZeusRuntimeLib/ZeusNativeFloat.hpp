#pragma once

#include "ZeusNativeDouble.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace ZeusNativeFloat {

inline float require(const ZeusType::UniversalVariant& value) {
    const double numeric = ZeusNativeDouble::require(value);
    if (!std::isfinite(numeric)) {
        throw std::runtime_error("Native FLOAT requires a finite value");
    }

    constexpr double maximum =
        static_cast<double>(std::numeric_limits<float>::max());
    if (numeric < -maximum || numeric > maximum) {
        throw std::runtime_error("Native FLOAT value is out of range");
    }

    const float result = static_cast<float>(numeric);
    if (!std::isfinite(result)) {
        throw std::runtime_error("Native FLOAT requires a finite value");
    }
    return result;
}

inline ZeusType::UniversalVariant boxed(float value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error(
            "Non-finite native FLOAT cannot be stored in a Zeus numeric value");
    }
    return ZeusNativeDouble::boxed(static_cast<double>(value));
}

inline ZeusType::UniversalVariant normalize(
    const ZeusType::UniversalVariant& value
) {
    return boxed(require(value));
}

} // namespace ZeusNativeFloat
