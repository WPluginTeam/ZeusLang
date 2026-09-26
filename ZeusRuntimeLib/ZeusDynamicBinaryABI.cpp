// ZeusRuntimeLib/ZeusDynamicBinaryABI.cpp

#include "ZeusDynamicBinaryABI.hpp"
#include "ZeusABIInternal.hpp"

#include "ZeusVMUtils.hpp"

#include <memory>

struct ZeusRuntimeContext {
    void* userData = nullptr;
    ZeusObjectBinaryDispatchFn dispatch = nullptr;
};

namespace {

ZeusType::UniversalVariant nullVariant() {
    return ZeusType::UniversalVariant(ZeusType::Variant(ZeusType::Null()));
}

ZeusBox* makeBox(const ZeusType::UniversalVariant& value) {
    ZeusBox* box = new ZeusBox();
    box->v = value;
    return box;
}

ZeusBox* makeNullBox() {
    return makeBox(nullVariant());
}

bool isObjectHandle(const ZeusBox* box) {
    return box && std::holds_alternative<ZeusType::ObjectHandle>(box->v);
}

ZeusBox* makeValueBox(const LexerType::Value& value) {
    return makeBox(ZeusType::UniversalVariant(value));
}

bool isNumeric(const std::string& type) {
    return type == "int" || type == "float";
}

bool equalValues(
    const ZeusType::UniversalVariant& left,
    const ZeusType::UniversalVariant& right
) {
    const bool leftNull = ZeusVMUtils::isNull(left);
    const bool rightNull = ZeusVMUtils::isNull(right);
    if (leftNull || rightNull) return leftNull && rightNull;

    if (std::holds_alternative<ZeusType::ClosureHandle>(left) &&
        std::holds_alternative<ZeusType::ClosureHandle>(right)) {
        return std::get<ZeusType::ClosureHandle>(left).id ==
               std::get<ZeusType::ClosureHandle>(right).id;
    }
    if (std::holds_alternative<ZeusType::NativeFuncHandle>(left) &&
        std::holds_alternative<ZeusType::NativeFuncHandle>(right)) {
        return std::get<ZeusType::NativeFuncHandle>(left).id ==
               std::get<ZeusType::NativeFuncHandle>(right).id;
    }

    LexerType::Value leftValue(0);
    LexerType::Value rightValue(0);
    if (!ZeusVMUtils::convertToValue(left, leftValue) ||
        !ZeusVMUtils::convertToValue(right, rightValue)) {
        return false;
    }

    const std::string leftType = leftValue.getStringType();
    const std::string rightType = rightValue.getStringType();
    if (isNumeric(leftType) && isNumeric(rightType)) {
        const ZeusType::BigDecimal leftNumber = leftType == "int"
            ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(leftValue.data))
            : std::get<ZeusType::BigDecimal>(leftValue.data);
        const ZeusType::BigDecimal rightNumber = rightType == "int"
            ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(rightValue.data))
            : std::get<ZeusType::BigDecimal>(rightValue.data);
        return leftNumber == rightNumber;
    }
    if (leftType != rightType) return false;

    if (leftType == "string") {
        return std::get<std::string>(leftValue.data) ==
               std::get<std::string>(rightValue.data);
    }
    if (leftType == "bool") {
        return std::get<bool>(leftValue.data) == std::get<bool>(rightValue.data);
    }
    if (leftType == "int") {
        return std::get<ZeusType::BigInteger>(leftValue.data) ==
               std::get<ZeusType::BigInteger>(rightValue.data);
    }
    if (leftType == "float") {
        return std::get<ZeusType::BigDecimal>(leftValue.data) ==
               std::get<ZeusType::BigDecimal>(rightValue.data);
    }
    return false;
}

ZeusBox* applyBuiltin(
    int32_t opcode,
    const ZeusBox* lhs,
    const ZeusBox* rhs
) {
    if (!lhs || !rhs) return makeNullBox();

    if (opcode == ZEUS_BINARY_EQUAL || opcode == ZEUS_BINARY_NOT_EQUAL) {
        const bool equal = equalValues(lhs->v, rhs->v);
        return makeValueBox(LexerType::Value(
            opcode == ZEUS_BINARY_EQUAL ? equal : !equal
        ));
    }

    if (opcode == ZEUS_BINARY_ADD) return zeus_int_add(lhs, rhs);
    // The runtime only implements strict less-than, so the inclusive forms swap
    // operands and negate the result.
    if (opcode == ZEUS_BINARY_LESS_THAN) return zeus_less_than(lhs, rhs);
    if (opcode == ZEUS_BINARY_GREATER_THAN) return zeus_less_than(rhs, lhs);
    if (opcode == ZEUS_BINARY_LESS_EQUAL || opcode == ZEUS_BINARY_GREATER_EQUAL) {
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> strict(
            zeus_less_than(opcode == ZEUS_BINARY_LESS_EQUAL ? rhs : lhs,
                           opcode == ZEUS_BINARY_LESS_EQUAL ? lhs : rhs),
            &zeus_box_release);
        return makeValueBox(LexerType::Value(zeus_truthy(strict.get()) == 0 ? 1 : 0));
    }

    LexerType::Value leftValue(0);
    LexerType::Value rightValue(0);
    if (!ZeusVMUtils::convertToValue(lhs->v, leftValue) ||
        !ZeusVMUtils::convertToValue(rhs->v, rightValue) ||
        !ZeusVMUtils::isNumericValue(leftValue) ||
        !ZeusVMUtils::isNumericValue(rightValue)) {
        return makeNullBox();
    }

    try {
        if (opcode == ZEUS_BINARY_SUBTRACT) {
            return makeValueBox(leftValue - rightValue);
        }
        if (opcode == ZEUS_BINARY_MULTIPLY) {
            return makeValueBox(leftValue * rightValue);
        }
        if (opcode == ZEUS_BINARY_DIVIDE) {
            return makeValueBox(leftValue / rightValue);
        }
        if (opcode == ZEUS_BINARY_MODULUS) {
            if (leftValue.getStringType() != "int" ||
                rightValue.getStringType() != "int") {
                return makeNullBox();
            }
            const auto& leftInt = std::get<ZeusType::BigInteger>(leftValue.data);
            const auto& rightInt = std::get<ZeusType::BigInteger>(rightValue.data);
            if (rightInt == ZeusType::BigInteger(0)) return makeNullBox();
            return makeValueBox(LexerType::Value(leftInt % rightInt));
        }
    } catch (...) {
        return makeNullBox();
    }

    return makeNullBox();
}

} // namespace

ZeusRuntimeContext* zeus_runtime_context_create(
    void* userData,
    ZeusObjectBinaryDispatchFn dispatch
) {
    ZeusRuntimeContext* context = new ZeusRuntimeContext();
    context->userData = userData;
    context->dispatch = dispatch;
    return context;
}

void zeus_runtime_context_release(ZeusRuntimeContext* context) {
    delete context;
}

ZeusBox* zeus_dynamic_binary(
    ZeusRuntimeContext* context,
    int32_t opcode,
    const ZeusBox* lhs,
    const ZeusBox* rhs
) {
    if (isObjectHandle(lhs)) {
        if (!context || !context->dispatch) {
            if (opcode == ZEUS_BINARY_EQUAL || opcode == ZEUS_BINARY_NOT_EQUAL) {
                const bool equal = equalValues(lhs->v, rhs ? rhs->v : nullVariant());
                return makeValueBox(LexerType::Value(
                    opcode == ZEUS_BINARY_EQUAL ? equal : !equal
                ));
            }
            return makeNullBox();
        }
        return context->dispatch(context->userData, opcode, lhs, rhs);
    }

    return applyBuiltin(opcode, lhs, rhs);
}
