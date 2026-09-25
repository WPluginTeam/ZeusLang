#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "LexerType.hpp"

namespace ZeusParser {

    struct FunctionParameterSyntax {
        std::string name;
        std::string typeName;
    };

    struct FunctionDecoratorSyntax {
        std::string name;
        std::string argument;
        bool hasArgument = false;
    };

    struct FunctionDecoratorPrefixSyntax {
        std::vector<FunctionDecoratorSyntax> decorators;
        size_t nextToken = 0;
        bool valid = true;
    };

    struct FunctionSignatureSyntax {
        std::string name;
        std::vector<FunctionParameterSyntax> parameters;
        std::string returnTypeName;
        std::vector<FunctionDecoratorSyntax> decorators;
        std::string nativeSymbol;
        bool valid = false;
    };

    inline bool isOperator(
        const LexerType::Token& token,
        const std::string& symbol
    ) {
        return std::holds_alternative<LexerType::Operator>(token) &&
               std::get<LexerType::Operator>(token).symbol == symbol;
    }

    inline FunctionDecoratorPrefixSyntax parseFunctionDecoratorPrefix(
        const std::vector<LexerType::Token>& tokens
    ) {
        FunctionDecoratorPrefixSyntax result;
        size_t position = 0;

        while (position < tokens.size() && isOperator(tokens[position], "@")) {
            ++position;
            if (position >= tokens.size() ||
                !std::holds_alternative<LexerType::Keyword>(tokens[position])) {
                result.valid = false;
                return result;
            }

            FunctionDecoratorSyntax decorator;
            decorator.name = std::get<LexerType::Keyword>(tokens[position++]).name;
            if (position < tokens.size() && isOperator(tokens[position], "(")) {
                ++position;
                if (position >= tokens.size() ||
                    !std::holds_alternative<LexerType::Value>(tokens[position])) {
                    result.valid = false;
                    return result;
                }
                const auto& value = std::get<LexerType::Value>(tokens[position++]);
                if (!std::holds_alternative<std::string>(value.data)) {
                    result.valid = false;
                    return result;
                }
                decorator.argument = std::get<std::string>(value.data);
                decorator.hasArgument = true;
                if (position >= tokens.size() || !isOperator(tokens[position], ")")) {
                    result.valid = false;
                    return result;
                }
                ++position;
            }
            result.decorators.push_back(std::move(decorator));
        }

        result.nextToken = position;
        return result;
    }

    inline bool parseTypeName(
        const std::vector<LexerType::Token>& tokens,
        size_t& position,
        size_t end,
        std::string& typeName
    ) {
        bool isReference = position < end && isOperator(tokens[position], "&");
        if (isReference) ++position;
        if (position >= end ||
            !std::holds_alternative<LexerType::Keyword>(tokens[position])) {
            if (isReference) --position;
            return false;
        }

        typeName = std::get<LexerType::Keyword>(tokens[position++]).name;
        if (isReference) typeName.insert(typeName.begin(), '&');
        return true;
    }

    inline FunctionSignatureSyntax parseForwardFunctionSignature(
        const std::vector<LexerType::Token>& tokens
    ) {
        FunctionSignatureSyntax signature;
        size_t functionKeyword = tokens.size();
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (std::holds_alternative<LexerType::Keyword>(tokens[i]) &&
                std::get<LexerType::Keyword>(tokens[i]).name == "func") {
                functionKeyword = i;
                break;
            }
        }
        if (functionKeyword == tokens.size() || functionKeyword + 1 >= tokens.size() ||
            !std::holds_alternative<LexerType::Keyword>(tokens[functionKeyword + 1])) {
            return signature;
        }

        const std::vector<LexerType::Token> prefixTokens(
            tokens.begin(), tokens.begin() + functionKeyword
        );
        const auto prefix = parseFunctionDecoratorPrefix(prefixTokens);
        if (!prefix.valid) return signature;
        size_t prefixEnd = prefix.nextToken;
        if (prefixEnd < prefixTokens.size() &&
            std::holds_alternative<LexerType::Keyword>(prefixTokens[prefixEnd]) &&
            std::get<LexerType::Keyword>(prefixTokens[prefixEnd]).name == "decl") {
            ++prefixEnd;
        }
        if (prefixEnd != prefixTokens.size()) return signature;
        signature.decorators = prefix.decorators;
        for (const auto& decorator : signature.decorators) {
            if (decorator.name == "native") {
                if (!decorator.hasArgument || decorator.argument.empty() ||
                    !signature.nativeSymbol.empty()) {
                    return FunctionSignatureSyntax{};
                }
                signature.nativeSymbol = decorator.argument;
            } else {
                return FunctionSignatureSyntax{};
            }
        }

        signature.name = std::get<LexerType::Keyword>(tokens[functionKeyword + 1]).name;
        size_t openParen = functionKeyword + 2;
        while (openParen < tokens.size() && !isOperator(tokens[openParen], "(")) ++openParen;
        if (openParen == tokens.size()) return signature;

        size_t closeParen = openParen + 1;
        int parenDepth = 1;
        for (; closeParen < tokens.size(); ++closeParen) {
            if (isOperator(tokens[closeParen], "(")) ++parenDepth;
            else if (isOperator(tokens[closeParen], ")") && --parenDepth == 0) break;
        }
        if (closeParen == tokens.size()) return signature;

        size_t segmentBegin = openParen + 1;
        int nestedDepth = 0;
        auto appendParameter = [&](size_t begin, size_t end) {
            if (begin >= end) return;

            FunctionParameterSyntax parameter;
            size_t colon = end;
            for (size_t i = begin; i < end; ++i) {
                if (isOperator(tokens[i], ":") && colon == end) {
                    colon = i;
                    continue;
                }
                if (std::holds_alternative<LexerType::Keyword>(tokens[i])) {
                    const std::string& keyword =
                        std::get<LexerType::Keyword>(tokens[i]).name;
                    if (parameter.name.empty()) parameter.name = keyword;
                    else if (colon < end && parameter.typeName.empty()) {
                        parameter.typeName = keyword;
                    }
                }
            }

            if (colon < end) {
                size_t typePosition = colon + 1;
                parseTypeName(tokens, typePosition, end, parameter.typeName);
            }
            if (!parameter.name.empty() && parameter.name != "*") {
                signature.parameters.push_back(std::move(parameter));
            }
        };

        for (size_t i = segmentBegin; i <= closeParen; ++i) {
            const bool atEnd = i == closeParen;
            const bool atComma = !atEnd && nestedDepth == 0 && isOperator(tokens[i], ",");
            if (atEnd || atComma) {
                appendParameter(segmentBegin, i);
                segmentBegin = i + 1;
                continue;
            }
            if (isOperator(tokens[i], "(") || isOperator(tokens[i], "[") ||
                isOperator(tokens[i], "{")) {
                ++nestedDepth;
            } else if (isOperator(tokens[i], ")") || isOperator(tokens[i], "]") ||
                       isOperator(tokens[i], "}")) {
                if (nestedDepth > 0) --nestedDepth;
            }
        }

        size_t returnPosition = closeParen + 1;
        if (returnPosition < tokens.size() && isOperator(tokens[returnPosition], "[")) {
            int captureDepth = 1;
            ++returnPosition;
            while (returnPosition < tokens.size() && captureDepth > 0) {
                if (isOperator(tokens[returnPosition], "[")) ++captureDepth;
                else if (isOperator(tokens[returnPosition], "]")) --captureDepth;
                ++returnPosition;
            }
        }

        if (returnPosition < tokens.size() && isOperator(tokens[returnPosition], "->")) {
            ++returnPosition;
            parseTypeName(tokens, returnPosition, tokens.size(), signature.returnTypeName);
        }

        signature.valid = true;
        return signature;
    }

}
