#ifndef IRPriorityFold_hpp
#define IRPriorityFold_hpp

#include "../ZeusParser/LexerType.hpp"
#include "IRExprFold.hpp"
#include <vector>
#include <stack>
#include <algorithm>

namespace IRGen {

// 中缀表达式转后缀表达式（考虑操作符优先级）
inline std::vector<LexerType::Token> priorityInfixToPostfix(
    const std::vector<LexerType::Token>& infix) {
    std::vector<LexerType::Token> postfix;
    std::stack<LexerType::Operator> opStack;
    
    for (const auto& token : infix) {
        if (std::holds_alternative<LexerType::Value>(token) || 
            std::holds_alternative<LexerType::Keyword>(token)) {
            // 变量或常量直接输出
            postfix.push_back(token);
        } 
        else if (std::holds_alternative<LexerType::Operator>(token)) {
            // 处理操作符优先级
            auto op = std::get<LexerType::Operator>(token);
            while (!opStack.empty() && 
                   opPrecedence(opStack.top().symbol) >= opPrecedence(op.symbol)) {
                postfix.push_back(opStack.top());
                opStack.pop();
            }
            opStack.push(op);
        }
    }
    
    // 将剩余操作符弹出
    while (!opStack.empty()) {
        postfix.push_back(opStack.top());
        opStack.pop();
    }
    
    return postfix;
}

// 后缀表达式求值（对于包含常量的部分进行折叠）
inline std::vector<LexerType::Token> evaluatePostfix(
    IRType::IRCompilerStatus& irstat,
    const std::vector<LexerType::Token>& postfix) {
    std::stack<LexerType::Token> evalStack;
    
    for (const auto& token : postfix) {
        if (std::holds_alternative<LexerType::Value>(token) || 
            std::holds_alternative<LexerType::Keyword>(token)) {
            // 变量或常量压栈
            evalStack.push(token);
        } 
        else if (std::holds_alternative<LexerType::Operator>(token)) {
            // 操作符处理
            auto op = std::get<LexerType::Operator>(token).symbol;
            
            // 需要两个操作数
            if (evalStack.size() < 2) {
                evalStack.push(token);
                continue;
            }
            
            auto rhs = evalStack.top(); evalStack.pop();
            auto lhs = evalStack.top(); evalStack.pop();
            
            // 如果两个操作数都是常量，计算结果
            if (std::holds_alternative<LexerType::Value>(lhs) &&
                std::holds_alternative<LexerType::Value>(rhs)) {

                if (irstat != IRType::IRCompilerStatus::Running) {
                    return {};
                }

                auto result = evalBinaryEx(
                    irstat,
                    std::get<LexerType::Value>(lhs),
                    std::get<LexerType::Value>(rhs),
                    op
                );

                evalStack.push(result);
            } else {
                // 含变量，无法计算，恢复原状
                evalStack.push(lhs);
                evalStack.push(rhs);
                evalStack.push(token);
            }
        }
    }
    
    // 将栈中内容转换为标记序列
    std::vector<LexerType::Token> result;
    while (!evalStack.empty()) {
        result.insert(result.begin(), evalStack.top());
        evalStack.pop();
    }
    
    return result;
}

// 后缀表达式转中缀表达式（用于显示）
inline std::vector<LexerType::Token> postfixToInfix(
    const std::vector<LexerType::Token>& postfix) {
    std::stack<std::vector<LexerType::Token>> stack;
    
    for (const auto& token : postfix) {
        if (std::holds_alternative<LexerType::Value>(token) || 
            std::holds_alternative<LexerType::Keyword>(token)) {
            // 变量或常量，创建单元素表达式
            stack.push({token});
        } 
        else if (std::holds_alternative<LexerType::Operator>(token)) {
            // 需要两个操作数
            if (stack.size() < 2) continue;
            
            auto rhs = stack.top(); stack.pop();
            auto lhs = stack.top(); stack.pop();
            
            // 构建新的表达式
            std::vector<LexerType::Token> expr;
            expr.insert(expr.end(), lhs.begin(), lhs.end());
            expr.push_back(token);
            expr.insert(expr.end(), rhs.begin(), rhs.end());
            
            stack.push(expr);
        }
    }
    
    return stack.empty() ? std::vector<LexerType::Token>() : stack.top();
}

// 基于优先级的表达式折叠
inline std::vector<LexerType::Token> priorityFoldExpression(
    IRType::IRCompilerStatus& irstat,
    const std::vector<LexerType::Token>& tokens) {
    // This folder's infix/postfix reconstruction is binary-only. Preserve a
    // prefix logical-not expression intact so the real expression parser can
    // lower it as a unary operation instead of silently dropping the token.
    if (std::any_of(tokens.begin(), tokens.end(), [](const LexerType::Token& token) {
            return std::holds_alternative<LexerType::Operator>(token) &&
                   std::get<LexerType::Operator>(token).symbol == "!";
        })) {
        return tokens;
    }

    // 辅助：检测是否是函数调用起点（Keyword 后紧跟 '('）
    auto isCallStart = [&](size_t i) -> bool {
        if (i + 1 >= tokens.size()) return false;
        if (!std::holds_alternative<LexerType::Keyword>(tokens[i])) return false;
        if (!std::holds_alternative<LexerType::Operator>(tokens[i+1])) return false;
        return std::get<LexerType::Operator>(tokens[i+1]).symbol == "(";
    };

    // 辅助：若存在括号或逗号，仅对“调用之外的纯常量片段”做局部折叠
    auto foldOutsideCalls = [&]() -> std::vector<LexerType::Token> {
        std::vector<LexerType::Token> out;
        size_t i = 0, n = tokens.size();
        while (i < n) {
            // 识别并原样复制函数调用段（含嵌套括号）
            if (isCallStart(i)) {
                // 复制函数名
                out.push_back(tokens[i++]);
                // 必定存在 '('，开始复制并跟踪括号深度
                int depth = 0;
                while (i < n) {
                    const auto& tk = tokens[i];
                    out.push_back(tk);
                    if (std::holds_alternative<LexerType::Operator>(tk)) {
                        auto sym = std::get<LexerType::Operator>(tk).symbol;
                        if (sym == "(") {
                            depth++;
                        } else if (sym == ")") {
                            depth--;
                            if (depth == 0) { i++; break; } // 完成一个调用
                        }
                    }
                    i++;
                }
                continue;
            }

            // 情况A：以 Value 开头的“纯常量 + 二元运算符”片段，尽可能折叠
            if (std::holds_alternative<LexerType::Value>(tokens[i])) {
                size_t k = i;
                while (k < n) {
                    const auto& tk = tokens[k];
                    if (std::holds_alternative<LexerType::Value>(tk) ||
                        (std::holds_alternative<LexerType::Operator>(tk) && isBinaryOp(tk))) {
                        k++;
                        continue;
                    }
                    break;
                }
                if (k > i) {
                    std::vector<LexerType::Token> seg(tokens.begin() + i, tokens.begin() + k);
                    auto folded = foldConstSegment(irstat, seg);
                    out.insert(out.end(), folded.begin(), folded.end());
                    i = k;
                    continue;
                }
            }

            // 情况B：当前位置是二元操作符，尝试折叠其“右侧紧跟的常量片段”（不跨越非常量/括号/逗号）
            if (std::holds_alternative<LexerType::Operator>(tokens[i]) && isBinaryOp(tokens[i])) {
                // 先原样输出操作符
                const auto curOp = std::get<LexerType::Operator>(tokens[i]).symbol;
                out.push_back(tokens[i]);
                i++;
                // 查找从 i 开始、以 Value 起始的常量子表达式
                if (i < n && std::holds_alternative<LexerType::Value>(tokens[i])) {
                    size_t k = i;
                    while (k < n) {
                        const auto& tk = tokens[k];
                        if (std::holds_alternative<LexerType::Value>(tk)) { k++; continue; }
                        if (std::holds_alternative<LexerType::Operator>(tk) && isBinaryOp(tk)) {
                            // 仅接受优先级不低于当前操作符的运算符，防止跨越低优先级边界
                            const auto nextOp = std::get<LexerType::Operator>(tk).symbol;
                            if (opPrecedence(nextOp) > opPrecedence(curOp)) { k++; continue; }
                            else break;
                        }
                        break;
                    }
                    if (k > i) {
                        std::vector<LexerType::Token> seg(tokens.begin() + i, tokens.begin() + k);
                        auto folded = foldConstSegment(irstat, seg);
                        out.insert(out.end(), folded.begin(), folded.end());
                        i = k;
                        continue;
                    }
                }
                // 右侧不是常量子段，就继续走常规复制
            }

            // 其他 token 原样复制
            out.push_back(tokens[i]);
            i++;
        }
        return out;
    };

    bool hasParenOrComma = std::any_of(tokens.begin(), tokens.end(), [](const LexerType::Token& t){
        if (!std::holds_alternative<LexerType::Operator>(t)) return false;
        const auto& s = std::get<LexerType::Operator>(t).symbol;
        return s == "(" || s == ")" || s == ",";
    });
    if (hasParenOrComma) {
        // 先对调用外的高优先级常量段做局部折叠
    auto outsideFolded = foldOutsideCalls();

        // 顶层（括号深度为0）按 + / - 划分项，再把纯常量项合并成 bias
    auto additiveFoldTopLevel = [&](const std::vector<LexerType::Token>& in) -> std::vector<LexerType::Token> {
            // 拆项
            struct Term { int sign; std::vector<LexerType::Token> toks; }; // sign: +1 or -1
            std::vector<Term> terms;
            int curSign = +1; // 第一项默认为正
            std::vector<LexerType::Token> cur;
            int parenDepth = 0;  // () 深度
            int bracketDepth = 0; // [] 深度
            int braceDepth = 0;   // {} 深度
            for (size_t i = 0; i < in.size(); ++i) {
                const auto& tk = in[i];
                if (std::holds_alternative<LexerType::Operator>(tk)) {
                    auto sym = std::get<LexerType::Operator>(tk).symbol;
                    // 跟踪所有类型的括号深度
                    if (sym == "(") { 
                        parenDepth++; cur.push_back(tk); 
                        continue; 
                    }
                    if (sym == ")") { 
                        parenDepth--; cur.push_back(tk); 
                        continue; 
                    }
                    if (sym == "[") { 
                        bracketDepth++; cur.push_back(tk); 
                        continue; 
                    }
                    if (sym == "]") { 
                        bracketDepth--; cur.push_back(tk); 
                        continue; 
                    }
                    if (sym == "{") { 
                        braceDepth++; cur.push_back(tk); 
                        continue; 
                    }
                    if (sym == "}") { 
                        braceDepth--; cur.push_back(tk); 
                        continue; 
                    }
                    // 只在所有括号都关闭的情况下才分割term
                    if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0 && (sym == "+" || sym == "-")) {
                        // 结束当前项
                        if (!cur.empty()) terms.push_back(Term{curSign, cur});
                        cur.clear();
                        curSign = (sym == "+") ? +1 : -1;
                        continue;
                    }
                }
                cur.push_back(tk);
            }
            if (!cur.empty()) terms.push_back(Term{curSign, cur});

            // 合并纯常量项到 bias
            bool hasBias = false;
            LexerType::Value bias(0); // 用 0 作为初始
            auto isZero = [](const LexerType::Value& v) -> bool {
                if (std::holds_alternative<ZeusType::BigInteger>(v.data)) return std::get<ZeusType::BigInteger>(v.data) == ZeusType::BigInteger(0);
                if (std::holds_alternative<ZeusType::BigDecimal>(v.data)) return std::get<ZeusType::BigDecimal>(v.data) == ZeusType::BigDecimal(0);
                return false; // 非数值不视为0
            };

            std::vector<Term> kept; // 非纯常量项保留
            for (auto& t : terms) {
                // 纯常量项：所有 token 仅为 Value 或 二元Operator，且不含括号/逗号/关键字
                bool pureConst = !t.toks.empty() && std::all_of(t.toks.begin(), t.toks.end(), [](const LexerType::Token& x){
                    if (std::holds_alternative<LexerType::Value>(x)) return true;
                    if (std::holds_alternative<LexerType::Operator>(x)) return isBinaryOp(x);
                    return false; // 关键字/其他视为非纯常量
                });

                if (pureConst) {
                    auto folded = foldConstSegment(irstat, t.toks);
                    if (folded.size() == 1 && std::holds_alternative<LexerType::Value>(folded[0])) {
                        const auto& val = std::get<LexerType::Value>(folded[0]);
                        // 仅数值才参与 bias 累加
                        if (std::holds_alternative<ZeusType::BigInteger>(val.data) || std::holds_alternative<ZeusType::BigDecimal>(val.data)) {
                            if (!hasBias) { bias = LexerType::Value(ZeusType::BigInteger(0)); hasBias = true; }
                            if (t.sign == +1) bias = evalBinaryEx(irstat, bias, val, "+");
                            else              bias = evalBinaryEx(irstat, bias, val, "-");
                            continue; // 吞掉该项
                        }
                    }
                }
                kept.push_back(std::move(t));
            }

            // 重建：bias（若非0或 kept 为空） + kept各项（带符号）
            std::vector<LexerType::Token> out2;
            bool emittedAny = false;
            if (hasBias && (!isZero(bias) || kept.empty())) {
                out2.push_back(bias);
                emittedAny = true;
            }
            for (auto& t : kept) {
                if (emittedAny) {
                    out2.push_back(LexerType::Operator(t.sign == +1 ? "+" : "-"));
                } else if (t.sign == -1) {
                    // 表达式首项为负，显式发出 '-'
                    out2.push_back(LexerType::Operator("-"));
                }
                out2.insert(out2.end(), t.toks.begin(), t.toks.end());
                emittedAny = true;
            }
            if (!emittedAny) {
                // 退化为 0
                out2.push_back(LexerType::Value(0));
            }
            return out2;
        };

        return additiveFoldTopLevel(outsideFolded);
    }

    // 对纯常量表达式使用原始折叠
    if (std::all_of(tokens.begin(), tokens.end(), [](const LexerType::Token& t) {
        return std::holds_alternative<LexerType::Value>(t) || 
               (std::holds_alternative<LexerType::Operator>(t) && isBinaryOp(t));
    })) {
        return foldConstSegment(irstat, tokens);
    }
    
    // 处理混合表达式，考虑操作符优先级
    std::vector<LexerType::Token> postfix = priorityInfixToPostfix(tokens);
    std::vector<LexerType::Token> evaluated = evaluatePostfix(irstat, postfix);
    
    // 如果需要转回中缀形式用于显示
    if (evaluated.size() > 1) {
        return postfixToInfix(evaluated);
    }
    
    return evaluated;
}

} // namespace IRGen

#endif // IRPriorityFold_hpp
