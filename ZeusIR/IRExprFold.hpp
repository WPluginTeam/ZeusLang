#ifndef ExprFold_hpp
#define ExprFold_hpp

#include "../ZeusParser/LexerType.hpp"

#include <stack>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <algorithm> // 添加algorithm头文件，提供std::all_of
#include "IRException.hpp"
#include "IRType.hpp"

namespace IRGen {

inline int opPrecedence(const std::string& op) {
    if (op == "*" || op == "/" || op == "%") return 5;
    if (op == "+" || op == "-") return 4;
    // 比较运算优先级低于加减
    if (op == "==" || op == "!=" || op == ">" || op == "<" || op == ">=" || op == "<=") return 3;
    // 逻辑运算符优先级最低,and高于or
    if (op == "and") return 2;
    if (op == "or") return 1;
    // 冒号用于字典字面量，不是二元操作符
    if (op == ":") return -1;
    return 0;
}

inline bool isBinaryOp(const LexerType::Token& tok) {
    if (!std::holds_alternative<LexerType::Operator>(tok)) return false;
    auto sym = std::get<LexerType::Operator>(tok).symbol;
    return sym == "+" || sym == "-" || sym == "*" || sym == "/" || sym == "%" ||
           sym == "==" || sym == "!=" || sym == ">" || sym == "<" || sym == ">=" || sym == "<=" ||
           sym == "and" || sym == "or";
}

// 检查是否是变量或其他非常量的标记
inline bool isVariable(const LexerType::Token& tok) {
    return std::holds_alternative<LexerType::Keyword>(tok);
}

inline LexerType::Value evalBinaryEx(IRType::IRCompilerStatus& irstat,
                                     const LexerType::Value& lhs,
                                     const LexerType::Value& rhs,
                                     const std::string& op) {
    auto l = lhs.data;
    auto r = rhs.data;

    if (std::holds_alternative<ZeusType::BigInteger>(l) && std::holds_alternative<ZeusType::BigInteger>(r)) {
        const ZeusType::BigInteger& li = std::get<ZeusType::BigInteger>(l);
        const ZeusType::BigInteger& ri = std::get<ZeusType::BigInteger>(r);
        if (op == "+") return LexerType::Value(li + ri);
        if (op == "-") return LexerType::Value(li - ri);
        if (op == "*") return LexerType::Value(li * ri);
        if (op == "/") {
            if (ri == ZeusType::BigInteger(0)) {
                TreeParser::Node dummy;
                dummy.type = NodeType::EXP_STMT;
                IRExceptionNS::IRException ex("Division by zero", 0, dummy);
                ex._throw(irstat);
                return LexerType::Value(0);
            }
            return LexerType::Value(li / ri);
        }
        if (op == "%") {
            if (ri == ZeusType::BigInteger(0)) {
                TreeParser::Node dummy;
                dummy.type = NodeType::EXP_STMT;
                IRExceptionNS::IRException ex("Division by zero", 0, dummy);
                ex._throw(irstat);
                return LexerType::Value(0);
            }
            return LexerType::Value(li % ri);
        }
    }

    if ((std::holds_alternative<ZeusType::BigInteger>(l) && std::holds_alternative<ZeusType::BigDecimal>(r)) ||
        (std::holds_alternative<ZeusType::BigDecimal>(l) && std::holds_alternative<ZeusType::BigInteger>(r)) ||
        (std::holds_alternative<ZeusType::BigDecimal>(l) && std::holds_alternative<ZeusType::BigDecimal>(r))) {
        ZeusType::BigDecimal lf = std::holds_alternative<ZeusType::BigInteger>(l) ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(l)) : std::get<ZeusType::BigDecimal>(l);
        ZeusType::BigDecimal rf = std::holds_alternative<ZeusType::BigInteger>(r) ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(r)) : std::get<ZeusType::BigDecimal>(r);
        if (op == "+") return LexerType::Value(lf + rf);
        if (op == "-") return LexerType::Value(lf - rf);
        if (op == "*") return LexerType::Value(lf * rf);
        if (op == "/") {
            if (rf == ZeusType::BigDecimal(0)) {
                TreeParser::Node dummy;
                dummy.type = NodeType::EXP_STMT;
                IRExceptionNS::IRException ex("Division by zero", 0, dummy);
                ex._throw(irstat);
                return LexerType::Value(ZeusType::BigDecimal(0));
            }
            return LexerType::Value(lf.divide(rf, 16));
        }
    }

    if (std::holds_alternative<std::string>(l) && std::holds_alternative<std::string>(r)) {
        if (op == "+") return LexerType::Value(std::get<std::string>(l) + std::get<std::string>(r));
        if (op == "==") return LexerType::Value(std::get<std::string>(l) == std::get<std::string>(r));
        if (op == "!=") return LexerType::Value(std::get<std::string>(l) != std::get<std::string>(r));
    }

    // 通用比较：尝试对任意同类型（或数值可互转）做 == != > < >= <=
    auto cmpNumbers = [&](double A, double B) -> LexerType::Value {
        if (op == "==") return LexerType::Value(A == B);
        if (op == "!=") return LexerType::Value(A != B);
        if (op == ">")  return LexerType::Value(A >  B);
        if (op == "<")  return LexerType::Value(A <  B);
        if (op == ">=") return LexerType::Value(A >= B);
        if (op == "<=") return LexerType::Value(A <= B);
    TreeParser::Node dummy;
    dummy.type = NodeType::EXP_STMT;
    IRExceptionNS::IRException ex("Unsupported numeric comparison operator", 0, dummy);
    ex._throw(irstat);
    return LexerType::Value(0);
    };

    // BigInteger/BigDecimal 混合比较（上面算术已处理；这里只管比较）
    if ((std::holds_alternative<ZeusType::BigInteger>(l) || std::holds_alternative<ZeusType::BigDecimal>(l)) &&
        (std::holds_alternative<ZeusType::BigInteger>(r) || std::holds_alternative<ZeusType::BigDecimal>(r))) {
        ZeusType::BigDecimal A = std::holds_alternative<ZeusType::BigInteger>(l) ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(l)) : std::get<ZeusType::BigDecimal>(l);
        ZeusType::BigDecimal B = std::holds_alternative<ZeusType::BigInteger>(r) ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(r)) : std::get<ZeusType::BigDecimal>(r);
        if (op == "==" || op == "!=" || op == ">" || op == "<" || op == ">=" || op == "<=") {
            double Ad = A.toDouble(), Bd = B.toDouble();
            return cmpNumbers(Ad, Bd);
        }
    }

    // bool 与 bool 的比较
    if (std::holds_alternative<bool>(l) && std::holds_alternative<bool>(r)) {
        bool A = std::get<bool>(l), B = std::get<bool>(r);
        if (op == "==") return LexerType::Value(A == B);
        if (op == "!=") return LexerType::Value(A != B);
        if (op == ">=") return LexerType::Value(A >= B);
        if (op == "<=") return LexerType::Value(A <= B);
        // 逻辑运算
        if (op == "and") return LexerType::Value(A && B);
        if (op == "or") return LexerType::Value(A || B);
    }

    // 逻辑运算支持任意类型(转换为bool)
    if (op == "and" || op == "or") {
        auto toBool = [](const std::variant<ZeusType::BigInteger, ZeusType::BigDecimal, std::string, bool>& v) -> bool {
            if (std::holds_alternative<bool>(v)) return std::get<bool>(v);
            if (std::holds_alternative<ZeusType::BigInteger>(v)) return std::get<ZeusType::BigInteger>(v) != ZeusType::BigInteger(0);
            if (std::holds_alternative<ZeusType::BigDecimal>(v)) return std::get<ZeusType::BigDecimal>(v) != ZeusType::BigDecimal(0);
            if (std::holds_alternative<std::string>(v)) return !std::get<std::string>(v).empty();
            return false;
        };
        bool A = toBool(l);
        bool B = toBool(r);
        if (op == "and") return LexerType::Value(A && B);
        if (op == "or") return LexerType::Value(A || B);
    }

    {
        TreeParser::Node dummy;
        dummy.type = NodeType::EXP_STMT;
        std::string msg = std::string("Unsupported operand types for operator ") + op + " (" +
            (std::holds_alternative<ZeusType::BigInteger>(l)?"int": std::holds_alternative<ZeusType::BigDecimal>(l)?"float": std::holds_alternative<std::string>(l)?"string": std::holds_alternative<bool>(l)?"bool":"?") +
            ", " +
            (std::holds_alternative<ZeusType::BigInteger>(r)?"int": std::holds_alternative<ZeusType::BigDecimal>(r)?"float": std::holds_alternative<std::string>(r)?"string": std::holds_alternative<bool>(r)?"bool":"?") + ")";
        IRExceptionNS::IRException ex(msg, 0, dummy);
        ex._throw(irstat);
        return LexerType::Value(ZeusType::BigInteger(0));
    }
}

// 折叠一个连续常量段
inline std::vector<LexerType::Token> foldConstSegment(IRType::IRCompilerStatus& irstat, const std::vector<LexerType::Token>& seg) {
    if (seg.empty()) return seg;
    std::stack<LexerType::Operator> opStack;
    std::stack<LexerType::Value> valStack;

    for (auto& tok : seg) {
        if (std::holds_alternative<LexerType::Value>(tok)) {
            valStack.push(std::get<LexerType::Value>(tok));
        } else if (std::holds_alternative<LexerType::Operator>(tok)) {
            auto op = std::get<LexerType::Operator>(tok);
            while (!opStack.empty() && opPrecedence(opStack.top().symbol) >= opPrecedence(op.symbol)) {
                if (valStack.size() < 2) return seg;
                auto rhs = valStack.top(); valStack.pop();
                auto lhs = valStack.top(); valStack.pop();
                valStack.push(evalBinaryEx(irstat, lhs, rhs, opStack.top().symbol));
                opStack.pop();
            }
            opStack.push(op);
        } else {
            return seg; // 遇到变量等中断
        }
    }

    while (!opStack.empty()) {
        if (valStack.size() < 2) return seg;
        auto rhs = valStack.top(); valStack.pop();
        auto lhs = valStack.top(); valStack.pop();
    valStack.push(evalBinaryEx(irstat, lhs, rhs, opStack.top().symbol));
        opStack.pop();
    }

    if (valStack.size() != 1) return seg;

    return { valStack.top() };
}

// 用于构建表达式树的结构
struct ExprNode {
    LexerType::Token token;
    ExprNode* left = nullptr;
    ExprNode* right = nullptr;
    
    ExprNode(const LexerType::Token& t) : token(t) {}
    ~ExprNode() {
        delete left;
        delete right;
    }
};

// 构建表达式树
inline ExprNode* buildExprTree(const std::vector<LexerType::Token>& tokens) {
    if (tokens.empty()) return nullptr;
    
    std::stack<ExprNode*> nodeStack;
    std::stack<LexerType::Operator> opStack;
    
    for (auto& tok : tokens) {
        if (std::holds_alternative<LexerType::Value>(tok) || isVariable(tok)) {
            nodeStack.push(new ExprNode(tok));
        } else if (std::holds_alternative<LexerType::Operator>(tok) && isBinaryOp(tok)) {
            auto op = std::get<LexerType::Operator>(tok);
            
            while (!opStack.empty() && opPrecedence(opStack.top().symbol) >= opPrecedence(op.symbol)) {
                auto opNode = new ExprNode(opStack.top());
                opStack.pop();
                
                if (nodeStack.size() < 2) {
                    delete opNode;
                    // 清理资源
                    while (!nodeStack.empty()) {
                        delete nodeStack.top();
                        nodeStack.pop();
                    }
                    return nullptr;
                }
                
                opNode->right = nodeStack.top(); nodeStack.pop();
                opNode->left = nodeStack.top(); nodeStack.pop();
                nodeStack.push(opNode);
            }
            
            opStack.push(op);
        }
    }
    
    while (!opStack.empty()) {
        auto opNode = new ExprNode(opStack.top());
        opStack.pop();
        
        if (nodeStack.size() < 2) {
            delete opNode;
            // 清理资源
            while (!nodeStack.empty()) {
                delete nodeStack.top();
                nodeStack.pop();
            }
            return nullptr;
        }
        
        opNode->right = nodeStack.top(); nodeStack.pop();
        opNode->left = nodeStack.top(); nodeStack.pop();
        nodeStack.push(opNode);
    }
    
    if (nodeStack.size() != 1) {
        // 清理资源
        while (!nodeStack.empty()) {
            delete nodeStack.top();
            nodeStack.pop();
        }
        return nullptr;
    }
    
    return nodeStack.top();
}

// 优化表达式树中的常量子表达式
inline bool optimizeConstantSubtrees(IRType::IRCompilerStatus& irstat, ExprNode* node) {
    if (!node) return false;
    
    // 首先递归优化左右子树
    if (node->left) optimizeConstantSubtrees(irstat, node->left);
    if (node->right) optimizeConstantSubtrees(irstat, node->right);
    
    // 如果是二元操作符，尝试优化
    if (isBinaryOp(node->token) && node->left && node->right) {
        // 尝试优化同级别的连续常量乘法或加法，例如 4*4 或 3+5
        if (std::holds_alternative<LexerType::Value>(node->left->token) && 
            std::holds_alternative<LexerType::Value>(node->right->token)) {
            
            auto lhs = std::get<LexerType::Value>(node->left->token);
            auto rhs = std::get<LexerType::Value>(node->right->token);
            auto op = std::get<LexerType::Operator>(node->token).symbol;
            
            node->token = evalBinaryEx(irstat, lhs, rhs, op);

            if (irstat != IRType::IRCompilerStatus::Running) {
                return false;
            }

            delete node->left;
            node->left = nullptr;

            delete node->right;
            node->right = nullptr;

            return true;
        }
        
        // 处理形如 x*4*4 的表达式，优化右侧的 4*4
        if (std::holds_alternative<LexerType::Operator>(node->token) && 
            std::get<LexerType::Operator>(node->token).symbol == "*" &&
            node->right && std::holds_alternative<LexerType::Operator>(node->right->token) &&
            std::get<LexerType::Operator>(node->right->token).symbol == "*") {
            
            auto rightOp = node->right;
            // 如果右侧的右子节点和左子节点都是常量，可以优化
            if (rightOp->left && rightOp->right && 
                std::holds_alternative<LexerType::Value>(rightOp->left->token) && 
                std::holds_alternative<LexerType::Value>(rightOp->right->token)) {
                
                auto lhs = std::get<LexerType::Value>(rightOp->left->token);
                auto rhs = std::get<LexerType::Value>(rightOp->right->token);
                auto op = std::get<LexerType::Operator>(rightOp->token).symbol;
                
                LexerType::Value result = evalBinaryEx(irstat, lhs, rhs, op);

                if (irstat != IRType::IRCompilerStatus::Running) {
                    return std::holds_alternative<LexerType::Value>(node->token);
                }

                ExprNode* newRightNode = new ExprNode(result);

                delete node->right;
                node->right = newRightNode;
            }
        }
    }
    
    return std::holds_alternative<LexerType::Value>(node->token);
}

// 将表达式树转换回中缀表达式的token序列
inline void treeToTokens(ExprNode* node, std::vector<LexerType::Token>& tokens) {
    if (!node) return;
    
    // 叶子节点（变量或常量）
    if (!node->left && !node->right) {
        tokens.push_back(node->token);
        return;
    }
    
    // 如果是二元操作符，使用中缀表示
    if (isBinaryOp(node->token)) {
        // 优先处理左子树
        treeToTokens(node->left, tokens);
        
        // 然后是操作符
        tokens.push_back(node->token);
        
        // 最后是右子树
        treeToTokens(node->right, tokens);
    } else {
        // 其他情况，直接添加节点
        tokens.push_back(node->token);
    }
}

// 表达式折叠（统一 irstat 版本）
inline std::vector<LexerType::Token> foldExpression(
    IRType::IRCompilerStatus& irstat,
    const std::vector<LexerType::Token>& tokens) {

    if (tokens.size() <= 1) {
        return tokens;
    }

    bool allConst = std::all_of(
        tokens.begin(),
        tokens.end(),
        [](const LexerType::Token& t) {
            return std::holds_alternative<LexerType::Value>(t) ||
                   (std::holds_alternative<LexerType::Operator>(t) && isBinaryOp(t));
        }
    );

    if (allConst) {
        return foldConstSegment(irstat, tokens);
    }

    std::vector<LexerType::Token> result;

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (isVariable(tokens[i])) {
            result.push_back(tokens[i]);
            continue;
        }

        if (std::holds_alternative<LexerType::Value>(tokens[i])) {
            bool pattern = i + 2 < tokens.size() &&
                           std::holds_alternative<LexerType::Operator>(tokens[i + 1]) &&
                           std::holds_alternative<LexerType::Value>(tokens[i + 2]);

            if (!pattern) {
                result.push_back(tokens[i]);
                continue;
            }

            auto lhs = std::get<LexerType::Value>(tokens[i]);
            auto op  = std::get<LexerType::Operator>(tokens[i + 1]).symbol;
            auto rhs = std::get<LexerType::Value>(tokens[i + 2]);

            result.push_back(evalBinaryEx(irstat, lhs, rhs, op));
            i += 2;
            continue;
        }

        if (std::holds_alternative<LexerType::Operator>(tokens[i])) {
            bool pattern = i + 3 < tokens.size() &&
                           std::holds_alternative<LexerType::Value>(tokens[i + 1]) &&
                           std::holds_alternative<LexerType::Operator>(tokens[i + 2]) &&
                           std::holds_alternative<LexerType::Value>(tokens[i + 3]);

            if (!pattern) {
                result.push_back(tokens[i]);
                continue;
            }

            auto op1  = std::get<LexerType::Operator>(tokens[i]).symbol;
            auto val1 = std::get<LexerType::Value>(tokens[i + 1]);
            auto op2  = std::get<LexerType::Operator>(tokens[i + 2]).symbol;
            auto val2 = std::get<LexerType::Value>(tokens[i + 3]);

            if (op1 == op2 && (op1 == "*" || op1 == "/")) {
                result.push_back(tokens[i]);
                result.push_back(evalBinaryEx(irstat, val1, val2, op1));
                i += 3;
                continue;
            }

            result.push_back(tokens[i]);
            continue;
        }

        result.push_back(tokens[i]);
    }

    if (result.size() == tokens.size()) {
        for (size_t i = 0; i + 4 < result.size(); ++i) {
            bool pattern = std::holds_alternative<LexerType::Keyword>(result[i]) &&
                           std::holds_alternative<LexerType::Operator>(result[i + 1]) &&
                           std::get<LexerType::Operator>(result[i + 1]).symbol == "*" &&
                           std::holds_alternative<LexerType::Value>(result[i + 2]) &&
                           std::holds_alternative<LexerType::Operator>(result[i + 3]) &&
                           std::get<LexerType::Operator>(result[i + 3]).symbol == "*" &&
                           std::holds_alternative<LexerType::Value>(result[i + 4]);

            if (!pattern) {
                continue;
            }

            auto lhs = std::get<LexerType::Value>(result[i + 2]);
            auto rhs = std::get<LexerType::Value>(result[i + 4]);
            auto op  = std::get<LexerType::Operator>(result[i + 3]).symbol;

            result[i + 2] = evalBinaryEx(irstat, lhs, rhs, op);
            result.erase(result.begin() + i + 3, result.begin() + i + 5);
        }
    }

    for (size_t i = 0; i < result.size(); ++i) {
        bool pattern = i + 2 < result.size() &&
                       std::holds_alternative<LexerType::Value>(result[i]) &&
                       std::holds_alternative<LexerType::Operator>(result[i + 1]) &&
                       std::get<LexerType::Operator>(result[i + 1]).symbol == "+" &&
                       std::holds_alternative<LexerType::Value>(result[i + 2]);

        if (!pattern) {
            continue;
        }

        auto lhs = std::get<LexerType::Value>(result[i]);
        auto rhs = std::get<LexerType::Value>(result[i + 2]);

        bool bothString = std::holds_alternative<std::string>(lhs.data) &&
                          std::holds_alternative<std::string>(rhs.data);

        if (!bothString) {
            continue;
        }

        result[i] = evalBinaryEx(irstat, lhs, rhs, "+");
        result.erase(result.begin() + i + 1, result.begin() + i + 3);
        i -= (i > 0 ? 1 : 0);
    }

    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i + 2 < result.size(); ++i) {
            bool pattern = std::holds_alternative<LexerType::Value>(result[i]) &&
                           std::holds_alternative<LexerType::Operator>(result[i + 1]) &&
                           std::holds_alternative<LexerType::Value>(result[i + 2]);

            if (!pattern) {
                continue;
            }

            auto lhs = std::get<LexerType::Value>(result[i]);
            auto op  = std::get<LexerType::Operator>(result[i + 1]).symbol;
            auto rhs = std::get<LexerType::Value>(result[i + 2]);

            result[i] = evalBinaryEx(irstat, lhs, rhs, op);
            result.erase(result.begin() + i + 1, result.begin() + i + 3);
            changed = true;
            if (i > 0) {
                --i;
            }
        }
    }

    return result;
}

std::vector<LexerType::Token> infixToPostfixExpr(const std::vector<LexerType::Token>& infix) {
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

}

#endif
