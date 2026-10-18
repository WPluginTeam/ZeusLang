#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Types.hpp"
#include "GenericTypeName.hpp"

namespace ZeusTypeInfer {

    // 类型引用：基础类型之外，预留函数类型的参数/返回值信息。
    // InferredType 继续保留，作为现有推断代码和旧接口的兼容表示。
    struct TypeRef {
        InferredType kind = InferredType::Unknown;
        std::string namedType;
        bool isReference = false;
        std::vector<std::string> namedTypeArguments;
        std::vector<TypeRef> parameterTypes;
        std::shared_ptr<TypeRef> returnType;

        TypeRef() = default;

        explicit TypeRef(InferredType type)
            : kind(type) {}

        TypeRef(InferredType type, std::string name)
            : kind(type), namedType(std::move(name)) {}

        bool isKnown() const {
            return kind != InferredType::Unknown;
        }

        bool isFunction() const {
            return kind == InferredType::Function;
        }

        std::string toString() const {
            if (!namedType.empty()) {
                std::string name = namedType;
                if (!namedTypeArguments.empty()) {
                    name += "<";
                    for (size_t i = 0; i < namedTypeArguments.size(); ++i) {
                        if (i != 0) name += ", ";
                        name += namedTypeArguments[i];
                    }
                    name += ">";
                }
                return isReference ? "&" + name : name;
            }
            if (!isFunction()) {
                const std::string baseName = typeName(kind);
                return isReference ? "&" + baseName : baseName;
            }

            std::string result = "func(";
            for (size_t i = 0; i < parameterTypes.size(); ++i) {
                if (i > 0) result += ", ";
                result += parameterTypes[i].toString();
            }
            result += ")";

            if (returnType) {
                result += " -> ";
                result += returnType->toString();
            }

            return result;
        }

        // IR 使用的规范类型名。基础类型与源代码注解保持一致，函数类型保留完整签名。
        std::string toIRTypeName() const {
            if (!namedType.empty() && !namedTypeArguments.empty()) {
                const std::string specialized = genericSpecializationName(
                    namedType, namedTypeArguments);
                return isReference ? "&" + specialized : specialized;
            }
            return toString();
        }

        bool operator==(const TypeRef& other) const {
            if (kind != other.kind || namedType != other.namedType ||
                isReference != other.isReference ||
                namedTypeArguments != other.namedTypeArguments ||
                parameterTypes != other.parameterTypes) {
                return false;
            }

            if (returnType == nullptr || other.returnType == nullptr) {
                return returnType == other.returnType;
            }

            return *returnType == *other.returnType;
        }

        bool operator!=(const TypeRef& other) const {
            return !(*this == other);
        }
    };

    inline TypeRef makeTypeRef(InferredType type) {
        return TypeRef(type);
    }

    inline TypeRef parseTypeRef(const std::string& name) {
        TypeRef result(parseTypeName(name));
        if (ZeusBuiltins::findNativeIntegerABI(name) ||
            ZeusBuiltins::isNativeFloatingPointType(name)) result.namedType = name;
        result.isReference = !name.empty() && name.front() == '&' &&
            result.kind != InferredType::RawBytesRange;
        if (result.isReference) {
            const std::string targetType = name.substr(1);
            if (ZeusBuiltins::findNativeIntegerABI(targetType) ||
                ZeusBuiltins::isNativeFloatingPointType(targetType)) result.namedType = targetType;
        }
        return result;
    }

    inline TypeRef makeFunctionType(
        std::vector<TypeRef> parameters,
        TypeRef returnType
    ) {
        TypeRef function(InferredType::Function);
        function.parameterTypes = std::move(parameters);
        function.returnType = std::make_shared<TypeRef>(std::move(returnType));
        return function;
    }

    // 绑定到 AST/IR 节点上的语义类型信息。
    // resolved 与 kind 分开，便于区分“明确知道是 Unknown”与“尚未推断”。
    struct TypeInfo {
        TypeRef type;
        bool resolved = false;
        bool compileTimeConstant = false;
        bool explicitAnnotation = false;
        std::uint32_t bindingNodeId = 0;

        bool isKnown() const {
            return resolved && type.isKnown();
        }

        bool isResolved() const {
            return resolved;
        }

        bool isDynamic() const {
            return resolved && type.kind == InferredType::Unknown;
        }

        static TypeInfo unresolved() {
            return {};
        }

        static TypeInfo dynamic() {
            TypeInfo result;
            result.type = TypeRef(InferredType::Unknown);
            result.resolved = true;
            return result;
        }

        static TypeInfo fromType(
            TypeRef value,
            bool constant = false,
            bool explicitType = false
        ) {
            TypeInfo result;
            result.type = std::move(value);
            result.resolved = result.type.isKnown();
            result.compileTimeConstant = constant;
            result.explicitAnnotation = explicitType;
            return result;
        }

        std::string toString() const {
            return isKnown() ? type.toString() : "unknown";
        }

        std::string toIRTypeName() const {
            return isKnown() ? type.toIRTypeName() : std::string();
        }
    };

    // Checker 使用的统一类型环境。
    // 保留 TypeInferer 旧的 InferredType 表示，避免一次性改动所有推断接口。
    using TypeEnvironment = std::unordered_map<std::string, TypeInfo>;

}
