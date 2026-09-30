#pragma once

#include "../ZeusBuiltins/NativeTypeABI.hpp"

#include <functional>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace ZeusLLVM::DynamicMemberBridge {

struct MethodBinding {
    std::string typeName;
    std::string methodName;
    std::string functionName;
    std::string returnType;
    std::vector<std::string> parameterTypes; // Includes the implicit receiver.
};

struct TypeRules {
    std::function<std::string(const std::string&)> llvmType;
    std::function<std::string(const std::string&)> parameterABIType;
    std::function<bool(const std::string&)> usesZeusValueABI;
    std::function<bool(const std::string&)> isClassType;
    std::function<std::string(const std::string&)> callTarget;
};

struct GeneratedIR {
    std::string constants;
    std::string adapters;
    std::string initializer;
    std::string error;

    bool empty() const { return adapters.empty(); }
};

inline std::string escapeBytes(const std::string& text) {
    std::string escaped;
    for (unsigned char c : text) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            escaped += static_cast<char>(c);
        } else {
            char encoded[5];
            std::snprintf(encoded, sizeof(encoded), "\\%02X", static_cast<unsigned>(c));
            escaped += encoded;
        }
    }
    return escaped;
}

inline bool isReferenceType(const std::string& typeName) {
    return !typeName.empty() && typeName.front() == '&';
}

inline std::string referencedType(const std::string& typeName) {
    return isReferenceType(typeName) ? typeName.substr(1) : std::string();
}

inline GeneratedIR generate(const std::vector<MethodBinding>& methods, const TypeRules& rules) {
    GeneratedIR generated;
    if (methods.empty()) return generated;

    auto addString = [&](size_t id, const char* suffix, const std::string& value) {
        const std::string symbol = "@.zeus_dynamic_member_" +
            std::to_string(id) + "_" + suffix;
        generated.constants += symbol + " = private unnamed_addr constant [" +
            std::to_string(value.size() + 1) + " x i8] c\"" + escapeBytes(value) +
            "\\00\", align 1\n";
        return symbol;
    };

    for (size_t methodIndex = 0; methodIndex < methods.size(); ++methodIndex) {
        const auto& method = methods[methodIndex];
        if (method.parameterTypes.empty() ||
            method.parameterTypes.front() != method.typeName) {
            generated.error = "dynamic method '" + method.typeName + "::" +
                method.methodName + "' must use its owner type as the first parameter";
            return generated;
        }

        const size_t argumentCount = method.parameterTypes.size() - 1;
        const std::string thunkName = "@__zeus_dynamic_member_" +
            std::to_string(methodIndex);
        const std::string typeString = addString(methodIndex, "type", method.typeName);
        const std::string methodString = addString(methodIndex, "method", method.methodName);
        const std::string labelString = addString(
            methodIndex, "label", method.typeName + "::" + method.methodName);

        generated.adapters += "define internal void " + thunkName +
            "(ptr %out, ptr %receiver, i32 %argc, ptr %arguments) {\nentry:\n";
        generated.adapters += "  %arity.ok = icmp eq i32 %argc, " +
            std::to_string(argumentCount) + "\n";
        generated.adapters += "  br i1 %arity.ok, label %arguments.valid, label %bad.arity\n";
        generated.adapters += "bad.arity:\n";
        generated.adapters += "  call void @zeus_dynamic_member_arity_error(ptr " +
            labelString + ", i32 " + std::to_string(argumentCount) +
            ", i32 %argc)\n  unreachable\n";
        generated.adapters += "arguments.valid:\n";
        generated.adapters += "  call void @zeus_value_init_nil(ptr %out)\n";

        const std::string receiverType = method.typeName;
        const std::string receiverABIType = rules.parameterABIType(receiverType);
        if (receiverABIType.empty()) {
            generated.error = "dynamic method '" + method.typeName + "::" +
                method.methodName + "' has no receiver ABI";
            return generated;
        }

        std::string receiverNative = "%receiver";
        if (rules.usesZeusValueABI(receiverType)) {
            // Dynamic-only native types are already represented by ZeusValue.
        } else if (receiverType == "int" || receiverType == "int32" ||
                   receiverType == "int64") {
            receiverNative = "%receiver.native";
            generated.adapters += "  " + receiverNative +
                " = call i64 @zeus_value_require_i64(ptr %receiver)\n";
        } else if (receiverType == "bool") {
            const std::string rawBool = "%receiver.bool.raw";
            receiverNative = "%receiver.native";
            generated.adapters += "  " + rawBool +
                " = call i32 @zeus_value_require_bool(ptr %receiver)\n";
            generated.adapters += "  " + receiverNative + " = icmp ne i32 " +
                rawBool + ", 0\n";
        } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(receiverType);
                   ZeusBuiltins::isNativeViewABI(abi)) {
            receiverNative = "%receiver.native";
            generated.adapters += "  " + receiverNative + " = call ptr @" +
                std::string(ZeusBuiltins::nativeViewFromDynamicHelper(abi)) +
                "(ptr %receiver)\n";
        } else if (rules.isClassType(receiverType)) {
            receiverNative = "%receiver.native";
            generated.adapters += "  " + receiverNative +
                " = call ptr @zeus_value_require_class_ptr(ptr %receiver, ptr " +
                typeString + ")\n";
        } else {
            generated.error = "dynamic method '" + method.typeName + "::" +
                method.methodName + "' cannot adapt receiver type '" +
                receiverType + "'";
            return generated;
        }

        std::vector<std::pair<std::string, std::string>> nativeArguments;
        nativeArguments.emplace_back(receiverABIType, receiverNative);

        for (size_t parameterIndex = 1;
             parameterIndex < method.parameterTypes.size();
             ++parameterIndex) {
            const std::string& parameterType = method.parameterTypes[parameterIndex];
            const size_t argumentIndex = parameterIndex - 1;
            const std::string suffix = std::to_string(argumentIndex);
            const std::string slot = "%argument.slot." + suffix;
            const std::string boxed = "%argument.box." + suffix;
            const std::string native = "%argument.native." + suffix;
            const std::string nativeType = rules.parameterABIType(parameterType);
            if (nativeType.empty()) {
                generated.error = "dynamic method '" + method.typeName + "::" +
                    method.methodName + "' has no ABI for parameter " +
                    std::to_string(parameterIndex);
                return generated;
            }

            generated.adapters += "  " + slot +
                " = getelementptr inbounds ptr, ptr %arguments, i64 " + suffix + "\n";
            generated.adapters += "  " + boxed + " = load ptr, ptr " + slot + ", align 8\n";

            std::string conversionType = parameterType;
            if (isReferenceType(parameterType)) {
                conversionType = referencedType(parameterType);
                const auto* abi = ZeusBuiltins::findNativeTypeABI(conversionType);
                if (abi != nullptr && abi->valueABI == ZeusBuiltins::NativeValueABI::ZeusValue) {
                    generated.error = "dynamic method '" + method.typeName + "::" +
                        method.methodName + "' cannot adapt reference parameter '" +
                        parameterType + "' through ZeusValue";
                    return generated;
                }
            } else if (rules.usesZeusValueABI(parameterType)) {
                nativeArguments.emplace_back(nativeType, boxed);
                continue;
            }

            if (conversionType == "int" || conversionType == "int32" ||
                conversionType == "int64") {
                if (isReferenceType(parameterType)) {
                    generated.adapters += "  " + native +
                        " = call ptr @zeus_value_require_i64_ptr(ptr " + boxed + ")\n";
                } else {
                    generated.adapters += "  " + native +
                        " = call i64 @zeus_value_require_i64(ptr " + boxed + ")\n";
                }
            } else if (conversionType == "bool") {
                if (isReferenceType(parameterType)) {
                    generated.adapters += "  " + native +
                        " = call ptr @zeus_value_require_bool_ptr(ptr " + boxed + ")\n";
                } else {
                    const std::string raw = "%argument.bool.raw." + suffix;
                    generated.adapters += "  " + raw +
                        " = call i32 @zeus_value_require_bool(ptr " + boxed + ")\n";
                    generated.adapters += "  " + native + " = icmp ne i32 " + raw + ", 0\n";
                }
            } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(conversionType);
                       ZeusBuiltins::isNativeViewABI(abi)) {
                generated.adapters += "  " + native + " = call ptr @" +
                    std::string(ZeusBuiltins::nativeViewFromDynamicHelper(abi)) +
                    "(ptr " + boxed + ")\n";
            } else if (rules.isClassType(conversionType)) {
                const std::string classString =
                    "@.zeus_dynamic_member_" + std::to_string(methodIndex) +
                    "_parameter_type_" + suffix;
                generated.constants += classString +
                    " = private unnamed_addr constant [" +
                    std::to_string(conversionType.size() + 1) +
                    " x i8] c\"" + escapeBytes(conversionType) +
                    "\\00\", align 1\n";
                generated.adapters += "  " + native +
                    " = call ptr @zeus_value_require_class_ptr(ptr " + boxed +
                    ", ptr " + classString + ")\n";
            } else {
                generated.error = "dynamic method '" + method.typeName + "::" +
                    method.methodName + "' cannot adapt parameter type '" +
                    parameterType + "'";
                return generated;
            }
            nativeArguments.emplace_back(nativeType, native);
        }

        const bool returnsDynamic = rules.usesZeusValueABI(method.returnType);
        const std::string returnType = returnsDynamic
            ? "void"
            : rules.llvmType(method.returnType);
        if (returnType.empty()) {
            generated.error = "dynamic method '" + method.typeName + "::" +
                method.methodName + "' has no LLVM return ABI";
            return generated;
        }

        std::string call;
        std::string rawResult;
        if (returnsDynamic) {
            call = "  call void " + rules.callTarget(method.functionName) + "(ptr %out";
        } else {
            rawResult = "%method.result";
            call = "  " + rawResult + " = call " + returnType + " " +
                rules.callTarget(method.functionName) + "(";
        }
        bool hasCallArgument = returnsDynamic;
        for (const auto& argument : nativeArguments) {
            if (hasCallArgument) call += ", ";
            call += argument.first + " " + argument.second;
            hasCallArgument = true;
        }
        call += ")\n";
        generated.adapters += call;

        if (!returnsDynamic) {
            const std::string resultType = isReferenceType(method.returnType)
                ? referencedType(method.returnType)
                : method.returnType;
            if (resultType == "int" || resultType == "int32" || resultType == "int64") {
                if (isReferenceType(method.returnType)) {
                    generated.adapters += "  call void @zeus_value_from_i64_ptr(ptr %out, ptr " +
                        rawResult + ")\n";
                } else {
                    generated.adapters += "  call void @zeus_value_init_i64(ptr %out, i64 " +
                        rawResult + ")\n";
                }
            } else if (resultType == "bool") {
                if (isReferenceType(method.returnType)) {
                    generated.adapters += "  call void @zeus_value_from_bool_ptr(ptr %out, ptr " +
                        rawResult + ")\n";
                } else {
                    const std::string rawBool = "%method.bool.raw";
                    generated.adapters += "  " + rawBool + " = zext i1 " + rawResult +
                        " to i32\n";
                    generated.adapters += "  call void @zeus_value_init_bool(ptr %out, i32 " +
                        rawBool + ")\n";
                }
            } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(resultType);
                       ZeusBuiltins::isNativeViewABI(abi)) {
                generated.adapters += "  call void @" +
                    std::string(ZeusBuiltins::nativeViewToDynamicHelper(abi)) +
                    "(ptr %out, ptr " + rawResult + ")\n";
            } else if (rules.isClassType(resultType)) {
                const std::string resultClassString =
                    "@.zeus_dynamic_member_" + std::to_string(methodIndex) +
                    "_return_type";
                generated.constants += resultClassString +
                    " = private unnamed_addr constant [" +
                    std::to_string(resultType.size() + 1) +
                    " x i8] c\"" + escapeBytes(resultType) +
                    "\\00\", align 1\n";
                generated.adapters += "  call void @zeus_value_from_class_ptr(ptr %out, ptr " +
                    rawResult + ", ptr " + resultClassString + ")\n";
            } else {
                generated.error = "dynamic method '" + method.typeName + "::" +
                    method.methodName + "' cannot box return type '" +
                    method.returnType + "'";
                return generated;
            }
        }

        generated.adapters += "  ret void\n}\n\n";
        generated.initializer += "  call void @zeus_dynamic_member_register(ptr " +
            typeString + ", ptr " + methodString + ", ptr " + thunkName + ")\n";
    }

    generated.adapters += "define internal void @__zeus_register_dynamic_members() {\nentry:\n";
    generated.adapters += generated.initializer;
    generated.adapters += "  ret void\n}\n\n";
    return generated;
}

struct RuntimeStackCallRules {
    std::function<std::string()> popValue;
    std::function<void(const std::string&)> emitLine;
    std::function<std::string()> freshTemporary;
    std::function<std::string(const std::string&)> poolString;
    std::function<void(const std::string&)> releaseValue;
    std::function<void(const std::string&)> pushValue;
};

inline bool emitRuntimeStackCall(
    const std::string& methodName,
    int32_t argumentCount,
    const RuntimeStackCallRules& rules,
    std::string& error
) {
    if (methodName.empty() || argumentCount < 0 ||
        !rules.popValue || !rules.emitLine || !rules.freshTemporary ||
        !rules.poolString || !rules.releaseValue || !rules.pushValue) {
        error = "invalid runtime stack method call";
        return false;
    }

    std::vector<std::string> arguments(static_cast<size_t>(argumentCount));
    for (int32_t i = argumentCount - 1; i >= 0; --i) {
        arguments[static_cast<size_t>(i)] = rules.popValue();
    }
    const std::string receiver = rules.popValue();

    std::string argumentArray = "null";
    if (!arguments.empty()) {
        argumentArray = rules.freshTemporary();
        rules.emitLine(argumentArray + " = alloca ptr, i64 " +
                       std::to_string(arguments.size()) + ", align 8");
        for (size_t i = 0; i < arguments.size(); ++i) {
            const std::string slot = rules.freshTemporary();
            rules.emitLine(slot + " = getelementptr inbounds ptr, ptr " +
                           argumentArray + ", i64 " + std::to_string(i));
            rules.emitLine("store ptr " + arguments[i] + ", ptr " +
                           slot + ", align 8");
        }
    }

    const std::string methodSymbol = rules.poolString(methodName);
    const std::string methodPointer = rules.freshTemporary();
    rules.emitLine(methodPointer + " = getelementptr inbounds [" +
                   std::to_string(methodName.size() + 1) + " x i8], ptr " +
                   methodSymbol + ", i64 0, i64 0");

    const std::string result = rules.freshTemporary();
    rules.emitLine(result + " = alloca %ZeusValue, align 8");
    rules.emitLine("call void @zeus_value_dynamic_member_call(ptr " + result +
                   ", ptr " + receiver + ", ptr " + methodPointer + ", i32 " +
                   std::to_string(argumentCount) + ", ptr " + argumentArray + ")");

    rules.releaseValue(receiver);
    for (const auto& argument : arguments) rules.releaseValue(argument);
    rules.pushValue(result);
    return true;
}

} // namespace ZeusLLVM::DynamicMemberBridge
