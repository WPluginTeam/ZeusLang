#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "LexerParser.hpp"
#include "GenericCallSyntax.hpp"
#include "SyntaxValidator.hpp"
#include "../ZeusDiagnostics.hpp"

namespace ZeusParser {
namespace NativeTypeDeclarationDetail {

using Tokens = std::vector<LexerType::Token>;
using Bindings = std::map<std::string, std::string>;

inline bool op(const Tokens& tokens, size_t i, const std::string& value) {
    return i < tokens.size() && isGenericCallOperator(tokens[i], value);
}

inline std::string word(const Tokens& tokens, size_t i) {
    if (i >= tokens.size()) return {};
    const auto* keyword = std::get_if<LexerType::Keyword>(&tokens[i]);
    return keyword ? keyword->name : std::string();
}

inline void error(std::vector<ZeusDiagnostics::Diagnostic>& diagnostics,
                  const Tokens& tokens, size_t i, const std::string& message) {
    ZeusDiagnostics::Diagnostic diagnostic{"P0043", message};
    if (i < tokens.size()) std::visit([&](const auto& token) {
        diagnostic.line = token.line;
        diagnostic.column = token.column;
    }, tokens[i]);
    diagnostics.push_back(std::move(diagnostic));
}

enum class NativeTypeDeclarationValueKind {
    None,
    NumericID,
    StaticValueReference
};

struct NativeTypeDeclarationSyntax {
    bool matched = false;
    size_t beginToken = 0;
    size_t endToken = 0;
    size_t nameToken = 0;
    size_t valueToken = 0;
    std::string name;
    NativeTypeDeclarationValueKind valueKind = NativeTypeDeclarationValueKind::None;
    std::string numericID;
    std::string staticValueReference;
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;

    bool valid() const {
        return matched && diagnostics.empty();
    }
};

inline bool isNativeTypeAliasIdentifier(const LexerType::Token& token) {
    if (!isSyntaxIdentifier(token)) return false;
    const std::string& name = std::get<LexerType::Keyword>(token).name;
    // Declaration modifiers and intrinsic decorators are contextual names in
    // the general parser, but cannot serve as a native type declaration name.
    static const std::set<std::string> reserved = {
        "inline", "native", "type", "no_safepoint", "addr", "faddr", "reveal",
        "this", "operator", "public", "private", "protected"
    };
    return !name.empty() && reserved.count(name) == 0;
}

inline NativeTypeDeclarationSyntax parseNativeTypeDeclaration(
    const Tokens& tokens, size_t begin
) {
    NativeTypeDeclarationSyntax declaration;
    if (!op(tokens, begin, "@") || word(tokens, begin + 1) != "native" ||
        word(tokens, begin + 2) != "decl") return declaration;
    if (word(tokens, begin + 3) == "func" && !op(tokens, begin + 4, "="))
        return declaration;

    declaration.matched = true;
    declaration.beginToken = begin;
    declaration.nameToken = begin + 3;
    declaration.valueToken = begin + 5;
    size_t end = begin + 3;
    while (end < tokens.size() && !op(tokens, end, ";") &&
           !op(tokens, end, "{") && !op(tokens, end, "}")) ++end;
    const bool terminated = op(tokens, end, ";");
    declaration.endToken = end + (terminated ? 1 : 0);

    if (declaration.nameToken >= tokens.size() ||
        !isNativeTypeAliasIdentifier(tokens[declaration.nameToken])) {
        error(declaration.diagnostics, tokens, declaration.nameToken,
              "native type alias requires a non-reserved identifier");
        return declaration;
    }
    declaration.name = word(tokens, declaration.nameToken);
    if (!terminated || !op(tokens, begin + 4, "=")) {
        error(declaration.diagnostics, tokens, begin,
              "expected '@native decl Name = TypeID;' or a static integer constant");
        return declaration;
    }

    if (declaration.valueToken + 1 == end &&
        std::holds_alternative<LexerType::Value>(tokens[declaration.valueToken])) {
        const auto& value = std::get<LexerType::Value>(tokens[declaration.valueToken]);
        if (std::holds_alternative<ZeusType::BigInteger>(value.data)) {
            declaration.valueKind = NativeTypeDeclarationValueKind::NumericID;
            declaration.numericID = value.toString();
            return declaration;
        }
    }
    size_t cursor = declaration.valueToken;
    if (parseGenericTypeName(tokens, cursor, declaration.staticValueReference) && cursor == end) {
        declaration.valueKind = NativeTypeDeclarationValueKind::StaticValueReference;
        return declaration;
    }
    error(declaration.diagnostics, tokens, declaration.valueToken,
          "native type declaration requires an integer type ID or a static integer constant");
    return declaration;
}

struct Import {
    std::string package;
    std::string alias;
    size_t token = 0;
    bool namespaceMode = false;
};

inline std::vector<Import> imports(const Tokens& tokens) {
    std::vector<Import> result;
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (op(tokens, i, "{")) ++depth;
        if (op(tokens, i, "}")) --depth;
        if (depth != 0 || word(tokens, i) != "import") continue;
        Import item;
        item.token = i;
        size_t cursor = i + 1;
        if (!parseGenericTypeName(tokens, cursor, item.package)) continue;
        if (op(tokens, cursor, "[") && op(tokens, cursor + 1, "&")) {
            item.namespaceMode = true;
            item.alias = word(tokens, cursor + 2);
            if (item.alias.empty()) {
                const size_t dot = item.package.find_last_of('.');
                item.alias = item.package.substr(dot == std::string::npos ? 0 : dot + 1);
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}

inline std::set<std::string> exports(const Tokens& tokens, bool& explicitList) {
    std::set<std::string> names;
    int depth = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (depth == 0 && word(tokens, i) == "using" && op(tokens, i + 1, "{")) {
            explicitList = true;
            for (i += 2; i < tokens.size() && !op(tokens, i, "}"); ++i) {
                const std::string name = word(tokens, i);
                if (!name.empty()) names.insert(name);
            }
            continue;
        }
        if (op(tokens, i, "{")) ++depth;
        if (op(tokens, i, "}")) --depth;
    }
    return names;
}

} // namespace NativeTypeDeclarationDetail
} // namespace ZeusParser
