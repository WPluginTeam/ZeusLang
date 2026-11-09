#ifndef ZEUS_PARSER_WHEN_SYNTAX_DESUGAR_HPP
#define ZEUS_PARSER_WHEN_SYNTAX_DESUGAR_HPP

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "LexerType.hpp"
#include "../ZeusDiagnostics.hpp"

namespace ZeusParser {

    inline bool isWhenOp(const LexerType::Token& tk, const std::string& sym) {
        return std::holds_alternative<LexerType::Operator>(tk) &&
               std::get<LexerType::Operator>(tk).symbol == sym;
    }

    inline bool isWhenKw(const LexerType::Token& tk, const std::string& name) {
        return std::holds_alternative<LexerType::Keyword>(tk) &&
               std::get<LexerType::Keyword>(tk).name == name;
    }

    inline int getWhenTokenLine(const LexerType::Token& tk) {
        return std::visit([](const auto& t) { return t.line; }, tk);
    }

    inline int getWhenTokenCol(const LexerType::Token& tk) {
        return std::visit([](const auto& t) { return t.column; }, tk);
    }

    inline LexerType::Token makeWhenKw(const std::string& name, int line, int col) {
        LexerType::Keyword kw(name);
        kw.line = line;
        kw.column = col;
        return kw;
    }

    inline LexerType::Token makeWhenOp(const std::string& sym, int line, int col) {
        LexerType::Operator op(sym);
        op.line = line;
        op.column = col;
        return op;
    }

    inline LexerType::Token makeWhenStrVal(const std::string& str, int line, int col) {
        LexerType::Value val(str);
        val.line = line;
        val.column = col;
        return val;
    }

    inline bool isWhenBranchSeparator(const LexerType::Token& tk) {
        return isWhenOp(tk, ":") || isWhenOp(tk, "->");
    }

    enum class WhenContextMode {
        STATEMENT,
        ASSIGN,
        RETURN
    };

    struct WhenBranch {
        bool isElse = false;
        std::vector<LexerType::Token> patternTokens;
        std::vector<LexerType::Token> bodyTokens;
        bool bodyIsBlock = false;
        int line = 1;
        int column = 1;
    };

    // Forward declaration for recursive desugaring
    inline std::vector<LexerType::Token> desugarWhenTokens(
        const std::vector<LexerType::Token>& tokens,
        std::vector<ZeusDiagnostics::Diagnostic>* diagnostics = nullptr
    );

    inline bool isWhenConstruct(const std::vector<LexerType::Token>& tokens, size_t i) {
        if (!isWhenKw(tokens[i], "when")) return false;
        // Cannot be preceded by '.' or '?.'
        if (i > 0) {
            if (isWhenOp(tokens[i - 1], ".") || isWhenOp(tokens[i - 1], "?.")) {
                return false;
            }
        }
        if (i + 1 >= tokens.size()) return false;
        if (isWhenOp(tokens[i + 1], "(")) {
            int depth = 1;
            size_t j = i + 2;
            while (j < tokens.size()) {
                if (isWhenOp(tokens[j], "(")) depth++;
                else if (isWhenOp(tokens[j], ")")) {
                    depth--;
                    if (depth == 0) {
                        j++;
                        break;
                    }
                }
                j++;
            }
            return j < tokens.size() && isWhenOp(tokens[j], "{");
        }
        if (isWhenOp(tokens[i + 1], "{")) {
            int depth = 1;
            size_t j = i + 2;
            while (j < tokens.size()) {
                if (isWhenOp(tokens[j], "{")) depth++;
                else if (isWhenOp(tokens[j], "}")) {
                    depth--;
                    if (depth == 0) break;
                } else if (depth == 1 && isWhenBranchSeparator(tokens[j])) {
                    return true;
                }
                j++;
            }
        }
        return false;
    }

    inline bool isStatementBlock(const std::vector<LexerType::Token>& tokens, size_t openBracePos) {
        if (openBracePos >= tokens.size() || !isWhenOp(tokens[openBracePos], "{")) {
            return false;
        }
        size_t k = openBracePos + 1;
        if (k < tokens.size()) {
            if (isWhenOp(tokens[k], "}")) {
                return true;
            }
            if (isWhenKw(tokens[k], "let") || isWhenKw(tokens[k], "var") ||
                isWhenKw(tokens[k], "return") || isWhenKw(tokens[k], "if") ||
                isWhenKw(tokens[k], "for") || isWhenKw(tokens[k], "while") ||
                isWhenKw(tokens[k], "throw") || isWhenKw(tokens[k], "try") ||
                isWhenKw(tokens[k], "print")) {
                return true;
            }
        }
        int depth = 1;
        bool hasSemicolonAtDepth1 = false;
        bool hasColonAtDepth1 = false;
        while (k < tokens.size()) {
            if (isWhenOp(tokens[k], "{")) depth++;
            else if (isWhenOp(tokens[k], "}")) {
                depth--;
                if (depth == 0) break;
            } else if (depth == 1) {
                if (isWhenOp(tokens[k], ";")) {
                    hasSemicolonAtDepth1 = true;
                } else if (isWhenOp(tokens[k], ":")) {
                    hasColonAtDepth1 = true;
                }
            }
            k++;
        }
        if (hasSemicolonAtDepth1) return true;
        if (hasColonAtDepth1) return false;
        return false;
    }

    inline std::vector<LexerType::Token> buildBranchCondition(
        const std::vector<LexerType::Token>& patternTokens,
        const std::string& targetVar,
        int line,
        int col
    ) {
        if (targetVar.empty()) {
            return patternTokens;
        }

        // Split patternTokens by comma at paren/bracket/brace depth 0
        std::vector<std::vector<LexerType::Token>> subPatterns;
        std::vector<LexerType::Token> current;
        int depth = 0;
        for (const auto& tk : patternTokens) {
            if (depth == 0 && isWhenOp(tk, ",")) {
                if (!current.empty()) {
                    subPatterns.push_back(std::move(current));
                    current.clear();
                }
            } else {
                if (isWhenOp(tk, "(") || isWhenOp(tk, "[") || isWhenOp(tk, "{")) depth++;
                else if (isWhenOp(tk, ")") || isWhenOp(tk, "]") || isWhenOp(tk, "}")) depth--;
                current.push_back(tk);
            }
        }
        if (!current.empty()) {
            subPatterns.push_back(std::move(current));
        }

        std::vector<LexerType::Token> cond;
        for (size_t k = 0; k < subPatterns.size(); ++k) {
            if (k > 0) {
                cond.push_back(makeWhenOp("or", line, col));
            }
            const auto& sub = subPatterns[k];
            if (sub.empty()) continue;

            // Check if sub matches `is TypeName`
            if (sub.size() >= 2 && isWhenKw(sub[0], "is")) {
                std::string typeName;
                for (size_t t = 1; t < sub.size(); ++t) {
                    std::string tokStr = std::visit([](const auto& t) { return t.toString(); }, sub[t]);
                    typeName += tokStr;
                }
                // (type(targetVar) == "typeName")
                cond.push_back(makeWhenOp("(", line, col));
                cond.push_back(makeWhenKw("type", line, col));
                cond.push_back(makeWhenOp("(", line, col));
                cond.push_back(makeWhenKw(targetVar, line, col));
                cond.push_back(makeWhenOp(")", line, col));
                cond.push_back(makeWhenOp("==", line, col));
                cond.push_back(makeWhenStrVal(typeName, line, col));
                cond.push_back(makeWhenOp(")", line, col));
            }
            // Check if sub matches `in start..end` or `start..end`
            else if (sub.size() >= 3) {
                bool hasIn = isWhenKw(sub[0], "in");
                size_t startIdx = hasIn ? 1 : 0;
                int dotDotPos = -1;
                size_t dotDotLen = 1;
                int subDepth = 0;
                for (size_t s = startIdx; s < sub.size(); ++s) {
                    if (isWhenOp(sub[s], "(") || isWhenOp(sub[s], "[") || isWhenOp(sub[s], "{")) subDepth++;
                    else if (isWhenOp(sub[s], ")") || isWhenOp(sub[s], "]") || isWhenOp(sub[s], "}")) subDepth--;
                    else if (subDepth == 0) {
                        if (isWhenOp(sub[s], "..")) {
                            dotDotPos = static_cast<int>(s);
                            dotDotLen = 1;
                            break;
                        } else if (isWhenOp(sub[s], ".") && s + 1 < sub.size() && isWhenOp(sub[s + 1], ".")) {
                            dotDotPos = static_cast<int>(s);
                            dotDotLen = 2;
                            break;
                        }
                    }
                }

                if (dotDotPos != -1) {
                    std::vector<LexerType::Token> startTokens(sub.begin() + startIdx, sub.begin() + dotDotPos);
                    std::vector<LexerType::Token> endTokens(sub.begin() + dotDotPos + dotDotLen, sub.end());

                    // ((targetVar >= (startTokens)) and (targetVar <= (endTokens)))
                    cond.push_back(makeWhenOp("(", line, col));
                    cond.push_back(makeWhenOp("(", line, col));
                    cond.push_back(makeWhenKw(targetVar, line, col));
                    cond.push_back(makeWhenOp(">=", line, col));
                    cond.push_back(makeWhenOp("(", line, col));
                    cond.insert(cond.end(), startTokens.begin(), startTokens.end());
                    cond.push_back(makeWhenOp(")", line, col));
                    cond.push_back(makeWhenOp(")", line, col));

                    cond.push_back(makeWhenOp("and", line, col));

                    cond.push_back(makeWhenOp("(", line, col));
                    cond.push_back(makeWhenKw(targetVar, line, col));
                    cond.push_back(makeWhenOp("<=", line, col));
                    cond.push_back(makeWhenOp("(", line, col));
                    cond.insert(cond.end(), endTokens.begin(), endTokens.end());
                    cond.push_back(makeWhenOp(")", line, col));
                    cond.push_back(makeWhenOp(")", line, col));
                    cond.push_back(makeWhenOp(")", line, col));
                } else {
                    cond.push_back(makeWhenOp("(", line, col));
                    cond.push_back(makeWhenKw(targetVar, line, col));
                    cond.push_back(makeWhenOp("==", line, col));
                    cond.push_back(makeWhenOp("(", line, col));
                    cond.insert(cond.end(), sub.begin(), sub.end());
                    cond.push_back(makeWhenOp(")", line, col));
                    cond.push_back(makeWhenOp(")", line, col));
                }
            }
            else {
                // (targetVar == (sub))
                cond.push_back(makeWhenOp("(", line, col));
                cond.push_back(makeWhenKw(targetVar, line, col));
                cond.push_back(makeWhenOp("==", line, col));
                cond.push_back(makeWhenOp("(", line, col));
                cond.insert(cond.end(), sub.begin(), sub.end());
                cond.push_back(makeWhenOp(")", line, col));
                cond.push_back(makeWhenOp(")", line, col));
            }
        }
        return cond;
    }

    inline std::vector<LexerType::Token> formatBranchBody(
        const std::vector<LexerType::Token>& bodyTokens,
        bool bodyIsBlock,
        WhenContextMode mode,
        const std::vector<LexerType::Token>& assignTargetTokens,
        int line,
        int col
    ) {
        std::vector<LexerType::Token> result;
        result.push_back(makeWhenOp("{", line, col));

        if (bodyIsBlock) {
            if (mode == WhenContextMode::STATEMENT) {
                result.insert(result.end(), bodyTokens.begin(), bodyTokens.end());
            } else if (mode == WhenContextMode::ASSIGN) {
                auto modified = bodyTokens;
                while (!modified.empty() && isWhenOp(modified.back(), ";")) {
                    modified.pop_back();
                }
                int depth = 0;
                size_t lastStmtStart = 0;
                for (size_t s = modified.size(); s-- > 0;) {
                    if (isWhenOp(modified[s], ";") && depth == 0) {
                        lastStmtStart = s + 1;
                        break;
                    }
                    if (isWhenOp(modified[s], "}") || isWhenOp(modified[s], ")") || isWhenOp(modified[s], "]")) depth++;
                    else if (isWhenOp(modified[s], "{") || isWhenOp(modified[s], "(") || isWhenOp(modified[s], "[")) depth--;
                }
                bool isReturnStmt = lastStmtStart < modified.size() && isWhenKw(modified[lastStmtStart], "return");
                bool hasAssign = false;
                for (size_t s = lastStmtStart; s < modified.size(); ++s) {
                    if (isWhenOp(modified[s], "=")) { hasAssign = true; break; }
                }
                if (!isReturnStmt && !hasAssign && lastStmtStart < modified.size()) {
                    std::vector<LexerType::Token> transformed(modified.begin(), modified.begin() + lastStmtStart);
                    transformed.insert(transformed.end(), assignTargetTokens.begin(), assignTargetTokens.end());
                    transformed.push_back(makeWhenOp("=", line, col));
                    transformed.insert(transformed.end(), modified.begin() + lastStmtStart, modified.end());
                    transformed.push_back(makeWhenOp(";", line, col));
                    result.insert(result.end(), transformed.begin(), transformed.end());
                } else {
                    result.insert(result.end(), bodyTokens.begin(), bodyTokens.end());
                }
            } else if (mode == WhenContextMode::RETURN) {
                auto modified = bodyTokens;
                while (!modified.empty() && isWhenOp(modified.back(), ";")) {
                    modified.pop_back();
                }
                int depth = 0;
                size_t lastStmtStart = 0;
                for (size_t s = modified.size(); s-- > 0;) {
                    if (isWhenOp(modified[s], ";") && depth == 0) {
                        lastStmtStart = s + 1;
                        break;
                    }
                    if (isWhenOp(modified[s], "}") || isWhenOp(modified[s], ")") || isWhenOp(modified[s], "]")) depth++;
                    else if (isWhenOp(modified[s], "{") || isWhenOp(modified[s], "(") || isWhenOp(modified[s], "[")) depth--;
                }
                bool isReturnStmt = lastStmtStart < modified.size() && isWhenKw(modified[lastStmtStart], "return");
                if (!isReturnStmt && lastStmtStart < modified.size()) {
                    std::vector<LexerType::Token> transformed(modified.begin(), modified.begin() + lastStmtStart);
                    transformed.push_back(makeWhenKw("return", line, col));
                    transformed.insert(transformed.end(), modified.begin() + lastStmtStart, modified.end());
                    transformed.push_back(makeWhenOp(";", line, col));
                    result.insert(result.end(), transformed.begin(), transformed.end());
                } else {
                    result.insert(result.end(), bodyTokens.begin(), bodyTokens.end());
                }
            }
        } else {
            result.insert(result.end(), bodyTokens.begin(), bodyTokens.end());
            if (result.size() <= 1 || !isWhenOp(result.back(), ";")) {
                result.push_back(makeWhenOp(";", line, col));
            }
        }

        result.push_back(makeWhenOp("}", line, col));
        return result;
    }

    inline std::vector<LexerType::Token> desugarWhenTokens(
        const std::vector<LexerType::Token>& tokens,
        std::vector<ZeusDiagnostics::Diagnostic>* diagnostics
    ) {
        bool hasWhen = false;
        for (const auto& tk : tokens) {
            if (isWhenKw(tk, "when")) {
                hasWhen = true;
                break;
            }
        }
        if (!hasWhen) return tokens;

        static std::atomic<std::uint64_t> s_whenCounter{0};
        std::vector<LexerType::Token> out;
        size_t i = 0;

        while (i < tokens.size()) {
            if (!isWhenConstruct(tokens, i)) {
                out.push_back(tokens[i++]);
                continue;
            }

            // Determine context mode
            WhenContextMode mode = WhenContextMode::STATEMENT;
            std::vector<LexerType::Token> assignTargetTokens;

            if (!out.empty() && isWhenKw(out.back(), "return")) {
                mode = WhenContextMode::RETURN;
                out.pop_back(); // Pop `return`
            } else if (out.size() >= 3 &&
                       isWhenOp(out.back(), "=") &&
                       std::holds_alternative<LexerType::Keyword>(out[out.size() - 2]) &&
                       (isWhenKw(out[out.size() - 3], "let") || isWhenKw(out[out.size() - 3], "var"))) {
                mode = WhenContextMode::ASSIGN;
                assignTargetTokens.push_back(out[out.size() - 2]);
                const int line = getWhenTokenLine(out.back());
                const int col = getWhenTokenCol(out.back());
                // Append `nil;` after `=` to form `let varName = nil;` so it is initialized
                out.push_back(makeWhenKw("nil", line, col));
                out.push_back(makeWhenOp(";", line, col));
            } else if (out.size() >= 2 &&
                       isWhenOp(out.back(), "=") &&
                       std::holds_alternative<LexerType::Keyword>(out[out.size() - 2]) &&
                       (out.size() < 3 || (!isWhenKw(out[out.size() - 3], "let") && !isWhenKw(out[out.size() - 3], "var")))) {
                mode = WhenContextMode::ASSIGN;
                assignTargetTokens.push_back(out[out.size() - 2]);
                out.pop_back(); // Pop '='
                out.pop_back(); // Pop identifier
            }

            size_t pos = i;
            const auto whenToken = tokens[pos++];
            int whenLine = getWhenTokenLine(whenToken);
            int whenCol = getWhenTokenCol(whenToken);

            // Optional target expression
            bool hasTarget = false;
            std::vector<LexerType::Token> targetExprTokens;
            if (pos < tokens.size() && isWhenOp(tokens[pos], "(")) {
                hasTarget = true;
                pos++; // consume '('
                int parenDepth = 1;
                while (pos < tokens.size()) {
                    if (isWhenOp(tokens[pos], "(")) parenDepth++;
                    else if (isWhenOp(tokens[pos], ")")) {
                        parenDepth--;
                        if (parenDepth == 0) {
                            pos++; // consume ')'
                            break;
                        }
                    }
                    targetExprTokens.push_back(tokens[pos++]);
                }
            }

            // Expect '{'
            if (pos < tokens.size() && isWhenOp(tokens[pos], "{")) {
                pos++; // consume '{'
            }

            // Parse branches
            std::vector<WhenBranch> branches;
            while (pos < tokens.size()) {
                while (pos < tokens.size() && (isWhenOp(tokens[pos], ";") || isWhenOp(tokens[pos], ","))) {
                    pos++;
                }
                if (pos >= tokens.size()) break;
                if (isWhenOp(tokens[pos], "}")) {
                    pos++; // consume '}'
                    break;
                }

                WhenBranch branch;
                branch.line = getWhenTokenLine(tokens[pos]);
                branch.column = getWhenTokenCol(tokens[pos]);

                if (isWhenKw(tokens[pos], "else")) {
                    branch.isElse = true;
                    pos++; // consume 'else'
                    if (pos < tokens.size() && isWhenBranchSeparator(tokens[pos])) {
                        pos++; // consume separator ':' or '->'
                    }
                } else {
                    int depth = 0;
                    while (pos < tokens.size()) {
                        if (depth == 0 && isWhenBranchSeparator(tokens[pos])) {
                            break;
                        }
                        if (isWhenOp(tokens[pos], "(") || isWhenOp(tokens[pos], "[") || isWhenOp(tokens[pos], "{")) {
                            depth++;
                        } else if (isWhenOp(tokens[pos], ")") || isWhenOp(tokens[pos], "]") || isWhenOp(tokens[pos], "}")) {
                            depth--;
                        }
                        branch.patternTokens.push_back(tokens[pos++]);
                    }
                    if (pos < tokens.size() && isWhenBranchSeparator(tokens[pos])) {
                        pos++; // consume separator ':' or '->'
                    }
                }

                // Parse body
                if (pos < tokens.size() && isWhenOp(tokens[pos], "{") && isStatementBlock(tokens, pos)) {
                    branch.bodyIsBlock = true;
                    pos++; // consume '{'
                    int braceDepth = 1;
                    while (pos < tokens.size()) {
                        if (isWhenOp(tokens[pos], "{")) braceDepth++;
                        else if (isWhenOp(tokens[pos], "}")) {
                            braceDepth--;
                            if (braceDepth == 0) {
                                pos++; // consume '}'
                                break;
                            }
                        }
                        branch.bodyTokens.push_back(tokens[pos++]);
                    }
                    if (pos < tokens.size() && (isWhenOp(tokens[pos], ",") || isWhenOp(tokens[pos], ";"))) {
                        pos++;
                    }
                } else {
                    branch.bodyIsBlock = false;
                    int depth = 0;
                    while (pos < tokens.size()) {
                        if (depth == 0) {
                            if (isWhenOp(tokens[pos], ";") || isWhenOp(tokens[pos], ",")) {
                                pos++;
                                break;
                            }
                            if (isWhenOp(tokens[pos], "}")) {
                                break;
                            }
                        }
                        if (isWhenOp(tokens[pos], "(") || isWhenOp(tokens[pos], "[") || isWhenOp(tokens[pos], "{")) {
                            depth++;
                        } else if (isWhenOp(tokens[pos], ")") || isWhenOp(tokens[pos], "]") || isWhenOp(tokens[pos], "}")) {
                            depth--;
                        }
                        branch.bodyTokens.push_back(tokens[pos++]);
                    }
                }

                branches.push_back(std::move(branch));
            }

            // Desugar and assemble into if-else chain (flat sequential statements, no outer block)
            std::vector<LexerType::Token> desugaredStmts;

            std::string targetVar;
            if (hasTarget) {
                targetVar = "__when_t_" + std::to_string(++s_whenCounter);
                desugaredStmts.push_back(makeWhenKw("let", whenLine, whenCol));
                desugaredStmts.push_back(makeWhenKw(targetVar, whenLine, whenCol));
                desugaredStmts.push_back(makeWhenOp("=", whenLine, whenCol));
                auto desugaredTarget = desugarWhenTokens(targetExprTokens, diagnostics);
                desugaredStmts.insert(desugaredStmts.end(), desugaredTarget.begin(), desugaredTarget.end());
                desugaredStmts.push_back(makeWhenOp(";", whenLine, whenCol));
            }

            std::vector<WhenBranch*> nonElse;
            WhenBranch* elseBranch = nullptr;
            for (auto& b : branches) {
                if (b.isElse) {
                    if (!elseBranch) elseBranch = &b;
                } else {
                    nonElse.push_back(&b);
                }
            }

            auto processBranchBody = [&](WhenBranch* b) {
                std::vector<LexerType::Token> rawTokens;
                if (!b->bodyIsBlock) {
                    if (mode == WhenContextMode::ASSIGN) {
                        rawTokens.insert(rawTokens.end(), assignTargetTokens.begin(), assignTargetTokens.end());
                        rawTokens.push_back(makeWhenOp("=", b->line, b->column));
                        rawTokens.insert(rawTokens.end(), b->bodyTokens.begin(), b->bodyTokens.end());
                        rawTokens.push_back(makeWhenOp(";", b->line, b->column));
                    } else if (mode == WhenContextMode::RETURN) {
                        rawTokens.push_back(makeWhenKw("return", b->line, b->column));
                        rawTokens.insert(rawTokens.end(), b->bodyTokens.begin(), b->bodyTokens.end());
                        rawTokens.push_back(makeWhenOp(";", b->line, b->column));
                    } else {
                        rawTokens = b->bodyTokens;
                    }
                } else {
                    rawTokens = b->bodyTokens;
                }
                auto desugared = desugarWhenTokens(rawTokens, diagnostics);
                return formatBranchBody(desugared, b->bodyIsBlock, mode, assignTargetTokens, b->line, b->column);
            };

            bool isFirstIf = true;
            for (auto* b : nonElse) {
                if (isFirstIf) {
                    desugaredStmts.push_back(makeWhenKw("if", b->line, b->column));
                    isFirstIf = false;
                } else {
                    desugaredStmts.push_back(makeWhenKw("else", b->line, b->column));
                    desugaredStmts.push_back(makeWhenKw("if", b->line, b->column));
                }
                desugaredStmts.push_back(makeWhenOp("(", b->line, b->column));
                auto condTokens = buildBranchCondition(b->patternTokens, targetVar, b->line, b->column);
                desugaredStmts.insert(desugaredStmts.end(), condTokens.begin(), condTokens.end());
                desugaredStmts.push_back(makeWhenOp(")", b->line, b->column));

                auto formattedBody = processBranchBody(b);
                desugaredStmts.insert(desugaredStmts.end(), formattedBody.begin(), formattedBody.end());
            }

            if (elseBranch) {
                auto formattedBody = processBranchBody(elseBranch);
                if (nonElse.empty()) {
                    desugaredStmts.insert(desugaredStmts.end(), formattedBody.begin(), formattedBody.end());
                } else {
                    desugaredStmts.push_back(makeWhenKw("else", elseBranch->line, elseBranch->column));
                    desugaredStmts.insert(desugaredStmts.end(), formattedBody.begin(), formattedBody.end());
                }
            }

            if (pos < tokens.size() && isWhenOp(tokens[pos], ";")) {
                if (mode == WhenContextMode::ASSIGN || mode == WhenContextMode::RETURN) {
                    pos++; // Consume statement-terminating semicolon
                }
            }

            out.insert(out.end(), desugaredStmts.begin(), desugaredStmts.end());
            i = pos;
        }

        return out;
    }

} // namespace ZeusParser

#endif // ZEUS_PARSER_WHEN_SYNTAX_DESUGAR_HPP
