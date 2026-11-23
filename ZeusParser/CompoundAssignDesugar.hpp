#pragma once

#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "DesugarTokenUtils.hpp"

namespace ZeusParser {

namespace DesugarDetail {

// -----------------------------------------------------------------------------
// 复合赋值与自增/自减脱糖（仅语句级）:
//    target += expr;   =>  target = target + (expr);
//    target -= expr;   =>  target = target - (expr);
//    target *= expr;   =>  target = target * (expr);
//    target /= expr;   =>  target = target / (expr);
//    target %= expr;   =>  target = target % (expr);
//    target++;         =>  target += 1;  =>  target = target + 1;
//    target--;         =>  target -= 1;  =>  target = target - 1;
// 成员和下标目标会先保存接收对象及索引，再执行读取和写入；调用表达式只求值一次。
// -----------------------------------------------------------------------------

inline bool isCompoundBaseOperator(const LexerType::Token& tk) {
    return isOp(tk, "+") || isOp(tk, "-") || isOp(tk, "*") || isOp(tk, "/") || isOp(tk, "%");
}

// tokens[i] 与 tokens[i+1] 是否构成紧挨着的 '<op>='（且后面不是第三个 '='，以免误伤 '=='）
inline bool isCompoundAssignAt(const std::vector<LexerType::Token>& tokens, size_t i) {
    if (i + 2 >= tokens.size()) return false;
    if (!isCompoundBaseOperator(tokens[i]) || !isOp(tokens[i + 1], "=")) return false;
    if (!areAdjacent(tokens[i], tokens[i + 1])) return false;
    return !isOp(tokens[i + 2], "=");
}

// tokens[i] 与 tokens[i+1] 是否构成紧挨着的 '++' / '--'，并且作为语句以 ';' 结尾
inline bool isStepStatementAt(const std::vector<LexerType::Token>& tokens, size_t i) {
    if (i + 2 >= tokens.size()) return false;
    const bool plus = isOp(tokens[i], "+") && isOp(tokens[i + 1], "+");
    const bool minus = isOp(tokens[i], "-") && isOp(tokens[i + 1], "-");
    if (!plus && !minus) return false;
    return areAdjacent(tokens[i], tokens[i + 1]) && isOp(tokens[i + 2], ";");
}

// 在已输出的 token 中向回寻找当前语句的起点，返回赋值目标的第一个 token 下标
inline size_t findAssignTargetStart(const std::vector<LexerType::Token>& out) {
    size_t idx = out.size();
    int depth = 0;
    while (idx > 0) {
        const auto& tk = out[idx - 1];
        if (isCloseBracket(tk)) {
            if (depth == 0 && isOp(tk, "}")) break;
            if (depth == 0 && isOp(tk, ")")) {
                // 无花括号的 if (...) / for (...) 等头部之后紧跟的语句
                size_t open = findMatchingOpenBackward(out, idx - 1);
                if (open > 0 && open < out.size() &&
                    (isKw(out[open - 1], "if") || isKw(out[open - 1], "for") ||
                     isKw(out[open - 1], "while") || isKw(out[open - 1], "with") ||
                     isKw(out[open - 1], "catch"))) {
                    break;
                }
            }
            depth++;
        } else if (isOpenBracket(tk)) {
            if (depth == 0) break;
            depth--;
        } else if (depth == 0 && (isOp(tk, ";") || isOp(tk, ":"))) {
            break;
        } else if (depth == 0 && (isKw(tk, "else") || isKw(tk, "try") || isKw(tk, "fin"))) {
            break;
        }
        idx--;
    }
    return idx;
}

// 赋值目标不能以声明、返回等语句关键字开头
inline bool isValidAssignTarget(const std::vector<LexerType::Token>& out, size_t start) {
    if (start >= out.size()) return false;
    const auto& first = out[start];
    return !(isKw(first, "let") || isKw(first, "var") || isKw(first, "return") ||
             isKw(first, "throw") || isKw(first, "delete") || isKw(first, "const"));
}

class CompoundTempNameGenerator {
public:
    explicit CompoundTempNameGenerator(const std::vector<LexerType::Token>& tokens) {
        for (const auto& token : tokens) {
            if (std::holds_alternative<LexerType::Keyword>(token)) {
                used_.insert(std::get<LexerType::Keyword>(token).name);
            }
        }
    }

    std::string next() {
        std::string name;
        do {
            name = "__zeus_compound_" + std::to_string(nextDesugarTempId());
        } while (!used_.insert(name).second);
        return name;
    }

private:
    std::unordered_set<std::string> used_;
};

struct CompoundTargetStep {
    enum class Kind { Member, Subscript, Call } kind;
    LexerType::Token member = LexerType::Keyword("");
    std::vector<LexerType::Token> expression;
};

struct CompoundTargetPath {
    std::vector<LexerType::Token> root;
    std::vector<CompoundTargetStep> steps;
};

inline bool isCompoundIdentifier(const LexerType::Token& token) {
    if (!std::holds_alternative<LexerType::Keyword>(token)) return false;
    const auto& name = std::get<LexerType::Keyword>(token).name;
    static const std::unordered_set<std::string> reserved = {
        "var", "let", "const", "shared", "if", "else", "for", "func", "class",
        "interface", "impl", "return", "break", "continue", "delete", "throw", "try",
        "catch", "with", "when", "import", "using", "original", "decl", "static", "new",
        "true", "false", "nil"
    };
    return reserved.count(name) == 0;
}

inline size_t findCompoundMatchingClose(
    const std::vector<LexerType::Token>& tokens,
    size_t open,
    size_t end
) {
    if (open >= end || !isOpenBracket(tokens[open])) return end;
    std::vector<std::string> expected;
    expected.push_back(isOp(tokens[open], "(") ? ")" :
                        isOp(tokens[open], "[") ? "]" : "}");
    for (size_t i = open + 1; i < end; ++i) {
        if (isOp(tokens[i], "(") || isOp(tokens[i], "[") || isOp(tokens[i], "{")) {
            expected.push_back(isOp(tokens[i], "(") ? ")" :
                                isOp(tokens[i], "[") ? "]" : "}");
        } else if (isOp(tokens[i], ")") || isOp(tokens[i], "]") || isOp(tokens[i], "}")) {
            if (expected.empty() || !isOp(tokens[i], expected.back())) return end;
            expected.pop_back();
            if (expected.empty()) return i;
        }
    }
    return end;
}

inline bool parseCompoundTargetPath(
    const std::vector<LexerType::Token>& target,
    CompoundTargetPath& path
) {
    if (target.empty()) return false;

    size_t index = 0;
    if (isCompoundIdentifier(target[index])) {
        path.root.push_back(target[index++]);
    } else if (isOp(target[index], "(")) {
        const size_t close = findCompoundMatchingClose(target, index, target.size());
        if (close == target.size()) return false;
        path.root.assign(target.begin(), target.begin() + close + 1);
        index = close + 1;
    } else {
        return false;
    }

    while (index < target.size()) {
        if (isOp(target[index], ".")) {
            if (index + 1 >= target.size() ||
                !std::holds_alternative<LexerType::Keyword>(target[index + 1])) {
                return false;
            }
            CompoundTargetStep step;
            step.kind = CompoundTargetStep::Kind::Member;
            step.member = target[index + 1];
            path.steps.push_back(std::move(step));
            index += 2;
            continue;
        }

        if (isOp(target[index], "[") || isOp(target[index], "(")) {
            const bool subscript = isOp(target[index], "[");
            const size_t close = findCompoundMatchingClose(target, index, target.size());
            if (close == target.size()) return false;
            CompoundTargetStep step;
            step.kind = subscript ? CompoundTargetStep::Kind::Subscript : CompoundTargetStep::Kind::Call;
            step.expression.assign(target.begin() + index + 1, target.begin() + close);
            if (subscript && step.expression.empty()) return false;
            path.steps.push_back(std::move(step));
            index = close + 1;
            continue;
        }

        return false;
    }

    return !path.steps.empty() || target.size() == 1;
}

inline void appendCompoundLet(
    std::vector<LexerType::Token>& out,
    const std::string& name,
    const std::vector<LexerType::Token>& expression,
    int line,
    int col
) {
    out.push_back(makeKw("let", line, col));
    out.push_back(makeKw(name, line, col));
    out.push_back(makeOp("=", line, col));
    out.insert(out.end(), expression.begin(), expression.end());
    out.push_back(makeOp(";", line, col));
}

inline bool compoundHasBracelessControlBody(
    const std::vector<LexerType::Token>& out,
    size_t targetStart
) {
    if (targetStart == 0) return false;
    if (isKw(out[targetStart - 1], "else")) return true;
    if (!isOp(out[targetStart - 1], ")")) return false;
    const size_t open = findMatchingOpenBackward(out, targetStart - 1);
    return open > 0 && open < out.size() &&
        (isKw(out[open - 1], "if") || isKw(out[open - 1], "for") ||
         isKw(out[open - 1], "while") || isKw(out[open - 1], "with") ||
         isKw(out[open - 1], "catch"));
}

inline bool compoundHasEnclosingBlock(
    const std::vector<LexerType::Token>& out,
    size_t targetStart
) {
    int depth = 0;
    for (size_t i = 0; i < targetStart; ++i) {
        if (isOp(out[i], "{")) ++depth;
        else if (isOp(out[i], "}") && depth > 0) --depth;
    }
    return depth > 0;
}

inline std::vector<LexerType::Token> appendCompoundSuffix(
    const std::vector<LexerType::Token>& base,
    const CompoundTargetStep& step,
    const std::string* subscriptName,
    int line,
    int col
) {
    std::vector<LexerType::Token> expression = base;
    if (step.kind == CompoundTargetStep::Kind::Member) {
        expression.push_back(makeOp(".", line, col));
        expression.push_back(step.member);
    } else if (step.kind == CompoundTargetStep::Kind::Subscript) {
        expression.push_back(makeOp("[", line, col));
        if (subscriptName) expression.push_back(makeKw(*subscriptName, line, col));
        else expression.insert(expression.end(), step.expression.begin(), step.expression.end());
        expression.push_back(makeOp("]", line, col));
    } else {
        expression.push_back(makeOp("(", line, col));
        expression.insert(expression.end(), step.expression.begin(), step.expression.end());
        expression.push_back(makeOp(")", line, col));
    }
    return expression;
}

inline bool appendCompoundExpansion(
    std::vector<LexerType::Token>& out,
    size_t targetStart,
    const LexerType::Token& opToken,
    const std::vector<LexerType::Token>& rhs,
    CompoundTempNameGenerator& tempNames,
    bool* emittedWrapper = nullptr
) {
    if (emittedWrapper) *emittedWrapper = false;
    const int line = getTokenLine(opToken);
    const int col = getTokenCol(opToken);
    const bool hasControlBody = compoundHasBracelessControlBody(out, targetStart);
    const bool needsStatementWrapper = !compoundHasEnclosingBlock(out, targetStart) ||
        hasControlBody;
    const std::vector<LexerType::Token> target(out.begin() + targetStart, out.end());
    CompoundTargetPath path;
    if (!parseCompoundTargetPath(target, path)) return false;

    const bool directVariable = path.steps.empty();
    std::vector<LexerType::Token> prelude;
    std::vector<LexerType::Token> current;
    std::vector<LexerType::Token> writableTarget;

    if (directVariable) {
        writableTarget = target;
    } else {
        const std::string rootName = tempNames.next();
        appendCompoundLet(prelude, rootName, path.root, line, col);
        current.push_back(makeKw(rootName, line, col));

        for (size_t i = 0; i < path.steps.size(); ++i) {
            const auto& step = path.steps[i];
            const bool isLast = i + 1 == path.steps.size();
            if (isLast) {
                if (step.kind == CompoundTargetStep::Kind::Call) return false;
                if (step.kind == CompoundTargetStep::Kind::Subscript) {
                    const std::string subscriptName = tempNames.next();
                    appendCompoundLet(prelude, subscriptName, step.expression, line, col);
                    writableTarget = appendCompoundSuffix(current, step, &subscriptName, line, col);
                } else {
                    writableTarget = appendCompoundSuffix(current, step, nullptr, line, col);
                }
                break;
            }

            // Keep a method receiver attached to its call (`object.method()`).
            const bool memberCallReceiver = step.kind == CompoundTargetStep::Kind::Member &&
                path.steps[i + 1].kind == CompoundTargetStep::Kind::Call;
            if (memberCallReceiver) {
                current = appendCompoundSuffix(current, step, nullptr, line, col);
                continue;
            }

            const auto evaluated = appendCompoundSuffix(current, step, nullptr, line, col);
            const std::string stepName = tempNames.next();
            appendCompoundLet(prelude, stepName, evaluated, line, col);
            current.clear();
            current.push_back(makeKw(stepName, line, col));
        }
    }

    if (writableTarget.empty()) return false;

    if (!directVariable) {
        out.erase(out.begin() + targetStart, out.end());
        if (needsStatementWrapper) {
            if (!hasControlBody) {
                out.push_back(makeKw("if", line, col));
                out.push_back(makeOp("(", line, col));
                out.push_back(makeKw("true", line, col));
                out.push_back(makeOp(")", line, col));
            }
            out.push_back(makeOp("{", line, col));
            if (emittedWrapper) *emittedWrapper = true;
        }
        out.insert(out.end(), prelude.begin(), prelude.end());
        out.insert(out.end(), writableTarget.begin(), writableTarget.end());
    }

    out.push_back(makeOp("=", line, col));
    out.insert(out.end(), writableTarget.begin(), writableTarget.end());
    out.push_back(opToken);
    if (rhs.size() > 1) out.push_back(makeOp("(", line, col));
    out.insert(out.end(), rhs.begin(), rhs.end());
    if (rhs.size() > 1) out.push_back(makeOp(")", line, col));

    if (!directVariable && needsStatementWrapper) {
        out.push_back(makeOp(";", line, col));
        out.push_back(makeOp("}", line, col));
    }
    return true;
}

inline std::vector<LexerType::Token> desugarCompoundAssignImpl(
    const std::vector<LexerType::Token>& tokens,
    CompoundTempNameGenerator& tempNames
) {
    std::vector<LexerType::Token> out;
    out.reserve(tokens.size());

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (isStepStatementAt(tokens, i)) {
            size_t start = findAssignTargetStart(out);
            if (isValidAssignTarget(out, start)) {
                std::vector<LexerType::Token> one{makeIntValue(1, getTokenLine(tokens[i]), getTokenCol(tokens[i]))};
                bool emittedWrapper = false;
                if (appendCompoundExpansion(out, start, tokens[i], one, tempNames, &emittedWrapper)) {
                    i += emittedWrapper ? 2 : 1;
                    continue;
                }
            }
        }

        if (isCompoundAssignAt(tokens, i)) {
            size_t start = findAssignTargetStart(out);
            size_t end = findStatementEndForward(tokens, i + 2);
            if (isValidAssignTarget(out, start) && end < tokens.size()) {
                std::vector<LexerType::Token> rhs(tokens.begin() + i + 2, tokens.begin() + end);
                auto desugaredRhs = desugarCompoundAssignImpl(rhs, tempNames);
                bool emittedWrapper = false;
                if (appendCompoundExpansion(out, start, tokens[i], desugaredRhs, tempNames, &emittedWrapper)) {
                    i = emittedWrapper ? end : end - 1;
                    continue;
                }
            }
        }

        out.push_back(tokens[i]);
    }

    return out;
}

inline std::vector<LexerType::Token> desugarCompoundAssign(
    const std::vector<LexerType::Token>& tokens
) {
    CompoundTempNameGenerator tempNames(tokens);
    return desugarCompoundAssignImpl(tokens, tempNames);
}

} // namespace DesugarDetail

} // namespace ZeusParser
