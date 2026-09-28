#ifndef ZEUS_PARSER_EXPRESSION_SYNTAX_VALIDATOR_HPP
#define ZEUS_PARSER_EXPRESSION_SYNTAX_VALIDATOR_HPP

#include <string>
#include <utility>
#include <vector>
#include "LexerType.hpp"
#include "../ZeusDiagnostics.hpp"

namespace ZeusParser {
namespace SyntaxDetail {

class ExpressionParser {
public:
    explicit ExpressionParser(const std::vector<LexerType::Token>& tokens)
        : tokens_(tokens) {}

    std::vector<ZeusDiagnostics::Diagnostic> parse() {
        if (tokens_.empty()) {
            fail("expected an expression");
            return diagnostics_;
        }
        if (!parseOr()) return diagnostics_;
        if (position_ != tokens_.size()) {
            fail("unexpected token '" + tokenText(tokens_[position_]) + "'");
        }
        return diagnostics_;
    }

private:
    const std::vector<LexerType::Token>& tokens_;
    size_t position_ = 0;
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics_;
    bool reported_ = false;

    static std::string tokenText(const LexerType::Token& token) {
        return std::visit([](const auto& value) { return value.toString(); }, token);
    }

    bool atOperator(const std::string& symbol) const {
        return position_ < tokens_.size() &&
               std::holds_alternative<LexerType::Operator>(tokens_[position_]) &&
               std::get<LexerType::Operator>(tokens_[position_]).symbol == symbol;
    }

    bool consumeOperator(const std::string& symbol) {
        if (!atOperator(symbol)) return false;
        ++position_;
        return true;
    }

    bool isIdentifier(const LexerType::Token& token) const {
        if (!std::holds_alternative<LexerType::Keyword>(token)) return false;
        const std::string& name = std::get<LexerType::Keyword>(token).name;
        static const std::vector<std::string> reserved = {
            "var", "let", "const", "shared", "if", "else", "for", "func", "class",
            "return", "break", "continue", "delete", "throw", "try", "catch", "with",
            "import", "using", "original", "decl", "static", "new", "true", "false", "nil"
        };
        for (const auto& keyword : reserved) {
            if (name == keyword) return false;
        }
        return true;
    }

    bool isMemberName(const LexerType::Token& token) const {
        // The lexer represents contextual names such as the built-in
        // `with` method as Keyword tokens. After '.', they are member names,
        // not control-flow keywords.
        return std::holds_alternative<LexerType::Keyword>(token);
    }

    bool fail(const std::string& message) {
        if (!reported_) {
            ZeusDiagnostics::Diagnostic diagnostic;
            diagnostic.code = "P0020";
            diagnostic.message = message;
            if (!tokens_.empty()) {
                const size_t index = position_ < tokens_.size()
                    ? position_
                    : tokens_.size() - 1;
                std::visit([&](const auto& token) {
                    diagnostic.line = token.line;
                    diagnostic.column = token.column;
                }, tokens_[index]);
            }
            diagnostics_.push_back(std::move(diagnostic));
            reported_ = true;
        }
        return false;
    }

    bool parseOr() {
        if (!parseAnd()) return false;
        while (atOperator("or") || (atOperator("|") && position_ + 1 < tokens_.size() &&
                                     std::holds_alternative<LexerType::Operator>(tokens_[position_ + 1]) &&
                                     std::get<LexerType::Operator>(tokens_[position_ + 1]).symbol == "|")) {
            if (atOperator("or")) ++position_;
            else position_ += 2; // Lexer currently emits `||` as two tokens.
            if (!parseAnd()) return fail("expected an expression after 'or'");
        }
        return true;
    }

    bool parseAnd() {
        if (!parseComparison()) return false;
        while (atOperator("and")) {
            ++position_;
            if (!parseComparison()) return fail("expected an expression after 'and'");
        }
        return true;
    }

    bool parseComparison() {
        if (!parseAdditive()) return false;
        while (atOperator("==") || atOperator("!=") || atOperator(">") ||
               atOperator("<") || atOperator(">=") || atOperator("<=")) {
            ++position_;
            if (!parseAdditive()) return fail("expected an expression after comparison operator");
        }
        return true;
    }

    bool parseAdditive() {
        if (!parseMultiplicative()) return false;
        while (atOperator("+") || atOperator("-")) {
            ++position_;
            if (!parseMultiplicative()) return fail("expected an expression after additive operator");
        }
        return true;
    }

    bool parseMultiplicative() {
        if (!parseUnary()) return false;
        while (atOperator("*") || atOperator("/") || atOperator("%")) {
            ++position_;
            if (!parseUnary()) return fail("expected an expression after multiplicative operator");
        }
        return true;
    }

    bool parseUnary() {
        if (consumeOperator("+") || consumeOperator("-") || consumeOperator("!")) {
            if (!parseUnary()) return fail("expected an expression after unary operator");
            return true;
        }
        return parsePostfix();
    }

    bool parsePostfix() {
        if (!parsePrimary()) return false;
        while (position_ < tokens_.size()) {
            if (consumeOperator(".")) {
                if (position_ >= tokens_.size() || !isMemberName(tokens_[position_])) {
                    return fail("expected a member name after '.'");
                }
                ++position_;
                continue;
            }
            if (consumeOperator("(")) {
                if (!parseArguments(")")) return false;
                continue;
            }
            if (consumeOperator("[")) {
                if (atOperator("]")) return fail("expected an index expression");
                if (!parseOr()) return false;
                if (!consumeOperator("]")) return fail("expected ']' after index expression");
                continue;
            }
            break;
        }
        return true;
    }

    bool parsePrimary() {
        if (position_ >= tokens_.size()) return fail("expected an expression");

        if (std::holds_alternative<LexerType::Value>(tokens_[position_]) ||
            std::holds_alternative<LexerType::BytesLiteral>(tokens_[position_])) {
            ++position_;
            return true;
        }

        if (std::holds_alternative<LexerType::Keyword>(tokens_[position_])) {
            const std::string name = std::get<LexerType::Keyword>(tokens_[position_]).name;
            if (name == "func") {
                return fail("anonymous functions and function expressions are not supported");
            }
            if (name == "new") {
                ++position_;
                if (position_ >= tokens_.size() || !isIdentifier(tokens_[position_])) {
                    return fail("expected a class name after 'new'");
                }
                ++position_;
                while (consumeOperator(".")) {
                    if (position_ >= tokens_.size() || !isIdentifier(tokens_[position_])) {
                        return fail("expected a class name after '.' in new expression");
                    }
                    ++position_;
                }
                return true;
            }
            if (isIdentifier(tokens_[position_]) || name == "true" || name == "false" || name == "nil") {
                ++position_;
                return true;
            }
            return fail("expected an expression");
        }

        if (consumeOperator("(")) {
            if (atOperator(")")) return fail("empty parentheses are not an expression");
            if (!parseOr()) return false;
            if (!consumeOperator(")")) return fail("expected ')' after grouped expression");
            return true;
        }
        if (consumeOperator("[")) return parseList();
        if (consumeOperator("{")) return parseDictionary();
        return fail("expected an expression");
    }

    bool parseArguments(const std::string& close) {
        if (consumeOperator(close)) return true;
        while (true) {
            if (position_ >= tokens_.size() || atOperator(",") || atOperator(close)) {
                return fail("expected a function argument");
            }
            if (position_ + 1 < tokens_.size() && isIdentifier(tokens_[position_]) &&
                isSyntaxAssignment(position_ + 1)) {
                position_ += 2;
                if (position_ >= tokens_.size() || atOperator(",") || atOperator(close)) {
                    return fail("expected a value for named argument");
                }
            }
            if (!parseOr()) return false;
            if (consumeOperator(close)) return true;
            if (!consumeOperator(",")) return fail("expected ',' or '" + close + "' after argument");
            if (consumeOperator(close)) return true; // trailing comma is accepted by the current parser
        }
    }

    bool parseList() {
        if (consumeOperator("]")) return true;
        while (true) {
            if (position_ >= tokens_.size() || atOperator(",") || atOperator("]")) {
                return fail("expected a list element");
            }
            if (!parseOr()) return false;
            if (consumeOperator("]")) return true;
            if (!consumeOperator(",")) return fail("expected ',' or ']' after list element");
            if (consumeOperator("]")) return true; // trailing comma is accepted by the current parser
        }
    }

    bool parseDictionary() {
        if (consumeOperator("}")) return true;
        while (true) {
            if (position_ >= tokens_.size() || atOperator(",") || atOperator("}")) {
                return fail("expected a dictionary key");
            }
            if (!parseOr()) return false;
            if (!consumeOperator(":")) return fail("expected ':' after dictionary key");
            if (position_ >= tokens_.size() || atOperator(",") || atOperator("}")) {
                return fail("expected a dictionary value");
            }
            if (!parseOr()) return false;
            if (consumeOperator("}")) return true;
            if (!consumeOperator(",")) return fail("expected ',' or '}' after dictionary entry");
            if (consumeOperator("}")) return true; // trailing comma is accepted by the current parser
        }
    }

    bool isSyntaxAssignment(size_t index) const {
        return index < tokens_.size() && std::holds_alternative<LexerType::Operator>(tokens_[index]) &&
               std::get<LexerType::Operator>(tokens_[index]).symbol == "=";
    }
};

} // namespace SyntaxDetail

inline std::vector<ZeusDiagnostics::Diagnostic> validateExpressionSyntax(
    const std::vector<LexerType::Token>& tokens
) {
    return SyntaxDetail::ExpressionParser(tokens).parse();
}

} // namespace ZeusParser

#endif
