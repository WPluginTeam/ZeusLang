#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "TypeInfer.hpp"
#include "../ZeusParser/GenericCallSyntax.hpp"
#include "../ZeusParser/AstElementView.hpp"
#include "../ZeusBuiltins/BuiltinRegistry.hpp"

namespace ZeusTypeInfer {

    enum class TypeCheckMode {
        Dynamic,
        Static
    };

    enum class TypeCheckSeverity {
        Warning,
        Error
    };

    enum class TypeCheckDiagnosticCode {
        General,
        CallArgumentTypeMismatch,
        GenericTypeArgumentMismatch,
        GenericTypeArgumentUnknown,
        GenericTypeParameterNameInvalid,
        GenericTypeParameterUnsupportedContext
    };

    struct TypeCheckDiagnostic {
        int nodeIndex = -1;
        TypeCheckSeverity severity = TypeCheckSeverity::Error;
        std::string message;
        int line = 0;
        int column = 0;
        TypeCheckDiagnosticCode code = TypeCheckDiagnosticCode::General;
    };

    struct TypeCheckOptions {
        TypeCheckMode mode = TypeCheckMode::Dynamic;
        bool requireExplicitFunctionParameterTypes = false;
        bool requireExplicitFunctionReturnTypes = false;
        bool requireExplicitVariableTypes = false;
        bool rejectUnknownExpressions = false;

        static TypeCheckOptions dynamic() {
            return {};
        }

        static TypeCheckOptions llvm() {
            TypeCheckOptions options;
            options.mode = TypeCheckMode::Static;
            options.requireExplicitFunctionParameterTypes = false;
            options.requireExplicitFunctionReturnTypes = false;
            options.requireExplicitVariableTypes = false;
            options.rejectUnknownExpressions = false;
            // Function boundaries may carry dynamic ZeusValue values when an
            // annotation is omitted. The emitter decides which operations can
            // still use native LLVM types.
            return options;
        }

        static TypeCheckOptions strictLLVM() {
            TypeCheckOptions options = llvm();
            options.requireExplicitFunctionParameterTypes = true;
            // Return types may be inferred from the function body. The checker
            // rejects a dynamic/ambiguous result for @static functions below.
            options.requireExplicitFunctionReturnTypes = false;
            // Locals may omit annotations when their initializer is inferable;
            // rejectUnknownExpressions still rejects unresolved dynamic values.
            options.requireExplicitVariableTypes = false;
            options.rejectUnknownExpressions = true;
            return options;
        }
    };

    class TypeChecker {
        using Environment = TypeEnvironment;

        struct FunctionContext {
            std::string name;
            InferredType expectedReturn = InferredType::Unknown;
            TypeRef inferredReturnType;
            bool hasInferredReturnType = false;
            bool inferredReturnIsDynamic = false;
            bool sawUnknownReturnType = false;
            bool hasReturn = false;
            bool expectedReturnIsReference = false;
            std::string expectedReturnTypeName;
        };

        const std::vector<TreeParser::NodeAndBlock>& nodes;
        TypeInferer inferer;
        std::vector<TypeCheckDiagnostic> diagnostics;
        TypeCheckOptions rootOptions = TypeCheckOptions::dynamic();
        int currentSourceLine = 0;
        int currentSourceColumn = 0;
        std::unordered_set<std::string> classNames;
        std::unordered_set<std::string> activeTypeParameters;
        std::unordered_map<std::string, std::vector<std::string>> genericClassParameters;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFields;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> externalClassFields;
        std::unordered_map<std::string, std::string> classReceiverAliases;
        std::unordered_map<std::string, std::string> operatorReturnTypes;
        std::unordered_map<TreeParser::NodeId, std::string> inferredFunctionReturnTypes;
        std::unordered_map<TreeParser::NodeId, std::string> inferredBindingTypesByNode;
        std::unordered_map<std::string, TypeInfo> inferredFunctionReturnInfos;
        bool finalizeUnknownReturnSummaries = false;
        mutable std::unordered_set<TreeParser::NodeId> observedBindingNodes;
        std::unordered_map<
            std::string,
            std::unordered_map<std::string, TreeParser::NodeId>
        > classFieldDeclarationNodes;
        std::unordered_map<std::string, std::unordered_set<std::string>>
            explicitlyAnnotatedClassFields;

        void setInferredBindingType(
            TreeParser::NodeId nodeId,
            const std::string& typeName
        ) {
            if (nodeId == TreeParser::InvalidNodeId || typeName.empty()) return;
            auto found = inferredBindingTypesByNode.find(nodeId);
            if (found == inferredBindingTypesByNode.end()) {
                inferredBindingTypesByNode.emplace(nodeId, typeName);
                return;
            }
            if (found->second == "unknown" || typeName == "unknown") {
                found->second = "unknown";
            } else {
                found->second = typeName;
            }
        }

        static bool sameFlowType(const TypeInfo& left, const TypeInfo& right) {
            return left.isResolved() == right.isResolved() &&
                left.type == right.type;
        }

        static bool sameFunctionReturnInfos(
            const std::unordered_map<std::string, TypeInfo>& left,
            const std::unordered_map<std::string, TypeInfo>& right
        ) {
            if (left.size() != right.size()) return false;
            for (const auto& entry : left) {
                const auto other = right.find(entry.first);
                if (other == right.end() ||
                    entry.second.isResolved() != other->second.isResolved() ||
                    entry.second.type != other->second.type) {
                    return false;
                }
            }
            return true;
        }

        TypeInfo mergeFlowTypes(
            const TypeInfo& left,
            const TypeInfo& right
        ) const {
            if (sameFlowType(left, right)) return left;
            if (left.type.kind == InferredType::Object &&
                right.type.kind == InferredType::Nil) {
                return left;
            }
            if (right.type.kind == InferredType::Object &&
                left.type.kind == InferredType::Nil) {
                return right;
            }
            return TypeInfo::dynamic();
        }

        static std::string flowTypeName(const TypeInfo& info) {
            if (info.isDynamic() || !info.isResolved()) return "unknown";
            TypeRef valueType = info.type;
            valueType.isReference = false;
            if (valueType.isFunction()) return {};
            return valueType.toIRTypeName();
        }

        static std::string substituteGenericTypeName(
            const std::string& typeName,
            const std::unordered_map<std::string, std::string>& substitutions
        ) {
            const bool isReference = !typeName.empty() && typeName.front() == '&';
            const std::string baseType = isReference ? typeName.substr(1) : typeName;
            const auto found = substitutions.find(baseType);
            if (found != substitutions.end()) {
                return (isReference ? "&" : "") + found->second;
            }

            std::string genericBase;
            std::vector<std::string> genericArguments;
            if (!ZeusTypeInfer::decodeGenericSpecializationName(
                    baseType, genericBase, genericArguments)) {
                return typeName;
            }
            bool changed = false;
            for (auto& argument : genericArguments) {
                const std::string substituted = substituteGenericTypeName(
                    argument, substitutions);
                changed = changed || substituted != argument;
                argument = substituted;
            }
            if (!changed) return typeName;

            const std::string specialized = ZeusTypeInfer::genericSpecializationName(
                genericBase, genericArguments);
            return (isReference ? "&" : "") + specialized;
        }

        static bool containsGenericTypeParameter(
            const std::string& typeName,
            const std::vector<std::string>& parameters
        ) {
            const std::string baseType = !typeName.empty() && typeName.front() == '&'
                ? typeName.substr(1) : typeName;
            if (std::find(parameters.begin(), parameters.end(), baseType) !=
                parameters.end()) {
                return true;
            }
            std::string genericBase;
            std::vector<std::string> genericArguments;
            if (!ZeusTypeInfer::decodeGenericSpecializationName(
                    baseType, genericBase, genericArguments)) {
                return false;
            }
            return std::any_of(genericArguments.begin(), genericArguments.end(),
                [&](const std::string& argument) {
                    return containsGenericTypeParameter(argument, parameters);
                });
        }

        static void collectGenericTypeParameters(
            const std::string& typeName,
            const std::vector<std::string>& parameters,
            std::unordered_set<std::string>& foundParameters
        ) {
            const std::string baseType = !typeName.empty() && typeName.front() == '&'
                ? typeName.substr(1) : typeName;
            if (std::find(parameters.begin(), parameters.end(), baseType) !=
                parameters.end()) {
                foundParameters.insert(baseType);
                return;
            }
            std::string genericBase;
            std::vector<std::string> genericArguments;
            if (!ZeusTypeInfer::decodeGenericSpecializationName(
                    baseType, genericBase, genericArguments)) {
                return;
            }
            for (const auto& argument : genericArguments) {
                collectGenericTypeParameters(
                    argument, parameters, foundParameters);
            }
        }

        static bool collectGenericTypeBindings(
            const std::string& formalType,
            const std::string& actualType,
            const std::vector<std::string>& parameters,
            std::unordered_map<std::string, std::string>& bindings
        ) {
            const std::string formalBase = !formalType.empty() && formalType.front() == '&'
                ? formalType.substr(1) : formalType;
            const std::string actualBase = !actualType.empty() && actualType.front() == '&'
                ? actualType.substr(1) : actualType;
            const auto parameter = std::find(
                parameters.begin(), parameters.end(), formalBase);
            if (parameter != parameters.end()) {
                const auto previous = bindings.find(formalBase);
                if (previous != bindings.end()) return previous->second == actualBase;
                bindings.emplace(formalBase, actualBase);
                return true;
            }

            if (!containsGenericTypeParameter(formalBase, parameters)) return true;

            std::string formalGenericBase;
            std::string actualGenericBase;
            std::vector<std::string> formalArguments;
            std::vector<std::string> actualArguments;
            if (!ZeusTypeInfer::decodeGenericSpecializationName(
                    formalBase, formalGenericBase, formalArguments) ||
                !ZeusTypeInfer::decodeGenericSpecializationName(
                    actualBase, actualGenericBase, actualArguments) ||
                formalGenericBase != actualGenericBase ||
                formalArguments.size() != actualArguments.size()) {
                return false;
            }
            for (size_t i = 0; i < formalArguments.size(); ++i) {
                if (!collectGenericTypeBindings(
                        formalArguments[i], actualArguments[i], parameters, bindings)) {
                    return false;
                }
            }
            return true;
        }

        void collectClassNames(
            const std::vector<TreeParser::NodeAndBlock>& blockNodes
        ) {
            for (const auto& element : blockNodes) {
                if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(element)) {
                    continue;
                }

                const auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(element);
                if (!block) continue;

                if (block->type == CodeBlockType::CLASS && !block->className.empty()) {
                    classNames.insert(block->className);
                    if (!block->typeParameters.empty()) {
                        genericClassParameters[block->className] = block->typeParameters;
                    }
                    auto& fields = classFields[block->className];
                    for (const auto& child : block->nodes) {
                        if (!std::holds_alternative<TreeParser::Node>(child)) continue;

                        const auto& fieldNode = std::get<TreeParser::Node>(child);
                        if (fieldNode.type == NodeType::EXP_STMT &&
                            fieldNode.tokens.size() >= 3 &&
                            isKeyword(fieldNode.tokens[0], "using") &&
                            isOperator(fieldNode.tokens[1], "&") &&
                            std::holds_alternative<LexerType::Keyword>(fieldNode.tokens[2])) {
                            classReceiverAliases[block->className] =
                                std::get<LexerType::Keyword>(fieldNode.tokens[2]).name;
                            continue;
                        }
                        if (fieldNode.type != NodeType::VAR_DECL_STMT) continue;

                        std::string fieldName;
                        std::string fieldType;
                        size_t equal = fieldNode.tokens.size();
                        size_t colon = fieldNode.tokens.size();
                        for (size_t i = 0; i < fieldNode.tokens.size(); ++i) {
                            const auto& token = fieldNode.tokens[i];
                            if (isOperator(token, "=")) {
                                equal = i;
                                break;
                            }
                            if (isOperator(token, ":")) {
                                if (colon == fieldNode.tokens.size()) colon = i;
                            }
                            if (!std::holds_alternative<LexerType::Keyword>(token)) continue;

                            const std::string& name =
                                std::get<LexerType::Keyword>(token).name;
                            if (name == "let" || name == "var" || name == "const") continue;
                            if (colon == fieldNode.tokens.size() && fieldName.empty()) {
                                fieldName = name;
                            }
                        }
                        if (colon < equal) {
                            size_t typePosition = colon + 1;
                            ZeusParser::parseGenericType(
                                fieldNode.tokens, typePosition, fieldType);
                        }

                        if (!fieldName.empty() && !fieldType.empty()) {
                            fields[fieldName] = fieldType;
                        }
                    }
                    for (const auto& child : block->nodes) {
                        if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(child)) {
                            continue;
                        }
                        const auto& childBlock =
                            std::get<std::unique_ptr<TreeParser::CodeBlock>>(child);
                        if (childBlock && childBlock->type == CodeBlockType::FUNC) {
                            childBlock->ownerClassName = block->className;
                            if (childBlock->isOperatorOverload &&
                                !childBlock->returnTypeName.empty()) {
                                for (const auto& operation : childBlock->overloadedOps) {
                                    std::string symbol = operation;
                                    if (symbol.size() >= 2 && symbol.front() == '[' &&
                                        symbol.back() == ']') {
                                        symbol = symbol.substr(1, symbol.size() - 2);
                                    }
                                    operatorReturnTypes[
                                        block->className + "::" + symbol
                                    ] = childBlock->returnTypeName;
                                }
                            }
                        }
                    }
                }

                collectClassNames(block->nodes);
            }
        }

        std::string instanceReceiverAlias(const TreeParser::CodeBlock& block) const {
            if (block.type != CodeBlockType::FUNC || !block.isClassMethod ||
                block.isStaticMethod || block.isCompileTimeStatic ||
                block.ownerClassName.empty()) {
                return {};
            }

            if (!block.paramNames.empty() && block.paramNames.front() == "this") {
                return "this";
            }

            auto declaredAlias = classReceiverAliases.find(block.ownerClassName);
            if (declaredAlias != classReceiverAliases.end()) return declaredAlias->second;
            if (!block.captureVars.empty()) return block.captureVars.front();
            // No declared alias and no explicit capture: the method takes no
            // receiver, so there is nothing to infer. Operators get no exemption
            // here; a receiver only exists if the class or the method asked for
            // one by name.
            return {};
        }

        InferredType parseDeclaredType(const std::string& name) const {
            InferredType type = parseTypeName(name);
            const std::string baseName =
                !name.empty() && name.front() == '&' ? name.substr(1) : name;
            if (activeTypeParameters.count(baseName) != 0) {
                return InferredType::TypeParameter;
            }
            std::string className = baseName;
            std::vector<std::string> ignoredTypeArguments;
            ZeusTypeInfer::decodeGenericSpecializationName(
                baseName, className, ignoredTypeArguments);
            if (type == InferredType::Unknown && classNames.count(className) != 0) {
                return InferredType::Object;
            }
            return type;
        }

        TypeRef parseDeclaredTypeRef(const std::string& name) const {
            TypeRef result = parseTypeRef(name);
            const InferredType type = parseDeclaredType(
                result.isReference ? name.substr(1) : name
            );
            result.kind = type;
            const std::string baseName = result.isReference ? name.substr(1) : name;
            if (type == InferredType::TypeParameter) {
                result.namedType = baseName;
            }
            if (type == InferredType::Object) {
                std::string className = baseName;
                std::vector<std::string> typeArguments;
                const bool isSpecialized =
                    ZeusTypeInfer::decodeGenericSpecializationName(
                        baseName, className, typeArguments);
                if (classNames.count(className) != 0) {
                    result.namedType = className;
                    if (isSpecialized) {
                        result.namedTypeArguments = std::move(typeArguments);
                    }
                }
            }
            return result;
        }

        TypeInfo makeFunctionValueInfo(const FunctionInfo& function) const {
            std::vector<TypeRef> parameterTypes;
            parameterTypes.reserve(function.parameterNames.size());
            for (size_t index = 0; index < function.parameterNames.size(); ++index) {
                if (index < function.parameterTypeNames.size() &&
                    !function.parameterTypeNames[index].empty()) {
                    parameterTypes.push_back(
                        parseDeclaredTypeRef(function.parameterTypeNames[index]));
                } else if (index < function.parameterTypes.size()) {
                    parameterTypes.emplace_back(function.parameterTypes[index]);
                } else {
                    parameterTypes.emplace_back(InferredType::Unknown);
                }
            }

            TypeRef returnType(InferredType::Unknown);
            const auto inferred = inferredFunctionReturnInfos.find(function.name);
            if (inferred != inferredFunctionReturnInfos.end()) {
                returnType = inferred->second.type;
                returnType.isReference = false;
            } else if (!function.returnTypeName.empty()) {
                returnType = parseDeclaredTypeRef(function.returnTypeName);
            } else if (!function.dynamicReturn ||
                       function.returnType != InferredType::Unknown) {
                returnType = TypeRef(function.returnType);
            }

            return TypeInfo::fromType(
                makeFunctionType(std::move(parameterTypes), std::move(returnType)),
                false,
                true
            );
        }

        void promoteClassFieldToDynamic(
            const std::string& className,
            const std::string& fieldName
        ) {
            classFields[className][fieldName] = "unknown";
            const auto classNodes = classFieldDeclarationNodes.find(className);
            if (classNodes == classFieldDeclarationNodes.end()) return;
            const auto fieldNode = classNodes->second.find(fieldName);
            if (fieldNode == classNodes->second.end() ||
                fieldNode->second == TreeParser::InvalidNodeId) {
                return;
            }
            setInferredBindingType(fieldNode->second, "unknown");
        }

        bool expressionUsesDynamicBinding(
            const std::vector<LexerType::Token>& expression,
            const Environment& environment
        ) const {
            for (size_t i = 0; i < expression.size(); ++i) {
                if (!std::holds_alternative<LexerType::Keyword>(expression[i])) {
                    continue;
                }
                if ((i > 0 && isOperator(expression[i - 1], ".")) ||
                    (i + 1 < expression.size() &&
                     isOperator(expression[i + 1], "."))) {
                    continue;
                }
                const std::string& name =
                    std::get<LexerType::Keyword>(expression[i]).name;
                const auto binding = environment.find(name);
                if (binding != environment.end() && binding->second.isDynamic()) {
                    return true;
                }
            }
            return false;
        }

        TypeInfo inferExpressionInfo(
            const std::vector<LexerType::Token>& expression,
            const Environment& environment
        ) const {
            for (size_t i = 0; i < expression.size(); ++i) {
                if (!std::holds_alternative<LexerType::Keyword>(expression[i])) {
                    continue;
                }
                if ((i > 0 && isOperator(expression[i - 1], ".")) ||
                    (i + 1 < expression.size() &&
                     isOperator(expression[i + 1], "."))) {
                    continue;
                }
                const std::string& name =
                    std::get<LexerType::Keyword>(expression[i]).name;
                const auto binding = environment.find(name);
                if (binding != environment.end() &&
                    binding->second.bindingNodeId != TreeParser::InvalidNodeId) {
                    observedBindingNodes.insert(binding->second.bindingNodeId);
                }
            }

            if (expression.size() >= 4 &&
                isKeyword(expression[0], "sizeof") &&
                isOperator(expression[1], "(")) {
                size_t typePosition = 2;
                std::string typeName;
                if (ZeusParser::parseGenericType(
                        expression, typePosition, typeName) &&
                    typePosition + 1 == expression.size() &&
                    isOperator(expression[typePosition], ")")) {
                    return TypeInfo::fromType(TypeRef(InferredType::Int));
                }
            }
            if (expression.size() >= 4 && isOperator(expression[0], "@") &&
                std::holds_alternative<LexerType::Keyword>(expression[1]) &&
                isOperator(expression[2], "(")) {
                const std::string& intrinsic =
                    std::get<LexerType::Keyword>(expression[1]).name;
                size_t close = 2;
                int depth = 0;
                for (; close < expression.size(); ++close) {
                    if (isOperator(expression[close], "(")) ++depth;
                    else if (isOperator(expression[close], ")") && --depth == 0) break;
                }
                if ((intrinsic == "addr" || intrinsic == "reveal") &&
                    close + 1 == expression.size()) {
                    return TypeInfo::fromType(TypeRef(InferredType::Int));
                }
                if (intrinsic == "faddr" && close + 3 == expression.size() &&
                    isOperator(expression[close + 1], "->") &&
                    std::holds_alternative<LexerType::Keyword>(expression.back())) {
                    return TypeInfo::fromType(parseDeclaredTypeRef(
                        std::get<LexerType::Keyword>(expression.back()).name));
                }
                if (intrinsic == "faddr" && close + 4 == expression.size() &&
                    isOperator(expression[close + 1], "->") &&
                    isOperator(expression[close + 2], "&") &&
                    isKeyword(expression[close + 3], "bytes")) {
                    return TypeInfo::fromType(parseDeclaredTypeRef("&bytes"));
                }
            }
            if (expression.size() >= 2 && isKeyword(expression.front(), "new") &&
                std::holds_alternative<LexerType::Keyword>(expression[1])) {
                size_t typePosition = 1;
                std::string className;
                if (!ZeusParser::parseGenericTypeName(
                        expression, typePosition, className)) {
                    return TypeInfo::fromType(parseDeclaredTypeRef(
                        std::get<LexerType::Keyword>(expression[1]).name));
                }
                std::vector<std::string> arguments;
                size_t afterArguments = typePosition;
                if (ZeusParser::parseGenericTypeArguments(
                        expression, typePosition, afterArguments, arguments)) {
                    typePosition = afterArguments;
                }
                TypeRef result = parseDeclaredTypeRef(className);
                const auto parameters = genericClassParameters.find(className);
                if (parameters != genericClassParameters.end() &&
                    parameters->second.size() == arguments.size()) {
                    result.kind = InferredType::Object;
                    result.namedType = className;
                    result.namedTypeArguments = std::move(arguments);
                }
                return TypeInfo::fromType(std::move(result));
            }

            if (expression.size() == 1 &&
                std::holds_alternative<LexerType::Keyword>(expression[0])) {
                const std::string& name =
                    std::get<LexerType::Keyword>(expression[0]).name;
                auto it = environment.find(name);
                if (it != environment.end()) return it->second;

                const auto function = inferer.getFunctions().find(name);
                if (function != inferer.getFunctions().end()) {
                    return makeFunctionValueInfo(function->second);
                }
            }

            if (expression.size() >= 4 && isOperator(expression.back(), "]")) {
                int bracketDepth = 0;
                size_t openBracket = expression.size();
                for (size_t i = expression.size(); i-- > 0;) {
                    if (isOperator(expression[i], "]")) {
                        ++bracketDepth;
                    } else if (isOperator(expression[i], "[") && --bracketDepth == 0) {
                        openBracket = i;
                        break;
                    }
                }
                if (openBracket > 0 && openBracket + 1 < expression.size() - 1) {
                    const std::vector<LexerType::Token> receiverExpression(
                        expression.begin(), expression.begin() + openBracket);
                    const TypeInfo receiver = inferExpressionInfo(
                        receiverExpression, environment);
                    if (receiver.type.kind == InferredType::RawBytesRange) {
                        return TypeInfo::fromType(TypeRef(InferredType::Int));
                    }
                }
            }

            if (expression.size() >= 3 &&
                isOperator(expression[expression.size() - 2], ".") &&
                std::holds_alternative<LexerType::Keyword>(expression.back())) {
                std::vector<LexerType::Token> receiverExpression(
                    expression.begin(), expression.end() - 2);
                TypeInfo objectInfo = inferExpressionInfo(
                    receiverExpression, environment);
                if (objectInfo.type.kind == InferredType::Object &&
                    !objectInfo.type.namedType.empty()) {
                    auto classIt = classFields.find(objectInfo.type.namedType);
                    if (classIt != classFields.end()) {
                        const std::string& fieldName =
                            std::get<LexerType::Keyword>(expression.back()).name;
                        auto fieldIt = classIt->second.find(fieldName);
                        if (fieldIt != classIt->second.end()) {
                            std::string fieldType = fieldIt->second;
                            const auto parameters = genericClassParameters.find(
                                objectInfo.type.namedType);
                            if (parameters != genericClassParameters.end() &&
                                parameters->second.size() ==
                                    objectInfo.type.namedTypeArguments.size()) {
                                std::unordered_map<std::string, std::string> substitutions;
                                for (size_t i = 0; i < parameters->second.size(); ++i) {
                                    substitutions.emplace(
                                        parameters->second[i],
                                        objectInfo.type.namedTypeArguments[i]);
                                }
                                fieldType = substituteGenericTypeName(
                                    fieldType, substitutions);
                            }
                            if (fieldType == "unknown" || fieldType == "any") {
                                return TypeInfo::dynamic();
                            }
                            return TypeInfo::fromType(
                                parseDeclaredTypeRef(fieldType),
                                false,
                                true
                            );
                        }
                    }
                }
            }

            if (expression.size() == 3 &&
                std::holds_alternative<LexerType::Keyword>(expression[0]) &&
                isOperator(expression[1], ".") &&
                std::holds_alternative<LexerType::Keyword>(expression[2])) {
                const std::string& owner =
                    std::get<LexerType::Keyword>(expression[0]).name;
                const std::string& method =
                    std::get<LexerType::Keyword>(expression[2]).name;
                const std::string candidates[] = {
                    owner + "::" + method,
                    owner + "." + method
                };
                for (const auto& candidate : candidates) {
                    const auto function = inferer.getFunctions().find(candidate);
                    if (function != inferer.getFunctions().end() &&
                        function->second.isStaticMethod) {
                        return makeFunctionValueInfo(function->second);
                    }
                }
            }

            auto functionResult = [&](const std::string& functionName,
                                      const std::vector<std::vector<LexerType::Token>>& callArguments,
                                      size_t parameterOffset,
                                      const std::vector<std::string>& explicitTypeArguments,
                                      const std::vector<std::string>* ownerTypeArguments = nullptr)
                                      -> TypeInfo {
                const auto& functions = inferer.getFunctions();
                auto function = functions.find(functionName);
                if (function == functions.end()) return TypeInfo::unresolved();

                if (function->second.typeParameterNames.empty() &&
                    function->second.ownerTypeParameterNames.empty()) {
                    const auto inferred = inferredFunctionReturnInfos.find(
                        function->first);
                    if (inferred != inferredFunctionReturnInfos.end()) {
                        TypeInfo result = inferred->second;
                        result.type.isReference = false;
                        return result;
                    }
                }

                std::unordered_map<std::string, std::string> substitutions;
                if (ownerTypeArguments != nullptr) {
                    const size_t count = std::min(
                        function->second.ownerTypeParameterNames.size(),
                        ownerTypeArguments->size());
                    for (size_t i = 0; i < count; ++i) {
                        substitutions.emplace(
                            function->second.ownerTypeParameterNames[i],
                            (*ownerTypeArguments)[i]);
                    }
                }

                const bool hasExplicitTypeArguments =
                    !explicitTypeArguments.empty() &&
                    explicitTypeArguments.size() ==
                        function->second.typeParameterNames.size();
                if (hasExplicitTypeArguments) {
                    for (size_t i = 0; i < explicitTypeArguments.size(); ++i) {
                        substitutions[function->second.typeParameterNames[i]] =
                            explicitTypeArguments[i];
                    }
                } else if (!function->second.typeParameterNames.empty() &&
                           rootOptions.mode == TypeCheckMode::Static) {
                    size_t positionalIndex = parameterOffset;
                    for (const auto& rawArgument : callArguments) {
                        if (rawArgument.empty()) continue;
                        size_t parameterIndex = positionalIndex;
                        std::vector<LexerType::Token> argument = rawArgument;
                        if (rawArgument.size() >= 3 &&
                            std::holds_alternative<LexerType::Keyword>(rawArgument[0]) &&
                            isOperator(rawArgument[1], "=")) {
                            const std::string& name =
                                std::get<LexerType::Keyword>(rawArgument[0]).name;
                            const auto named = std::find(
                                function->second.parameterNames.begin(),
                                function->second.parameterNames.end(), name);
                            if (named == function->second.parameterNames.end()) continue;
                            parameterIndex = static_cast<size_t>(
                                named - function->second.parameterNames.begin());
                            argument.assign(rawArgument.begin() + 2, rawArgument.end());
                        } else {
                            ++positionalIndex;
                        }
                        if (parameterIndex >= function->second.parameterTypeNames.size()) {
                            continue;
                        }
                        const std::string parameterType = substituteGenericTypeName(
                            function->second.parameterTypeNames[parameterIndex],
                            substitutions);

                        TypeInfo actual = inferExpressionInfo(argument, environment);
                        if (!actual.isKnown()) continue;
                        TypeRef actualType = actual.type;
                        actualType.isReference = false;
                        if (!collectGenericTypeBindings(
                                parameterType, actualType.toIRTypeName(),
                                function->second.typeParameterNames, substitutions)) {
                            return TypeInfo::unresolved();
                        }
                    }
                }

                if (!function->second.returnTypeName.empty()) {
                    const std::string specializedReturn = substituteGenericTypeName(
                        function->second.returnTypeName, substitutions);
                    TypeRef result = parseDeclaredTypeRef(specializedReturn);
                    result.isReference = false;
                    return TypeInfo::fromType(std::move(result), false, true);
                }

                if (function->second.dynamicReturn &&
                    function->second.returnType == InferredType::Unknown) {
                    return TypeInfo::unresolved();
                }

                if (function->second.returnType == InferredType::TypeParameter &&
                    !function->second.typeParameterNames.empty()) {
                    const auto concrete = substitutions.find(
                        function->second.typeParameterNames.front());
                    if (concrete != substitutions.end()) {
                        TypeRef result = parseDeclaredTypeRef(concrete->second);
                        result.isReference = false;
                        return TypeInfo::fromType(std::move(result), false, true);
                    }
                    return TypeInfo::unresolved();
                }

                TypeRef result = function->second.returnTypeName.empty()
                    ? TypeRef(function->second.returnType)
                    : parseDeclaredTypeRef(function->second.returnTypeName);
                result.isReference = false;
                return TypeInfo::fromType(
                    std::move(result),
                    false,
                    !function->second.returnTypeName.empty()
                );
            };

            if (expression.size() >= 3 &&
                std::holds_alternative<LexerType::Keyword>(expression.front())) {
                size_t callOpenParen = 1;
                std::vector<std::string> explicitTypeArguments;
                bool hasExplicitTypeArguments = false;
                for (size_t i = 1; i < expression.size(); ++i) {
                    if (!isOperator(expression[i], "(")) continue;
                    size_t calleeNameIndex = 0;
                    std::vector<std::string> candidateTypeArguments;
                    if (ZeusParser::findGenericCallTypeArguments(
                            expression, i, calleeNameIndex, candidateTypeArguments) &&
                        calleeNameIndex == 0) {
                        callOpenParen = i;
                        explicitTypeArguments = std::move(candidateTypeArguments);
                        hasExplicitTypeArguments = true;
                        break;
                    }
                }
                if (hasExplicitTypeArguments || isOperator(expression[1], "(")) {
                    size_t closeParen = callOpenParen;
                    std::vector<std::vector<LexerType::Token>> callArguments;
                    if (splitCallArguments(expression, callOpenParen, closeParen,
                                           callArguments) &&
                        closeParen + 1 == expression.size()) {
                        const std::string& callableName =
                            std::get<LexerType::Keyword>(expression.front()).name;
                        const auto callable = environment.find(callableName);
                        if (callable != environment.end()) {
                            if (callable->second.type.isFunction() &&
                                callable->second.type.returnType) {
                                const TypeRef& returnType =
                                    *callable->second.type.returnType;
                                return returnType.kind == InferredType::Unknown
                                    ? TypeInfo::unresolved()
                                    : TypeInfo::fromType(returnType);
                            }
                            if (callable->second.isDynamic() ||
                                callable->second.type.kind == InferredType::Unknown) {
                                return TypeInfo::dynamic();
                            }
                        }

                        TypeInfo result = functionResult(
                            callableName,
                            callArguments,
                            0,
                            explicitTypeArguments
                        );
                        if (result.resolved) return result;
                        if (const auto* builtin =
                                ZeusBuiltins::findBuiltinFunctionType(callableName)) {
                            return TypeInfo::fromType(parseDeclaredTypeRef(
                                std::string(builtin->returnType)), false, true);
                        }
                    }
                }
            }

            if (expression.size() >= 5 &&
                isOperator(expression[1], ".") &&
                std::holds_alternative<LexerType::Keyword>(expression[2])) {
                const size_t methodNameIndex = 2;
                size_t callOpenParen = 3;
                std::vector<std::string> explicitTypeArguments;
                bool hasExplicitTypeArguments = false;
                for (size_t i = methodNameIndex + 1; i < expression.size(); ++i) {
                    if (!isOperator(expression[i], "(")) continue;
                    size_t calleeNameIndex = methodNameIndex;
                    std::vector<std::string> candidateTypeArguments;
                    if (ZeusParser::findGenericCallTypeArguments(
                            expression, i, calleeNameIndex, candidateTypeArguments) &&
                        calleeNameIndex == methodNameIndex) {
                        callOpenParen = i;
                        explicitTypeArguments = std::move(candidateTypeArguments);
                        hasExplicitTypeArguments = true;
                        break;
                    }
                }

                if (hasExplicitTypeArguments || isOperator(expression[3], "(")) {
                    size_t closeParen = callOpenParen;
                    std::vector<std::vector<LexerType::Token>> callArguments;
                    if (!splitCallArguments(expression, callOpenParen, closeParen,
                                            callArguments) ||
                        closeParen + 1 != expression.size()) {
                        return TypeInfo::unresolved();
                    }

                    TypeInfo receiver = inferExpressionInfo(
                        std::vector<LexerType::Token>{ expression[0] }, environment);
                    std::string receiverType = receiver.type.namedType;
                    if (receiverType.empty() &&
                        std::holds_alternative<LexerType::Keyword>(expression[0])) {
                        const std::string& identifier =
                            std::get<LexerType::Keyword>(expression[0]).name;
                        if (classNames.count(identifier) != 0) receiverType = identifier;
                    }
                    if (receiverType.empty() && receiver.isKnown()) {
                        receiverType = typeName(receiver.type.kind);
                    }
                    if (const auto* builtin =
                            ZeusBuiltins::findTypeImplementation(receiverType)) {
                        receiverType = builtin->canonicalName;
                    }
                    if (!receiverType.empty()) {
                        const std::string method =
                            std::get<LexerType::Keyword>(expression[methodNameIndex]).name;
                        TypeInfo result = TypeInfo::unresolved();
                        const std::string candidates[] = {
                            receiverType + "::" + method,
                            receiverType + "." + method,
                            method
                        };
                        for (const auto& candidate : candidates) {
                            const auto found = inferer.getFunctions().find(candidate);
                            if (found == inferer.getFunctions().end()) continue;
                            const size_t parameterOffset =
                                found->second.isClassMethod &&
                                !found->second.isStaticMethod ? 1 : 0;
                            result = functionResult(
                                candidate, callArguments, parameterOffset,
                                explicitTypeArguments,
                                &receiver.type.namedTypeArguments);
                            if (result.resolved) break;
                        }
                        if (result.resolved) return result;
                    }
                }
            }

            int depth = 0;
            for (size_t i = 0; i < expression.size(); ++i) {
                if (isOperator(expression[i], "(")) {
                    ++depth;
                    continue;
                }
                if (isOperator(expression[i], ")")) {
                    --depth;
                    continue;
                }
                if (depth != 0 || !std::holds_alternative<LexerType::Operator>(expression[i])) {
                    continue;
                }

                const std::string& symbol = std::get<LexerType::Operator>(expression[i]).symbol;
                if (symbol != "+" && symbol != "-" && symbol != "*" &&
                    symbol != "/" && symbol != "%" && symbol != "==" &&
                    symbol != "!=" && symbol != "<" && symbol != ">" &&
                    symbol != "<=" && symbol != ">=") {
                    continue;
                }

                std::vector<LexerType::Token> left(expression.begin(), expression.begin() + i);
                std::vector<LexerType::Token> right(expression.begin() + i + 1, expression.end());
                TypeInfo leftInfo = inferExpressionInfo(left, environment);
                TypeInfo rightInfo = inferExpressionInfo(right, environment);

                const bool isArithmetic = symbol == "+" || symbol == "-" ||
                    symbol == "*" || symbol == "/" || symbol == "%";
                if (isArithmetic &&
                    leftInfo.type.kind == InferredType::TypeParameter &&
                    rightInfo.type.kind == InferredType::TypeParameter &&
                    !leftInfo.type.namedType.empty() &&
                    leftInfo.type.namedType == rightInfo.type.namedType) {
                    // Keep operations on the same symbolic T typed as T. The
                    // concrete specialization validates whether that operator
                    // is supported for the selected runtime type.
                    return TypeInfo::fromType(TypeRef(
                        InferredType::TypeParameter, leftInfo.type.namedType));
                }

                if (leftInfo.type.kind == InferredType::Object &&
                    rightInfo.type.kind == InferredType::Object &&
                    !leftInfo.type.namedType.empty() &&
                    leftInfo.type.namedType == rightInfo.type.namedType) {
                    if (symbol == "==" || symbol == "!=" || symbol == "<" ||
                        symbol == ">" || symbol == "<=" || symbol == ">=") {
                        return TypeInfo::fromType(TypeRef(InferredType::Bool));
                    }

                    auto operatorType = operatorReturnTypes.find(
                        leftInfo.type.namedType + "::" + symbol
                    );
                    if (operatorType != operatorReturnTypes.end()) {
                        return TypeInfo::fromType(
                            parseDeclaredTypeRef(operatorType->second),
                            false,
                            true
                        );
                    }
                    return leftInfo;
                }

                const bool leftNumeric = leftInfo.type.kind == InferredType::Int ||
                                         leftInfo.type.kind == InferredType::Float;
                const bool rightNumeric = rightInfo.type.kind == InferredType::Int ||
                                          rightInfo.type.kind == InferredType::Float;
                if (leftNumeric && rightNumeric) {
                    if (symbol == "==" || symbol == "!=" || symbol == "<" ||
                        symbol == ">" || symbol == "<=" || symbol == ">=") {
                        return TypeInfo::fromType(TypeRef(InferredType::Bool));
                    }
                    return TypeInfo::fromType(TypeRef(
                        leftInfo.type.kind == InferredType::Float ||
                        rightInfo.type.kind == InferredType::Float
                            ? InferredType::Float
                            : InferredType::Int
                    ));
                }

                if ((symbol == "==" || symbol == "!=" || symbol == "<" ||
                     symbol == ">" || symbol == "<=" || symbol == ">=") &&
                    leftInfo.type.kind == rightInfo.type.kind &&
                    leftInfo.type.kind != InferredType::Unknown) {
                    return TypeInfo::fromType(TypeRef(InferredType::Bool));
                }
            }

            TypeInfo inferred = inferer.inferExpressionInfo(expression, environment);
            if (inferred.type.kind == InferredType::Unknown &&
                expressionUsesDynamicBinding(expression, environment)) {
                return TypeInfo::dynamic();
            }
            return inferred;
        }

        static bool isOperator(const LexerType::Token& token, const std::string& symbol) {
            return std::holds_alternative<LexerType::Operator>(token) &&
                   std::get<LexerType::Operator>(token).symbol == symbol;
        }

        static bool isKeyword(const LexerType::Token& token, const std::string& name) {
            return std::holds_alternative<LexerType::Keyword>(token) &&
                   std::get<LexerType::Keyword>(token).name == name;
        }

        void addDiagnostic(
            int nodeIndex,
            TypeCheckSeverity severity,
            std::string message,
            TypeCheckDiagnosticCode code = TypeCheckDiagnosticCode::General
        ) {
            diagnostics.push_back({
                nodeIndex,
                severity,
                std::move(message),
                currentSourceLine,
                currentSourceColumn,
                code
            });
        }

        static bool canAssign(InferredType expected, InferredType actual) {
            return expected == actual;
        }

        void checkValueType(
            int nodeIndex,
            InferredType expected,
            const TypeInfo& actualInfo,
            const std::string& valueDescription,
            const TypeCheckOptions& options
        ) {
            const InferredType actual = actualInfo.type.kind;

            if (expected == InferredType::Unknown) return;

            if (actual == InferredType::Unknown) {
                if (options.rejectUnknownExpressions) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        valueDescription + "的类型无法在静态模式下确定"
                    );
                }
                return;
            }

            if (expected != InferredType::Unknown && !canAssign(expected, actual)) {
                addDiagnostic(
                    nodeIndex,
                    TypeCheckSeverity::Error,
                    valueDescription + "类型不匹配: 需要 " + typeName(expected) +
                    ", 实际是 " + typeName(actual)
                );
            }
        }

        static bool hasSameDeclaredType(
            const TypeInfo& expected,
            const TypeInfo& actual
        ) {
            if (expected.type.isReference && !actual.type.isReference) return false;
            if (!expected.type.isReference && actual.type.isReference &&
                expected.type.kind != InferredType::Int &&
                expected.type.kind != InferredType::Bool) {
                return false;
            }
            if (expected.type.kind != actual.type.kind) return false;
            if ((expected.type.kind == InferredType::Object ||
                 expected.type.kind == InferredType::TypeParameter) &&
                !expected.type.namedType.empty()) {
                return expected.type.namedType == actual.type.namedType &&
                    expected.type.namedTypeArguments == actual.type.namedTypeArguments;
            }
            return true;
        }

        static bool splitCallArguments(
            const std::vector<LexerType::Token>& tokens,
            size_t openParen,
            size_t& closeParen,
            std::vector<std::vector<LexerType::Token>>& arguments
        ) {
            arguments.clear();
            if (openParen >= tokens.size() || !isOperator(tokens[openParen], "(")) {
                return false;
            }

            int parenDepth = 1;
            int bracketDepth = 0;
            int braceDepth = 0;
            size_t segmentBegin = openParen + 1;
            for (size_t i = openParen + 1; i < tokens.size(); ++i) {
                if (isOperator(tokens[i], "(")) {
                    ++parenDepth;
                } else if (isOperator(tokens[i], ")")) {
                    --parenDepth;
                    if (parenDepth == 0) {
                        closeParen = i;
                        if (segmentBegin < i) {
                            arguments.emplace_back(tokens.begin() + segmentBegin,
                                                   tokens.begin() + i);
                        }
                        return true;
                    }
                } else if (isOperator(tokens[i], "[")) {
                    ++bracketDepth;
                } else if (isOperator(tokens[i], "]")) {
                    --bracketDepth;
                } else if (isOperator(tokens[i], "{")) {
                    ++braceDepth;
                } else if (isOperator(tokens[i], "}")) {
                    --braceDepth;
                } else if (isOperator(tokens[i], ",") && parenDepth == 1 &&
                           bracketDepth == 0 && braceDepth == 0) {
                    arguments.emplace_back(tokens.begin() + segmentBegin,
                                           tokens.begin() + i);
                    segmentBegin = i + 1;
                }
            }
            return false;
        }

        void checkCallArgumentTypes(
            const std::vector<LexerType::Token>& tokens,
            const Environment& environment,
            int nodeIndex
        ) {
            const auto& functions = inferer.getFunctions();
            for (size_t openParen = 0; openParen < tokens.size(); ++openParen) {
                if (!isOperator(tokens[openParen], "(") || openParen == 0) continue;

                const FunctionInfo* function = nullptr;
                FunctionInfo indirectFunction;
                size_t parameterOffset = 0;
                std::string callName;
                std::vector<std::string> ownerTypeArguments;
                std::vector<std::string> explicitTypeArguments;
                size_t callableNameIndex = openParen - 1;
                const bool hasExplicitTypeArguments =
                    ZeusParser::findGenericCallTypeArguments(
                        tokens, openParen, callableNameIndex, explicitTypeArguments);

                if (callableNameIndex >= 2 &&
                    std::holds_alternative<LexerType::Keyword>(tokens[callableNameIndex]) &&
                    isOperator(tokens[callableNameIndex - 1], ".") &&
                    std::holds_alternative<LexerType::Keyword>(tokens[callableNameIndex - 2])) {
                    const std::string methodName =
                        std::get<LexerType::Keyword>(tokens[callableNameIndex]).name;
                    const std::vector<LexerType::Token> receiverTokens{
                        tokens[callableNameIndex - 2]};
                    const TypeInfo receiver = inferExpressionInfo(receiverTokens, environment);
                    ownerTypeArguments = receiver.type.namedTypeArguments;
                    std::string receiverType = receiver.type.namedType;
                    if (receiverType.empty() && receiverTokens.size() == 1 &&
                        std::holds_alternative<LexerType::Keyword>(receiverTokens.front())) {
                        const std::string& identifier =
                            std::get<LexerType::Keyword>(receiverTokens.front()).name;
                        if (classNames.count(identifier) != 0) receiverType = identifier;
                    }
                    if (receiverType.empty() && receiver.isKnown()) {
                        receiverType = typeName(receiver.type.kind);
                    }
                    if (const auto* builtin =
                            ZeusBuiltins::findTypeImplementation(receiverType)) {
                        receiverType = builtin->canonicalName;
                    }

                    if (!receiverType.empty()) {
                        auto method = functions.find(receiverType + "::" + methodName);
                        if (method == functions.end()) {
                            method = functions.find(receiverType + "." + methodName);
                        }
                        if (method != functions.end()) {
                            function = &method->second;
                            callName = method->second.name;
                            if (method->second.isClassMethod &&
                                !method->second.isStaticMethod) {
                                parameterOffset = 1;
                            }
                        }
                    }
                } else if (callableNameIndex < tokens.size() &&
                           std::holds_alternative<LexerType::Keyword>(tokens[callableNameIndex])) {
                    const std::string& name =
                        std::get<LexerType::Keyword>(tokens[callableNameIndex]).name;
                    const auto local = environment.find(name);
                    if (local != environment.end()) {
                        if (local->second.type.isFunction()) {
                            indirectFunction.name = name;
                            for (const auto& parameterType :
                                 local->second.type.parameterTypes) {
                                indirectFunction.parameterNames.emplace_back();
                                indirectFunction.parameterTypeNames.push_back(
                                    parameterType.toIRTypeName());
                            }
                            function = &indirectFunction;
                            callName = name;
                        }
                    } else {
                        const auto found = functions.find(name);
                        if (found != functions.end()) {
                            function = &found->second;
                            callName = found->second.name;
                        }
                    }
                }

                if (!function) continue;

                if (hasExplicitTypeArguments && function->typeParameterNames.empty()) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        callName + "(): explicit type arguments require a generic function",
                        TypeCheckDiagnosticCode::GenericTypeArgumentMismatch
                    );
                    continue;
                }
                if (function->parameterTypeNames.empty() &&
                    function->typeParameterNames.empty()) {
                    continue;
                }

                size_t closeParen = openParen;
                std::vector<std::vector<LexerType::Token>> arguments;
                if (!splitCallArguments(tokens, openParen, closeParen, arguments)) continue;

                std::unordered_map<std::string, TypeRef> explicitGenericTypes;
                std::unordered_map<std::string, std::string> explicitGenericTypeNames;
                bool explicitTypeArityValid = true;
                if (hasExplicitTypeArguments) {
                    explicitTypeArityValid =
                        explicitTypeArguments.size() == function->typeParameterNames.size();
                    if (!explicitTypeArityValid) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            callName + "(): expected " +
                                std::to_string(function->typeParameterNames.size()) +
                                " explicit type argument(s), got " +
                                std::to_string(explicitTypeArguments.size()),
                            TypeCheckDiagnosticCode::GenericTypeArgumentMismatch
                        );
                    } else {
                        for (size_t i = 0; i < explicitTypeArguments.size(); ++i) {
                            TypeRef type = parseDeclaredTypeRef(explicitTypeArguments[i]);
                            if (!type.isKnown()) {
                                addDiagnostic(
                                    nodeIndex,
                                    TypeCheckSeverity::Error,
                                    callName + "(): unknown type argument '" +
                                        explicitTypeArguments[i] + "'",
                                    TypeCheckDiagnosticCode::GenericTypeArgumentUnknown
                                );
                                explicitTypeArityValid = false;
                                continue;
                            }
                            type.isReference = false;
                            explicitGenericTypeNames.emplace(
                                function->typeParameterNames[i], type.toIRTypeName());
                            explicitGenericTypes.emplace(
                                function->typeParameterNames[i], std::move(type));
                        }
                    }
                }

                std::unordered_map<std::string, std::string> ownerTypeBindings;
                const size_t ownerTypeCount = std::min(
                    function->ownerTypeParameterNames.size(), ownerTypeArguments.size());
                for (size_t i = 0; i < ownerTypeCount; ++i) {
                    ownerTypeBindings.emplace(
                        function->ownerTypeParameterNames[i], ownerTypeArguments[i]);
                }

                size_t positionalIndex = parameterOffset;
                std::unordered_map<std::string, TypeRef> inferredGenericTypes;
                std::unordered_map<std::string, std::string> inferredGenericTypeNames;
                std::unordered_set<std::string> seenGenericArguments;
                for (const auto& rawArgument : arguments) {
                    if (rawArgument.empty()) continue;

                    size_t parameterIndex = positionalIndex;
                    std::vector<LexerType::Token> expression = rawArgument;
                    if (rawArgument.size() >= 3 &&
                        std::holds_alternative<LexerType::Keyword>(rawArgument[0]) &&
                        isOperator(rawArgument[1], "=")) {
                        const std::string& name =
                            std::get<LexerType::Keyword>(rawArgument[0]).name;
                        const auto named = std::find(
                            function->parameterNames.begin(),
                            function->parameterNames.end(), name);
                        if (named == function->parameterNames.end()) continue;
                        parameterIndex = static_cast<size_t>(named - function->parameterNames.begin());
                        expression.assign(rawArgument.begin() + 2, rawArgument.end());
                    } else {
                        ++positionalIndex;
                    }

                    if (parameterIndex >= function->parameterTypeNames.size()) continue;
                    std::string expectedName = substituteGenericTypeName(
                        function->parameterTypeNames[parameterIndex], ownerTypeBindings);
                    if (expectedName.empty()) continue;
                    std::unordered_set<std::string> argumentTypeParameters;
                    collectGenericTypeParameters(
                        expectedName, function->typeParameterNames,
                        argumentTypeParameters);
                    if (!argumentTypeParameters.empty()) {
                        seenGenericArguments.insert(
                            argumentTypeParameters.begin(),
                            argumentTypeParameters.end());
                        const TypeInfo actual = inferExpressionInfo(expression, environment);
                        if (hasExplicitTypeArguments) {
                            if (!explicitTypeArityValid) continue;
                            if (!actual.isKnown()) {
                                addDiagnostic(
                                    nodeIndex,
                                    TypeCheckSeverity::Error,
                                    callName + "(): cannot verify argument type for '" +
                                        *argumentTypeParameters.begin() + "'",
                                    TypeCheckDiagnosticCode::GenericTypeArgumentUnknown
                                );
                                continue;
                            }

                            const std::string specializedExpectedName =
                                substituteGenericTypeName(
                                    expectedName, explicitGenericTypeNames);
                            const TypeInfo expectedType = TypeInfo::fromType(
                                parseDeclaredTypeRef(specializedExpectedName));
                            TypeInfo actualType = actual;
                            actualType.type.isReference = false;
                            if (!hasSameDeclaredType(expectedType, actualType)) {
                                const size_t displayIndex = parameterIndex - parameterOffset + 1;
                                addDiagnostic(
                                    nodeIndex,
                                    TypeCheckSeverity::Error,
                                    callName + "(): argument " +
                                        std::to_string(displayIndex) + " must " +
                                        expectedType.toString() + ", not " +
                                        actualType.toString(),
                                    TypeCheckDiagnosticCode::GenericTypeArgumentMismatch
                                );
                            }
                            continue;
                        }

                        if (rootOptions.mode == TypeCheckMode::Static) {
                            const size_t displayIndex = parameterIndex - parameterOffset + 1;
                            if (!actual.isKnown()) {
                                addDiagnostic(
                                    nodeIndex,
                                    TypeCheckSeverity::Error,
                                    callName + "(): generic type parameter '" +
                                        *argumentTypeParameters.begin() +
                                        "' cannot be inferred from argument " +
                                        std::to_string(displayIndex),
                                    TypeCheckDiagnosticCode::GenericTypeArgumentUnknown
                                );
                                continue;
                            }

                            TypeRef actualType = actual.type;
                            actualType.isReference = false;
                            std::unordered_map<std::string, std::string> argumentBindings;
                            if (!collectGenericTypeBindings(
                                    expectedName, actualType.toIRTypeName(),
                                    function->typeParameterNames, argumentBindings)) {
                                addDiagnostic(
                                    nodeIndex,
                                    TypeCheckSeverity::Error,
                                    callName + "(): argument " +
                                        std::to_string(displayIndex) +
                                        " does not match its generic type pattern",
                                    TypeCheckDiagnosticCode::GenericTypeArgumentMismatch
                                );
                                continue;
                            }
                            for (const auto& binding : argumentBindings) {
                                const auto previous = inferredGenericTypeNames.find(
                                    binding.first);
                                if (previous != inferredGenericTypeNames.end() &&
                                    previous->second != binding.second) {
                                    addDiagnostic(
                                        nodeIndex,
                                        TypeCheckSeverity::Error,
                                        callName + "(): arguments for generic type parameter '" +
                                            binding.first + "' must have the same type",
                                        TypeCheckDiagnosticCode::GenericTypeArgumentMismatch
                                    );
                                    continue;
                                }
                                inferredGenericTypeNames[binding.first] = binding.second;
                                inferredGenericTypes[binding.first] =
                                    parseDeclaredTypeRef(binding.second);
                            }

                            const std::string specializedExpectedName =
                                substituteGenericTypeName(
                                    expectedName, inferredGenericTypeNames);
                            const TypeInfo expectedType = TypeInfo::fromType(
                                parseDeclaredTypeRef(specializedExpectedName));
                            TypeInfo actualInfo = actual;
                            actualInfo.type.isReference = false;
                            if (expectedType.isKnown() &&
                                !hasSameDeclaredType(expectedType, actualInfo)) {
                                addDiagnostic(
                                    nodeIndex,
                                    TypeCheckSeverity::Error,
                                    callName + "(): argument " +
                                        std::to_string(displayIndex) + " must " +
                                        expectedType.toString() + ", not " +
                                        actualInfo.toString(),
                                    TypeCheckDiagnosticCode::GenericTypeArgumentMismatch
                                );
                            }
                        }
                        continue;
                    }

                    const TypeInfo expected = TypeInfo::fromType(
                        parseDeclaredTypeRef(expectedName), false, true);
                    const TypeInfo actual = inferExpressionInfo(expression, environment);
                    if (!actual.isKnown() || !expected.isKnown() ||
                        hasSameDeclaredType(expected, actual)) {
                        continue;
                    }

                    const size_t displayIndex = parameterIndex - parameterOffset + 1;
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        callName + "(): argument " + std::to_string(displayIndex) +
                            " must " + expected.toString() + ", not " + actual.toString(),
                        TypeCheckDiagnosticCode::CallArgumentTypeMismatch
                    );
                }

                for (const auto& typeParameter : function->typeParameterNames) {
                    if (hasExplicitTypeArguments) {
                        continue;
                    }
                    if (explicitGenericTypes.find(typeParameter) !=
                        explicitGenericTypes.end()) {
                        continue;
                    }
                    if (seenGenericArguments.find(typeParameter) !=
                        seenGenericArguments.end()) {
                        continue;
                    }
                    if (inferredGenericTypes.find(typeParameter) !=
                        inferredGenericTypes.end()) {
                        continue;
                    }

                    const bool appearsInFormalArguments = std::any_of(
                        function->parameterTypeNames.begin(),
                        function->parameterTypeNames.end(),
                        [&](const std::string& formalType) {
                            return containsGenericTypeParameter(
                                formalType, function->typeParameterNames) &&
                                containsGenericTypeParameter(
                                    formalType, std::vector<std::string>{typeParameter});
                        });
                    if (rootOptions.mode == TypeCheckMode::Static ||
                        !appearsInFormalArguments) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            callName + "(): generic type parameter '" + typeParameter +
                                "' cannot be inferred from the supplied arguments; "
                                "provide it explicitly",
                            TypeCheckDiagnosticCode::GenericTypeArgumentUnknown
                        );
                    }
                }
            }
        }

        static bool splitVariableDeclaration(
            const std::vector<LexerType::Token>& tokens,
            std::string& name,
            bool& hasDeclaredType,
            std::string& declaredTypeName,
            std::vector<LexerType::Token>& expression
        ) {
            name.clear();
            hasDeclaredType = false;
            declaredTypeName.clear();
            expression.clear();

            size_t equal = tokens.size();
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (isOperator(tokens[i], "=")) {
                    equal = i;
                    break;
                }
            }

            size_t nameIndex = tokens.size();
            for (size_t i = 0; i < equal; ++i) {
                if (!std::holds_alternative<LexerType::Keyword>(tokens[i])) continue;

                const auto& keyword = std::get<LexerType::Keyword>(tokens[i]).name;
                if (keyword != "let" && keyword != "var" && keyword != "const" &&
                    keyword != "shared") {
                    nameIndex = i;
                    name = keyword;
                    break;
                }
            }

            if (nameIndex == tokens.size()) return false;

            if (nameIndex + 1 < equal && isOperator(tokens[nameIndex + 1], ":")) {
                hasDeclaredType = true;
                size_t typePosition = nameIndex + 2;
                ZeusParser::parseGenericType(tokens, typePosition, declaredTypeName);
            }

            if (equal < tokens.size()) {
                expression.assign(tokens.begin() + equal + 1, tokens.end());
            }

            return true;
        }

        static bool splitAssignment(
            const std::vector<LexerType::Token>& tokens,
            std::string& name,
            std::vector<LexerType::Token>& expression
        ) {
            name.clear();
            expression.clear();

            size_t equal = tokens.size();
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (isOperator(tokens[i], "=")) {
                    equal = i;
                    break;
                }
            }

            if (equal == tokens.size()) return false;

            for (size_t i = 0; i < equal; ++i) {
                if (std::holds_alternative<LexerType::Keyword>(tokens[i])) {
                    name = std::get<LexerType::Keyword>(tokens[i]).name;
                    break;
                }
            }

            if (name.empty()) return false;

            expression.assign(tokens.begin() + equal + 1, tokens.end());
            return true;
        }

        static bool isSubscriptAssignmentTarget(
            const std::vector<LexerType::Token>& tokens,
            size_t end
        ) {
            if (end == 0 || !isOperator(tokens[end - 1], "]")) return false;
            for (size_t i = 0; i < end; ++i) {
                if (isOperator(tokens[i], "[")) return true;
            }
            return false;
        }

        static std::vector<LexerType::Token> returnExpression(
            const TreeParser::Node& statement
        ) {
            if (statement.tokens.size() <= 1) return {};

            return std::vector<LexerType::Token>(
                statement.tokens.begin() + 1,
                statement.tokens.end()
            );
        }

        void checkVariableDeclaration(
            const TreeParser::Node& statement,
            Environment& environment,
            int nodeIndex,
            const TypeCheckOptions& options,
            FunctionContext* currentFunction = nullptr
        ) {
            std::string name;
            bool hasDeclaredType = false;
            InferredType declaredType = InferredType::Unknown;
            std::string declaredTypeName;
            std::vector<LexerType::Token> expression;

            if (!splitVariableDeclaration(
                    statement.tokens,
                    name,
                    hasDeclaredType,
                    declaredTypeName,
                    expression)) {
                return;
            }

            if (hasDeclaredType) declaredType = parseDeclaredType(declaredTypeName);

            if (hasDeclaredType &&
                parseDeclaredTypeRef(declaredTypeName).isReference) {
                addDiagnostic(
                    nodeIndex,
                    TypeCheckSeverity::Error,
                    "ordinary variable declarations cannot use &T"
                );
            }

            if (hasDeclaredType && declaredType == InferredType::Unknown) {
                addDiagnostic(
                    nodeIndex,
                    TypeCheckSeverity::Error,
                    "变量 " + name + " 使用了无法识别的类型注解"
                );
            }

            TypeInfo actualInfo = expression.empty()
                ? TypeInfo::unresolved()
                : inferExpressionInfo(expression, environment);
            if (rootOptions.mode == TypeCheckMode::Static && expression.size() == 1 &&
                std::holds_alternative<LexerType::Keyword>(expression.front())) {
                const auto& functions = inferer.getFunctions();
                const auto referenced = functions.find(
                    std::get<LexerType::Keyword>(expression.front()).name);
                if (referenced != functions.end() &&
                    !referenced->second.typeParameterNames.empty()) {
                    addDiagnostic(nodeIndex, TypeCheckSeverity::Error,
                        "@type 泛型静态函数目前只支持直接调用，不能绑定为普通函数值",
                        TypeCheckDiagnosticCode::GenericTypeParameterUnsupportedContext);
                }
            }
            InferredType actualType = actualInfo.type.kind;
            const std::string bindingTypeName = hasDeclaredType
                ? parseDeclaredTypeRef(declaredTypeName).toIRTypeName()
                : flowTypeName(actualInfo);
            if (statement.id != TreeParser::InvalidNodeId &&
                !bindingTypeName.empty()) {
                setInferredBindingType(statement.id, bindingTypeName);
            }

            if (hasDeclaredType) {
                checkValueType(
                    nodeIndex,
                    declaredType,
                    actualInfo,
                    "变量 " + name,
                    options
                );
                environment[name] = TypeInfo::fromType(
                    hasDeclaredType
                        ? parseDeclaredTypeRef(declaredTypeName)
                        : TypeRef(declaredType),
                    false,
                    true
                );
                environment[name].bindingNodeId = statement.id;
            } else {
                if (options.requireExplicitVariableTypes) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "变量 " + name + " 必须显式声明类型"
                    );
                }

                if (actualType == InferredType::Unknown &&
                    !actualInfo.isDynamic() && options.rejectUnknownExpressions) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "变量 " + name + " 没有类型注解，且初始化表达式无法推断类型"
                    );
                }
                if (actualType == InferredType::Unknown &&
                    !actualInfo.isDynamic() && currentFunction == nullptr) {
                    actualInfo = TypeInfo::dynamic();
                }
                environment[name] = actualInfo;
                environment[name].type.isReference = false;
                environment[name].explicitAnnotation = false;
                environment[name].bindingNodeId = statement.id;
            }
        }

        void checkAssignment(
            const TreeParser::Node& statement,
            Environment& environment,
            int nodeIndex,
            const TypeCheckOptions& options,
            FunctionContext* currentFunction = nullptr
        ) {
            std::string name;
            std::vector<LexerType::Token> expression;
            if (!splitAssignment(statement.tokens, name, expression)) return;

            TypeInfo actualInfo = inferExpressionInfo(expression, environment);

            size_t equalIndex = statement.tokens.size();
            for (size_t i = 0; i < statement.tokens.size(); ++i) {
                if (isOperator(statement.tokens[i], "=")) {
                    equalIndex = i;
                    break;
                }
            }
            if (equalIndex > 0 && isOperator(statement.tokens[0], "@") &&
                statement.tokens.size() > 1 && isKeyword(statement.tokens[1], "faddr")) {
                const std::vector<LexerType::Token> target(
                    statement.tokens.begin(), statement.tokens.begin() + equalIndex);
                const TypeInfo targetInfo = inferExpressionInfo(target, environment);
                if (targetInfo.type.kind == InferredType::RawBytesRange &&
                    (actualInfo.type.kind == InferredType::Bytes ||
                     actualInfo.type.kind == InferredType::RawBytesRange)) {
                    return;
                }
                checkValueType(nodeIndex, targetInfo.type.kind, actualInfo,
                               "@faddr 写入值", options);
                return;
            }
            if (isSubscriptAssignmentTarget(statement.tokens, equalIndex)) {
                // Container elements are dynamic ZeusValue slots. Assigning
                // one must not be type-checked as rebinding the container
                // variable itself (for example list -> int on arr[0] = 1).
                return;
            }
            bool hasSubscriptedAttributeReceiver = false;
            if (equalIndex >= 3 &&
                isOperator(statement.tokens[equalIndex - 2], ".")) {
                for (size_t i = 0; i + 2 < equalIndex; ++i) {
                    if (isOperator(statement.tokens[i], "[")) {
                        hasSubscriptedAttributeReceiver = true;
                        break;
                    }
                }
            }
            if (equalIndex >= 3 &&
                (isOperator(statement.tokens[1], ".") ||
                 hasSubscriptedAttributeReceiver)) {
                const std::vector<LexerType::Token> target(
                    statement.tokens.begin(), statement.tokens.begin() + equalIndex
                );
                TypeInfo targetInfo = inferExpressionInfo(target, environment);
                std::string fieldOwner;
                std::string fieldName;
                if (equalIndex >= 3 &&
                    isOperator(statement.tokens[equalIndex - 2], ".") &&
                    std::holds_alternative<LexerType::Keyword>(
                        statement.tokens[equalIndex - 1])) {
                    const std::vector<LexerType::Token> receiverTokens(
                        statement.tokens.begin(), statement.tokens.begin() + equalIndex - 2);
                    const TypeInfo receiverInfo = inferExpressionInfo(
                        receiverTokens, environment);
                    fieldOwner = receiverInfo.type.namedType;
                    fieldName = std::get<LexerType::Keyword>(
                        statement.tokens[equalIndex - 1]).name;
                }

                if (!fieldOwner.empty() && !fieldName.empty()) {
                    const auto ownerFields = classFields.find(fieldOwner);
                    const bool fieldExists = ownerFields != classFields.end() &&
                        ownerFields->second.find(fieldName) != ownerFields->second.end();
                    const auto annotated = explicitlyAnnotatedClassFields.find(fieldOwner);
                    const bool isExplicitlyAnnotated =
                        annotated != explicitlyAnnotatedClassFields.end() &&
                        annotated->second.count(fieldName) != 0;
                    if (fieldExists && !isExplicitlyAnnotated &&
                        targetInfo.type.kind == InferredType::Object &&
                        actualInfo.type.kind == InferredType::Nil) {
                        return;
                    }
                    if (fieldExists && !isExplicitlyAnnotated &&
                        !targetInfo.isDynamic()) {
                        const bool sameType = targetInfo.isKnown() &&
                            actualInfo.isKnown() && targetInfo.type == actualInfo.type;
                        if (!sameType) {
                            promoteClassFieldToDynamic(fieldOwner, fieldName);
                            targetInfo = TypeInfo::dynamic();
                        }
                    }
                }

                if (targetInfo.type.kind == InferredType::Unknown) {
                    if (!targetInfo.isDynamic() && options.rejectUnknownExpressions) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            "类字段赋值目标类型无法确定"
                        );
                    }
                    return;
                }

                checkValueType(
                    nodeIndex,
                    targetInfo.type.kind,
                    actualInfo,
                    "类字段赋值",
                    options
                );
                return;
            }

            auto variable = environment.find(name);
            if (variable == environment.end()) {
                if (options.rejectUnknownExpressions) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "变量 " + name + " 尚未声明"
                    );
                } else {
                    environment[name] = actualInfo;
                }
                return;
            }

            // An inferred class local is nullable. The VM stores nil as an
            // ordinary runtime value, while LLVM represents a known class
            // local as a nullable pointer. Preserve the inferred class type so
            // later method calls can still use the typed path.
            if (!variable->second.explicitAnnotation &&
                variable->second.type.kind == InferredType::Object &&
                actualInfo.type.kind == InferredType::Nil) {
                return;
            }

            if (!variable->second.explicitAnnotation) {
                TypeInfo nextInfo = actualInfo;
                const bool actualIsUnresolved = !actualInfo.isResolved();
                if (actualIsUnresolved && options.rejectUnknownExpressions) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "assignment value type is not inferable"
                    );
                }

                const bool previouslyObserved =
                    variable->second.bindingNodeId != TreeParser::InvalidNodeId &&
                    observedBindingNodes.count(variable->second.bindingNodeId) != 0;
                const bool initialNilCanRefine =
                    variable->second.type.kind == InferredType::Nil &&
                    actualInfo.isKnown() &&
                    actualInfo.type.kind != InferredType::Nil &&
                    !previouslyObserved;

                if (variable->second.isDynamic() || actualInfo.isDynamic()) {
                    nextInfo = TypeInfo::dynamic();
                } else if (actualIsUnresolved) {
                    nextInfo = currentFunction == nullptr
                        ? TypeInfo::dynamic()
                        : TypeInfo::unresolved();
                } else if (initialNilCanRefine ||
                           (!variable->second.isResolved() &&
                            actualInfo.isKnown() && !previouslyObserved)) {
                    nextInfo = actualInfo;
                } else if (!sameFlowType(variable->second, actualInfo)) {
                    nextInfo = TypeInfo::dynamic();
                }

                nextInfo.type.isReference = false;
                nextInfo.explicitAnnotation = false;
                nextInfo.bindingNodeId = variable->second.bindingNodeId;
                variable->second = std::move(nextInfo);
                setInferredBindingType(
                    variable->second.bindingNodeId,
                    flowTypeName(variable->second));
                return;
            }

            checkValueType(
                nodeIndex,
                variable->second.type.kind,
                actualInfo,
                "变量 " + name,
                options
            );
        }

        static bool allPathsReturn(
            const std::vector<TreeParser::NodeAndBlock>& blockNodes
        ) {
            auto sequenceReturns = [](
                auto&& self,
                auto&& blockReturns,
                const std::vector<TreeParser::NodeAndBlock>& statements,
                bool skipElseBranches
            ) -> bool {
                for (const auto& element : statements) {
                    if (std::holds_alternative<TreeParser::Node>(element)) {
                        if (std::get<TreeParser::Node>(element).type == NodeType::RETURN_STMT) {
                            return true;
                        }
                        continue;
                    }

                    const auto& nested = std::get<std::unique_ptr<TreeParser::CodeBlock>>(element);
                    if (!nested || nested->type == CodeBlockType::FUNC ||
                        nested->type == CodeBlockType::CLASS ||
                        (skipElseBranches && nested->isElseBranch)) {
                        continue;
                    }
                    if (nested->type == CodeBlockType::IF &&
                        blockReturns(blockReturns, self, *nested)) {
                        return true;
                    }
                }
                return false;
            };

            auto blockReturns = [](
                auto&& self,
                auto&& sequenceReturns,
                const TreeParser::CodeBlock& block
            ) -> bool {
                if (block.type != CodeBlockType::IF) {
                    return sequenceReturns(sequenceReturns, self, block.nodes, false);
                }

                const TreeParser::CodeBlock* elseBranch = nullptr;
                for (const auto& element : block.nodes) {
                    if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(element)) {
                        continue;
                    }
                    const auto& nested = std::get<std::unique_ptr<TreeParser::CodeBlock>>(element);
                    if (!nested || !nested->isElseBranch) continue;
                    if (elseBranch != nullptr) return false;
                    elseBranch = nested.get();
                }
                if (elseBranch == nullptr ||
                    !sequenceReturns(sequenceReturns, self, block.nodes, true)) {
                    return false;
                }
                return self(self, sequenceReturns, *elseBranch);
            };

            return sequenceReturns(sequenceReturns, blockReturns, blockNodes, false);
        }

        void checkReturn(
            const TreeParser::Node& statement,
            Environment& environment,
            FunctionContext* function,
            int nodeIndex,
            const TypeCheckOptions& options
        ) {
            std::vector<LexerType::Token> expression = returnExpression(statement);
            TypeInfo actualInfo = expression.empty()
                ? TypeInfo::fromType(TypeRef(InferredType::Nil))
                : inferExpressionInfo(expression, environment);
            InferredType actualType = actualInfo.type.kind;

            if (function == nullptr) {
                if (options.rejectUnknownExpressions && actualType == InferredType::Unknown) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "顶层 return 表达式无法推断类型"
                    );
                }
                return;
            }

            function->hasReturn = true;

            if (function->expectedReturn != InferredType::Unknown) {
                if (function->expectedReturnIsReference &&
                    expression.empty()) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "函数 " + function->name + " 返回 &T 时必须提供引用值"
                    );
                    return;
                }
                if (function->expectedReturnIsReference) {
                    const bool constructsObject = !expression.empty() &&
                        isKeyword(expression.front(), "new");
                    const std::string expectedType =
                        !function->expectedReturnTypeName.empty() &&
                            function->expectedReturnTypeName.front() == '&'
                        ? function->expectedReturnTypeName.substr(1)
                        : function->expectedReturnTypeName;
                    const size_t genericBegin = expectedType.find('<');
                    const std::string expectedClass = genericBegin == std::string::npos
                        ? expectedType : expectedType.substr(0, genericBegin);
                    std::string actualClass;
                    if (constructsObject && expression.size() > 1 &&
                        std::holds_alternative<LexerType::Keyword>(expression[1])) {
                        actualClass = std::get<LexerType::Keyword>(expression[1]).name;
                    } else {
                        actualClass = actualInfo.type.namedType;
                    }
                    if (!expectedClass.empty() && !actualClass.empty() &&
                        expectedClass != actualClass) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            "function " + function->name +
                                " returns a different class from " +
                                function->expectedReturnTypeName
                        );
                        return;
                    }
                }
                checkValueType(
                    nodeIndex,
                    function->expectedReturn,
                    actualInfo,
                    "函数 " + function->name + " 的返回值",
                    options
                );
                return;
            }

            if (actualType == InferredType::Unknown) {
                if (actualInfo.isDynamic()) {
                    function->inferredReturnIsDynamic = true;
                } else {
                    function->sawUnknownReturnType = true;
                }
                if (options.rejectUnknownExpressions) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "函数 " + function->name + " 的返回值无法推断类型"
                    );
                }
                return;
            }

            if (!function->hasInferredReturnType) {
                function->inferredReturnType = actualInfo.type;
                function->hasInferredReturnType = true;
                return;
            }

            if (function->inferredReturnType != actualInfo.type) {
                // Unannotated functions can return heterogeneous values through
                // the dynamic ZeusValue ABI instead of making this a type error.
                function->inferredReturnIsDynamic = true;
            }
        }

        void checkFunction(
            const TreeParser::CodeBlock& block,
            Environment environment,
            int& nodeIndex
        ) {
            const int previousSourceLine = currentSourceLine;
            const int previousSourceColumn = currentSourceColumn;
            currentSourceLine = block.sourceLine;
            currentSourceColumn = block.sourceColumn;
            FunctionContext function;
            function.name = block.funcName;
            const std::string receiverAlias = instanceReceiverAlias(block);
            TypeCheckOptions functionOptions = rootOptions;
            const bool hasGenericTypeParameters = !block.typeParameters.empty() ||
                !block.classTypeParameters.empty();
            const bool isStaticCompile = block.isCompileTimeStatic ||
                hasGenericTypeParameters;
            const auto previousTypeParameters = activeTypeParameters;
            activeTypeParameters.insert(
                block.typeParameters.begin(), block.typeParameters.end());
            activeTypeParameters.insert(
                block.classTypeParameters.begin(), block.classTypeParameters.end());

            std::unordered_set<std::string> declaredTypeParameters;
            for (const auto& typeParameter : block.classTypeParameters) {
                declaredTypeParameters.insert(typeParameter);
                if (parseTypeName(typeParameter) != InferredType::Unknown ||
                    ZeusBuiltins::findTypeImplementation(typeParameter) != nullptr ||
                    classNames.count(typeParameter) != 0) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "@type 类参数名 '" + typeParameter +
                            "' 不能与内置类型或 class 名称相同",
                        TypeCheckDiagnosticCode::GenericTypeParameterNameInvalid
                    );
                }
            }
            for (const auto& typeParameter : block.typeParameters) {
                if (!declaredTypeParameters.insert(typeParameter).second) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "@type 参数 '" + typeParameter +
                            "' 与所属 class 的类型参数重名",
                        TypeCheckDiagnosticCode::GenericTypeParameterNameInvalid
                    );
                }
                if (parseTypeName(typeParameter) != InferredType::Unknown ||
                    ZeusBuiltins::findTypeImplementation(typeParameter) != nullptr ||
                    classNames.count(typeParameter) != 0) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "@type 参数名 '" + typeParameter +
                            "' 不能与内置类型或 class 名称相同",
                        TypeCheckDiagnosticCode::GenericTypeParameterNameInvalid
                    );
                }
            }

            for (const auto& decorator : block.decorators) {
                if (decorator != "static" && decorator != "no_safepoint" &&
                    decorator != "type") {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "函数 " + block.funcName + " 使用了不支持的修饰符 @" + decorator
                    );
                }
            }
            if (isStaticCompile &&
                (!block.captureVars.empty() || !block.copyVars.empty() ||
                 block.isCaptureAll || block.isCopyAll)) {
                addDiagnostic(
                    nodeIndex,
                    TypeCheckSeverity::Error,
                    (block.typeParameters.empty()
                        ? "@static 函数 "
                        : "@type 泛型静态函数 ") +
                        block.funcName + " 不能捕获外部变量"
                );
            }
            if (isStaticCompile && rootOptions.mode == TypeCheckMode::Static) {
                functionOptions = TypeCheckOptions::strictLLVM();
                if (hasGenericTypeParameters) {
                    // Operations involving T are checked after each concrete
                    // specialization; keep the remaining strict LLVM rules.
                    functionOptions.rejectUnknownExpressions = false;
                }
            }

            if (functionOptions.requireExplicitFunctionParameterTypes) {
                for (size_t i = 0; i < block.paramNames.size(); ++i) {
                bool hasType = i < block.paramTypeNames.size() &&
                               !block.paramTypeNames[i].empty();
                if (!hasType && !receiverAlias.empty() &&
                    block.paramNames[i] == receiverAlias) {
                    hasType = true;
                }
                if (!hasType) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            "函数 " + block.funcName + " 的参数 " +
                            block.paramNames[i] + " 必须显式声明类型"
                        );
                    }
                }
            }

            for (size_t i = 0; i < block.paramNames.size(); ++i) {
                const bool hasAnnotation = i < block.paramTypeNames.size() &&
                    !block.paramTypeNames[i].empty();
                const bool isReceiverParameter = !receiverAlias.empty() &&
                    block.paramNames[i] == receiverAlias;
                TypeRef parameterType;
                if (hasAnnotation) {
                    parameterType = parseDeclaredTypeRef(block.paramTypeNames[i]);
                    if (parameterType.kind == InferredType::Unknown) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            "函数 " + block.funcName + " 的参数 " +
                            block.paramNames[i] + " 使用了无法识别的类型注解"
                        );
                    }
                } else if (isReceiverParameter && !block.ownerClassName.empty()) {
                    parameterType = TypeRef(InferredType::Object, block.ownerClassName);
                    parameterType.namedTypeArguments = block.classTypeParameters;
                }
                if (!hasAnnotation && !isReceiverParameter) {
                    environment[block.paramNames[i]] = TypeInfo::dynamic();
                } else {
                    environment[block.paramNames[i]] = TypeInfo::fromType(
                        std::move(parameterType),
                        false,
                        hasAnnotation
                    );
                }
            }

            if (!receiverAlias.empty() && !block.ownerClassName.empty() &&
                block.paramNames.end() == std::find(
                    block.paramNames.begin(), block.paramNames.end(), receiverAlias
                )) {
                TypeRef receiverType(InferredType::Object, block.ownerClassName);
                receiverType.namedTypeArguments = block.classTypeParameters;
                environment[receiverAlias] = TypeInfo::fromType(
                    std::move(receiverType),
                    false,
                    true
                );
            }

            const size_t receiverParameterOffset = receiverAlias.empty() ? 0 : 1;
            for (size_t i = 0; i < block.paramDefaults.size(); ++i) {
                if (block.paramDefaults[i].empty()) continue;
                checkCallArgumentTypes(block.paramDefaults[i], environment, nodeIndex);
                if (i >= block.paramTypeNames.size() ||
                    block.paramTypeNames[i].empty() ||
                    i < receiverParameterOffset) {
                    continue;
                }

                const TypeInfo expected = TypeInfo::fromType(
                    parseDeclaredTypeRef(block.paramTypeNames[i]), false, true);
                const TypeInfo actual = inferExpressionInfo(
                    block.paramDefaults[i], environment);
                if (!actual.isKnown() || !expected.isKnown() ||
                    hasSameDeclaredType(expected, actual)) {
                    continue;
                }

                addDiagnostic(
                    nodeIndex,
                    TypeCheckSeverity::Error,
                    block.funcName + "(): argument " +
                        std::to_string(i - receiverParameterOffset + 1) +
                        " must " + expected.toString() + ", not " + actual.toString(),
                    TypeCheckDiagnosticCode::CallArgumentTypeMismatch
                );
            }

            bool hasReturnAnnotation = !block.returnTypeName.empty();
            function.expectedReturnIsReference = parseTypeRef(
                block.returnTypeName
            ).isReference;
            function.expectedReturnTypeName = block.returnTypeName;
            if (function.expectedReturnIsReference) {
                const std::string targetType = block.returnTypeName.substr(1);
                const size_t genericBegin = targetType.find('<');
                const std::string baseType = genericBegin == std::string::npos
                    ? targetType : targetType.substr(0, genericBegin);
                const bool isGenericParameter =
                    activeTypeParameters.find(targetType) != activeTypeParameters.end();
                const bool isClassType = classNames.find(baseType) != classNames.end();
                if (parseTypeName(targetType) != InferredType::Unknown ||
                    (!isClassType && !isGenericParameter)) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "->& return type must be a managed class or a class type parameter"
                    );
                }
            }
            if (functionOptions.requireExplicitFunctionReturnTypes && !hasReturnAnnotation) {
                addDiagnostic(
                    nodeIndex,
                    TypeCheckSeverity::Error,
                    "函数 " + block.funcName + " 必须显式声明返回类型"
                );
            }
            if (hasReturnAnnotation) {
                function.expectedReturn = parseDeclaredType(block.returnTypeName);
                if (function.expectedReturn == InferredType::Unknown) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "函数 " + block.funcName + " 使用了无法识别的返回类型注解"
                    );
                }
            }

            checkBlock(block.nodes, std::move(environment), nodeIndex, functionOptions, &function);
            currentSourceLine = block.sourceLine;
            currentSourceColumn = block.sourceColumn;

            if (!hasReturnAnnotation) {
                if (!function.hasReturn) {
                    function.inferredReturnType = TypeRef(InferredType::Nil);
                    function.hasInferredReturnType = true;
                } else if (!function.inferredReturnIsDynamic &&
                           function.hasInferredReturnType &&
                           function.inferredReturnType.kind != InferredType::Nil &&
                           !allPathsReturn(block.nodes)) {
                    // A conditional return may fall through to implicit nil.
                    // Keep that function on the dynamic ABI unless nil is the
                    // only possible result.
                    function.inferredReturnIsDynamic = true;
                }

                if (!function.inferredReturnIsDynamic &&
                    function.hasInferredReturnType &&
                    block.id != TreeParser::InvalidNodeId) {
                    inferredFunctionReturnTypes[block.id] =
                        function.inferredReturnType.toIRTypeName();
                }

                if (functionOptions.rejectUnknownExpressions && function.hasReturn &&
                    function.inferredReturnIsDynamic && !function.sawUnknownReturnType) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "@static 函数 " + block.funcName +
                        " 的返回类型无法唯一推断为静态类型"
                    );
                }
            }

            if (function.expectedReturn != InferredType::Unknown &&
                function.expectedReturn != InferredType::Nil && !function.hasReturn) {
                addDiagnostic(
                    nodeIndex,
                    TypeCheckSeverity::Error,
                    "函数 " + block.funcName + " 声明了返回类型，但没有 return"
                );
            }

            std::string ownerType = block.ownerClassName;
            if (const auto* builtin =
                    ZeusBuiltins::findTypeImplementation(ownerType)) {
                ownerType = builtin->canonicalName;
            }
            const std::string functionKey = block.isClassMethod && !ownerType.empty()
                ? ownerType + "::" + block.funcName
                : block.funcName;
            if (hasReturnAnnotation) {
                inferredFunctionReturnInfos[functionKey] = TypeInfo::fromType(
                    parseDeclaredTypeRef(block.returnTypeName), false, true);
            } else if (!function.hasReturn) {
                inferredFunctionReturnInfos[functionKey] =
                    TypeInfo::fromType(TypeRef(InferredType::Nil));
            } else if (function.inferredReturnIsDynamic) {
                inferredFunctionReturnInfos[functionKey] = TypeInfo::dynamic();
            } else if (function.sawUnknownReturnType ||
                       !function.hasInferredReturnType) {
                inferredFunctionReturnInfos[functionKey] =
                    finalizeUnknownReturnSummaries
                        ? TypeInfo::dynamic()
                        : TypeInfo::unresolved();
            } else {
                inferredFunctionReturnInfos[functionKey] = TypeInfo::fromType(
                    function.inferredReturnType);
            }
            currentSourceLine = previousSourceLine;
            currentSourceColumn = previousSourceColumn;
            activeTypeParameters = previousTypeParameters;
        }

        void prePromoteClassFieldAssignments(
            const std::vector<TreeParser::NodeAndBlock>& blockNodes,
            Environment environment,
            const std::string& className
        ) {
            for (const auto& node : blockNodes) {
                TreeParser::AstElementView element(node);
                if (element.isNode()) {
                    const auto& statement = element.node();
                    if (statement.type == NodeType::VAR_DECL_STMT) {
                        std::string name;
                        bool hasDeclaredType = false;
                        std::string declaredTypeName;
                        std::vector<LexerType::Token> expression;
                        if (!splitVariableDeclaration(
                                statement.tokens, name, hasDeclaredType,
                                declaredTypeName, expression)) {
                            continue;
                        }
                        TypeInfo value = expression.empty()
                            ? TypeInfo::unresolved()
                            : inferExpressionInfo(expression, environment);
                        if (hasDeclaredType) {
                            value = TypeInfo::fromType(
                                parseDeclaredTypeRef(declaredTypeName), false, true);
                        } else if (!value.isResolved()) {
                            value = TypeInfo::dynamic();
                        }
                        environment[name] = std::move(value);
                        continue;
                    }

                    if (statement.type != NodeType::VAR_ASSIGN_STMT) continue;
                    size_t equalIndex = statement.tokens.size();
                    for (size_t i = 0; i < statement.tokens.size(); ++i) {
                        if (isOperator(statement.tokens[i], "=")) {
                            equalIndex = i;
                            break;
                        }
                    }
                    if (equalIndex == statement.tokens.size()) continue;

                    const std::vector<LexerType::Token> valueTokens(
                        statement.tokens.begin() + equalIndex + 1,
                        statement.tokens.end());
                    const TypeInfo valueInfo = inferExpressionInfo(
                        valueTokens, environment);
                    const bool fieldAssignment = equalIndex >= 3 &&
                        isOperator(statement.tokens[equalIndex - 2], ".") &&
                        std::holds_alternative<LexerType::Keyword>(
                            statement.tokens[equalIndex - 1]);
                    if (fieldAssignment) {
                        const std::vector<LexerType::Token> receiverTokens(
                            statement.tokens.begin(),
                            statement.tokens.begin() + equalIndex - 2);
                        const TypeInfo receiverInfo = inferExpressionInfo(
                            receiverTokens, environment);
                        const std::string fieldOwner = receiverInfo.type.namedType;
                        const std::string fieldName = std::get<LexerType::Keyword>(
                            statement.tokens[equalIndex - 1]).name;
                        if (fieldOwner != className) continue;

                        const auto fields = classFields.find(className);
                        if (fields == classFields.end()) continue;
                        const auto field = fields->second.find(fieldName);
                        if (field == fields->second.end()) continue;
                        const auto annotated = explicitlyAnnotatedClassFields.find(
                            className);
                        if (annotated != explicitlyAnnotatedClassFields.end() &&
                            annotated->second.count(fieldName) != 0) {
                            continue;
                        }
                        if (field->second == "unknown") continue;

                        const TypeInfo fieldInfo = TypeInfo::fromType(
                            parseDeclaredTypeRef(field->second), false, true);
                        if (fieldInfo.type.kind == InferredType::Object &&
                            valueInfo.type.kind == InferredType::Nil) {
                            continue;
                        }
                        const bool sameType = fieldInfo.isKnown() &&
                            valueInfo.isKnown() && fieldInfo.type == valueInfo.type;
                        if (!sameType) {
                            promoteClassFieldToDynamic(className, fieldName);
                        }
                        continue;
                    }

                    if (equalIndex == 1 &&
                        std::holds_alternative<LexerType::Keyword>(
                            statement.tokens.front())) {
                        const std::string& name = std::get<LexerType::Keyword>(
                            statement.tokens.front()).name;
                        auto variable = environment.find(name);
                        if (variable == environment.end()) continue;
                        if (variable->second.isDynamic() || valueInfo.isDynamic() ||
                            !valueInfo.isResolved() ||
                            (variable->second.isKnown() && valueInfo.isKnown() &&
                             variable->second.type != valueInfo.type)) {
                            variable->second = TypeInfo::dynamic();
                        } else if (variable->second.type.kind == InferredType::Nil &&
                                   valueInfo.isKnown()) {
                            variable->second = valueInfo;
                        }
                    }
                    continue;
                }

                const auto* nested = element.block();
                if (nested == nullptr || nested->type == CodeBlockType::FUNC ||
                    nested->type == CodeBlockType::CLASS ||
                    nested->type == CodeBlockType::INTERFACE) {
                    continue;
                }
                prePromoteClassFieldAssignments(
                    nested->nodes, environment, className);
            }
        }

        void checkClassBody(
            const TreeParser::CodeBlock& classBlock,
            Environment environment,
            int& nodeIndex,
            const TypeCheckOptions& options,
            FunctionContext* currentFunction
        ) {
            TypeRef receiverType(InferredType::Object, classBlock.className);
            receiverType.namedTypeArguments = classBlock.typeParameters;
            const TypeInfo receiverInfo = TypeInfo::fromType(
                std::move(receiverType), false, true);

            Environment fieldEnvironment = environment;
            fieldEnvironment["this"] = receiverInfo;
            const auto receiverAlias = classReceiverAliases.find(classBlock.className);
            if (receiverAlias != classReceiverAliases.end()) {
                fieldEnvironment[receiverAlias->second] = receiverInfo;
            }

            const int classNodeIndex = nodeIndex;
            for (const auto& child : classBlock.nodes) {
                ++nodeIndex;
                TreeParser::AstElementView element(child);
                if (!element.isNode()) continue;

                const auto& statement = element.node();
                if (statement.type != NodeType::VAR_DECL_STMT) continue;
                currentSourceLine = statement.sourceLine;
                currentSourceColumn = statement.sourceColumn;
                checkVariableDeclaration(
                    statement, fieldEnvironment, nodeIndex, options);

                std::string fieldName;
                bool hasDeclaredType = false;
                std::string declaredTypeName;
                std::vector<LexerType::Token> expression;
                if (splitVariableDeclaration(
                        statement.tokens,
                        fieldName,
                        hasDeclaredType,
                        declaredTypeName,
                        expression) &&
                    statement.id != TreeParser::InvalidNodeId) {
                    classFieldDeclarationNodes[classBlock.className][fieldName] =
                        statement.id;
                    if (hasDeclaredType) {
                        explicitlyAnnotatedClassFields[classBlock.className].insert(
                            fieldName);
                    } else {
                        explicitlyAnnotatedClassFields[classBlock.className].erase(
                            fieldName);
                    }
                    const auto inferred = inferredBindingTypesByNode.find(statement.id);
                    if (inferred != inferredBindingTypesByNode.end() &&
                        !inferred->second.empty()) {
                        classFields[classBlock.className][fieldName] =
                            inferred->second;
                    } else if (hasDeclaredType) {
                        classFields[classBlock.className][fieldName] =
                            parseDeclaredTypeRef(declaredTypeName).toIRTypeName();
                    } else {
                        classFields[classBlock.className][fieldName] = "unknown";
                        setInferredBindingType(statement.id, "unknown");
                    }
                }
                checkCallArgumentTypes(
                    statement.tokens, fieldEnvironment, nodeIndex);
            }

            const auto observedBeforePrepass = observedBindingNodes;
            for (const auto& child : classBlock.nodes) {
                TreeParser::AstElementView element(child);
                if (!element.isBlock()) continue;
                const auto* method = element.block();
                if (method == nullptr || method->type != CodeBlockType::FUNC) continue;

                Environment methodEnvironment = fieldEnvironment;
                const std::string receiverAlias = instanceReceiverAlias(*method);
                for (size_t i = 0; i < method->paramNames.size(); ++i) {
                    const bool hasAnnotation = i < method->paramTypeNames.size() &&
                        !method->paramTypeNames[i].empty();
                    const bool isReceiverParameter = !receiverAlias.empty() &&
                        method->paramNames[i] == receiverAlias;
                    if (!hasAnnotation && !isReceiverParameter) {
                        methodEnvironment[method->paramNames[i]] = TypeInfo::dynamic();
                    } else if (hasAnnotation) {
                        methodEnvironment[method->paramNames[i]] = TypeInfo::fromType(
                            parseDeclaredTypeRef(method->paramTypeNames[i]), false, true);
                    }
                }
                if (!receiverAlias.empty() &&
                    method->paramNames.end() == std::find(
                        method->paramNames.begin(), method->paramNames.end(),
                        receiverAlias)) {
                    methodEnvironment[receiverAlias] = receiverInfo;
                }
                prePromoteClassFieldAssignments(
                    method->nodes, std::move(methodEnvironment), classBlock.className);
            }
            observedBindingNodes = observedBeforePrepass;

            nodeIndex = classNodeIndex;
            for (const auto& child : classBlock.nodes) {
                ++nodeIndex;
                TreeParser::AstElementView element(child);

                if (element.isNode()) {
                    const auto& statement = element.node();
                    if (statement.type == NodeType::VAR_DECL_STMT) continue;

                    currentSourceLine = statement.sourceLine;
                    currentSourceColumn = statement.sourceColumn;
                    if (statement.type == NodeType::VAR_ASSIGN_STMT) {
                        checkAssignment(
                            statement, fieldEnvironment, nodeIndex, options);
                    } else if (statement.type == NodeType::RETURN_STMT) {
                        checkReturn(
                            statement, fieldEnvironment, currentFunction,
                            nodeIndex, options);
                    }

                    checkCallArgumentTypes(
                        statement.tokens, fieldEnvironment, nodeIndex);
                    continue;
                }

                const auto* block = element.block();
                if (block == nullptr || block->type == CodeBlockType::INTERFACE) {
                    continue;
                }
                currentSourceLine = block->sourceLine;
                currentSourceColumn = block->sourceColumn;
                checkCallArgumentTypes(
                    block->conditionTokens, environment, nodeIndex);

                if (block->type == CodeBlockType::FUNC) {
                    checkFunction(*block, environment, nodeIndex);
                } else {
                    const auto previousTypeParameters = activeTypeParameters;
                    if (block->type == CodeBlockType::CLASS) {
                        activeTypeParameters.insert(
                            block->typeParameters.begin(),
                            block->typeParameters.end());
                        checkClassBody(
                            *block, fieldEnvironment, nodeIndex,
                            options, currentFunction);
                    } else {
                        checkBlock(
                            block->nodes, fieldEnvironment, nodeIndex,
                            options, currentFunction);
                    }
                    activeTypeParameters = previousTypeParameters;
                }
            }
        }

        void checkIfBranchEnvironments(
            const TreeParser::CodeBlock& ifBlock,
            const Environment& baseEnvironment,
            std::vector<Environment>& branchEnvironments,
            int& nodeIndex,
            const TypeCheckOptions& options,
            FunctionContext* currentFunction
        ) {
            currentSourceLine = ifBlock.sourceLine;
            currentSourceColumn = ifBlock.sourceColumn;
            (void)inferExpressionInfo(ifBlock.conditionTokens, baseEnvironment);
            checkCallArgumentTypes(ifBlock.conditionTokens, baseEnvironment, nodeIndex);
            const auto branchEntryObserved = observedBindingNodes;

            const TreeParser::CodeBlock* elseBranch = nullptr;
            for (const auto& child : ifBlock.nodes) {
                TreeParser::AstElementView element(child);
                if (!element.isBlock()) continue;
                const auto* nested = element.block();
                if (nested != nullptr && nested->isElseBranch) {
                    elseBranch = nested;
                }
            }

            Environment thenEnvironment = baseEnvironment;
            thenEnvironment = checkBlock(
                ifBlock.nodes,
                std::move(thenEnvironment),
                nodeIndex,
                options,
                currentFunction,
                elseBranch == nullptr
                    ? TreeParser::InvalidNodeId
                    : elseBranch->id
            );
            branchEnvironments.push_back(std::move(thenEnvironment));
            const auto thenObserved = observedBindingNodes;
            observedBindingNodes = branchEntryObserved;

            if (elseBranch == nullptr) {
                branchEnvironments.push_back(baseEnvironment);
                observedBindingNodes.insert(thenObserved.begin(), thenObserved.end());
                return;
            }

            if (elseBranch->type == CodeBlockType::IF) {
                checkIfBranchEnvironments(
                    *elseBranch,
                    baseEnvironment,
                    branchEnvironments,
                    nodeIndex,
                    options,
                    currentFunction
                );
                const auto elseObserved = observedBindingNodes;
                observedBindingNodes = branchEntryObserved;
                observedBindingNodes.insert(thenObserved.begin(), thenObserved.end());
                observedBindingNodes.insert(elseObserved.begin(), elseObserved.end());
                return;
            }

            currentSourceLine = elseBranch->sourceLine;
            currentSourceColumn = elseBranch->sourceColumn;
            (void)inferExpressionInfo(elseBranch->conditionTokens, baseEnvironment);
            checkCallArgumentTypes(
                elseBranch->conditionTokens, baseEnvironment, nodeIndex);
            Environment elseEnvironment = baseEnvironment;
            elseEnvironment = checkBlock(
                elseBranch->nodes,
                std::move(elseEnvironment),
                nodeIndex,
                options,
                currentFunction
            );
            branchEnvironments.push_back(std::move(elseEnvironment));
            const auto elseObserved = observedBindingNodes;
            observedBindingNodes = branchEntryObserved;
            observedBindingNodes.insert(thenObserved.begin(), thenObserved.end());
            observedBindingNodes.insert(elseObserved.begin(), elseObserved.end());
        }

        void mergeBranchEnvironments(
            Environment& destination,
            const Environment& baseEnvironment,
            const std::vector<Environment>& branchEnvironments
        ) {
            if (branchEnvironments.empty()) return;
            for (const auto& binding : baseEnvironment) {
                TypeInfo merged = binding.second;
                auto first = branchEnvironments.front().find(binding.first);
                if (first != branchEnvironments.front().end()) {
                    merged = first->second;
                }
                for (size_t index = 1; index < branchEnvironments.size(); ++index) {
                    const auto next = branchEnvironments[index].find(binding.first);
                    if (next != branchEnvironments[index].end()) {
                        merged = mergeFlowTypes(merged, next->second);
                    }
                }
                merged.bindingNodeId = binding.second.bindingNodeId;
                merged.explicitAnnotation = binding.second.explicitAnnotation;
                destination[binding.first] = merged;
                setInferredBindingType(
                    merged.bindingNodeId,
                    flowTypeName(merged));
            }
        }

        Environment checkBlock(
            const std::vector<TreeParser::NodeAndBlock>& blockNodes,
            Environment environment,
            int& nodeIndex,
            const TypeCheckOptions& options,
            FunctionContext* currentFunction,
            TreeParser::NodeId skippedElementId = TreeParser::InvalidNodeId
        ) {
            for (const auto& node : blockNodes) {
                ++nodeIndex;
                TreeParser::AstElementView element(node);
                if (skippedElementId != TreeParser::InvalidNodeId &&
                    element.id() == skippedElementId) {
                    continue;
                }

                if (element.isNode()) {
                    const auto& statement = element.node();
                    currentSourceLine = statement.sourceLine;
                    currentSourceColumn = statement.sourceColumn;

                    if (statement.type == NodeType::VAR_DECL_STMT) {
                        checkVariableDeclaration(
                            statement, environment, nodeIndex, options, currentFunction);
                    } else if (statement.type == NodeType::VAR_ASSIGN_STMT) {
                        checkAssignment(
                            statement, environment, nodeIndex, options, currentFunction);
                    } else if (statement.type == NodeType::RETURN_STMT) {
                        checkReturn(statement, environment, currentFunction, nodeIndex, options);
                    }

                    checkCallArgumentTypes(statement.tokens, environment, nodeIndex);

                    continue;
                }

                const auto* block = element.block();
                if (block == nullptr) continue;
                if (block->type == CodeBlockType::INTERFACE) continue;
                currentSourceLine = block->sourceLine;
                currentSourceColumn = block->sourceColumn;

                if (block->type == CodeBlockType::IF) {
                    const Environment baseEnvironment = environment;
                    std::vector<Environment> branchEnvironments;
                    checkIfBranchEnvironments(
                        *block,
                        baseEnvironment,
                        branchEnvironments,
                        nodeIndex,
                        options,
                        currentFunction
                    );
                    mergeBranchEnvironments(
                        environment, baseEnvironment, branchEnvironments);
                } else if (block->type == CodeBlockType::FUNC) {
                    checkCallArgumentTypes(block->conditionTokens, environment, nodeIndex);
                    checkFunction(*block, environment, nodeIndex);
                } else if (block->type == CodeBlockType::CLASS) {
                    checkCallArgumentTypes(block->conditionTokens, environment, nodeIndex);
                    const auto previousTypeParameters = activeTypeParameters;
                    activeTypeParameters.insert(
                        block->typeParameters.begin(),
                        block->typeParameters.end());
                    checkClassBody(
                        *block, environment, nodeIndex,
                        options, currentFunction);
                    activeTypeParameters = previousTypeParameters;
                } else {
                    checkCallArgumentTypes(block->conditionTokens, environment, nodeIndex);
                    const Environment baseEnvironment = environment;
                    Environment nestedEnvironment = environment;
                    if (block->type == CodeBlockType::FOR) {
                        TypeInfo bindingType = TypeInfo::dynamic();
                        if (block->forKind == TreeParser::CodeBlock::ForKind::COUNT ||
                            block->forKind == TreeParser::CodeBlock::ForKind::COUNT_SIMPLE) {
                            bindingType = TypeInfo::fromType(
                                TypeRef(InferredType::Int));
                        }
                        for (const auto& binding : block->forBindings) {
                            nestedEnvironment[binding.name] = bindingType;
                        }
                    }
                    nestedEnvironment = checkBlock(
                        block->nodes,
                        std::move(nestedEnvironment),
                        nodeIndex,
                        options,
                        currentFunction
                    );
                    if (block->type == CodeBlockType::FOR) {
                        for (const auto& binding : block->forBindings) {
                            nestedEnvironment.erase(binding.name);
                        }
                        mergeBranchEnvironments(
                            environment,
                            baseEnvironment,
                            std::vector<Environment>{baseEnvironment, nestedEnvironment}
                        );
                    } else if (block->type == CodeBlockType::TRY ||
                        block->type == CodeBlockType::CATCH) {
                        mergeBranchEnvironments(
                            environment,
                            baseEnvironment,
                            std::vector<Environment>{baseEnvironment, nestedEnvironment}
                        );
                    } else {
                        for (const auto& binding : baseEnvironment) {
                            const auto updated = nestedEnvironment.find(binding.first);
                            if (updated == nestedEnvironment.end()) continue;
                            TypeInfo next = updated->second;
                            next.bindingNodeId = binding.second.bindingNodeId;
                            next.explicitAnnotation = binding.second.explicitAnnotation;
                            environment[binding.first] = next;
                            setInferredBindingType(
                                next.bindingNodeId,
                                flowTypeName(next));
                        }
                    }
                }
            }
            return environment;
        }

    public:
        explicit TypeChecker(const std::vector<TreeParser::NodeAndBlock>& nodes)
            : nodes(nodes), inferer(nodes) {}

        void registerExternalFunction(
            const std::string& name,
            const std::string& returnType,
            const std::vector<std::string>& parameterTypes
        ) {
            inferer.registerExternalFunction(name, returnType, parameterTypes);
        }

        void registerExternalClass(
            const std::string& name,
            const std::vector<std::pair<std::string, std::string>>& fields
        ) {
            auto& registeredFields = externalClassFields[name];
            for (const auto& field : fields) {
                if (!field.first.empty() && !field.second.empty()) {
                    registeredFields[field.first] = field.second;
                }
            }
        }

        bool check(const TypeCheckOptions& options = TypeCheckOptions::dynamic()) {
            diagnostics.clear();
            inferredFunctionReturnTypes.clear();
            inferredBindingTypesByNode.clear();
            inferredFunctionReturnInfos.clear();
            finalizeUnknownReturnSummaries = false;
            currentSourceLine = 0;
            currentSourceColumn = 0;
            rootOptions = options;
            inferer.infer();

            auto runPass = [&]() {
                diagnostics.clear();
                inferredFunctionReturnTypes.clear();
                inferredBindingTypesByNode.clear();
                observedBindingNodes.clear();
                classFieldDeclarationNodes.clear();
                explicitlyAnnotatedClassFields.clear();
                classNames.clear();
                classFields.clear();
                genericClassParameters.clear();
                classReceiverAliases.clear();
                operatorReturnTypes.clear();
                for (const auto& builtin : ZeusBuiltins::typeImplementations()) {
                    if (builtin.category == ZeusBuiltins::TypeCategory::Opaque &&
                        builtin.inheritable) {
                        classNames.insert(std::string(builtin.canonicalName));
                    }
                }
                for (const auto& externalClass : externalClassFields) {
                    classNames.insert(externalClass.first);
                    classFields[externalClass.first] = externalClass.second;
                }
                collectClassNames(nodes);

                Environment environment;
                int nodeIndex = -1;
                checkBlock(nodes, std::move(environment), nodeIndex, options, nullptr);
            };

            while (true) {
                const auto previousReturns = inferredFunctionReturnInfos;
                runPass();
                if (sameFunctionReturnInfos(
                        previousReturns, inferredFunctionReturnInfos)) {
                    break;
                }
            }

            bool finalizedUnknownReturn = false;
            for (auto& summary : inferredFunctionReturnInfos) {
                if (!summary.second.isResolved()) {
                    summary.second = TypeInfo::dynamic();
                    finalizedUnknownReturn = true;
                }
            }
            if (finalizedUnknownReturn) {
                finalizeUnknownReturnSummaries = true;
                while (true) {
                    const auto previousReturns = inferredFunctionReturnInfos;
                    runPass();
                    if (sameFunctionReturnInfos(
                            previousReturns, inferredFunctionReturnInfos)) {
                        break;
                    }
                }
            }
            return !hasErrors();
        }

        bool check(TypeCheckMode mode) {
            return check(
                mode == TypeCheckMode::Static
                    ? TypeCheckOptions::llvm()
                    : TypeCheckOptions::dynamic()
            );
        }

        bool hasErrors() const {
            for (const auto& diagnostic : diagnostics) {
                if (diagnostic.severity == TypeCheckSeverity::Error) return true;
            }

            return false;
        }

        const std::vector<TypeCheckDiagnostic>& getDiagnostics() const {
            return diagnostics;
        }

        const TypeInferer& getInferer() const {
            return inferer;
        }

        TypeInfo getTypeInfo(TreeParser::NodeId nodeId) const {
            return inferer.getTypeInfo(nodeId);
        }

        TypeInfo getTypeInfoByIndex(int nodeIndex) const {
            return inferer.getTypeInfoByIndex(nodeIndex);
        }

        const std::unordered_map<TreeParser::NodeId, std::string>&
        getInferredFunctionReturnTypes() const {
            return inferredFunctionReturnTypes;
        }

        const std::unordered_map<TreeParser::NodeId, std::string>&
        getInferredBindingTypesByNode() const {
            return inferredBindingTypesByNode;
        }

    };

}
