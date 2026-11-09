#pragma once

#include <string>
#include <vector>
#include <variant>
#include <atomic>
#include "LexerType.hpp"
#include "WhenSyntaxDesugar.hpp"
#include "../ZeusDiagnostics.hpp"

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

// 识别严格的 '..' 或 '..=' 范围运算符（排除 '...' 变长参数）
inline bool isRangeOperator(
    const std::vector<LexerType::Token>& tokens,
    size_t i,
    size_t& dotLen,
    bool& inclusive
) {
    if (i + 1 >= tokens.size()) return false;
    if (!isOp(tokens[i], ".") || !isOp(tokens[i + 1], ".")) return false;
    // 排除 '...' 或更多点
    if (i > 0 && isOp(tokens[i - 1], ".")) return false;
    if (i + 2 < tokens.size() && isOp(tokens[i + 2], ".")) return false;

    dotLen = 2;
    inclusive = false;
    if (i + 2 < tokens.size() && isOp(tokens[i + 2], "=")) {
        dotLen = 3;
        inclusive = true;
    }
    return true;
}

// -----------------------------------------------------------------------------
// 1. 字典无值 key 展开 / Set 字面量: {x, y} => {x: nil, y: nil}
// -----------------------------------------------------------------------------
inline std::vector<LexerType::Token> desugarDictShorthand(
    const std::vector<LexerType::Token>& tokens
) {
    std::vector<LexerType::Token> out;
    out.reserve(tokens.size());

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!isOp(tokens[i], "{")) {
            out.push_back(tokens[i]);
            continue;
        }

        // 判断当前 '{' 是否处于字典字面量上下文（赋值右侧、传参、列表元素、条件等）
        bool isDictContext = false;
        if (!out.empty()) {
            const auto& prev = out.back();
            if (isOp(prev, "=") || isOp(prev, "(") || isOp(prev, "[") ||
                isOp(prev, ",") || isOp(prev, ":") || isOp(prev, "?") ||
                isOp(prev, "==") || isOp(prev, "!=") || isOp(prev, "!") ||
                isOp(prev, "+") || isOp(prev, "-") || isOp(prev, "*") ||
                isOp(prev, "/") || isOp(prev, "%") ||
                isOp(prev, "and") || isOp(prev, "or") ||
                isKw(prev, "return") || isKw(prev, "in") || isKw(prev, "throw")) {
                isDictContext = true;
            }
        }

        if (!isDictContext) {
            out.push_back(tokens[i]);
            continue;
        }

        // 寻找配对的 '}'，同时检查大括号最外层是否包含分号 ';'
        // 若包含 ';' 则必然是语句代码块而非字典字面量，必须原样跳过
        size_t closeIdx = i + 1;
        int depth = 1;
        bool hasSemicolonAtDepth1 = false;
        while (closeIdx < tokens.size() && depth > 0) {
            if (isOp(tokens[closeIdx], "{")) depth++;
            else if (isOp(tokens[closeIdx], "}")) depth--;
            else if (depth == 1 && isOp(tokens[closeIdx], ";")) {
                hasSemicolonAtDepth1 = true;
            }
            if (depth == 0) break;
            closeIdx++;
        }

        if (depth > 0 || closeIdx >= tokens.size() || hasSemicolonAtDepth1) {
            out.push_back(tokens[i]);
            continue;
        }

        out.push_back(tokens[i]); // 输出 '{'
        
        int innerBraceDepth = 0;
        int parenDepth = 0;
        int bracketDepth = 0;
        bool hasColonAtDepth0 = false;
        std::vector<LexerType::Token> entryTokens;

        for (size_t j = i + 1; j <= closeIdx; ++j) {
            bool isEnd = (j == closeIdx);
            bool isComma = (!isEnd && isOp(tokens[j], ",") && innerBraceDepth == 0 && parenDepth == 0 && bracketDepth == 0);

            if (!isEnd && !isComma) {
                if (isOp(tokens[j], "{")) innerBraceDepth++;
                else if (isOp(tokens[j], "}")) innerBraceDepth--;
                else if (isOp(tokens[j], "(")) parenDepth++;
                else if (isOp(tokens[j], ")")) parenDepth--;
                else if (isOp(tokens[j], "[")) bracketDepth++;
                else if (isOp(tokens[j], "]")) bracketDepth--;
                else if (isOp(tokens[j], ":") && innerBraceDepth == 0 && parenDepth == 0 && bracketDepth == 0) {
                    hasColonAtDepth0 = true;
                }
                entryTokens.push_back(tokens[j]);
            } else {
                if (!entryTokens.empty()) {
                    auto processedEntry = desugarDictShorthand(entryTokens);
                    out.insert(out.end(), processedEntry.begin(), processedEntry.end());

                    if (!hasColonAtDepth0) {
                        int line = getTokenLine(entryTokens.back());
                        int col = getTokenCol(entryTokens.back()) + 1;
                        out.push_back(makeOp(":", line, col));
                        out.push_back(makeKw("nil", line, col + 1));
                    }
                    entryTokens.clear();
                }

                if (isComma) {
                    out.push_back(tokens[j]); // 输出 ','
                }
                hasColonAtDepth0 = false;
            }
        }

        out.push_back(tokens[closeIdx]); // 输出 '}'
        i = closeIdx;
    }

    return out;
}

// -----------------------------------------------------------------------------
// 2. 范围关系测试与独立范围表达式脱糖:
//    - target in start..end   => ((target >= (start)) and (target < (end)))
//    - target in start..=end  => ((target >= (start)) and (target <= (end)))
//    - start..end             => range(start, end)
//    - start..=end            => range(start, end, 1, true)
// -----------------------------------------------------------------------------
inline std::vector<LexerType::Token> desugarRangeTokens(
    const std::vector<LexerType::Token>& tokens
) {
    std::vector<LexerType::Token> out;
    out.reserve(tokens.size());

    int forParenDepth = 0;
    int forBracketDepth = 0;
    bool inForHeader = false;

    for (size_t i = 0; i < tokens.size(); ++i) {
        // 跳过已有的 for (...) [ bindings ] 循环头部
        if (isKw(tokens[i], "for")) {
            inForHeader = true;
            forParenDepth = 0;
            forBracketDepth = 0;
            out.push_back(tokens[i]);
            continue;
        }
        if (inForHeader) {
            if (isOp(tokens[i], "(")) forParenDepth++;
            else if (isOp(tokens[i], ")")) {
                forParenDepth--;
            } else if (isOp(tokens[i], "[")) forBracketDepth++;
            else if (isOp(tokens[i], "]")) forBracketDepth--;
            else if (isOp(tokens[i], "{") && forParenDepth == 0 && forBracketDepth == 0) {
                inForHeader = false;
            }
            out.push_back(tokens[i]);
            continue;
        }

        // 模式 A: target in start..end 或 target in start..=end
        if (isKw(tokens[i], "in")) {
            size_t dotPos = 0;
            size_t dotLen = 0;
            bool inclusive = false;
            bool foundDotDot = false;
            int depth = 0;
            for (size_t k = i + 1; k < tokens.size(); ++k) {
                if (isOp(tokens[k], "(") || isOp(tokens[k], "[") || isOp(tokens[k], "{")) depth++;
                else if (isOp(tokens[k], ")") || isOp(tokens[k], "]") || isOp(tokens[k], "}")) {
                    depth--;
                    if (depth < 0) break;
                } else if (depth == 0) {
                    if (isOp(tokens[k], ";") || isOp(tokens[k], ",") ||
                        isOp(tokens[k], "and") || isOp(tokens[k], "or") ||
                        isOp(tokens[k], ":")) {
                        break;
                    }
                    if (isRangeOperator(tokens, k, dotLen, inclusive)) {
                        dotPos = k;
                        foundDotDot = true;
                        break;
                    }
                }
            }

            if (foundDotDot && dotPos > i + 1) {
                std::vector<LexerType::Token> startTokens(tokens.begin() + i + 1, tokens.begin() + dotPos);

                size_t endIdx = dotPos + dotLen;
                int endDepth = 0;
                while (endIdx < tokens.size()) {
                    if (isOp(tokens[endIdx], "(") || isOp(tokens[endIdx], "[") || isOp(tokens[endIdx], "{")) endDepth++;
                    else if (isOp(tokens[endIdx], ")") || isOp(tokens[endIdx], "]") || isOp(tokens[endIdx], "}")) {
                        if (endDepth == 0) break;
                        endDepth--;
                    } else if (endDepth == 0) {
                        if (isOp(tokens[endIdx], ";") || isOp(tokens[endIdx], ",") ||
                            isOp(tokens[endIdx], "and") || isOp(tokens[endIdx], "or") ||
                            isOp(tokens[endIdx], ":")) {
                            break;
                        }
                    }
                    endIdx++;
                }

                if (endIdx > dotPos + dotLen) {
                    std::vector<LexerType::Token> endTokens(tokens.begin() + dotPos + dotLen, tokens.begin() + endIdx);

                    // 向前提取 targetTokens
                    std::vector<LexerType::Token> targetTokens;
                    int tDepth = 0;
                    while (!out.empty()) {
                        const auto& tk = out.back();
                        if (isOp(tk, ")") || isOp(tk, "]") || isOp(tk, "}")) tDepth++;
                        else if (isOp(tk, "(") || isOp(tk, "[") || isOp(tk, "{")) {
                            if (tDepth == 0) break;
                            tDepth--;
                        } else if (tDepth == 0) {
                            if (isOp(tk, ";") || isOp(tk, ",") || isOp(tk, "=") ||
                                isOp(tk, "and") || isOp(tk, "or") ||
                                isKw(tk, "if") || isKw(tk, "while") || isKw(tk, "return")) {
                                break;
                            }
                        }
                        targetTokens.push_back(tk);
                        out.pop_back();
                    }
                    std::reverse(targetTokens.begin(), targetTokens.end());

                    int line = getTokenLine(tokens[i]);
                    int col = getTokenCol(tokens[i]);

                    // 如果 target 为单一标识符或字面量，直接展开为 ((x >= (start)) and (x < (end)))
                    // 共有 5 对括号，严格对称闭合
                    if (targetTokens.size() == 1) {
                        out.push_back(makeOp("(", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.insert(out.end(), targetTokens.begin(), targetTokens.end());
                        out.push_back(makeOp(">=", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.insert(out.end(), startTokens.begin(), startTokens.end());
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp(")", line, col));

                        out.push_back(makeOp("and", line, col));

                        out.push_back(makeOp("(", line, col));
                        out.insert(out.end(), targetTokens.begin(), targetTokens.end());
                        out.push_back(makeOp(inclusive ? "<=" : "<", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.insert(out.end(), endTokens.begin(), endTokens.end());
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp(")", line, col));
                    } else {
                        // 复合表达式：生成立即执行闭包 IIFE，随后由匿名函数脱糖器统一提升
                        // ((func(__r_target) { return ((__r_target >= (start)) and (__r_target < (end))); })(target))
                        static std::atomic<uint64_t> s_rangeClosureCounter{0};
                        std::string targetVar = "__r_val_" + std::to_string(++s_rangeClosureCounter);

                        out.push_back(makeOp("(", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.push_back(makeKw("func", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.push_back(makeKw(targetVar, line, col));
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp("{", line, col));
                        out.push_back(makeKw("return", line, col));

                        out.push_back(makeOp("(", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.push_back(makeKw(targetVar, line, col));
                        out.push_back(makeOp(">=", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.insert(out.end(), startTokens.begin(), startTokens.end());
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp(")", line, col));

                        out.push_back(makeOp("and", line, col));

                        out.push_back(makeOp("(", line, col));
                        out.push_back(makeKw(targetVar, line, col));
                        out.push_back(makeOp(inclusive ? "<=" : "<", line, col));
                        out.push_back(makeOp("(", line, col));
                        out.insert(out.end(), endTokens.begin(), endTokens.end());
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp(")", line, col));

                        out.push_back(makeOp(";", line, col));
                        out.push_back(makeOp("}", line, col));
                        out.push_back(makeOp(")", line, col));

                        out.push_back(makeOp("(", line, col));
                        out.insert(out.end(), targetTokens.begin(), targetTokens.end());
                        out.push_back(makeOp(")", line, col));
                        out.push_back(makeOp(")", line, col));
                    }

                    i = endIdx - 1;
                    continue;
                }
            }
        }

        // 模式 B: 独立范围表达式 start..end 或 start..=end => range(start, end[, 1, true])
        size_t dotLen = 0;
        bool inclusive = false;
        if (isRangeOperator(tokens, i, dotLen, inclusive)) {
            size_t dotPos = i;
            size_t endIdx = dotPos + dotLen;
            int endDepth = 0;
            while (endIdx < tokens.size()) {
                if (isOp(tokens[endIdx], "(") || isOp(tokens[endIdx], "[") || isOp(tokens[endIdx], "{")) endDepth++;
                else if (isOp(tokens[endIdx], ")") || isOp(tokens[endIdx], "]") || isOp(tokens[endIdx], "}")) {
                    if (endDepth == 0) break;
                    endDepth--;
                } else if (endDepth == 0) {
                    if (isOp(tokens[endIdx], ";") || isOp(tokens[endIdx], ",") ||
                        isOp(tokens[endIdx], "and") || isOp(tokens[endIdx], "or") ||
                        isOp(tokens[endIdx], ":")) {
                        break;
                    }
                }
                endIdx++;
            }

            if (endIdx > dotPos + dotLen) {
                // 向前提取 startTokens
                std::vector<LexerType::Token> startTokens;
                int sDepth = 0;
                while (!out.empty()) {
                    const auto& tk = out.back();
                    if (isOp(tk, ")") || isOp(tk, "]") || isOp(tk, "}")) sDepth++;
                    else if (isOp(tk, "(") || isOp(tk, "[") || isOp(tk, "{")) {
                        if (sDepth == 0) break;
                        sDepth--;
                    } else if (sDepth == 0) {
                        if (isOp(tk, ";") || isOp(tk, ",") || isOp(tk, "=") ||
                            isOp(tk, "and") || isOp(tk, "or") ||
                            isOp(tk, ":") ||
                            isKw(tk, "return") || isKw(tk, "throw")) {
                            break;
                        }
                    }
                    startTokens.insert(startTokens.begin(), tk);
                    out.pop_back();
                }

                if (!startTokens.empty()) {
                    std::vector<LexerType::Token> endTokens(tokens.begin() + dotPos + dotLen, tokens.begin() + endIdx);
                    int line = getTokenLine(startTokens.front());
                    int col = getTokenCol(startTokens.front());

                    // 生成 range(start, end) 或 range(start, end, 1, true)
                    out.push_back(makeKw("range", line, col));
                    out.push_back(makeOp("(", line, col));
                    out.insert(out.end(), startTokens.begin(), startTokens.end());
                    out.push_back(makeOp(",", line, col));
                    out.insert(out.end(), endTokens.begin(), endTokens.end());
                    if (inclusive) {
                        out.push_back(makeOp(",", line, col));
                        LexerType::Value stepOne(ZeusType::BigInteger(1));
                        stepOne.line = line; stepOne.column = col;
                        out.push_back(stepOne);
                        out.push_back(makeOp(",", line, col));
                        out.push_back(makeKw("true", line, col));
                    }
                    out.push_back(makeOp(")", line, col));

                    i = endIdx - 1;
                    continue;
                } else {
                    out.insert(out.end(), startTokens.begin(), startTokens.end());
                }
            }
        }

        out.push_back(tokens[i]);
    }

    return out;
}

// -----------------------------------------------------------------------------
// 3. 匿名函数脱糖: func(params) [captures] { body }
//    提升到当前语句前面作为独立局部命名函数声明:
//    func __zeus_anon_N(params) [captures] { desugared_body }
//    原表达式位置替换为: __zeus_anon_N
// -----------------------------------------------------------------------------
inline bool isBlockOpenBrace(const std::vector<LexerType::Token>& out, size_t braceIdx) {
    if (braceIdx == 0) return true; // 文件开头的块
    const auto& prev = out[braceIdx - 1];
    // 如果前一个 token 是表达式运算符或引导符，则该 '{' 是字典字面量而非语句块
    if (isOp(prev, "=") || isOp(prev, ":") || isOp(prev, "(") ||
        isOp(prev, "[") || isOp(prev, ",") || isOp(prev, "?") ||
        isOp(prev, "==") || isOp(prev, "!=") || isOp(prev, "+") ||
        isOp(prev, "-") || isOp(prev, "*") || isOp(prev, "/") ||
        isOp(prev, "%") || isOp(prev, "and") || isOp(prev, "or") ||
        isKw(prev, "return") || isKw(prev, "throw")) {
        return false;
    }
    return true;
}

inline size_t findCurrentStatementStart(const std::vector<LexerType::Token>& out) {
    if (out.empty()) return 0;
    int depth = 0;
    for (size_t k = out.size(); k-- > 0;) {
        const auto& tk = out[k];
        if (isOp(tk, ")") || isOp(tk, "]")) {
            depth++;
        } else if (isOp(tk, "(") || isOp(tk, "[")) {
            if (depth > 0) depth--;
        } else if (isOp(tk, "}")) {
            if (depth > 0) {
                depth++;
            } else {
                return k + 1;
            }
        } else if (isOp(tk, "{")) {
            if (depth > 0) {
                depth--;
            } else {
                if (isBlockOpenBrace(out, k)) {
                    return k + 1;
                }
                // 若为字典字面量 '{'，继续向前寻找包含它的主干语句
            }
        } else if (depth == 0 && isOp(tk, ";")) {
            return k + 1;
        }
    }
    return 0;
}

inline std::vector<LexerType::Token> desugarAnonymousFunctions(
    const std::vector<LexerType::Token>& tokens
) {
    bool hasAnonFunc = false;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (isKw(tokens[i], "func") && isOp(tokens[i + 1], "(")) {
            hasAnonFunc = true;
            break;
        }
    }
    if (!hasAnonFunc) return tokens;

    static std::atomic<uint64_t> s_anonCounter{0};
    std::vector<LexerType::Token> out;
    out.reserve(tokens.size());

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (isKw(tokens[i], "func") && i + 1 < tokens.size() && isOp(tokens[i + 1], "(")) {
            int line = getTokenLine(tokens[i]);
            int col = getTokenCol(tokens[i]);

            // 1. 匹配参数列表 '(' ... ')'
            size_t paramStart = i + 1;
            int pDepth = 1;
            size_t pos = paramStart + 1;
            while (pos < tokens.size() && pDepth > 0) {
                if (isOp(tokens[pos], "(")) pDepth++;
                else if (isOp(tokens[pos], ")")) pDepth--;
                if (pDepth == 0) break;
                pos++;
            }

            if (pDepth == 0 && pos < tokens.size()) {
                size_t paramEnd = pos; // ')' 所在下标

                // 2. 从 paramEnd + 1 开始向后扫描直到找到函数体起始 '{'
                // 中间可能包含类型注解 '->' 和捕获列表 '[...]'
                size_t headerScan = paramEnd + 1;
                int bBracketDepth = 0;
                size_t bodyOpenIdx = tokens.size();

                while (headerScan < tokens.size()) {
                    if (isOp(tokens[headerScan], "[")) bBracketDepth++;
                    else if (isOp(tokens[headerScan], "]")) {
                        if (bBracketDepth > 0) bBracketDepth--;
                    } else if (bBracketDepth == 0 && isOp(tokens[headerScan], "{")) {
                        bodyOpenIdx = headerScan;
                        break;
                    } else if (bBracketDepth == 0 && isOp(tokens[headerScan], ";")) {
                        break;
                    }
                    headerScan++;
                }

                if (bodyOpenIdx < tokens.size()) {
                    // 3. 找到匹配的函数体结束 '}'
                    int bodyDepth = 1;
                    size_t bodyCloseIdx = tokens.size();
                    for (size_t k = bodyOpenIdx + 1; k < tokens.size(); ++k) {
                        if (isOp(tokens[k], "{")) bodyDepth++;
                        else if (isOp(tokens[k], "}")) {
                            bodyDepth--;
                            if (bodyDepth == 0) {
                                bodyCloseIdx = k;
                                break;
                            }
                        }
                    }

                    if (bodyCloseIdx < tokens.size()) {
                        // 提取 header tokens (从 paramStart 到 bodyOpenIdx 之前)
                        std::vector<LexerType::Token> headerTokens(tokens.begin() + paramStart, tokens.begin() + bodyOpenIdx);

                        // 提取 body tokens (从 bodyOpenIdx + 1 到 bodyCloseIdx - 1)
                        std::vector<LexerType::Token> innerBody(tokens.begin() + bodyOpenIdx + 1, tokens.begin() + bodyCloseIdx);

                        // 递归对函数体脱糖（支持嵌套匿名函数）
                        auto desugaredInnerBody = desugarAnonymousFunctions(innerBody);

                        // 生成唯一函数名
                        std::string fnName = "__zeus_anon_" + std::to_string(++s_anonCounter);

                        // 组装命名函数声明: func __zeus_anon_N(params) [captures] { desugaredInnerBody }
                        std::vector<LexerType::Token> hoistedDecl;
                        hoistedDecl.push_back(makeKw("func", line, col));
                        hoistedDecl.push_back(makeKw(fnName, line, col));
                        hoistedDecl.insert(hoistedDecl.end(), headerTokens.begin(), headerTokens.end());
                        hoistedDecl.push_back(makeOp("{", line, col));
                        hoistedDecl.insert(hoistedDecl.end(), desugaredInnerBody.begin(), desugaredInnerBody.end());
                        hoistedDecl.push_back(makeOp("}", line, col));

                        // 查找当前主干语句在 out 中的起始插入位置并提升
                        size_t insertPos = findCurrentStatementStart(out);
                        out.insert(out.begin() + insertPos, hoistedDecl.begin(), hoistedDecl.end());

                        // 在原表达式位置留下函数名标识符
                        out.push_back(makeKw(fnName, line, col));

                        // 推进 i 到匿名函数结束处
                        i = bodyCloseIdx;
                        continue;
                    }
                }
            }
        }

        out.push_back(tokens[i]);
    }

    return out;
}

} // namespace DesugarDetail

// 主入口: 执行所有词法后脱糖
// 顺序: when 表达式/语句 -> 字典简写 -> 范围关系与独立范围 -> 匿名函数提升
inline std::vector<LexerType::Token> desugarTokens(
    const std::vector<LexerType::Token>& tokens,
    std::vector<ZeusDiagnostics::Diagnostic>* diagnostics = nullptr
) {
    auto t1 = desugarWhenTokens(tokens, diagnostics);
    auto t2 = DesugarDetail::desugarDictShorthand(t1);
    auto t3 = DesugarDetail::desugarRangeTokens(t2);
    auto t4 = DesugarDetail::desugarAnonymousFunctions(t3);
    return t4;
}

} // namespace ZeusParser
