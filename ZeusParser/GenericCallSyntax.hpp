#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "LexerType.hpp"
#include "../ZeusTyping/GenericTypeName.hpp"

namespace ZeusParser {

    inline bool isGenericCallOperator(
        const LexerType::Token& token,
        const std::string& symbol
    ) {
        const auto* op = std::get_if<LexerType::Operator>(&token);
        return op != nullptr && op->symbol == symbol;
    }

    inline bool parseGenericTypeArguments(
        const std::vector<LexerType::Token>& tokens,
        std::size_t start,
        std::size_t& afterClose,
        std::vector<std::string>& typeArguments
    );

    inline bool parseGenericType(
        const std::vector<LexerType::Token>& tokens,
        std::size_t& position,
        std::string& typeName
    );

    inline bool parseGenericTypeName(
        const std::vector<LexerType::Token>& tokens,
        std::size_t& position,
        std::string& typeName
    ) {
        if (position >= tokens.size()) return false;
        const auto* first = std::get_if<LexerType::Keyword>(&tokens[position]);
        if (first == nullptr || first->name.empty()) return false;
        typeName = first->name;
        ++position;

        while (position + 1 < tokens.size() &&
               isGenericCallOperator(tokens[position], ".")) {
            const auto* member =
                std::get_if<LexerType::Keyword>(&tokens[position + 1]);
            if (member == nullptr || member->name.empty()) return false;
            typeName += "." + member->name;
            position += 2;
        }

        return true;
    }

    inline bool parseGenericTypeArguments(
        const std::vector<LexerType::Token>& tokens,
        std::size_t start,
        std::size_t& afterClose,
        std::vector<std::string>& typeArguments
    ) {
        if (start >= tokens.size() ||
            !isGenericCallOperator(tokens[start], "<")) {
            return false;
        }

        std::size_t cursor = start + 1;
        std::vector<std::string> parsed;
        while (true) {
            std::string typeName;
            if (!parseGenericType(tokens, cursor, typeName)) return false;
            parsed.push_back(std::move(typeName));
            if (cursor >= tokens.size()) return false;
            if (isGenericCallOperator(tokens[cursor], ",")) {
                ++cursor;
                continue;
            }
            if (!isGenericCallOperator(tokens[cursor], ">")) return false;
            ++cursor;
            break;
        }

        afterClose = cursor;
        typeArguments = std::move(parsed);
        return true;
    }

    inline bool parseGenericType(
        const std::vector<LexerType::Token>& tokens,
        std::size_t& position,
        std::string& typeName
    ) {
        const std::size_t start = position;
        const bool isReference = position < tokens.size() &&
            isGenericCallOperator(tokens[position], "&");
        if (isReference) ++position;

        std::string baseName;
        if (!parseGenericTypeName(tokens, position, baseName)) {
            position = start;
            return false;
        }

        std::size_t afterArguments = position;
        std::vector<std::string> arguments;
        if (parseGenericTypeArguments(tokens, position, afterArguments, arguments)) {
            baseName = ZeusTypeInfer::genericSpecializationName(baseName, arguments);
            position = afterArguments;
        }
        typeName = (isReference ? "&" : "") + baseName;
        return true;
    }

    inline bool consumeGenericCallSuffix(
        const std::vector<LexerType::Token>& tokens,
        std::size_t& position,
        std::vector<std::string>& typeArguments
    ) {
        std::size_t afterClose = position;
        std::vector<std::string> parsed;
        if (!parseGenericTypeArguments(tokens, position, afterClose, parsed) ||
            afterClose >= tokens.size() ||
            !isGenericCallOperator(tokens[afterClose], "(")) {
            return false;
        }
        position = afterClose;
        typeArguments = std::move(parsed);
        return true;
    }

    inline bool findGenericCallTypeArguments(
        const std::vector<LexerType::Token>& tokens,
        std::size_t openParen,
        std::size_t& calleeNameIndex,
        std::vector<std::string>& typeArguments
    ) {
        if (openParen == 0 || openParen >= tokens.size() ||
            !isGenericCallOperator(tokens[openParen], "(")) {
            return false;
        }

        std::size_t start = tokens.size();
        std::size_t depth = 0;
        for (std::size_t cursor = openParen; cursor > 0; --cursor) {
            const std::size_t index = cursor - 1;
            if (isGenericCallOperator(tokens[index], ">")) {
                ++depth;
            } else if (isGenericCallOperator(tokens[index], "<")) {
                if (depth == 0) return false;
                --depth;
                if (depth == 0) {
                    start = index;
                    break;
                }
            }
        }
        if (start == tokens.size() || start == 0) return false;

        std::size_t afterClose = start;
        std::vector<std::string> parsed;
        if (!parseGenericTypeArguments(tokens, start, afterClose, parsed) ||
            afterClose != openParen ||
            !std::holds_alternative<LexerType::Keyword>(tokens[start - 1])) {
            return false;
        }

        calleeNameIndex = start - 1;
        typeArguments = std::move(parsed);
        return true;
    }

}
