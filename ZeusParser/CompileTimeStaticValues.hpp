#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "LexerParser.hpp"
#include "../ZeusTyping/Types.hpp"

namespace ZeusParser {
namespace CompileTimeStaticValues {

using Tokens = std::vector<LexerType::Token>;
using Values = std::map<std::string, LexerType::Value>;

struct Declaration {
    std::string name;
    std::string annotation;
    Tokens initializer;
};

inline std::string word(const Tokens& tokens, size_t index) {
    if (index >= tokens.size()) return {};
    const auto* keyword = std::get_if<LexerType::Keyword>(&tokens[index]);
    return keyword == nullptr ? std::string() : keyword->name;
}

inline std::string op(const Tokens& tokens, size_t index) {
    if (index >= tokens.size()) return {};
    const auto* token = std::get_if<LexerType::Operator>(&tokens[index]);
    return token == nullptr ? std::string() : token->symbol;
}

inline int line(const LexerType::Token& token) {
    return std::visit([](const auto& value) { return value.line; }, token);
}

inline std::string scalarType(const std::string& name) {
    const auto type = ZeusTypeInfer::parseTypeName(name);
    switch (type) {
        case ZeusTypeInfer::InferredType::Int: return "int";
        case ZeusTypeInfer::InferredType::BigInt: return "bigint";
        case ZeusTypeInfer::InferredType::Float: return "float";
        case ZeusTypeInfer::InferredType::Bool: return "bool";
        case ZeusTypeInfer::InferredType::String: return "string";
        default: return {};
    }
}

inline bool matchesScalarType(const LexerType::Value& value,
                              const std::string& typeName) {
    const std::string type = scalarType(typeName);
    if (type == "int" || type == "bigint")
        return std::holds_alternative<ZeusType::BigInteger>(value.data);
    if (type == "float")
        return std::holds_alternative<ZeusType::BigDecimal>(value.data);
    if (type == "bool") return std::holds_alternative<bool>(value.data);
    if (type == "string") return std::holds_alternative<std::string>(value.data);
    return false;
}

inline std::map<std::string, Declaration> collectDeclarations(
    const Tokens& tokens
) {
    std::map<std::string, Declaration> declarations;
    int braceDepth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string symbol = op(tokens, i);
        if (symbol == "}") --braceDepth;
        const bool moduleScope = braceDepth == 0;
        const std::string keyword = word(tokens, i);
        if (moduleScope && (keyword == "let" || keyword == "var") &&
            word(tokens, i + 1) == "const") {
            const size_t nameIndex = i + 2;
            const std::string name = word(tokens, nameIndex);
            size_t assignment = nameIndex + 1;
            while (assignment < tokens.size() && op(tokens, assignment) != "=" &&
                   op(tokens, assignment) != ";" && op(tokens, assignment) != "{" &&
                   op(tokens, assignment) != "}") {
                if (line(tokens[assignment]) > line(tokens[i])) break;
                ++assignment;
            }
            if (!name.empty() && assignment < tokens.size() &&
                op(tokens, assignment) == "=") {
                Declaration declaration;
                declaration.name = name;
                if (nameIndex + 1 < assignment) {
                    if (op(tokens, nameIndex + 1) != ":" ||
                        nameIndex + 3 != assignment ||
                        word(tokens, nameIndex + 2).empty()) {
                        declaration.name.clear();
                    } else {
                        declaration.annotation = word(tokens, nameIndex + 2);
                    }
                }
                size_t end = assignment + 1;
                const int declarationLine = line(tokens[i]);
                int initializerBraceDepth = 0;
                while (end < tokens.size()) {
                    const std::string endSymbol = op(tokens, end);
                    if (initializerBraceDepth == 0 && endSymbol == ";") break;
                    if (initializerBraceDepth == 0 && line(tokens[end]) > declarationLine) break;
                    if (endSymbol == "{") ++initializerBraceDepth;
                    if (endSymbol == "}") {
                        if (initializerBraceDepth == 0) break;
                        --initializerBraceDepth;
                    }
                    ++end;
                }
                declaration.initializer.assign(
                    tokens.begin() + assignment + 1,
                    tokens.begin() + end
                );
                if (!declaration.name.empty()) {
                    declarations.emplace(declaration.name, std::move(declaration));
                }
            }
        }
        if (symbol == "{") ++braceDepth;
        if (symbol == "}") braceDepth = std::max(braceDepth, 0);
    }
    return declarations;
}

inline bool parseIdentifierPath(const Tokens& tokens, std::string& name) {
    if (tokens.empty() || word(tokens, 0).empty()) return false;
    name = word(tokens, 0);
    size_t cursor = 1;
    while (cursor < tokens.size()) {
        if (op(tokens, cursor) != "." || word(tokens, cursor + 1).empty()) return false;
        name += "." + word(tokens, cursor + 1);
        cursor += 2;
    }
    return true;
}

inline std::optional<LexerType::Value> evaluateLiteralOrReference(
    const Tokens& expression,
    const std::map<std::string, Declaration>& declarations,
    const Values& externalValues,
    Values& resolvedValues,
    std::set<std::string>& resolving
) {
    if (expression.size() == 1) {
        if (const auto* value = std::get_if<LexerType::Value>(&expression[0])) return *value;
    }
    if (expression.size() == 2 &&
        (op(expression, 0) == "-" || op(expression, 0) == "+")) {
        const auto* value = std::get_if<LexerType::Value>(&expression[1]);
        if (value == nullptr ||
            (!std::holds_alternative<ZeusType::BigInteger>(value->data) &&
             !std::holds_alternative<ZeusType::BigDecimal>(value->data))) {
            return std::nullopt;
        }
        if (op(expression, 0) == "+") return *value;
        return LexerType::Value(0) - *value;
    }

    std::string reference;
    if (!parseIdentifierPath(expression, reference)) return std::nullopt;
    const auto local = declarations.find(reference);
    if (local != declarations.end()) {
        const auto ready = resolvedValues.find(reference);
        if (ready != resolvedValues.end()) return ready->second;
        if (!resolving.insert(reference).second) return std::nullopt;
        const Declaration& declaration = local->second;
        const auto value = evaluateLiteralOrReference(
            declaration.initializer, declarations, externalValues,
            resolvedValues, resolving);
        resolving.erase(reference);
        if (!value.has_value()) return std::nullopt;
        if (!declaration.annotation.empty() &&
            !matchesScalarType(*value, declaration.annotation)) return std::nullopt;
        resolvedValues.emplace(reference, *value);
        return value;
    }
    const auto external = externalValues.find(reference);
    if (external != externalValues.end()) return external->second;
    return std::nullopt;
}

inline Values resolveDeclarations(
    const Tokens& tokens,
    const Values& externalValues
) {
    const auto declarations = collectDeclarations(tokens);
    Values values;
    std::set<std::string> resolving;
    for (const auto& declaration : declarations) {
        if (!declaration.second.annotation.empty() &&
            scalarType(declaration.second.annotation).empty()) continue;
        const auto value = evaluateLiteralOrReference(
            declaration.second.initializer, declarations, externalValues,
            values, resolving);
        if (!value.has_value()) continue;
        if (!declaration.second.annotation.empty() &&
            !matchesScalarType(*value, declaration.second.annotation)) continue;
        values.emplace(declaration.first, *value);
    }
    return values;
}

} // namespace CompileTimeStaticValues
} // namespace ZeusParser
