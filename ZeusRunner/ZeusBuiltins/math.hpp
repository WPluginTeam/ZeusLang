#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusNativeDouble.hpp"
#include "../../ZeusRuntimeLib/ZeusNativeFloat.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace ZeusBuiltins {

#define ZEUS_MATH_UNARY_DOUBLE(NAME, FUNC) \
inline ZeusType::UniversalVariant builtin_##NAME(const std::vector<ZeusType::UniversalVariant>& args) { \
    if (args.empty()) throw std::runtime_error(#NAME " expects 1 argument"); \
    double val = ZeusNativeDouble::require(args[0]); \
    double res = FUNC(val); \
    return ZeusNativeDouble::boxed(res); \
}

#define ZEUS_MATH_BINARY_DOUBLE(NAME, FUNC) \
inline ZeusType::UniversalVariant builtin_##NAME(const std::vector<ZeusType::UniversalVariant>& args) { \
    if (args.size() < 2) throw std::runtime_error(#NAME " expects 2 arguments"); \
    double a = ZeusNativeDouble::require(args[0]); \
    double b = ZeusNativeDouble::require(args[1]); \
    double res = FUNC(a, b); \
    return ZeusNativeDouble::boxed(res); \
}

#define ZEUS_MATH_UNARY_FLOAT(NAME, FUNC) \
inline ZeusType::UniversalVariant builtin_##NAME(const std::vector<ZeusType::UniversalVariant>& args) { \
    if (args.empty()) throw std::runtime_error(#NAME " expects 1 argument"); \
    float val = ZeusNativeFloat::require(args[0]); \
    float res = FUNC(val); \
    return ZeusNativeFloat::boxed(res); \
}

#define ZEUS_MATH_BINARY_FLOAT(NAME, FUNC) \
inline ZeusType::UniversalVariant builtin_##NAME(const std::vector<ZeusType::UniversalVariant>& args) { \
    if (args.size() < 2) throw std::runtime_error(#NAME " expects 2 arguments"); \
    float a = ZeusNativeFloat::require(args[0]); \
    float b = ZeusNativeFloat::require(args[1]); \
    float res = FUNC(a, b); \
    return ZeusNativeFloat::boxed(res); \
}

// 三角函数与反三角函数
ZEUS_MATH_UNARY_DOUBLE(sin, std::sin)
ZEUS_MATH_UNARY_DOUBLE(cos, std::cos)
ZEUS_MATH_UNARY_DOUBLE(tan, std::tan)
ZEUS_MATH_UNARY_DOUBLE(asin, std::asin)
ZEUS_MATH_UNARY_DOUBLE(acos, std::acos)
ZEUS_MATH_UNARY_DOUBLE(atan, std::atan)
ZEUS_MATH_BINARY_DOUBLE(atan2, std::atan2)

// 双曲函数与反双曲函数
ZEUS_MATH_UNARY_DOUBLE(sinh, std::sinh)
ZEUS_MATH_UNARY_DOUBLE(cosh, std::cosh)
ZEUS_MATH_UNARY_DOUBLE(tanh, std::tanh)
ZEUS_MATH_UNARY_DOUBLE(asinh, std::asinh)
ZEUS_MATH_UNARY_DOUBLE(acosh, std::acosh)
ZEUS_MATH_UNARY_DOUBLE(atanh, std::atanh)

// 幂、根号与指数对数
ZEUS_MATH_UNARY_DOUBLE(sqrt, std::sqrt)
ZEUS_MATH_UNARY_DOUBLE(cbrt, std::cbrt)
ZEUS_MATH_BINARY_DOUBLE(hypot, std::hypot)
ZEUS_MATH_BINARY_DOUBLE(pow, std::pow)
ZEUS_MATH_UNARY_DOUBLE(exp, std::exp)
ZEUS_MATH_UNARY_DOUBLE(exp2, std::exp2)
ZEUS_MATH_UNARY_DOUBLE(log, std::log)
ZEUS_MATH_UNARY_DOUBLE(log2, std::log2)
ZEUS_MATH_UNARY_DOUBLE(log10, std::log10)

// 取整与截断
ZEUS_MATH_UNARY_DOUBLE(floor, std::floor)
ZEUS_MATH_UNARY_DOUBLE(ceil, std::ceil)
ZEUS_MATH_UNARY_DOUBLE(round, std::round)
ZEUS_MATH_UNARY_DOUBLE(trunc, std::trunc)

// 浮点绝对值与杂项
ZEUS_MATH_UNARY_DOUBLE(fabs, std::fabs)
ZEUS_MATH_BINARY_DOUBLE(copysign, std::copysign)
ZEUS_MATH_BINARY_DOUBLE(fmod, std::fmod)

// 单精度 float 硬件加速版本
ZEUS_MATH_UNARY_FLOAT(sinf, std::sin)
ZEUS_MATH_UNARY_FLOAT(cosf, std::cos)
ZEUS_MATH_UNARY_FLOAT(tanf, std::tan)
ZEUS_MATH_UNARY_FLOAT(asinf, std::asin)
ZEUS_MATH_UNARY_FLOAT(acosf, std::acos)
ZEUS_MATH_UNARY_FLOAT(atanf, std::atan)
ZEUS_MATH_BINARY_FLOAT(atan2f, std::atan2)
ZEUS_MATH_UNARY_FLOAT(sinhf, std::sinh)
ZEUS_MATH_UNARY_FLOAT(coshf, std::cosh)
ZEUS_MATH_UNARY_FLOAT(tanhf, std::tanh)
ZEUS_MATH_UNARY_FLOAT(asinhf, std::asinh)
ZEUS_MATH_UNARY_FLOAT(acoshf, std::acosh)
ZEUS_MATH_UNARY_FLOAT(atanhf, std::atanh)
ZEUS_MATH_UNARY_FLOAT(sqrtf, std::sqrt)
ZEUS_MATH_UNARY_FLOAT(cbrtf, std::cbrt)
ZEUS_MATH_BINARY_FLOAT(hypotf, std::hypot)
ZEUS_MATH_BINARY_FLOAT(powf, std::pow)
ZEUS_MATH_UNARY_FLOAT(expf, std::exp)
ZEUS_MATH_UNARY_FLOAT(exp2f, std::exp2)
ZEUS_MATH_UNARY_FLOAT(logf, std::log)
ZEUS_MATH_UNARY_FLOAT(log2f, std::log2)
ZEUS_MATH_UNARY_FLOAT(log10f, std::log10)
ZEUS_MATH_UNARY_FLOAT(floorf, std::floor)
ZEUS_MATH_UNARY_FLOAT(ceilf, std::ceil)
ZEUS_MATH_UNARY_FLOAT(roundf, std::round)
ZEUS_MATH_UNARY_FLOAT(truncf, std::trunc)
ZEUS_MATH_UNARY_FLOAT(fabsf, std::fabs)
ZEUS_MATH_BINARY_FLOAT(copysignf, std::copysign)
ZEUS_MATH_BINARY_FLOAT(fmodf, std::fmod)

#undef ZEUS_MATH_UNARY_DOUBLE
#undef ZEUS_MATH_BINARY_DOUBLE
#undef ZEUS_MATH_UNARY_FLOAT
#undef ZEUS_MATH_BINARY_FLOAT

} // namespace ZeusBuiltins
