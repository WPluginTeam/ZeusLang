#pragma once

#include <cstddef>
#include <string>
#include <variant>
#include <vector>

#include "../ZeusParser/LexerType.hpp"

namespace ZeusTypeInfer {

    // Recognize the declared receiver, including aliases and harmless grouping.
    // Returning fields, parameters, or locally created objects keeps value rules.
    inline bool isDirectReceiverReturn(
        const std::vector<LexerType::Token>& expression,
        const std::string& receiverAlias
    ) {
        if (receiverAlias.empty()) return false;
        size_t begin = 0;
        size_t end = expression.size();
        auto isOperator = [&](size_t index, const char* symbol) {
            const auto* op = std::get_if<LexerType::Operator>(&expression[index]);
            return op && op->symbol == symbol;
        };
        while (end > begin + 2 && isOperator(begin, "(") && isOperator(end - 1, ")")) {
            // Only a pair surrounding the whole expression may be removed.
            int depth = 0;
            for (size_t i = begin; i < end - 1; ++i) {
                if (isOperator(i, "(")) ++depth;
                if (isOperator(i, ")")) --depth;
                if (depth == 0) return false;
            }
            ++begin;
            --end;
        }
        if (end != begin + 1) return false;
        const auto* name = std::get_if<LexerType::Keyword>(&expression[begin]);
        return name && name->name == receiverAlias;
    }

}
