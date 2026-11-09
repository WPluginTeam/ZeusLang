#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace ZeusLLVM {

inline unsigned integerLLVMWidth(std::string_view type) {
    if (type == "i8") return 8;
    if (type == "i16") return 16;
    if (type == "i32") return 32;
    if (type == "i64") return 64;
    return 0;
}

inline unsigned signedIntegerLLVMWidth(std::string_view type) {
    return integerLLVMWidth(type);
}

inline bool isIntegerLLVMType(std::string_view type) {
    return integerLLVMWidth(type) != 0;
}

inline bool isSignedIntegerLLVMType(std::string_view type) {
    return signedIntegerLLVMWidth(type) != 0;
}

// Source ptr denotes a boxed Zeus integer. Narrowing is checked before truncation.
template <typename Emit, typename Fresh>
std::string emitSignedIntegerConversion(
    const std::string& value, const std::string& sourceType,
    const std::string& targetType, Emit&& emit, Fresh&& fresh
) {
    const unsigned targetBits = integerLLVMWidth(targetType);
    const unsigned sourceBits = integerLLVMWidth(sourceType);
    if (!targetBits || (!sourceBits && sourceType != "ptr" &&
        sourceType != "double" && sourceType != "float")) return {};
    if (sourceType == targetType) return value;

    std::string wide = value;
    if (sourceType == "ptr") {
        wide = fresh();
        emit(wide + " = call i64 @zeus_value_require_signed_integer(ptr " +
             value + ", i32 " + std::to_string(targetBits) + ")");
    } else if (sourceType == "double" || sourceType == "float") {
        std::string doubleValue = value;
        if (sourceType == "float") {
            doubleValue = fresh();
            emit(doubleValue + " = fpext float " + value + " to double");
        }
        wide = fresh();
        emit(wide + " = call i64 @zeus_double_require_signed_integer(double " +
             doubleValue + ", i32 " + std::to_string(targetBits) + ")");
    } else if (sourceBits < 64) {
        wide = fresh();
        emit(wide + " = sext " + sourceType + " " + value + " to i64");
    }

    if (targetBits == 64) return wide;
    if (sourceBits > targetBits) {
        const std::string checked = fresh();
        emit(checked + " = call i64 @zeus_checked_signed_integer(i64 " +
             wide + ", i32 " + std::to_string(targetBits) + ")");
        wide = checked;
    }
    const std::string result = fresh();
    emit(result + " = trunc i64 " + wide + " to " + targetType);
    return result;
}

template <typename Emit, typename Fresh>
std::string emitUnsignedIntegerConversion(
    const std::string& value, const std::string& sourceType,
    const std::string& targetType, bool sourceIsUnsigned,
    Emit&& emit, Fresh&& fresh
) {
    const unsigned targetBits = integerLLVMWidth(targetType);
    const unsigned sourceBits = integerLLVMWidth(sourceType);
    if (!targetBits || (!sourceBits && sourceType != "ptr" &&
        sourceType != "double" && sourceType != "float")) return {};
    if (sourceType == targetType && sourceIsUnsigned) return value;

    std::string wide = value;
    if (sourceType == "ptr") {
        wide = fresh();
        emit(wide + " = call i64 @zeus_value_require_unsigned_integer(ptr " +
             value + ", i32 " + std::to_string(targetBits) + ")");
    } else if (sourceType == "double" || sourceType == "float") {
        std::string doubleValue = value;
        if (sourceType == "float") {
            doubleValue = fresh();
            emit(doubleValue + " = fpext float " + value + " to double");
        }
        wide = fresh();
        emit(wide + " = call i64 @zeus_double_require_unsigned_integer(double " +
             doubleValue + ", i32 " + std::to_string(targetBits) + ")");
    } else if (sourceIsUnsigned || sourceType == "i1") {
        if (sourceBits < 64) {
            wide = fresh();
            emit(wide + " = zext " + sourceType + " " + value + " to i64");
        }
        if (sourceBits > targetBits) {
            const std::string checked = fresh();
            emit(checked + " = call i64 @zeus_checked_unsigned_integer(i64 " +
                 wide + ", i32 " + std::to_string(targetBits) + ")");
            wide = checked;
        }
    } else {
        std::string signedWide = value;
        if (sourceBits < 64) {
            signedWide = fresh();
            emit(signedWide + " = sext " + sourceType + " " + value + " to i64");
        }
        wide = fresh();
        emit(wide + " = call i64 @zeus_signed_require_unsigned_integer(i64 " +
             signedWide + ", i32 " +
             std::to_string(targetBits) + ")");
    }

    if (targetBits == 64) return wide;
    const std::string result = fresh();
    emit(result + " = trunc i64 " + wide + " to " + targetType);
    return result;
}

template <typename Emit, typename Fresh>
std::string emitNativeIntegerConversion(
    const std::string& value, const std::string& sourceType,
    const std::string& targetType, bool sourceIsUnsigned,
    bool targetIsUnsigned, Emit&& emit, Fresh&& fresh
) {
    if (targetIsUnsigned) {
        return emitUnsignedIntegerConversion(value, sourceType, targetType,
            sourceIsUnsigned, std::forward<Emit>(emit), std::forward<Fresh>(fresh));
    }

    const unsigned targetBits = integerLLVMWidth(targetType);
    const unsigned sourceBits = integerLLVMWidth(sourceType);
    if (!targetBits || (!sourceBits && sourceType != "ptr" &&
        sourceType != "double" && sourceType != "float")) return {};
    if (!sourceIsUnsigned || sourceType == "ptr" || sourceType == "double" ||
        sourceType == "float") {
        return emitSignedIntegerConversion(value, sourceType, targetType,
            std::forward<Emit>(emit), std::forward<Fresh>(fresh));
    }

    std::string unsignedWide = value;
    if (sourceBits < 64) {
        unsignedWide = fresh();
        emit(unsignedWide + " = zext " + sourceType + " " + value + " to i64");
    }
    const std::string checked = fresh();
    emit(checked + " = call i64 @zeus_unsigned_require_signed_integer(i64 " +
         unsignedWide + ", i32 " +
         std::to_string(targetBits) + ")");
    if (targetBits == 64) return checked;
    const std::string result = fresh();
    emit(result + " = trunc i64 " + checked + " to " + targetType);
    return result;
}

} // namespace ZeusLLVM
