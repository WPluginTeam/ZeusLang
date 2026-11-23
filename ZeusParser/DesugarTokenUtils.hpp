#pragma once

#include <atomic>
#include <string>
#include <variant>
#include <vector>
#include "LexerType.hpp"

namespace ZeusParser {

namespace DesugarDetail {

inline bool isOp(const LexerType::Token& tk, const std::string& sym) {
    return std::holds_alternative<LexerType::Operator>(tk) &&
           std::get<LexerType::Operator>(tk).symbol == sym;
}

inline bool isKw(const LexerType::Token& tk, const std::string& name) {
    return std::holds_alternative<LexerType::Keyword>(tk) &&
           std::get<LexerType::Keyword>(tk).name == name;
}

inline int getTokenLine(const LexerType::Token& tk) {
    return std::visit([](const auto& t) { return t.line; }, tk);
}

inline int getTokenCol(const LexerType::Token& tk) {
    return std::visit([](const auto& t) { return t.column; }, tk);
}

inline LexerType::Operator makeOp(const std::string& sym, int line = 0, int col = 0) {
    LexerType::Operator op(sym);
    op.line = line;
    op.column = col;
    return op;
}

inline LexerType::Keyword makeKw(const std::string& name, int line = 0, int col = 0) {
    LexerType::Keyword kw(name);
    kw.line = line;
    kw.column = col;
    return kw;
}

inline LexerType::Value makeStringValue(const std::string& text, int line = 0, int col = 0) {
    LexerType::Value value(text);
    value.line = line;
    value.column = col;
    return value;
}

inline LexerType::Value makeIntValue(int number, int line = 0, int col = 0) {
    LexerType::Value value(number);
    value.line = line;
    value.column = col;
    return value;
}

// 两个相邻 token 是否在同一行紧挨着（用于把 '+' '=' 识别为 '+='，而不是 '+ ='）
inline bool areAdjacent(const LexerType::Token& left, const LexerType::Token& right) {
    return getTokenLine(left) == getTokenLine(right) &&
           getTokenCol(left) + 1 == getTokenCol(right);
}

inline bool isOpenBracket(const LexerType::Token& tk) {
    return isOp(tk, "(") || isOp(tk, "[") || isOp(tk, "{");
}

inline bool isCloseBracket(const LexerType::Token& tk) {
    return isOp(tk, ")") || isOp(tk, "]") || isOp(tk, "}");
}

// 返回以 tokens[closeIdx] 这个闭括号为结尾的配对开括号下标；找不到时返回 tokens.size()
inline size_t findMatchingOpenBackward(const std::vector<LexerType::Token>& tokens, size_t closeIdx) {
    int depth = 0;
    for (size_t idx = closeIdx + 1; idx > 0; --idx) {
        const auto& tk = tokens[idx - 1];
        if (isCloseBracket(tk)) depth++;
        else if (isOpenBracket(tk) && --depth == 0) return idx - 1;
    }
    return tokens.size();
}

// 从 start 向后扫描到深度 0 的 ';'（不含）；遇到未配对的闭括号或到达末尾时返回 tokens.size()
inline size_t findStatementEndForward(const std::vector<LexerType::Token>& tokens, size_t start) {
    int depth = 0;
    for (size_t idx = start; idx < tokens.size(); ++idx) {
        const auto& tk = tokens[idx];
        if (isOpenBracket(tk)) depth++;
        else if (isCloseBracket(tk)) {
            if (depth == 0) return tokens.size();
            depth--;
        } else if (depth == 0 && isOp(tk, ";")) return idx;
    }
    return tokens.size();
}

// 为脱糖生成的临时变量提供全局唯一的编号
inline int nextDesugarTempId() {
    static std::atomic<int> counter{0};
    return counter.fetch_add(1);
}

} // namespace DesugarDetail

} // namespace ZeusParser
