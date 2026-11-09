#pragma once

#include "ZeusIntegerABI.hpp"

namespace ZeusLLVM {

template <typename Emit, typename Fresh>
std::string emitNativeFloatingConversion(const std::string& value,
    const std::string& sourceType, const std::string& targetType,
    bool sourceIsUnsigned, Emit&& emit, Fresh&& fresh) {
    if (targetType != "float" && targetType != "double") return {};
    if (sourceType == targetType) return value;

    if (sourceType == "ptr") {
        const std::string converted = fresh();
        if (targetType == "float") {
            emit(converted + " = call float @zeus_value_require_float(ptr " + value + ")");
        } else {
            emit(converted + " = call double @zeus_value_require_double(ptr " + value + ")");
        }
        return converted;
    }

    if (sourceType == "double" && targetType == "float") {
        const std::string converted = fresh();
        emit(converted + " = call float @zeus_double_require_float(double " + value + ")");
        return converted;
    }
    if (sourceType == "float" && targetType == "double") {
        const std::string converted = fresh();
        emit(converted + " = fpext float " + value + " to double");
        return converted;
    }

    if (!isIntegerLLVMType(sourceType) && sourceType != "i1") return {};
    const std::string converted = fresh();
    const std::string opcode = (sourceIsUnsigned || sourceType == "i1")
        ? "uitofp" : "sitofp";
    emit(converted + " = " + opcode + " " + sourceType + " " + value +
         " to " + targetType);
    return converted;
}

template <typename Emit, typename Fresh>
std::string emitDoubleConversion(const std::string& value,
    const std::string& sourceType, Emit&& emit, Fresh&& fresh) {
    return emitNativeFloatingConversion(value, sourceType, "double", false,
        std::forward<Emit>(emit), std::forward<Fresh>(fresh));
}

template <typename Emit, typename Fresh>
std::string emitFloatConversion(const std::string& value,
    const std::string& sourceType, bool sourceIsUnsigned,
    Emit&& emit, Fresh&& fresh) {
    return emitNativeFloatingConversion(value, sourceType, "float", sourceIsUnsigned,
        std::forward<Emit>(emit), std::forward<Fresh>(fresh));
}

inline std::string doubleBinaryOpcode(VMOperator operation) {
    switch (operation) {
        case VMOperator::ADD: return "fadd";
        case VMOperator::SUBTRACT: return "fsub";
        case VMOperator::MULTIPLY: return "fmul";
        case VMOperator::DIVIDE: return "fdiv";
        case VMOperator::MODULUS: return "frem";
        case VMOperator::EQUL: return "fcmp oeq";
        case VMOperator::NOT_EQUAL: return "fcmp une";
        case VMOperator::GREATER_THAN: return "fcmp ogt";
        case VMOperator::LESS_THAN: return "fcmp olt";
        case VMOperator::GREATER_EQUAL: return "fcmp oge";
        case VMOperator::LESS_EQUAL: return "fcmp ole";
        default: return {};
    }
}

template <typename Value, typename Emit, typename Fresh>
bool emitNativeFloatingBinary(VMOperator operation,
    const Value& left, const Value& right, Value& result,
    bool leftUnsigned, bool rightUnsigned, Emit&& emit, Fresh&& fresh) {
    if (left.sourceType == "char" || right.sourceType == "char") return false;
    const bool hasFloat = left.type == "float" || right.type == "float";
    const bool hasDouble = left.type == "double" || right.type == "double";
    if (!hasFloat && !hasDouble) return false;
    const std::string targetType = hasDouble ? "double" : "float";
    const std::string opcode = doubleBinaryOpcode(operation);
    if (opcode.empty()) return false;
    const std::string leftType = left.dynamic ? "ptr" : left.type;
    const std::string rightType = right.dynamic ? "ptr" : right.type;
    const auto lhs = emitNativeFloatingConversion(
        left.value, leftType, targetType, leftUnsigned, emit, fresh);
    const auto rhs = emitNativeFloatingConversion(
        right.value, rightType, targetType, rightUnsigned, emit, fresh);
    if (lhs.empty() || rhs.empty()) return false;
    const auto value = fresh();
    emit(value + " = " + opcode + " " + targetType + " " + lhs + ", " + rhs);
    const bool comparison = opcode.rfind("fcmp", 0) == 0;
    const std::string floatingSource = left.type == targetType
        ? left.sourceType
        : right.type == targetType ? right.sourceType
        : left.type == "float" || left.type == "double"
            ? left.sourceType : right.sourceType;
    result = Value{value, comparison ? "i1" : targetType, false, false,
        comparison ? "bool" : floatingSource};
    return true;
}

} // namespace ZeusLLVM
