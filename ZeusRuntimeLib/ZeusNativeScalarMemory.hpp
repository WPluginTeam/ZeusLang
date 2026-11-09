#pragma once

#include "ZeusFloatingPointABI.hpp"
#include "../ZeusBuiltins/NativeTypeIDs.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace ZeusNativeScalarMemory {

inline const ZeusBuiltins::NativeTypeIDEntry* type(std::string_view name) {
    return ZeusBuiltins::findNativeScalarCanonical(name);
}

inline bool load(ZeusValue* out, const void* storage, std::string_view name) {
    const auto* scalar = type(name);
    if (!scalar) return false;

    if (scalar->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint) {
        if (scalar->bits == 32) {
            float value;
            std::memcpy(&value, storage, sizeof(value));
            zeus_value_init_float(out, value);
        } else if (scalar->bits == 64) {
            double value;
            std::memcpy(&value, storage, sizeof(value));
            zeus_value_init_double(out, value);
        } else {
            throw std::runtime_error("Unsupported native floating-point storage width");
        }
        return true;
    }

    if (scalar->valueABI == ZeusBuiltins::NativeValueABI::UnsignedInteger) {
        uint64_t value = 0;
        switch (scalar->bits) {
            case 8: {
                uint8_t narrow;
                std::memcpy(&narrow, storage, sizeof(narrow));
                value = narrow;
                break;
            }
            case 16: {
                uint16_t narrow;
                std::memcpy(&narrow, storage, sizeof(narrow));
                value = narrow;
                break;
            }
            case 32: {
                uint32_t narrow;
                std::memcpy(&narrow, storage, sizeof(narrow));
                value = narrow;
                break;
            }
            case 64:
                std::memcpy(&value, storage, sizeof(value));
                break;
            default:
                throw std::runtime_error("Unsupported native unsigned integer storage width");
        }
        zeus_value_init_unsigned_integer(out, value, scalar->bits);
        return true;
    }

    int64_t value = 0;
    switch (scalar->bits) {
        case 8: {
            int8_t narrow;
            std::memcpy(&narrow, storage, sizeof(narrow));
            value = narrow;
            break;
        }
        case 16: {
            int16_t narrow;
            std::memcpy(&narrow, storage, sizeof(narrow));
            value = narrow;
            break;
        }
        case 32: {
            int32_t narrow;
            std::memcpy(&narrow, storage, sizeof(narrow));
            value = narrow;
            break;
        }
        case 64:
            std::memcpy(&value, storage, sizeof(value));
            break;
        default:
            throw std::runtime_error("Unsupported native signed integer storage width");
    }
    zeus_value_init_i64(out, value);
    return true;
}

inline bool store(void* storage, const ZeusValue* value, std::string_view name) {
    const auto* scalar = type(name);
    if (!scalar) return false;

    if (scalar->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint) {
        if (scalar->bits == 32) {
            const float native = zeus_value_require_float(value);
            std::memcpy(storage, &native, sizeof(native));
        } else if (scalar->bits == 64) {
            const double native = zeus_value_require_double(value);
            std::memcpy(storage, &native, sizeof(native));
        } else {
            throw std::runtime_error("Unsupported native floating-point storage width");
        }
        return true;
    }

    if (scalar->valueABI == ZeusBuiltins::NativeValueABI::UnsignedInteger) {
        const uint64_t native = zeus_value_require_unsigned_integer(value, scalar->bits);
        switch (scalar->bits) {
            case 8: {
                const auto narrow = static_cast<uint8_t>(native);
                std::memcpy(storage, &narrow, sizeof(narrow));
                break;
            }
            case 16: {
                const auto narrow = static_cast<uint16_t>(native);
                std::memcpy(storage, &narrow, sizeof(narrow));
                break;
            }
            case 32: {
                const auto narrow = static_cast<uint32_t>(native);
                std::memcpy(storage, &narrow, sizeof(narrow));
                break;
            }
            case 64:
                std::memcpy(storage, &native, sizeof(native));
                break;
            default:
                throw std::runtime_error("Unsupported native unsigned integer storage width");
        }
        return true;
    }

    const int64_t native = zeus_value_require_signed_integer(value, scalar->bits);
    switch (scalar->bits) {
        case 8: {
            const auto narrow = static_cast<int8_t>(native);
            std::memcpy(storage, &narrow, sizeof(narrow));
            break;
        }
        case 16: {
            const auto narrow = static_cast<int16_t>(native);
            std::memcpy(storage, &narrow, sizeof(narrow));
            break;
        }
        case 32: {
            const auto narrow = static_cast<int32_t>(native);
            std::memcpy(storage, &narrow, sizeof(narrow));
            break;
        }
        case 64:
            std::memcpy(storage, &native, sizeof(native));
            break;
        default:
            throw std::runtime_error("Unsupported native signed integer storage width");
    }
    return true;
}

inline bool normalize(ZeusValue* out, const ZeusValue* value, std::string_view name) {
    const auto* scalar = type(name);
    if (!scalar) return false;

    if (scalar->valueABI == ZeusBuiltins::NativeValueABI::FloatingPoint) {
        if (scalar->bits == 32) {
            zeus_value_init_float(out, zeus_value_require_float(value));
        } else if (scalar->bits == 64) {
            zeus_value_init_double(out, zeus_value_require_double(value));
        } else {
            throw std::runtime_error("Unsupported native floating-point storage width");
        }
    } else if (scalar->valueABI == ZeusBuiltins::NativeValueABI::UnsignedInteger) {
        zeus_value_init_unsigned_integer(
            out, zeus_value_require_unsigned_integer(value, scalar->bits), scalar->bits);
    } else {
        zeus_value_init_i64(out, zeus_value_require_signed_integer(value, scalar->bits));
    }
    return true;
}

} // namespace ZeusNativeScalarMemory
