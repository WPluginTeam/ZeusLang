#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../ZeusParser/TreeParser.hpp"
#include "../ZeusParser/AstElementView.hpp"
#include "../ZeusParser/TreeType.hpp"

#include "Types.hpp"
#include "TypeModel.hpp"

namespace ZeusTypeInfer {

    struct InferredNode {
        int index = -1;
        TreeParser::NodeId nodeId = TreeParser::InvalidNodeId;
        InferredType type = InferredType::Unknown;
        TypeInfo info;
    };

    struct InferenceDiagnostic {
        int nodeIndex = -1;
        std::string message;
    };

    struct FunctionInfo {
        std::string name;
        std::vector<InferredType> parameterTypes;
        std::vector<std::string> parameterTypeNames;
        InferredType returnType = InferredType::Unknown;
        std::string returnTypeName;
        bool returnsReference = false;
        bool dynamicReturn = false;
    };

    class TypeInferer {
        using SymbolTable = std::unordered_map<std::string, InferredType>;

        const std::vector<TreeParser::NodeAndBlock>& nodes;
        std::vector<InferredNode> inferredTypes;
        std::vector<InferenceDiagnostic> diagnostics;
        std::unordered_map<std::string, InferredType> symbols;
        std::unordered_map<std::string, FunctionInfo> functions;
        std::unordered_map<std::string, FunctionInfo> externalFunctions;
        std::unordered_map<TreeParser::NodeId, TypeInfo> nodeTypeInfo;

        static SymbolTable toLegacySymbolTable(const TypeEnvironment& environment) {
            SymbolTable result;
            result.reserve(environment.size());

            for (const auto& entry : environment) {
                result[entry.first] = entry.second.type.kind;
            }

            return result;
        }

        static bool isOperator(const LexerType::Token& token, const std::string& symbol) {
            return std::holds_alternative<LexerType::Operator>(token) &&
                   std::get<LexerType::Operator>(token).symbol == symbol;
        }

        static bool isKeyword(const LexerType::Token& token, const std::string& name) {
            return std::holds_alternative<LexerType::Keyword>(token) &&
                   std::get<LexerType::Keyword>(token).name == name;
        }

        static InferredType inferLiteralType(const LexerType::Token& token) {
            if (std::holds_alternative<LexerType::BytesLiteral>(token)) {
                return InferredType::Bytes;
            }

            if (!std::holds_alternative<LexerType::Value>(token)) {
                if (isKeyword(token, "true") || isKeyword(token, "false")) {
                    return InferredType::Bool;
                }

                if (isKeyword(token, "nil")) {
                    return InferredType::Nil;
                }

                return InferredType::Unknown;
            }

            const auto& value = std::get<LexerType::Value>(token);

            if (std::holds_alternative<ZeusType::BigInteger>(value.data)) {
                int64_t integerValue = 0;
                if (std::get<ZeusType::BigInteger>(value.data).tryToInt64(integerValue)) {
                    return InferredType::Int;
                }

                return InferredType::BigInt;
            }

            if (std::holds_alternative<ZeusType::BigDecimal>(value.data)) {
                return InferredType::Float;
            }

            if (std::holds_alternative<std::string>(value.data)) {
                return InferredType::String;
            }

            if (std::holds_alternative<bool>(value.data)) {
                return InferredType::Bool;
            }

            return InferredType::Unknown;
        }

        static int operatorPrecedence(const std::string& symbol) {
            if (symbol == "or") return 1;
            if (symbol == "and") return 2;
            if (symbol == "==" || symbol == "!=" || symbol == "<" || symbol == ">" ||
                symbol == "<=" || symbol == ">=") {
                return 3;
            }
            if (symbol == "+" || symbol == "-") return 4;
            if (symbol == "*" || symbol == "/" || symbol == "%") return 5;

            return -1;
        }

        static InferredType mergeNumericTypes(InferredType left, InferredType right) {
            if (left == InferredType::Float || right == InferredType::Float) {
                return InferredType::Float;
            }

            if (left == InferredType::BigInt || right == InferredType::BigInt) {
                return InferredType::BigInt;
            }

            if (left == InferredType::Int && right == InferredType::Int) {
                return InferredType::Int;
            }

            return InferredType::Unknown;
        }

        static InferredType inferBinaryType(
            const std::string& symbol,
            InferredType left,
            InferredType right
        ) {
            if (symbol == "and" || symbol == "or" || symbol == "==" || symbol == "!=" ||
                symbol == "<" || symbol == ">" || symbol == "<=" || symbol == ">=") {
                return InferredType::Bool;
            }

            if (symbol == "+" && left == InferredType::String && right == InferredType::String) {
                return InferredType::String;
            }

            if (symbol == "+" && left == InferredType::Bytes && right == InferredType::Bytes) {
                return InferredType::Bytes;
            }

            if (isNumeric(left) && isNumeric(right)) {
                return mergeNumericTypes(left, right);
            }

            return InferredType::Unknown;
        }

        InferredType inferExpressionRange(
            const std::vector<LexerType::Token>& tokens,
            size_t begin,
            size_t end,
            const SymbolTable& environment
        ) const {
            while (begin < end && isOperator(tokens[begin], "(") &&
                   end > begin + 1 && isOperator(tokens[end - 1], ")")) {
                int depth = 0;
                bool wrapsWholeExpression = true;

                for (size_t i = begin; i < end; ++i) {
                    if (isOperator(tokens[i], "(")) depth++;
                    if (isOperator(tokens[i], ")")) depth--;

                    if (depth == 0 && i + 1 < end) {
                        wrapsWholeExpression = false;
                        break;
                    }
                }

                if (!wrapsWholeExpression) break;
                ++begin;
                --end;
            }

            if (begin >= end) return InferredType::Unknown;

            int selectedPrecedence = std::numeric_limits<int>::max();
            size_t selectedOperator = end;
            std::string selectedSymbol;
            int parenDepth = 0;
            int bracketDepth = 0;
            int braceDepth = 0;

            for (size_t i = begin; i < end; ++i) {
                if (isOperator(tokens[i], "(")) {
                    parenDepth++;
                    continue;
                }
                if (isOperator(tokens[i], ")")) {
                    parenDepth--;
                    continue;
                }
                if (isOperator(tokens[i], "[")) {
                    bracketDepth++;
                    continue;
                }
                if (isOperator(tokens[i], "]")) {
                    bracketDepth--;
                    continue;
                }
                if (isOperator(tokens[i], "{")) {
                    braceDepth++;
                    continue;
                }
                if (isOperator(tokens[i], "}")) {
                    braceDepth--;
                    continue;
                }

                if (parenDepth != 0 || bracketDepth != 0 || braceDepth != 0) continue;
                if (!std::holds_alternative<LexerType::Operator>(tokens[i])) continue;

                const auto& symbol = std::get<LexerType::Operator>(tokens[i]).symbol;
                int precedence = operatorPrecedence(symbol);
                if (precedence >= 0 && precedence <= selectedPrecedence) {
                    selectedPrecedence = precedence;
                    selectedOperator = i;
                    selectedSymbol = symbol;
                }
            }

            if (selectedOperator < end) {
                InferredType left = inferExpressionRange(tokens, begin, selectedOperator, environment);
                InferredType right = inferExpressionRange(tokens, selectedOperator + 1, end, environment);
                return inferBinaryType(selectedSymbol, left, right);
            }

            if (isKeyword(tokens[begin], "new")) {
                return InferredType::Object;
            }

            if (isOperator(tokens[begin], "[") && isOperator(tokens[end - 1], "]")) {
                return InferredType::List;
            }

            if (isOperator(tokens[begin], "{") && isOperator(tokens[end - 1], "}")) {
                return InferredType::Dict;
            }

            if (begin + 1 < end && std::holds_alternative<LexerType::Keyword>(tokens[begin]) &&
                isOperator(tokens[begin + 1], "(")) {
                const auto& name = std::get<LexerType::Keyword>(tokens[begin]).name;

                if (name == "print" || name == "delayPrint") {
                    return InferredType::Nil;
                }

                auto function = functions.find(name);
                if (function != functions.end()) {
                    return function->second.returnType;
                }

                return InferredType::Unknown;
            }

            if (begin + 3 < end &&
                std::holds_alternative<LexerType::Keyword>(tokens[begin]) &&
                isOperator(tokens[begin + 1], ".") &&
                std::holds_alternative<LexerType::Keyword>(tokens[begin + 2]) &&
                isOperator(tokens[begin + 3], "(")) {
                const std::string name = std::get<LexerType::Keyword>(tokens[begin]).name + "." +
                                         std::get<LexerType::Keyword>(tokens[begin + 2]).name;
                auto function = functions.find(name);
                if (function != functions.end()) return function->second.returnType;
                return InferredType::Unknown;
            }

            InferredType literalType = inferLiteralType(tokens[begin]);
            if (literalType != InferredType::Unknown) return literalType;

            if (std::holds_alternative<LexerType::Keyword>(tokens[begin]) && begin + 1 == end) {
                const auto& name = std::get<LexerType::Keyword>(tokens[begin]).name;
                auto symbol = environment.find(name);
                if (symbol != environment.end()) return symbol->second;
            }

            return InferredType::Unknown;
        }

        InferredType inferExpressionInEnvironment(
            const std::vector<LexerType::Token>& tokens,
            const SymbolTable& environment
        ) const {
            return inferExpressionRange(tokens, 0, tokens.size(), environment);
        }

        static bool splitVariableDeclaration(
            const std::vector<LexerType::Token>& tokens,
            std::string& name,
            InferredType& declaredType,
            std::vector<LexerType::Token>& expression
        ) {
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
                if (std::holds_alternative<LexerType::Keyword>(tokens[i])) {
                    const auto& keyword = std::get<LexerType::Keyword>(tokens[i]).name;
                    if (keyword != "let" && keyword != "var" && keyword != "const" &&
                        keyword != "shared") {
                        nameIndex = i;
                        name = keyword;
                        break;
                    }
                }
            }

            if (nameIndex == tokens.size()) return false;

            if (nameIndex + 2 < equal && isOperator(tokens[nameIndex + 1], ":") &&
                std::holds_alternative<LexerType::Keyword>(tokens[nameIndex + 2])) {
                declaredType = parseTypeName(
                    std::get<LexerType::Keyword>(tokens[nameIndex + 2]).name
                );
            }

            if (equal < tokens.size()) {
                expression.assign(tokens.begin() + equal + 1, tokens.end());
            }

            return true;
        }

        void collectFunctions(
            const std::vector<TreeParser::NodeAndBlock>& blockNodes
        ) {
            for (const auto& node : blockNodes) {
                TreeParser::AstElementView element(node);
                if (element.isNode()) {
                    const auto& statement = element.node();
                    if (statement.type == NodeType::FUNC_FWD_DECL_STMT) {
                        const auto signature = ZeusParser::parseForwardFunctionSignature(
                            statement.tokens
                        );
                        if (signature.valid) {
                            FunctionInfo info;
                            info.name = signature.name;
                            info.returnType = parseTypeName(signature.returnTypeName);
                            info.returnTypeName = signature.returnTypeName;
                            info.dynamicReturn = signature.returnTypeName.empty();
                            info.returnsReference = parseTypeRef(
                                signature.returnTypeName
                            ).isReference;
                            for (const auto& parameter : signature.parameters) {
                                info.parameterTypes.push_back(
                                    parseTypeName(parameter.typeName)
                                );
                                info.parameterTypeNames.push_back(parameter.typeName);
                            }
                            functions[info.name] = std::move(info);
                        }
                    }
                    continue;
                }

                if (!element.isBlock()) continue;

                const auto* block = element.block();
                if (block == nullptr) continue;

                if (block->type == CodeBlockType::FUNC && !block->funcName.empty()) {
                    FunctionInfo info;
                    info.name = block->funcName;
                    info.returnTypeName = block->returnTypeName;
                    if (block->returnTypeName.empty() &&
                        !TreeParser::hasExplicitReturnStatement(block->nodes)) {
                        info.returnTypeName = "nil";
                        info.returnType = InferredType::Nil;
                    } else {
                        info.returnType = parseTypeName(block->returnTypeName);
                        info.dynamicReturn = block->returnTypeName.empty();
                    }
                    info.returnsReference = parseTypeRef(
                        info.returnTypeName
                    ).isReference;

                    for (const auto& typeName : block->paramTypeNames) {
                        info.parameterTypes.push_back(parseTypeName(typeName));
                        info.parameterTypeNames.push_back(typeName);
                    }

                    functions[info.name] = std::move(info);
                }

                collectFunctions(block->nodes);
            }
        }

        void inferBlock(
            const std::vector<TreeParser::NodeAndBlock>& blockNodes,
            SymbolTable environment,
            int& nodeIndex,
            FunctionInfo* currentFunction = nullptr
        ) {
            for (const auto& node : blockNodes) {
                ++nodeIndex;
                TreeParser::AstElementView element(node);

                if (element.isNode()) {
                    const auto& statement = element.node();
                    InferredType statementType = InferredType::Unknown;

                    if (statement.type == NodeType::VAR_DECL_STMT) {
                        std::string name;
                        InferredType declaredType = InferredType::Unknown;
                        std::vector<LexerType::Token> expression;

                        if (splitVariableDeclaration(statement.tokens, name, declaredType, expression)) {
                            InferredType expressionType = inferExpressionInEnvironment(expression, environment);
                            statementType = declaredType != InferredType::Unknown
                                ? declaredType
                                : expressionType;
                            environment[name] = statementType;

                            if (currentFunction == nullptr) {
                                symbols[name] = statementType;
                            }
                        }
                    } else if (statement.type == NodeType::RETURN_STMT) {
                        std::vector<LexerType::Token> expression;
                        if (statement.tokens.size() > 1) {
                            expression.assign(statement.tokens.begin() + 1, statement.tokens.end());
                        }

                        statementType = inferExpressionInEnvironment(expression, environment);
                        if (currentFunction != nullptr &&
                            !currentFunction->dynamicReturn &&
                            currentFunction->returnType == InferredType::Unknown) {
                            currentFunction->returnType = statementType;
                        }
                    } else if (statement.type == NodeType::EXP_STMT) {
                        statementType = inferExpressionInEnvironment(statement.tokens, environment);
                    }

                    TypeInfo info = TypeInfo::fromType(TypeRef(statementType));
                    inferredTypes.push_back({ nodeIndex, element.id(), statementType, info });
                    if (element.id() != TreeParser::InvalidNodeId) {
                        nodeTypeInfo[element.id()] = info;
                    }
                    continue;
                }

                const auto* block = element.block();
                if (block == nullptr) continue;

                if (block->type == CodeBlockType::FUNC) {
                    auto function = functions.find(block->funcName);
                    if (function == functions.end()) continue;

                    SymbolTable functionEnvironment = environment;
                    for (size_t i = 0; i < block->paramNames.size(); ++i) {
                        InferredType type = InferredType::Unknown;
                        if (i < block->paramTypeNames.size()) {
                            type = parseTypeName(block->paramTypeNames[i]);
                        }
                        functionEnvironment[block->paramNames[i]] = type;
                    }

                    inferBlock(block->nodes, functionEnvironment, nodeIndex, &function->second);

                    std::vector<TypeRef> parameterTypes;
                    parameterTypes.reserve(function->second.parameterTypes.size());
                    for (InferredType parameterType : function->second.parameterTypes) {
                        parameterTypes.push_back(TypeRef(parameterType));
                    }

                    TypeInfo functionInfo = TypeInfo::fromType(
                        makeFunctionType(
                            std::move(parameterTypes),
                            [&]() {
                                TypeRef result(function->second.returnType);
                                result.isReference = function->second.returnsReference;
                                return result;
                            }()
                        ),
                        false,
                        !block->returnTypeName.empty()
                    );
                    if (block->id != TreeParser::InvalidNodeId) {
                        nodeTypeInfo[block->id] = functionInfo;
                    }
                    continue;
                }

                inferBlock(block->nodes, environment, nodeIndex, currentFunction);
            }
        }

    public:
        explicit TypeInferer(const std::vector<TreeParser::NodeAndBlock>& nodes)
            : nodes(nodes) {}

        void infer() {
            inferredTypes.clear();
            diagnostics.clear();
            symbols.clear();
            functions.clear();
            nodeTypeInfo.clear();

            collectFunctions(nodes);
            for (const auto& external : externalFunctions) {
                functions.emplace(external.first, external.second);
            }

            int nodeIndex = -1;
            inferBlock(nodes, symbols, nodeIndex);
        }

        void registerExternalFunction(
            const std::string& name,
            const std::string& returnType,
            const std::vector<std::string>& parameterTypes
        ) {
            FunctionInfo info;
            info.name = name;
            info.returnTypeName = returnType;
            info.returnType = ZeusTypeInfer::parseTypeName(returnType);
            info.dynamicReturn = returnType.empty();
            info.returnsReference = ZeusTypeInfer::parseTypeRef(returnType).isReference;
            info.parameterTypes.reserve(parameterTypes.size());
            for (const auto& parameterType : parameterTypes) {
                info.parameterTypes.push_back(ZeusTypeInfer::parseTypeName(parameterType));
                info.parameterTypeNames.push_back(parameterType);
            }
            externalFunctions[name] = std::move(info);
        }

        InferredType inferExpression(
            const std::vector<LexerType::Token>& tokens,
            const std::unordered_map<std::string, InferredType>& environment
        ) const {
            return inferExpressionInEnvironment(tokens, environment);
        }

        InferredType inferExpression(
            const std::vector<LexerType::Token>& tokens,
            const TypeEnvironment& environment
        ) const {
            const SymbolTable legacyEnvironment = toLegacySymbolTable(environment);
            return inferExpressionInEnvironment(tokens, legacyEnvironment);
        }

        TypeInfo inferExpressionInfo(
            const std::vector<LexerType::Token>& tokens,
            const TypeEnvironment& environment
        ) const {
            return TypeInfo::fromType(TypeRef(inferExpression(tokens, environment)));
        }

        const std::vector<InferredNode>& getInferredTypes() const {
            return inferredTypes;
        }

        TypeInfo getTypeInfo(TreeParser::NodeId nodeId) const {
            auto it = nodeTypeInfo.find(nodeId);
            if (it == nodeTypeInfo.end()) return TypeInfo::unresolved();
            return it->second;
        }

        TypeInfo getTypeInfoByIndex(int nodeIndex) const {
            for (const auto& inferred : inferredTypes) {
                if (inferred.index == nodeIndex) return inferred.info;
            }
            return TypeInfo::unresolved();
        }

        const std::unordered_map<std::string, InferredType>& getSymbols() const {
            return symbols;
        }

        const std::unordered_map<std::string, FunctionInfo>& getFunctions() const {
            return functions;
        }

        const std::vector<InferenceDiagnostic>& getDiagnostics() const {
            return diagnostics;
        }
    };

}
