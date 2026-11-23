#pragma once

#include <string>
#include <vector>
#include "DesugarTokenUtils.hpp"

namespace ZeusParser {

namespace DesugarDetail {

// -----------------------------------------------------------------------------
// 解构声明脱糖（仅语句级）:
//    let a, b = list;
// =>
//    let __zeus_destruct_N = list;
//    if (!(__zeus_destruct_N.length() == 2)) { throw "..."; }
//    let a = __zeus_destruct_N[0];
//    let b = __zeus_destruct_N[1];
//
// 名字为 '_' 的位置只做长度占位，不生成绑定。 'var' 与 'let' 均受支持。
// -----------------------------------------------------------------------------

struct DestructurePattern {
    std::vector<std::string> names;
    size_t assignIdx = 0; // 对应 '=' 的 token 下标
};

// 是否位于语句起点（前一个 token 为空、';'、'{'、'}'）
inline bool isStatementBoundary(const std::vector<LexerType::Token>& out) {
    if (out.empty()) return true;
    return isOp(out.back(), ";") || isOp(out.back(), "{") || isOp(out.back(), "}");
}

// 匹配 'let|var IDENT (, IDENT)+ ='，成功时填充 pattern
inline bool matchDestructurePattern(
    const std::vector<LexerType::Token>& tokens,
    size_t i,
    DestructurePattern& pattern
) {
    if (!(isKw(tokens[i], "let") || isKw(tokens[i], "var"))) return false;

    pattern.names.clear();
    size_t idx = i + 1;
    while (idx < tokens.size() && std::holds_alternative<LexerType::Keyword>(tokens[idx])) {
        pattern.names.push_back(std::get<LexerType::Keyword>(tokens[idx]).name);
        idx++;
        if (idx < tokens.size() && isOp(tokens[idx], ",")) {
            idx++;
            continue;
        }
        break;
    }

    if (pattern.names.size() < 2) return false;
    if (idx >= tokens.size() || !isOp(tokens[idx], "=")) return false;
    // 排除 '==': 词法阶段 '==' 是独立记号，这里的 '=' 一定是赋值
    pattern.assignIdx = idx;
    return true;
}

inline void appendTokens(std::vector<LexerType::Token>& out, std::initializer_list<LexerType::Token> items) {
    out.insert(out.end(), items.begin(), items.end());
}

// let T = rhs;
inline void appendDestructureTemp(
    std::vector<LexerType::Token>& out,
    const LexerType::Token& declKw,
    const std::string& temp,
    const std::vector<LexerType::Token>& rhs,
    int line,
    int col
) {
    appendTokens(out, {declKw, makeKw(temp, line, col), makeOp("=", line, col)});
    out.insert(out.end(), rhs.begin(), rhs.end());
    out.push_back(makeOp(";", line, col));
}

// if (!(T.length() == N)) { throw "..."; }
inline void appendDestructureLengthCheck(
    std::vector<LexerType::Token>& out,
    const std::string& temp,
    size_t count,
    int line,
    int col
) {
    const std::string message = "destructure error: expected " + std::to_string(count) + " values";

    appendTokens(out, {
        makeKw("if", line, col), makeOp("(", line, col), makeOp("!", line, col), makeOp("(", line, col),
        makeKw(temp, line, col), makeOp(".", line, col), makeKw("length", line, col),
        makeOp("(", line, col), makeOp(")", line, col),
        makeOp("==", line, col), makeIntValue(static_cast<int>(count), line, col),
        makeOp(")", line, col), makeOp(")", line, col),
        makeOp("{", line, col), makeKw("throw", line, col), makeStringValue(message, line, col),
        makeOp(";", line, col), makeOp("}", line, col)
    });
}

// let name = T[index];
inline void appendDestructureBinding(
    std::vector<LexerType::Token>& out,
    const LexerType::Token& declKw,
    const std::string& name,
    const std::string& temp,
    size_t index,
    int line,
    int col
) {
    appendTokens(out, {
        declKw, makeKw(name, line, col), makeOp("=", line, col),
        makeKw(temp, line, col), makeOp("[", line, col),
        makeIntValue(static_cast<int>(index), line, col), makeOp("]", line, col),
        makeOp(";", line, col)
    });
}

inline std::vector<LexerType::Token> desugarDestructure(
    const std::vector<LexerType::Token>& tokens
) {
    std::vector<LexerType::Token> out;
    out.reserve(tokens.size());

    for (size_t i = 0; i < tokens.size(); ++i) {
        DestructurePattern pattern;
        if (!isStatementBoundary(out) || !matchDestructurePattern(tokens, i, pattern)) {
            out.push_back(tokens[i]);
            continue;
        }

        size_t end = findStatementEndForward(tokens, pattern.assignIdx + 1);
        if (end >= tokens.size() || end == pattern.assignIdx + 1) {
            out.push_back(tokens[i]);
            continue;
        }

        const int line = getTokenLine(tokens[i]);
        const int col = getTokenCol(tokens[i]);
        const std::string temp = "__zeus_destruct_" + std::to_string(nextDesugarTempId());
        std::vector<LexerType::Token> rhs(tokens.begin() + pattern.assignIdx + 1, tokens.begin() + end);

        appendDestructureTemp(out, tokens[i], temp, desugarDestructure(rhs), line, col);
        appendDestructureLengthCheck(out, temp, pattern.names.size(), line, col);
        for (size_t n = 0; n < pattern.names.size(); ++n) {
            if (pattern.names[n] == "_") continue;
            appendDestructureBinding(out, tokens[i], pattern.names[n], temp, n, line, col);
        }

        i = end; // 跳过原语句（包含 ';'）；脱糖结果已自带分号
    }

    return out;
}

} // namespace DesugarDetail

} // namespace ZeusParser
