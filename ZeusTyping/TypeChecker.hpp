#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "TypeInfer.hpp"
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

    struct TypeCheckDiagnostic {
        int nodeIndex = -1;
        TypeCheckSeverity severity = TypeCheckSeverity::Error;
        std::string message;
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
        std::unordered_set<std::string> classNames;
        std::unordered_map<std::string, std::unordered_map<std::string, std::string>> classFields;
        std::unordered_map<std::string, std::string> classReceiverAliases;
        std::unordered_map<std::string, std::string> operatorReturnTypes;
        std::unordered_map<TreeParser::NodeId, std::string> inferredFunctionReturnTypes;

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
                        bool readingType = false;
                        size_t equal = fieldNode.tokens.size();
                        for (size_t i = 0; i < fieldNode.tokens.size(); ++i) {
                            const auto& token = fieldNode.tokens[i];
                            if (isOperator(token, "=")) {
                                equal = i;
                                break;
                            }
                            if (isOperator(token, ":")) {
                                readingType = true;
                                continue;
                            }
                            if (!std::holds_alternative<LexerType::Keyword>(token)) continue;

                            const std::string& name =
                                std::get<LexerType::Keyword>(token).name;
                            if (name == "let" || name == "var" || name == "const") continue;
                            if (readingType) {
                                fieldType = name;
                                readingType = false;
                            } else if (fieldName.empty()) {
                                fieldName = name;
                            }
                        }

                        if (fieldType.empty() && equal + 1 < fieldNode.tokens.size()) {
                            const auto& value = fieldNode.tokens[equal + 1];
                            if (std::holds_alternative<LexerType::Value>(value)) {
                                const auto& data = std::get<LexerType::Value>(value).data;
                                if (std::holds_alternative<ZeusType::BigInteger>(data)) {
                                    fieldType = "int";
                                } else if (std::holds_alternative<ZeusType::BigDecimal>(data)) {
                                    fieldType = "float";
                                } else if (std::holds_alternative<bool>(data)) {
                                    fieldType = "bool";
                                } else if (std::holds_alternative<std::string>(data)) {
                                    fieldType = "string";
                                }
                            }
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
                block.isStaticMethod || block.ownerClassName.empty()) {
                return {};
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
            if (type == InferredType::Unknown && classNames.count(baseName) != 0) {
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
            if (type == InferredType::Object && classNames.count(baseName) != 0) {
                result.namedType = baseName;
            }
            return result;
        }

        TypeInfo inferExpressionInfo(
            const std::vector<LexerType::Token>& expression,
            const Environment& environment
        ) const {
            if (expression.size() >= 2 && isKeyword(expression.front(), "new") &&
                std::holds_alternative<LexerType::Keyword>(expression[1])) {
                return TypeInfo::fromType(parseDeclaredTypeRef(
                    std::get<LexerType::Keyword>(expression[1]).name));
            }

            if (expression.size() == 1 &&
                std::holds_alternative<LexerType::Keyword>(expression[0])) {
                const std::string& name =
                    std::get<LexerType::Keyword>(expression[0]).name;
                auto it = environment.find(name);
                if (it != environment.end()) return it->second;
            }

            if (expression.size() == 3 &&
                std::holds_alternative<LexerType::Keyword>(expression[0]) &&
                isOperator(expression[1], ".") &&
                std::holds_alternative<LexerType::Keyword>(expression[2])) {
                TypeInfo objectInfo = inferExpressionInfo(
                    std::vector<LexerType::Token>{ expression[0] },
                    environment
                );
                if (objectInfo.type.kind == InferredType::Object &&
                    !objectInfo.type.namedType.empty()) {
                    auto classIt = classFields.find(objectInfo.type.namedType);
                    if (classIt != classFields.end()) {
                        const std::string& fieldName =
                            std::get<LexerType::Keyword>(expression[2]).name;
                        auto fieldIt = classIt->second.find(fieldName);
                        if (fieldIt != classIt->second.end()) {
                            return TypeInfo::fromType(
                                parseDeclaredTypeRef(fieldIt->second),
                                false,
                                true
                            );
                        }
                    }
                }
            }

            auto functionResult = [&](const std::string& functionName) -> TypeInfo {
                const auto& functions = inferer.getFunctions();
                auto function = functions.find(functionName);
                if (function == functions.end()) return TypeInfo::unresolved();

                TypeRef result = function->second.returnTypeName.empty()
                    ? TypeRef(function->second.returnType)
                    : parseDeclaredTypeRef(function->second.returnTypeName);
                if (function->second.returnTypeName.empty()) {
                    result.isReference = function->second.returnsReference;
                }
                return TypeInfo::fromType(
                    std::move(result),
                    false,
                    !function->second.returnTypeName.empty()
                );
            };

            if (expression.size() >= 3 &&
                std::holds_alternative<LexerType::Keyword>(expression.front()) &&
                isOperator(expression[1], "(") &&
                isOperator(expression.back(), ")")) {
                TypeInfo result = functionResult(
                    std::get<LexerType::Keyword>(expression.front()).name
                );
                if (result.resolved) return result;
            } else if (expression.size() >= 5 &&
                       std::holds_alternative<LexerType::Keyword>(expression[0]) &&
                       isOperator(expression[1], ".") &&
                       std::holds_alternative<LexerType::Keyword>(expression[2]) &&
                       isOperator(expression[3], "(") &&
                       isOperator(expression.back(), ")")) {
                TypeInfo receiver = inferExpressionInfo(
                    std::vector<LexerType::Token>{ expression[0] },
                    environment
                );
                if (!receiver.type.namedType.empty()) {
                    const std::string method =
                        std::get<LexerType::Keyword>(expression[2]).name;
                    TypeInfo result = functionResult(
                        receiver.type.namedType + "::" + method
                    );
                    if (!result.resolved) {
                        result = functionResult(receiver.type.namedType + "." + method);
                    }
                    if (!result.resolved) result = functionResult(method);
                    if (result.resolved) return result;
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

            return inferer.inferExpressionInfo(expression, environment);
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
            std::string message
        ) {
            diagnostics.push_back({ nodeIndex, severity, std::move(message) });
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

        static bool splitVariableDeclaration(
            const std::vector<LexerType::Token>& tokens,
            std::string& name,
            bool& hasDeclaredType,
            InferredType& declaredType,
            std::vector<LexerType::Token>& expression
        ) {
            name.clear();
            hasDeclaredType = false;
            declaredType = InferredType::Unknown;
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
                if (nameIndex + 2 < equal &&
                    std::holds_alternative<LexerType::Keyword>(tokens[nameIndex + 2])) {
                    declaredType = parseTypeName(
                        std::get<LexerType::Keyword>(tokens[nameIndex + 2]).name
                    );
                }
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
            const TypeCheckOptions& options
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
                    declaredType,
                    expression)) {
                return;
            }

            for (size_t i = 0; i + 1 < statement.tokens.size(); ++i) {
                if (!isOperator(statement.tokens[i], ":") ||
                    !std::holds_alternative<LexerType::Keyword>(statement.tokens[i + 1])) {
                    continue;
                }
                declaredTypeName = std::get<LexerType::Keyword>(statement.tokens[i + 1]).name;
                declaredType = parseDeclaredType(declaredTypeName);
                break;
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
            InferredType actualType = actualInfo.type.kind;

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
            } else {
                if (options.requireExplicitVariableTypes) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "变量 " + name + " 必须显式声明类型"
                    );
                }

                if (actualType == InferredType::Unknown && options.rejectUnknownExpressions) {
                    addDiagnostic(
                        nodeIndex,
                        TypeCheckSeverity::Error,
                        "变量 " + name + " 没有类型注解，且初始化表达式无法推断类型"
                    );
                }
                environment[name] = actualInfo;
            }
        }

        void checkAssignment(
            const TreeParser::Node& statement,
            Environment& environment,
            int nodeIndex,
            const TypeCheckOptions& options
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
            if (isSubscriptAssignmentTarget(statement.tokens, equalIndex)) {
                // Container elements are dynamic ZeusValue slots. Assigning
                // one must not be type-checked as rebinding the container
                // variable itself (for example list -> int on arr[0] = 1).
                return;
            }
            if (equalIndex >= 3 && isOperator(statement.tokens[1], ".")) {
                const std::vector<LexerType::Token> target(
                    statement.tokens.begin(), statement.tokens.begin() + equalIndex
                );
                TypeInfo targetInfo = inferExpressionInfo(target, environment);
                if (targetInfo.type.kind == InferredType::Unknown) {
                    if (options.rejectUnknownExpressions) {
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
                    const bool returnsVariable = expression.size() == 1 &&
                        std::holds_alternative<LexerType::Keyword>(expression.front());
                    const bool constructsObject = !expression.empty() &&
                        isKeyword(expression.front(), "new");
                    if (!returnsVariable && !constructsObject &&
                        !actualInfo.type.isReference) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            "函数 " + function->name +
                            " 返回 &T 时必须返回变量、new 表达式或引用返回值"
                        );
                        return;
                    }
                    if (constructsObject && !function->expectedReturnTypeName.empty()) {
                        const std::string expectedClass =
                            function->expectedReturnTypeName.front() == '&'
                                ? function->expectedReturnTypeName.substr(1)
                                : function->expectedReturnTypeName;
                        if (expression.size() < 2 ||
                            !std::holds_alternative<LexerType::Keyword>(expression[1]) ||
                            std::get<LexerType::Keyword>(expression[1]).name != expectedClass) {
                            addDiagnostic(
                                nodeIndex,
                                TypeCheckSeverity::Error,
                                "函数 " + function->name + " 返回值的 class 类型与 " +
                                function->expectedReturnTypeName + " 不匹配"
                            );
                            return;
                        }
                    }
                    if (!constructsObject && !function->expectedReturnTypeName.empty()) {
                        const std::string expectedClass =
                            function->expectedReturnTypeName.front() == '&'
                                ? function->expectedReturnTypeName.substr(1)
                                : function->expectedReturnTypeName;
                        if (classNames.count(expectedClass) != 0 &&
                            !actualInfo.type.namedType.empty() &&
                            actualInfo.type.namedType != expectedClass) {
                            addDiagnostic(
                                nodeIndex,
                                TypeCheckSeverity::Error,
                                "函数 " + function->name + " 返回值的 class 类型与 " +
                                function->expectedReturnTypeName + " 不匹配"
                            );
                            return;
                        }
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
                function->inferredReturnIsDynamic = true;
                function->sawUnknownReturnType = true;
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
            FunctionContext function;
            function.name = block.funcName;
            const std::string receiverAlias = instanceReceiverAlias(block);
            TypeCheckOptions functionOptions = rootOptions;
            const bool isStaticCompile = block.isCompileTimeStatic;

            for (const auto& decorator : block.decorators) {
                if (decorator != "static") {
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
                    "@static 函数 " + block.funcName + " 不能捕获外部变量"
                );
            }
            if (isStaticCompile && rootOptions.mode == TypeCheckMode::Static) {
                functionOptions = TypeCheckOptions::strictLLVM();
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
                InferredType parameterType = InferredType::Unknown;
                if (i < block.paramTypeNames.size() && !block.paramTypeNames[i].empty()) {
                    parameterType = parseDeclaredType(block.paramTypeNames[i]);
                    if (parameterType == InferredType::Unknown) {
                        addDiagnostic(
                            nodeIndex,
                            TypeCheckSeverity::Error,
                            "函数 " + block.funcName + " 的参数 " +
                            block.paramNames[i] + " 使用了无法识别的类型注解"
                        );
                    }
                } else if (!receiverAlias.empty() &&
                           block.paramNames[i] == receiverAlias &&
                           !block.ownerClassName.empty()) {
                    parameterType = InferredType::Object;
                }
                const bool isReceiverParameter = !receiverAlias.empty() &&
                    block.paramNames[i] == receiverAlias;
                environment[block.paramNames[i]] = TypeInfo::fromType(
                    (parameterType == InferredType::Object &&
                     ((isReceiverParameter && !block.ownerClassName.empty()) ||
                      (i < block.paramTypeNames.size() &&
                       classNames.count(block.paramTypeNames[i]) != 0)))
                        ? TypeRef(
                            parameterType,
                            isReceiverParameter
                                ? block.ownerClassName
                                : block.paramTypeNames[i]
                          )
                        : TypeRef(parameterType),
                    false,
                    i < block.paramTypeNames.size() && !block.paramTypeNames[i].empty()
                );
            }

            if (!receiverAlias.empty() && !block.ownerClassName.empty() &&
                block.paramNames.end() == std::find(
                    block.paramNames.begin(), block.paramNames.end(), receiverAlias
                )) {
                environment[receiverAlias] = TypeInfo::fromType(
                    TypeRef(InferredType::Object, block.ownerClassName),
                    false,
                    true
                );
            }

            bool hasReturnAnnotation = !block.returnTypeName.empty();
            function.expectedReturnIsReference = parseTypeRef(
                block.returnTypeName
            ).isReference;
            function.expectedReturnTypeName = block.returnTypeName;
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
        }

        void checkBlock(
            const std::vector<TreeParser::NodeAndBlock>& blockNodes,
            Environment environment,
            int& nodeIndex,
            const TypeCheckOptions& options,
            FunctionContext* currentFunction
        ) {
            for (const auto& node : blockNodes) {
                ++nodeIndex;
                TreeParser::AstElementView element(node);

                if (element.isNode()) {
                    const auto& statement = element.node();

                    if (statement.type == NodeType::VAR_DECL_STMT) {
                        checkVariableDeclaration(statement, environment, nodeIndex, options);
                    } else if (statement.type == NodeType::VAR_ASSIGN_STMT) {
                        checkAssignment(statement, environment, nodeIndex, options);
                    } else if (statement.type == NodeType::RETURN_STMT) {
                        checkReturn(statement, environment, currentFunction, nodeIndex, options);
                    }

                    continue;
                }

                const auto* block = element.block();
                if (block == nullptr) continue;

                if (block->type == CodeBlockType::FUNC) {
                    checkFunction(*block, environment, nodeIndex);
                } else {
                    checkBlock(block->nodes, environment, nodeIndex, options, currentFunction);
                }
            }
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

        bool check(const TypeCheckOptions& options = TypeCheckOptions::dynamic()) {
            diagnostics.clear();
            inferredFunctionReturnTypes.clear();
            rootOptions = options;
            classNames.clear();
            classFields.clear();
            classReceiverAliases.clear();
            operatorReturnTypes.clear();
            for (const auto& builtin : ZeusBuiltins::typeImplementations()) {
                if (builtin.category == ZeusBuiltins::TypeCategory::Opaque &&
                    builtin.inheritable) {
                    classNames.insert(std::string(builtin.canonicalName));
                }
            }
            collectClassNames(nodes);
            inferer.infer();

            Environment environment;
            int nodeIndex = -1;
            checkBlock(nodes, std::move(environment), nodeIndex, options, nullptr);
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
    };

}
