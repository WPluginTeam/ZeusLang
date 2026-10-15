#ifndef ZEUS_PARSER_SYNTAX_VALIDATOR_HPP
#define ZEUS_PARSER_SYNTAX_VALIDATOR_HPP

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>
#include "ExpressionSyntaxValidator.hpp"
#include "LexerType.hpp"
#include "TreeParser.hpp"
#include "../ZeusDiagnostics.hpp"
#include "../ZeusBuiltins/BuiltinRegistry.hpp"

namespace ZeusParser {

inline bool isSyntaxOperator(const LexerType::Token& token, const std::string& symbol) {
    return std::holds_alternative<LexerType::Operator>(token) &&
           std::get<LexerType::Operator>(token).symbol == symbol;
}

inline bool isSyntaxIdentifier(const LexerType::Token& token) {
    if (!std::holds_alternative<LexerType::Keyword>(token)) return false;
    const std::string& name = std::get<LexerType::Keyword>(token).name;
    static const std::vector<std::string> reserved = {
        "var", "let", "const", "shared", "if", "else", "for", "func", "class", "interface", "impl",
        "return", "break", "continue", "delete", "throw", "try", "catch", "with",
        "import", "using", "original", "decl", "static", "new", "true", "false", "nil"
    };
    for (const auto& keyword : reserved) {
        if (name == keyword) return false;
    }
    return true;
}

inline bool isSyntaxPackageIdentifier(const LexerType::Token& token) {
    if (!std::holds_alternative<LexerType::Keyword>(token)) return false;
    const std::string& name = std::get<LexerType::Keyword>(token).name;
    return !name.empty() && name != "true" && name != "false" && name != "nil";
}

inline bool isSyntaxTypePath(const std::string& name) {
    if (name.empty()) return false;
    bool expectIdentifierStart = true;
    for (unsigned char ch : name) {
        if (ch == '.') {
            if (expectIdentifierStart) return false;
            expectIdentifierStart = true;
            continue;
        }
        if (expectIdentifierStart) {
            if (!(std::isalpha(ch) || ch == '_')) return false;
            expectIdentifierStart = false;
        } else if (!(std::isalnum(ch) || ch == '_')) {
            return false;
        }
    }
    return !expectIdentifierStart;
}

inline std::string canonicalSyntaxTypeName(const std::string& name) {
    if (const auto* builtin = ZeusBuiltins::findTypeImplementation(name)) {
        return std::string(builtin->canonicalName);
    }
    return name;
}

inline bool isSyntaxFunctionName(const LexerType::Token& token) {
    if (isSyntaxIdentifier(token)) return true;
    // `with` is also used as a method name by the bundled thread API.
    return std::holds_alternative<LexerType::Keyword>(token) &&
           std::get<LexerType::Keyword>(token).name == "with";
}

inline bool isSyntaxMemberKeyword(const std::vector<LexerType::Token>& tokens, size_t index) {
    return index > 0 && isSyntaxOperator(tokens[index - 1], ".");
}

inline bool isSyntaxFunctionKeyword(const std::vector<LexerType::Token>& tokens, size_t index) {
    if (index > 0 && std::holds_alternative<LexerType::Keyword>(tokens[index - 1]) &&
        std::get<LexerType::Keyword>(tokens[index - 1]).name == "func") {
        return true;
    }
    return index > 1 &&
           std::holds_alternative<LexerType::Keyword>(tokens[index - 1]) &&
           std::get<LexerType::Keyword>(tokens[index - 1]).name == "static" &&
           std::holds_alternative<LexerType::Keyword>(tokens[index - 2]) &&
           std::get<LexerType::Keyword>(tokens[index - 2]).name == "func";
}

inline int syntaxTokenLine(const LexerType::Token& token) {
    return std::visit([](const auto& value) { return value.line; }, token);
}

inline int syntaxTokenColumn(const LexerType::Token& token) {
    return std::visit([](const auto& value) { return value.column; }, token);
}

inline size_t matchingSyntaxDelimiter(
    const std::vector<LexerType::Token>& tokens,
    size_t openIndex
) {
    if (openIndex >= tokens.size() || !std::holds_alternative<LexerType::Operator>(tokens[openIndex])) {
        return tokens.size();
    }

    const std::string open = std::get<LexerType::Operator>(tokens[openIndex]).symbol;
    const std::string close = open == "(" ? ")" : open == "[" ? "]" : open == "{" ? "}" : "";
    if (close.empty()) return tokens.size();

    size_t depth = 0;
    for (size_t i = openIndex; i < tokens.size(); ++i) {
        if (isSyntaxOperator(tokens[i], open)) ++depth;
        else if (isSyntaxOperator(tokens[i], close) && --depth == 0) return i;
    }
    return tokens.size();
}

inline std::vector<ZeusDiagnostics::Diagnostic> validateSyntax(
    const std::vector<LexerType::Token>& tokens
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;

    auto addDiagnostic = [&](const std::string& code, const std::string& message,
                             const LexerType::Token& token) {
        ZeusDiagnostics::Diagnostic diagnostic;
        diagnostic.code = code;
        diagnostic.message = message;
        diagnostic.line = syntaxTokenLine(token);
        diagnostic.column = syntaxTokenColumn(token);
        diagnostics.push_back(std::move(diagnostic));
    };

    // Operator overload names contain operator tokens inside brackets. Those
    // tokens are part of a declaration, not expression operators.
    std::vector<bool> inOperatorName(tokens.size(), false);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (!std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "operator" ||
            !isSyntaxOperator(tokens[i + 1], "[")) {
            continue;
        }
        const size_t close = matchingSyntaxDelimiter(tokens, i + 1);
        const size_t end = close == tokens.size() ? i + 1 : close;
        for (size_t j = i; j <= end; ++j) inOperatorName[j] = true;
    }

    std::vector<bool> inInterfaceScope(tokens.size(), false);
    std::vector<bool> interfaceScopes;
    size_t declarationStart = 0;
    for (size_t i = 0; i < tokens.size(); ++i) {
        inInterfaceScope[i] = !interfaceScopes.empty() && interfaceScopes.back();
        if (isSyntaxOperator(tokens[i], "{")) {
            bool opensInterface = false;
            for (size_t j = declarationStart; j < i; ++j) {
                if (!inOperatorName[j] &&
                    std::holds_alternative<LexerType::Keyword>(tokens[j]) &&
                    std::get<LexerType::Keyword>(tokens[j]).name == "interface" &&
                    !isSyntaxMemberKeyword(tokens, j)) {
                    opensInterface = true;
                    break;
                }
            }
            interfaceScopes.push_back(opensInterface);
            declarationStart = i + 1;
        } else if (isSyntaxOperator(tokens[i], "}")) {
            if (!interfaceScopes.empty()) interfaceScopes.pop_back();
            declarationStart = i + 1;
        } else if (isSyntaxOperator(tokens[i], ";")) {
            declarationStart = i + 1;
        }
    }

    // Report mismatched and unclosed delimiters before the permissive tree
    // parser can discard or misclassify the remaining tokens.
    struct Delimiter {
        std::string open;
        size_t index;
    };
    std::vector<Delimiter> delimiters;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!std::holds_alternative<LexerType::Operator>(tokens[i])) continue;
        const std::string& symbol = std::get<LexerType::Operator>(tokens[i]).symbol;
        if (symbol == "(" || symbol == "[" || symbol == "{") {
            delimiters.push_back({ symbol, i });
            continue;
        }
        if (symbol != ")" && symbol != "]" && symbol != "}") continue;

        const std::string expectedOpen = symbol == ")" ? "(" : symbol == "]" ? "[" : "{";
        if (delimiters.empty() || delimiters.back().open != expectedOpen) {
            addDiagnostic("P0005", "unmatched closing delimiter '" + symbol + "'", tokens[i]);
            continue;
        }
        delimiters.pop_back();
    }
    for (const auto& delimiter : delimiters) {
        const std::string expectedClose = delimiter.open == "(" ? ")" : delimiter.open == "[" ? "]" : "}";
        addDiagnostic("P0005", "expected '" + expectedClose + "' to close '" + delimiter.open + "'",
                      tokens[delimiter.index]);
    }

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            isSyntaxMemberKeyword(tokens, i)) {
            continue;
        }
        const std::string& declaration = std::get<LexerType::Keyword>(tokens[i]).name;
        if (declaration == "interface") {
            const size_t name = i + 1;
            if (name >= tokens.size() || !isSyntaxIdentifier(tokens[name])) {
                addDiagnostic("P0040", "expected an interface name", tokens[i]);
            } else if (name + 1 >= tokens.size() || !isSyntaxOperator(tokens[name + 1], "{")) {
                addDiagnostic("P0040", "expected '{' after interface name", tokens[name]);
            }
            continue;
        }
        if (declaration != "class") continue;

        size_t name = i + 1;
        if (name < tokens.size() && std::holds_alternative<LexerType::Keyword>(tokens[name]) &&
            std::get<LexerType::Keyword>(tokens[name]).name == "shared") {
            ++name;
        }
        if (name >= tokens.size() || !isSyntaxIdentifier(tokens[name])) continue;

        size_t position = name + 1;
        if (position >= tokens.size() || !isSyntaxOperator(tokens[position], "[")) continue;
        ++position;
        bool expectPath = true;
        bool sawPath = false;
        while (position < tokens.size() && !isSyntaxOperator(tokens[position], "]")) {
            if (expectPath) {
                if (!isSyntaxIdentifier(tokens[position])) {
                    addDiagnostic("P0041", "expected an interface name in class interface list",
                                  tokens[position]);
                    break;
                }
                sawPath = true;
                ++position;
                expectPath = false;
                continue;
            }
            if (isSyntaxOperator(tokens[position], ".")) {
                ++position;
                if (position >= tokens.size() || !isSyntaxIdentifier(tokens[position])) {
                    addDiagnostic("P0041", "expected a name after '.' in class interface list",
                                  tokens[position - 1]);
                    break;
                }
                ++position;
                continue;
            }
            if (isSyntaxOperator(tokens[position], ",")) {
                ++position;
                expectPath = true;
                continue;
            }
            addDiagnostic("P0041", "expected ',' or ']' in class interface list",
                          tokens[position]);
            break;
        }
        if (!sawPath || expectPath) {
            addDiagnostic("P0041", "class interface list cannot be empty or end with ','",
                          tokens[i]);
        }
        if (position >= tokens.size() || !isSyntaxOperator(tokens[position], "]")) {
            addDiagnostic("P0041", "expected ']' after class interface list", tokens[i]);
        } else if (position + 1 >= tokens.size() || !isSyntaxOperator(tokens[position + 1], "{")) {
            addDiagnostic("P0041", "expected '{' after class interface list", tokens[position]);
        }
    }

    // Angle brackets in a declared generic type are type delimiters, not
    // comparison operators. Record them once so expression validation below
    // can keep its ordinary comparison checks.
    std::vector<bool> inGenericType(tokens.size(), false);
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!isSyntaxOperator(tokens[i], ":")) continue;
        size_t typeEnd = i + 1;
        std::string typeName;
        if (!parseGenericType(tokens, typeEnd, typeName)) continue;
        std::string genericBase;
        std::vector<std::string> genericArguments;
        if (!ZeusTypeInfer::decodeGenericSpecializationName(
                typeName, genericBase, genericArguments)) {
            continue;
        }
        for (size_t typeToken = i + 1; typeToken < typeEnd; ++typeToken) {
            if (isSyntaxOperator(tokens[typeToken], "<") ||
                isSyntaxOperator(tokens[typeToken], ">")) {
                inGenericType[typeToken] = true;
            }
        }
    }

    auto isExpressionBoundary = [&](size_t index) {
        if (index >= tokens.size()) return true;
        if (!std::holds_alternative<LexerType::Operator>(tokens[index])) return false;
        const std::string& symbol = std::get<LexerType::Operator>(tokens[index]).symbol;
        return symbol == ";" || symbol == ")" || symbol == "]" ||
               symbol == "}" || symbol == ",";
    };

    // An assignment/default argument with no expression on its right side is
    // a syntax error. Do not treat '=' inside operator[[]=] as an assignment.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || !isSyntaxOperator(tokens[i], "=")) continue;
        if (i + 1 >= tokens.size() || isExpressionBoundary(i + 1)) {
            addDiagnostic("P0002", "expected an expression after '='", tokens[i]);
        }
    }

    // Catch incomplete binary/postfix expressions such as `1 +;` and `item.`.
    static const std::vector<std::string> operatorsRequiringRightOperand = {
        "+", "-", "*", "/", "%", "==", "!=", ">", "<", ">=", "<=",
        "and", "or", "."
    };
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || inGenericType[i] ||
            !std::holds_alternative<LexerType::Operator>(tokens[i])) continue;
        const std::string& symbol = std::get<LexerType::Operator>(tokens[i]).symbol;
        bool needsRightOperand = false;
        for (const auto& op : operatorsRequiringRightOperand) {
            if (symbol == op) {
                needsRightOperand = true;
                break;
            }
        }
        if (needsRightOperand && (i + 1 >= tokens.size() || isExpressionBoundary(i + 1))) {
            addDiagnostic("P0006", "expected an expression after '" + symbol + "'", tokens[i]);
        }
    }

    // `new` always takes a class expression; a call with no class name is not
    // a constructor expression.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "new") {
            continue;
        }
        if (i + 1 >= tokens.size() || !std::holds_alternative<LexerType::Keyword>(tokens[i + 1])) {
            addDiagnostic("P0003", "expected a class name after 'new'", tokens[i]);
        }
    }

    // Function values/anonymous functions are not part of the language. A
    // `func` declaration must name a function or introduce an operator overload.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "func") {
            continue;
        }

        size_t nameIndex = i + 1;
        if (nameIndex < tokens.size() && std::holds_alternative<LexerType::Keyword>(tokens[nameIndex]) &&
            std::get<LexerType::Keyword>(tokens[nameIndex]).name == "static") {
            ++nameIndex;
        }

        if (nameIndex >= tokens.size() || !std::holds_alternative<LexerType::Keyword>(tokens[nameIndex])) {
            addDiagnostic("P0004", "function declarations require a name; anonymous functions are not supported",
                          tokens[i]);
            continue;
        }
        const bool operatorFunction =
            std::get<LexerType::Keyword>(tokens[nameIndex]).name == "operator";
        if (!operatorFunction && !isSyntaxFunctionName(tokens[nameIndex])) {
            addDiagnostic("P0004", "function name must be an identifier", tokens[nameIndex]);
            continue;
        }

        size_t paramsOpen = nameIndex + 1;
        if (operatorFunction) {
            if (paramsOpen >= tokens.size() || !isSyntaxOperator(tokens[paramsOpen], "[")) {
                addDiagnostic("P0007", "expected '[' after 'operator' in function declaration", tokens[nameIndex]);
                continue;
            }
            const size_t operatorClose = matchingSyntaxDelimiter(tokens, paramsOpen);
            if (operatorClose == tokens.size()) continue; // unmatched delimiter is already reported
            if (operatorClose == paramsOpen + 1) {
                addDiagnostic("P0007", "operator overload requires at least one operator symbol",
                              tokens[nameIndex]);
            }
            paramsOpen = operatorClose + 1;
        }

        if (paramsOpen < tokens.size() && std::holds_alternative<LexerType::Keyword>(tokens[paramsOpen]) &&
            std::get<LexerType::Keyword>(tokens[paramsOpen]).name == "static") {
            ++paramsOpen;
        }
        if (paramsOpen >= tokens.size() || !isSyntaxOperator(tokens[paramsOpen], "(")) {
            addDiagnostic("P0008", "expected a parameter list after the function name", tokens[i]);
            continue;
        }

        const size_t paramsClose = matchingSyntaxDelimiter(tokens, paramsOpen);
        if (paramsClose == tokens.size()) continue; // unmatched delimiter is already reported

        size_t segmentStart = paramsOpen + 1;
        std::vector<std::pair<size_t, size_t>> parameterSegments;
        bool hasParameter = false;
        bool previousWasComma = false;
        bool emptyParameterReported = false;
        int parenDepth = 0;
        int bracketDepth = 0;
        int braceDepth = 0;
        int genericTypeDepth = 0;
        bool readingType = false;
        for (size_t j = paramsOpen + 1; j < paramsClose; ++j) {
            const bool atTopLevelComma = isSyntaxOperator(tokens[j], ",") &&
                parenDepth == 0 && bracketDepth == 0 && braceDepth == 0 &&
                genericTypeDepth == 0;
            if (atTopLevelComma) {
                if (segmentStart == j) {
                    if (!emptyParameterReported) {
                        addDiagnostic("P0018", "expected a parameter between commas",
                                      tokens[j]);
                        emptyParameterReported = true;
                    }
                } else {
                    hasParameter = true;
                    parameterSegments.push_back({ segmentStart, j });
                }
                segmentStart = j + 1;
                previousWasComma = true;
                readingType = false;
                genericTypeDepth = 0;
                continue;
            }

            previousWasComma = false;
            if (!std::holds_alternative<LexerType::Operator>(tokens[j])) continue;
            const std::string& symbol = std::get<LexerType::Operator>(tokens[j]).symbol;
            if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
                if (symbol == ":") readingType = true;
                else if (symbol == "=") readingType = false;
                else if (readingType && symbol == "<") ++genericTypeDepth;
                else if (readingType && symbol == ">" && genericTypeDepth > 0) {
                    --genericTypeDepth;
                }
            }
            if (symbol == "(") ++parenDepth;
            else if (symbol == ")" && parenDepth > 0) --parenDepth;
            else if (symbol == "[") ++bracketDepth;
            else if (symbol == "]" && bracketDepth > 0) --bracketDepth;
            else if (symbol == "{") ++braceDepth;
            else if (symbol == "}" && braceDepth > 0) --braceDepth;
        }
        if (paramsClose > paramsOpen + 1 && segmentStart == paramsClose &&
            previousWasComma && !hasParameter && !emptyParameterReported) {
            addDiagnostic("P0018", "expected a parameter between commas", tokens[paramsClose]);
        }
        if (segmentStart < paramsClose) {
            parameterSegments.push_back({ segmentStart, paramsClose });
        }

        const bool hasDeclPrefix = i > 0 &&
            std::holds_alternative<LexerType::Keyword>(tokens[i - 1]) &&
            std::get<LexerType::Keyword>(tokens[i - 1]).name == "decl";
        const bool interfaceMethodDeclaration = inInterfaceScope[i] && hasDeclPrefix;
        const bool forwardDeclaration = hasDeclPrefix;
        bool nativeForwardDeclaration = false;
        if (forwardDeclaration) {
            for (size_t prefixEnd = i; prefixEnd > 0; --prefixEnd) {
                const size_t prefixStart = prefixEnd - 1;
                if (!isSyntaxOperator(tokens[prefixStart], "@")) continue;
                const std::vector<LexerType::Token> prefixTokens(
                    tokens.begin() + prefixStart, tokens.begin() + i);
                const auto prefix = parseFunctionPrefix(prefixTokens);
                const size_t declarationIndex = prefixStart + prefix.nextToken;
                const bool namesThisDeclaration = prefix.valid &&
                    declarationIndex + 1 == i &&
                    std::holds_alternative<LexerType::Keyword>(tokens[declarationIndex]) &&
                    std::get<LexerType::Keyword>(tokens[declarationIndex]).name == "decl";
                nativeForwardDeclaration = namesThisDeclaration &&
                    std::any_of(prefix.decorators.begin(), prefix.decorators.end(),
                        [](const FunctionDecoratorSyntax& decorator) {
                            return decorator.name == "native";
                        });
                break;
            }
        }
        for (const auto& segment : parameterSegments) {
            size_t position = segment.first;
            const size_t end = segment.second;
            const bool variadic = isSyntaxOperator(tokens[position], "*");
            if (variadic) {
                ++position;
                if (position < end && isSyntaxOperator(tokens[position], "*")) ++position;
            }
            const bool byReference = position < end &&
                isSyntaxOperator(tokens[position], "&");
            if (byReference) ++position;
            if (byReference && forwardDeclaration && !interfaceMethodDeclaration &&
                !nativeForwardDeclaration) {
                addDiagnostic("P0019",
                              "non-native 'decl func' cannot declare reference parameters",
                              tokens[segment.first]);
            }
            if (position >= end || !isSyntaxIdentifier(tokens[position])) {
                addDiagnostic("P0019", "expected a parameter name", tokens[segment.first]);
                continue;
            }
            ++position;

            if (position < end && isSyntaxOperator(tokens[position], ":")) {
                const size_t typeStart = ++position;
                while (position < end && !isSyntaxOperator(tokens[position], "=")) ++position;
                if (position == typeStart) {
                    addDiagnostic("P0019", "expected a type after ':' in parameter declaration",
                                  tokens[segment.first]);
                    continue;
                }
            }

            if (position < end && isSyntaxOperator(tokens[position], "=")) {
                if (byReference) {
                    addDiagnostic("P0019", "reference parameters cannot have default values",
                                  tokens[position]);
                }
                ++position;
                if (position == end) {
                    addDiagnostic("P0019", "expected a default expression after '=' in parameter declaration",
                                  tokens[segment.first]);
                    continue;
                }
                position = end;
            }

            if (byReference && variadic) {
                addDiagnostic("P0019", "reference parameters cannot be variadic",
                              tokens[segment.first]);
            } else if (variadic && position != end) {
                addDiagnostic("P0019", "variadic parameters cannot have a type or default value",
                              tokens[segment.first]);
            } else if (position != end) {
                addDiagnostic("P0019", "unexpected token in parameter declaration",
                              tokens[position]);
            }
        }

        size_t bodyIndex = paramsClose + 1;
        if (bodyIndex < tokens.size() && isSyntaxOperator(tokens[bodyIndex], "[")) {
            const size_t captureOpen = bodyIndex;
            const size_t captureClose = matchingSyntaxDelimiter(tokens, captureOpen);
            if (captureClose != tokens.size()) {
                const bool captureAllByRef = captureClose == captureOpen + 2 &&
                    isSyntaxOperator(tokens[captureOpen + 1], "&");
                if (!captureAllByRef && captureClose > captureOpen + 1) {
                    size_t captureSegmentStart = captureOpen + 1;
                    bool hasCapture = false;
                    bool previousWasComma = false;
                    for (size_t j = captureOpen + 1; j <= captureClose; ++j) {
                        const bool atEnd = j == captureClose;
                        const bool atComma = !atEnd && isSyntaxOperator(tokens[j], ",");
                        if (!atEnd && !atComma) {
                            previousWasComma = false;
                            continue;
                        }
                        if (captureSegmentStart == j) {
                            if (!(atEnd && previousWasComma && hasCapture)) {
                                addDiagnostic("P0021", "expected a capture name between commas",
                                              atEnd ? tokens[captureClose] : tokens[j]);
                            }
                        } else {
                            size_t item = captureSegmentStart;
                            if (isSyntaxOperator(tokens[item], "&")) ++item;
                            if (item >= j || !isSyntaxIdentifier(tokens[item]) || item + 1 != j) {
                                addDiagnostic("P0021", "capture entries must be identifiers optionally prefixed by '&'",
                                              tokens[captureSegmentStart]);
                            } else {
                                hasCapture = true;
                            }
                        }
                        if (atEnd) break;
                        captureSegmentStart = j + 1;
                        previousWasComma = true;
                    }
                }
                bodyIndex = captureClose + 1;
            }
        }
        if (bodyIndex < tokens.size() && isSyntaxOperator(tokens[bodyIndex], "->")) {
            const size_t arrowIndex = bodyIndex++;
            const bool referenceType = bodyIndex < tokens.size() && isSyntaxOperator(tokens[bodyIndex], "&");
            if (referenceType) ++bodyIndex;
            if (bodyIndex >= tokens.size() || !isSyntaxIdentifier(tokens[bodyIndex])) {
                addDiagnostic("P0022", "expected a return type after '->'", tokens[arrowIndex]);
            } else {
                ++bodyIndex;
            }
            while (bodyIndex < tokens.size() && !isSyntaxOperator(tokens[bodyIndex], "{") &&
                   !isSyntaxOperator(tokens[bodyIndex], ";")) {
                ++bodyIndex;
            }
        }
        if (forwardDeclaration) {
            if (bodyIndex >= tokens.size() || !isSyntaxOperator(tokens[bodyIndex], ";")) {
                addDiagnostic("P0023", "expected ';' after function forward declaration", tokens[i]);
            }
        } else {
            if (bodyIndex >= tokens.size() || !isSyntaxOperator(tokens[bodyIndex], "{")) {
                addDiagnostic("P0009", "expected a function body after the parameter list", tokens[i]);
            }
        }
    }

    // Required control-flow headers cannot be empty or omit their parentheses.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || !std::holds_alternative<LexerType::Keyword>(tokens[i])) continue;
        const std::string& keyword = std::get<LexerType::Keyword>(tokens[i]).name;
        if (keyword != "if" && keyword != "for" && keyword != "with") continue;
        if (isSyntaxMemberKeyword(tokens, i)) continue;
        if (keyword == "with" && isSyntaxFunctionKeyword(tokens, i)) continue;
        if (i + 1 >= tokens.size() || !isSyntaxOperator(tokens[i + 1], "(")) {
            addDiagnostic("P0010", "expected '(' after '" + keyword + "'", tokens[i]);
        } else if (i + 2 < tokens.size() && isSyntaxOperator(tokens[i + 2], ")")) {
            addDiagnostic("P0011", "expected an expression inside '" + keyword + "' header", tokens[i + 1]);
        } else if (keyword == "if") {
            const size_t headerClose = matchingSyntaxDelimiter(tokens, i + 1);
            if (headerClose != tokens.size()) {
                const size_t body = headerClose + 1;
                const bool missingBody = body >= tokens.size() ||
                    isSyntaxOperator(tokens[body], ";") ||
                    isSyntaxOperator(tokens[body], "}") ||
                    (std::holds_alternative<LexerType::Keyword>(tokens[body]) &&
                     std::get<LexerType::Keyword>(tokens[body]).name == "else");
                if (missingBody) {
                    addDiagnostic("P0032", "expected a statement or block after the 'if' header",
                                  tokens[i]);
                }
            }
        }
    }

    // `else` also requires a real branch. Empty statements are not part of
    // the grammar and must not make an incomplete conditional look valid.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || isSyntaxMemberKeyword(tokens, i) ||
            !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "else") {
            continue;
        }
        if (i + 1 >= tokens.size() || isSyntaxOperator(tokens[i + 1], ";") ||
            isSyntaxOperator(tokens[i + 1], "}") ||
            (std::holds_alternative<LexerType::Keyword>(tokens[i + 1]) &&
             std::get<LexerType::Keyword>(tokens[i + 1]).name == "else")) {
            addDiagnostic("P0032", "expected a statement or block after 'else'", tokens[i]);
        }
    }

    // Validate the complete catch header before TreeParser extracts only the
    // first keyword it finds. Otherwise punctuation and extra names in
    // `catch (...)` are silently discarded.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || isSyntaxMemberKeyword(tokens, i) ||
            !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "catch") {
            continue;
        }

        // A catch clause is valid only when it immediately follows a try
        // block. TreeParser otherwise drops a stray catch from the AST before
        // the parsed-tree validation can attach a useful source location.
        bool followsTryBlock = false;
        if (i > 0 && isSyntaxOperator(tokens[i - 1], "}")) {
            size_t bodyOpen = tokens.size();
            int braceDepth = 0;
            for (size_t j = i; j-- > 0;) {
                if (isSyntaxOperator(tokens[j], "}")) {
                    ++braceDepth;
                } else if (isSyntaxOperator(tokens[j], "{") && --braceDepth == 0) {
                    bodyOpen = j;
                    break;
                }
            }
            followsTryBlock = bodyOpen > 0 && bodyOpen < tokens.size() &&
                std::holds_alternative<LexerType::Keyword>(tokens[bodyOpen - 1]) &&
                std::get<LexerType::Keyword>(tokens[bodyOpen - 1]).name == "try";
        }
        if (!followsTryBlock) {
            addDiagnostic("P0031", "'catch' block must immediately follow a 'try' block", tokens[i]);
        }

        if (i + 1 >= tokens.size() || !isSyntaxOperator(tokens[i + 1], "(")) {
            addDiagnostic("P0034", "expected '(' after 'catch'", tokens[i]);
            continue;
        }
        const size_t close = matchingSyntaxDelimiter(tokens, i + 1);
        if (close == tokens.size()) continue; // delimiter diagnostics are reported above

        size_t position = i + 2;
        const bool byReference = position < close && isSyntaxOperator(tokens[position], "&");
        if (byReference) ++position;
        bool hasCatchName = false;
        if (position < close && isSyntaxIdentifier(tokens[position])) {
            hasCatchName = true;
            ++position;
        }
        const bool emptyCatch = !byReference && close == i + 2;
        if ((!emptyCatch && !hasCatchName) || position != close) {
            addDiagnostic("P0034", "catch accepts only an optional '&' and one variable name",
                          position < tokens.size() ? tokens[position] : tokens[close]);
            continue;
        }
        if (close + 1 >= tokens.size() || !isSyntaxOperator(tokens[close + 1], "{")) {
            addDiagnostic("P0034", "expected a block after the catch header", tokens[close]);
        }
    }

    // `with` and `for` have optional binding zones after their parenthesized
    // expression. Validate those zones here because TreeParser currently
    // keeps only recognized names and ignores other tokens in the brackets.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || isSyntaxMemberKeyword(tokens, i) ||
            !std::holds_alternative<LexerType::Keyword>(tokens[i])) {
            continue;
        }
        const std::string& keyword = std::get<LexerType::Keyword>(tokens[i]).name;
        if (keyword != "with" && keyword != "for") continue;
        if (keyword == "with" && isSyntaxFunctionKeyword(tokens, i)) {
            continue;
        }
        if (i + 1 >= tokens.size() || !isSyntaxOperator(tokens[i + 1], "(")) continue;
        const size_t headerClose = matchingSyntaxDelimiter(tokens, i + 1);
        if (headerClose == tokens.size()) continue; // delimiter diagnostics are reported above

        size_t bodyIndex = headerClose + 1;
        if (bodyIndex < tokens.size() && isSyntaxOperator(tokens[bodyIndex], "[")) {
            const size_t bindingClose = matchingSyntaxDelimiter(tokens, bodyIndex);
            if (bindingClose == tokens.size()) continue; // delimiter diagnostics are reported above

            if (keyword == "with") {
                size_t binding = bodyIndex + 1;
                if (binding < bindingClose && isSyntaxOperator(tokens[binding], "&")) ++binding;
                if (binding >= bindingClose || !isSyntaxIdentifier(tokens[binding]) ||
                    binding + 1 != bindingClose) {
                    addDiagnostic("P0035", "with capture must be one name optionally prefixed by '&'",
                                  tokens[bodyIndex]);
                }
            } else {
                auto isBinding = [&](size_t begin, size_t end) {
                    if (begin < end && std::holds_alternative<LexerType::Keyword>(tokens[begin]) &&
                        std::get<LexerType::Keyword>(tokens[begin]).name == "const") {
                        ++begin;
                    }
                    if (begin < end && isSyntaxOperator(tokens[begin], "&")) ++begin;
                    return begin < end && isSyntaxIdentifier(tokens[begin]) && begin + 1 == end;
                };

                size_t colon = bindingClose;
                bool multipleColons = false;
                for (size_t j = bodyIndex + 1; j < bindingClose; ++j) {
                    if (!isSyntaxOperator(tokens[j], ":")) continue;
                    if (colon != bindingClose) multipleColons = true;
                    colon = j;
                }

                bool validBinding = false;
                if (!multipleColons && colon == bindingClose) {
                    validBinding = isBinding(bodyIndex + 1, bindingClose);
                } else if (!multipleColons) {
                    size_t valueBegin = colon + 1;
                    // zeus.io uses `...` as an explicit separator in
                    // its kwargs key/value binding; TreeParser treats it as a
                    // separator and preserves the two bindings.
                    if (valueBegin + 2 < bindingClose &&
                        isSyntaxOperator(tokens[valueBegin], ".") &&
                        isSyntaxOperator(tokens[valueBegin + 1], ".") &&
                        isSyntaxOperator(tokens[valueBegin + 2], ".")) {
                        valueBegin += 3;
                    }
                    validBinding = isBinding(bodyIndex + 1, colon) &&
                                   isBinding(valueBegin, bindingClose);
                }
                if (!validBinding) {
                    addDiagnostic("P0036", "for binding must be one name or a key/value pair",
                                  tokens[bodyIndex]);
                }
            }
            bodyIndex = bindingClose + 1;
        }

        if (bodyIndex >= tokens.size() || !isSyntaxOperator(tokens[bodyIndex], "{")) {
            addDiagnostic(keyword == "with" ? "P0035" : "P0036",
                          "expected a block after the '" + keyword + "' header",
                          tokens[headerClose]);
        }
    }

    // @static and @no_safepoint decorate function definitions;
    // @native("symbol", "raw|slot") decorates a `decl func`.
    // Validate the whole prefix so malformed/unknown annotations cannot be
    // silently dropped by TreeParser::leadingDecorators().
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!isSyntaxOperator(tokens[i], "@") || inOperatorName[i]) continue;

        if (i + 2 < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[i + 1]) &&
            std::get<LexerType::Keyword>(tokens[i + 1]).name == "this") {
            if (!isSyntaxOperator(tokens[i + 2], "=")) {
                addDiagnostic("P0039", "@this is only valid as an assignment target",
                              tokens[i]);
            }
            ++i;
            continue;
        }

        if (i + 2 < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[i + 1]) &&
            (std::get<LexerType::Keyword>(tokens[i + 1]).name == "addr" ||
             std::get<LexerType::Keyword>(tokens[i + 1]).name == "reveal" ||
             std::get<LexerType::Keyword>(tokens[i + 1]).name == "faddr") &&
            isSyntaxOperator(tokens[i + 2], "(")) {
            const std::string intrinsicName =
                std::get<LexerType::Keyword>(tokens[i + 1]).name;
            const bool isFetchAddress = intrinsicName == "faddr";
            const size_t close = matchingSyntaxDelimiter(tokens, i + 2);
            if (close >= tokens.size()) {
                addDiagnostic("P0039", "expected ')' after @" + intrinsicName +
                              " argument", tokens[i]);
                continue;
            }
            int depth = 0;
            size_t argumentCount = 0;
            bool hasArgument = false;
            for (size_t j = i + 3; j < close; ++j) {
                if (isSyntaxOperator(tokens[j], "(") || isSyntaxOperator(tokens[j], "[") ||
                    isSyntaxOperator(tokens[j], "{")) {
                    ++depth;
                } else if (isSyntaxOperator(tokens[j], ")") || isSyntaxOperator(tokens[j], "]") ||
                           isSyntaxOperator(tokens[j], "}")) {
                    --depth;
                } else if (depth == 0 && isSyntaxOperator(tokens[j], ",")) {
                    ++argumentCount;
                } else {
                    hasArgument = true;
                }
            }
            if (!hasArgument ||
                (isFetchAddress ? argumentCount > 1 : argumentCount != 0)) {
                addDiagnostic("P0039", isFetchAddress
                    ? "@faddr requires an address and at most one size argument"
                    : "@" + intrinsicName + " requires exactly one value argument",
                    tokens[i]);
            }
            if (isFetchAddress) {
                const size_t arrow = close + 1;
                if (arrow >= tokens.size() || !isSyntaxOperator(tokens[arrow], "->")) {
                    addDiagnostic("P0039",
                                  "@faddr requires '-> <type>' after its address argument",
                                  tokens[i]);
                } else {
                    const bool hasNamedType = arrow + 1 < tokens.size() &&
                        std::holds_alternative<LexerType::Keyword>(tokens[arrow + 1]);
                    const bool hasRawBytesRangeType = arrow + 2 < tokens.size() &&
                        isSyntaxOperator(tokens[arrow + 1], "&") &&
                        std::holds_alternative<LexerType::Keyword>(tokens[arrow + 2]) &&
                        std::get<LexerType::Keyword>(tokens[arrow + 2]).name == "bytes";
                    if (!hasNamedType && !hasRawBytesRangeType) {
                        addDiagnostic("P0039", "@faddr requires a Zeus type after '->'",
                                      tokens[i]);
                    } else {
                        i = arrow + (hasRawBytesRangeType ? 2 : 1);
                        continue;
                    }
                }
            }
            i = close;
            continue;
        }

        const std::vector<LexerType::Token> suffix(tokens.begin() + i, tokens.end());
        const auto prefix = parseFunctionPrefix(suffix);
        if (!prefix.valid || prefix.decorators.empty()) {
            addDiagnostic("P0038", "malformed function decorator prefix", tokens[i]);
            continue;
        }

        size_t declarationIndex = i + prefix.nextToken;
        bool isForwardDeclaration = false;
        if (declarationIndex < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[declarationIndex]) &&
            std::get<LexerType::Keyword>(tokens[declarationIndex]).name == "decl") {
            isForwardDeclaration = true;
            ++declarationIndex;
        }
        size_t classKeywordIndex = declarationIndex;
        if (classKeywordIndex < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[classKeywordIndex]) &&
            std::get<LexerType::Keyword>(tokens[classKeywordIndex]).name == "shared") {
            ++classKeywordIndex;
        }
        const bool isClassDeclaration = classKeywordIndex < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[classKeywordIndex]) &&
            std::get<LexerType::Keyword>(tokens[classKeywordIndex]).name == "class";
        const bool isFunctionDeclaration = declarationIndex < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[declarationIndex]) &&
            std::get<LexerType::Keyword>(tokens[declarationIndex]).name == "func";
        if (isClassDeclaration) {
            for (const auto& decorator : prefix.decorators) {
                if (decorator.name != "type") {
                    addDiagnostic("P0038", "only @type may decorate a class definition",
                                  tokens[i]);
                    continue;
                }
                const std::vector<std::string>& parameters = decorator.typeArguments;
                if (parameters.empty()) {
                    addDiagnostic("P0038", "@type on a class requires one or more type-parameter names",
                                  tokens[i]);
                    continue;
                }
                std::unordered_set<std::string> uniqueParameters;
                for (const auto& parameter : parameters) {
                    if (parameter.empty() || !uniqueParameters.insert(parameter).second) {
                        addDiagnostic("P0038", "@type class parameters must be distinct identifiers",
                                      tokens[i]);
                    }
                }
            }
            i = classKeywordIndex;
            continue;
        }
        if (!isFunctionDeclaration) {
            addDiagnostic("P0038", "function decorators must immediately precede 'func' or 'decl func'",
                          tokens[i]);
            continue;
        }

        bool sawNative = false;
        bool sawStatic = false;
        bool sawNoSafepoint = false;
        bool sawTypeParameter = false;
        for (const auto& decorator : prefix.decorators) {
            if (decorator.name == "native") {
                if (sawNative || !decorator.hasArgument || decorator.argument.empty() ||
                    (decorator.hasSecondaryArgument &&
                     decorator.secondaryArgument != "raw" &&
                     decorator.secondaryArgument != "slot")) {
                    addDiagnostic("P0038",
                                  "@native requires a symbol and optional reference ABI 'raw' or 'slot'",
                                  tokens[i]);
                }
                if (!isForwardDeclaration) {
                    addDiagnostic("P0038", "@native must decorate a 'decl func' declaration",
                                  tokens[i]);
                }
                sawNative = true;
            } else if (decorator.name == "static") {
                if (sawStatic || decorator.hasArgument) {
                    addDiagnostic("P0038", "@static does not accept an argument and may appear only once",
                                  tokens[i]);
                }
                if (isForwardDeclaration) {
                    addDiagnostic("P0038", "@static is only supported on function definitions",
                                  tokens[i]);
                }
                sawStatic = true;
            } else if (decorator.name == "no_safepoint") {
                if (sawNoSafepoint || decorator.hasArgument) {
                    addDiagnostic("P0038", "@no_safepoint takes no arguments and may appear only once",
                                  tokens[i]);
                }
                if (isForwardDeclaration) {
                    addDiagnostic("P0038", "@no_safepoint is only supported on function definitions",
                                  tokens[i]);
                }
                sawNoSafepoint = true;
            } else if (decorator.name == "type") {
                if (sawTypeParameter || !decorator.hasArgument ||
                    !decorator.argumentIsIdentifier || decorator.typeArguments.empty()) {
                    addDiagnostic("P0038",
                                  "@type requires one or more type-parameter names, such as @type(T, T2)",
                                  tokens[i]);
                } else {
                    std::unordered_set<std::string> uniqueParameters;
                    for (const auto& parameter : decorator.typeArguments) {
                        if (parameter.empty() ||
                            !uniqueParameters.insert(parameter).second) {
                            addDiagnostic("P0038",
                                          "@type parameters must be distinct identifiers",
                                          tokens[i]);
                        }
                    }
                }
                if (isForwardDeclaration) {
                    addDiagnostic("P0038", "@type is only supported on function definitions",
                                  tokens[i]);
                }
                sawTypeParameter = true;
            } else {
                addDiagnostic("P0038", "unsupported function decorator '@" + decorator.name + "'",
                              tokens[i]);
            }
        }
        i = declarationIndex;
    }

    // `inline` is a function-definition modifier, not an @decorator. It only
    // changes LLVM code generation; external `decl func` bodies cannot inline.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] ||
            !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "inline" ||
            (i > 0 && isSyntaxOperator(tokens[i - 1], "@"))) {
            continue;
        }

        const std::vector<LexerType::Token> suffix(tokens.begin() + i, tokens.end());
        const auto prefix = parseFunctionPrefix(suffix);
        if (!prefix.valid) {
            const bool followsInlinePrefix = i + 1 < tokens.size() &&
                ((std::holds_alternative<LexerType::Keyword>(tokens[i + 1]) &&
                  (std::get<LexerType::Keyword>(tokens[i + 1]).name == "inline" ||
                   std::get<LexerType::Keyword>(tokens[i + 1]).name == "func")) ||
                 isSyntaxOperator(tokens[i + 1], "@"));
            if (followsInlinePrefix) {
                addDiagnostic("P0038", "malformed inline function modifier", tokens[i]);
            }
            continue;
        }
        if (!prefix.isInline) continue;

        size_t declarationIndex = i + prefix.nextToken;
        bool isForwardDeclaration = false;
        if (declarationIndex < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[declarationIndex]) &&
            std::get<LexerType::Keyword>(tokens[declarationIndex]).name == "decl") {
            isForwardDeclaration = true;
            ++declarationIndex;
        }
        const bool namesFunction = declarationIndex < tokens.size() &&
            std::holds_alternative<LexerType::Keyword>(tokens[declarationIndex]) &&
            std::get<LexerType::Keyword>(tokens[declarationIndex]).name == "func";
        if (isForwardDeclaration || (namesFunction && i > 0 &&
            std::holds_alternative<LexerType::Keyword>(tokens[i - 1]) &&
            std::get<LexerType::Keyword>(tokens[i - 1]).name == "decl")) {
            addDiagnostic("P0038", "inline only applies to function definitions with a body", tokens[i]);
        }
    }

    // `while` is not a Zeus control-flow keyword. Keep it usable as a dynamic
    // name, but reject the familiar loop form instead of silently dropping it.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "while" ||
            isSyntaxMemberKeyword(tokens, i) ||
            i + 1 >= tokens.size() || !isSyntaxOperator(tokens[i + 1], "(")) {
            continue;
        }

        const bool functionName =
            (i > 0 && std::holds_alternative<LexerType::Keyword>(tokens[i - 1]) &&
             std::get<LexerType::Keyword>(tokens[i - 1]).name == "func") ||
            (i > 1 && std::holds_alternative<LexerType::Keyword>(tokens[i - 1]) &&
             std::get<LexerType::Keyword>(tokens[i - 1]).name == "static" &&
             std::holds_alternative<LexerType::Keyword>(tokens[i - 2]) &&
             std::get<LexerType::Keyword>(tokens[i - 2]).name == "func");
        if (functionName) continue;

        const size_t closeParen = matchingSyntaxDelimiter(tokens, i + 1);
        if (closeParen + 1 < tokens.size() && isSyntaxOperator(tokens[closeParen + 1], "{")) {
            addDiagnostic("P0001", "'while' loops are not supported by the current parser", tokens[i]);
        }
    }

    // Validate the supported forms of `using` statements/blocks and
    // class receiver aliases. It is otherwise a reserved statement keyword.
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (inOperatorName[i] || !std::holds_alternative<LexerType::Keyword>(tokens[i]) ||
            std::get<LexerType::Keyword>(tokens[i]).name != "using" ||
            isSyntaxMemberKeyword(tokens, i)) {
            continue;
        }
        if (i + 1 < tokens.size() && std::holds_alternative<LexerType::Keyword>(tokens[i + 1]) &&
            std::get<LexerType::Keyword>(tokens[i + 1]).name == "original") {
            const size_t after = i + 2;
            if (after >= tokens.size() ||
                (!isSyntaxOperator(tokens[after], ";") && !isSyntaxOperator(tokens[after], "{"))) {
                addDiagnostic("P0030", "expected ';' or '{' after 'using original'", tokens[i + 1]);
            }
        } else if (i + 2 < tokens.size() &&
                   std::holds_alternative<LexerType::Keyword>(tokens[i + 1]) &&
                   (std::get<LexerType::Keyword>(tokens[i + 1]).name == "LLVM" ||
                    std::get<LexerType::Keyword>(tokens[i + 1]).name == "llvm" ||
                    std::get<LexerType::Keyword>(tokens[i + 1]).name == "VM" ||
                    std::get<LexerType::Keyword>(tokens[i + 1]).name == "vm") &&
                   isSyntaxOperator(tokens[i + 2], "{")) {
            // `using LLVM/VM { ... }` selects a compile-target implementation block.
        } else if (i + 2 >= tokens.size() || !isSyntaxOperator(tokens[i + 1], "&") ||
                   !isSyntaxIdentifier(tokens[i + 2])) {
            addDiagnostic("P0030", "expected 'original' or '&name' after 'using'", tokens[i]);
        } else if (i + 3 >= tokens.size() || !isSyntaxOperator(tokens[i + 3], ";")) {
            addDiagnostic("P0030", "expected ';' after class receiver alias", tokens[i + 2]);
        }
    }

    return diagnostics;
}

// Compatibility for call sites that still use the former narrow name.
inline std::vector<ZeusDiagnostics::Diagnostic> validateUnsupportedSyntax(
    const std::vector<LexerType::Token>& tokens
) {
    return validateSyntax(tokens);
}

inline std::vector<ZeusDiagnostics::Diagnostic> validateParsedSyntax(
    const std::vector<TreeParser::NodeAndBlock>& ast
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    auto addDiagnostic = [&](const std::string& code, const std::string& message,
                             int line, int column) {
        diagnostics.push_back({ code, message, line, column });
    };
    const auto validateExpression = [&](const std::vector<LexerType::Token>& expression) {
        if (!expression.empty()) {
            ZeusDiagnostics::append(diagnostics, validateExpressionSyntax(expression));
        }
    };
    const auto findTopLevelAssignment = [](const std::vector<LexerType::Token>& tokens) {
        int parenDepth = 0;
        int bracketDepth = 0;
        int braceDepth = 0;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (std::holds_alternative<LexerType::Operator>(tokens[i])) {
                const std::string& symbol = std::get<LexerType::Operator>(tokens[i]).symbol;
                if (symbol == "=" && parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) return i;
                if (symbol == "(") ++parenDepth;
                else if (symbol == ")" && parenDepth > 0) --parenDepth;
                else if (symbol == "[") ++bracketDepth;
                else if (symbol == "]" && bracketDepth > 0) --bracketDepth;
                else if (symbol == "{") ++braceDepth;
                else if (symbol == "}" && braceDepth > 0) --braceDepth;
            }
        }
        return tokens.size();
    };
    const auto expressionAfterAssignment = [&](const std::vector<LexerType::Token>& tokens) {
        const size_t assignment = findTopLevelAssignment(tokens);
        if (assignment == tokens.size() || assignment + 1 >= tokens.size()) {
            return std::vector<LexerType::Token>{};
        }
        return std::vector<LexerType::Token>(tokens.begin() + assignment + 1, tokens.end());
    };
    const auto isFaddrLValue = [](const std::vector<LexerType::Token>& tokens) {
        if (tokens.size() < 7 || !isSyntaxOperator(tokens[0], "@") ||
            !std::holds_alternative<LexerType::Keyword>(tokens[1]) ||
            std::get<LexerType::Keyword>(tokens[1]).name != "faddr" ||
            !isSyntaxOperator(tokens[2], "(")) {
            return false;
        }
        const size_t close = matchingSyntaxDelimiter(tokens, 2);
        const bool simpleType = close + 3 == tokens.size() &&
            close + 1 < tokens.size() && isSyntaxOperator(tokens[close + 1], "->") &&
            std::holds_alternative<LexerType::Keyword>(tokens[close + 2]);
        const bool rawBytesRangeType = close + 4 == tokens.size() &&
            close + 3 < tokens.size() && isSyntaxOperator(tokens[close + 1], "->") &&
            isSyntaxOperator(tokens[close + 2], "&") &&
            isSyntaxIdentifier(tokens[close + 3]) &&
            std::get<LexerType::Keyword>(tokens[close + 3]).name == "bytes";
        return simpleType || rawBytesRangeType;
    };
    const auto isThisLValue = [](const std::vector<LexerType::Token>& tokens) {
        return tokens.size() == 2 && isSyntaxOperator(tokens[0], "@") &&
            std::holds_alternative<LexerType::Keyword>(tokens[1]) &&
            std::get<LexerType::Keyword>(tokens[1]).name == "this";
    };

    const auto visit = [&](const auto& self,
                           const std::vector<TreeParser::NodeAndBlock>& elements,
                           bool parentIsTry,
                           bool parentIsClass,
                           const std::string& parentReceiverAlias,
                           bool parentIsInterface,
                           bool parentIsTopLevel) -> void {
        for (const auto& element : elements) {
            if (std::holds_alternative<TreeParser::Node>(element)) {
                const auto& node = std::get<TreeParser::Node>(element);
                if (node.type == NodeType::VAR_DECL_STMT || node.type == NodeType::VAR_ASSIGN_STMT) {
                    const size_t assignment = findTopLevelAssignment(node.tokens);
                    if (node.type == NodeType::VAR_DECL_STMT) {
                        const size_t headerEnd = assignment == node.tokens.size() ? node.tokens.size() : assignment;
                        size_t nameIndex = 1;
                        if (!node.tokens.empty() && std::holds_alternative<LexerType::Keyword>(node.tokens[0]) &&
                            (std::get<LexerType::Keyword>(node.tokens[0]).name == "let" ||
                             std::get<LexerType::Keyword>(node.tokens[0]).name == "var")) {
                            if (nameIndex < headerEnd && std::holds_alternative<LexerType::Keyword>(node.tokens[nameIndex]) &&
                                std::get<LexerType::Keyword>(node.tokens[nameIndex]).name == "const") ++nameIndex;
                            if (nameIndex < headerEnd && std::holds_alternative<LexerType::Keyword>(node.tokens[nameIndex]) &&
                                std::get<LexerType::Keyword>(node.tokens[nameIndex]).name == "shared") ++nameIndex;
                            if (nameIndex >= headerEnd || !isSyntaxIdentifier(node.tokens[nameIndex])) {
                                addDiagnostic("P0024", "expected a variable name in declaration",
                                              node.sourceLine, node.sourceColumn);
                            } else {
                                ++nameIndex;
                                if (nameIndex < headerEnd && isSyntaxOperator(node.tokens[nameIndex], ":") &&
                                    nameIndex + 1 >= headerEnd) {
                                    addDiagnostic("P0024", "expected a type after ':' in declaration",
                                                  syntaxTokenLine(node.tokens[nameIndex]),
                                                  syntaxTokenColumn(node.tokens[nameIndex]));
                                }
                            }
                        }
                    } else if (assignment != node.tokens.size()) {
                        const std::vector<LexerType::Token> left(node.tokens.begin(), node.tokens.begin() + assignment);
                        bool validLValue = (left.size() == 1 && isSyntaxIdentifier(left[0])) ||
                            isThisLValue(left);
                        if (!validLValue && isFaddrLValue(left)) {
                            validateExpression(left);
                            validLValue = true;
                        } else if (left.size() >= 2 && isSyntaxOperator(left.back(), "]")) {
                            int depth = 0;
                            size_t bracketOpen = left.size();
                            for (size_t i = left.size(); i-- > 0;) {
                                if (isSyntaxOperator(left[i], "]")) ++depth;
                                else if (isSyntaxOperator(left[i], "[") && --depth == 0) {
                                    bracketOpen = i;
                                    break;
                                }
                            }
                            if (bracketOpen > 0 && bracketOpen < left.size() - 1) {
                                validateExpression(std::vector<LexerType::Token>(left.begin(), left.begin() + bracketOpen));
                                validateExpression(std::vector<LexerType::Token>(left.begin() + bracketOpen + 1, left.end() - 1));
                                validLValue = true;
                            }
                        } else if (left.size() >= 3 && isSyntaxOperator(left[left.size() - 2], ".") &&
                                   isSyntaxIdentifier(left.back())) {
                            validateExpression(std::vector<LexerType::Token>(left.begin(), left.end() - 2));
                            validLValue = true;
                        }
                        if (!validLValue) {
                            addDiagnostic("P0025", "left side of assignment must be a variable, member, index, or @this",
                                          node.sourceLine, node.sourceColumn);
                        }
                    }
                    if (assignment != node.tokens.size()) {
                        validateExpression(expressionAfterAssignment(node.tokens));
                    }
                } else if (node.type == NodeType::EXP_STMT) {
                    const bool classUsingAlias = node.tokens.size() >= 3 &&
                        std::holds_alternative<LexerType::Keyword>(node.tokens[0]) &&
                        std::get<LexerType::Keyword>(node.tokens[0]).name == "using" &&
                        isSyntaxOperator(node.tokens[1], "&");
                    if (classUsingAlias) {
                        if (node.tokens.size() != 3 || !isSyntaxIdentifier(node.tokens[2])) {
                            addDiagnostic("P0026", "class receiver alias must be written as 'using &name'",
                                          node.sourceLine, node.sourceColumn);
                        } else if (!parentIsClass) {
                            addDiagnostic("P0026", "'using &name' is only valid inside a class or interface",
                                          node.sourceLine, node.sourceColumn);
                        }
                    } else if (!node.tokens.empty()) {
                        validateExpression(node.tokens);
                    }
                } else if (node.type == NodeType::RETURN_STMT && node.tokens.size() > 1) {
                    validateExpression(std::vector<LexerType::Token>(node.tokens.begin() + 1, node.tokens.end()));
                } else if (node.type == NodeType::THROW_STMT) {
                    if (node.tokens.size() <= 1) {
                        addDiagnostic("P0012", "expected an expression after 'throw'",
                                      node.sourceLine, node.sourceColumn);
                    } else {
                        validateExpression(std::vector<LexerType::Token>(node.tokens.begin() + 1, node.tokens.end()));
                    }
                } else if (node.type == NodeType::BREAK_STMT && node.tokens.size() != 1) {
                    addDiagnostic("P0027", "'break' does not take arguments", node.sourceLine, node.sourceColumn);
                } else if (node.type == NodeType::DELETE_STMT) {
                    bool expectName = true;
                    for (size_t i = 1; i < node.tokens.size(); ++i) {
                        if (expectName) {
                            if (!isSyntaxIdentifier(node.tokens[i])) {
                                addDiagnostic("P0028", "expected a variable name after 'delete' or ','",
                                              syntaxTokenLine(node.tokens[i]), syntaxTokenColumn(node.tokens[i]));
                                break;
                            }
                        } else if (!isSyntaxOperator(node.tokens[i], ",")) {
                            addDiagnostic("P0028", "expected ',' between names in 'delete' statement",
                                          syntaxTokenLine(node.tokens[i]), syntaxTokenColumn(node.tokens[i]));
                            break;
                        }
                        expectName = !expectName;
                    }
                    if (node.tokens.size() <= 1 || expectName) {
                        addDiagnostic("P0028", "expected a variable name after 'delete'",
                                      node.sourceLine, node.sourceColumn);
                    }
                } else if (node.type == NodeType::IMPORT_STMT) {
                    size_t position = 1;
                    if (position >= node.tokens.size() ||
                        !isSyntaxPackageIdentifier(node.tokens[position])) {
                        addDiagnostic("P0029", "expected a package name after 'import'",
                                      node.sourceLine, node.sourceColumn);
                    } else {
                        ++position;
                        while (position < node.tokens.size() && isSyntaxOperator(node.tokens[position], ".")) {
                            ++position;
                            if (position >= node.tokens.size() ||
                                !isSyntaxPackageIdentifier(node.tokens[position])) {
                                addDiagnostic("P0029", "expected a package name after '.'",
                                              syntaxTokenLine(node.tokens[position - 1]),
                                              syntaxTokenColumn(node.tokens[position - 1]));
                                break;
                            }
                            ++position;
                        }
                        if (position < node.tokens.size() && isSyntaxOperator(node.tokens[position], "[")) {
                            ++position;
                            if (position >= node.tokens.size() || !isSyntaxOperator(node.tokens[position], "&")) {
                                addDiagnostic("P0029", "expected '&' in namespace import clause",
                                              syntaxTokenLine(node.tokens[position - 1]),
                                              syntaxTokenColumn(node.tokens[position - 1]));
                            } else {
                                ++position;
                                if (position < node.tokens.size() && !isSyntaxOperator(node.tokens[position], "]")) {
                                    if (!isSyntaxIdentifier(node.tokens[position])) {
                                        addDiagnostic("P0029", "expected an alias name in namespace import clause",
                                                      syntaxTokenLine(node.tokens[position]),
                                                      syntaxTokenColumn(node.tokens[position]));
                                    }
                                    ++position;
                                }
                            }
                            if (position >= node.tokens.size() || !isSyntaxOperator(node.tokens[position], "]")) {
                                addDiagnostic("P0029", "expected ']' after namespace import clause",
                                              node.sourceLine, node.sourceColumn);
                            } else {
                                ++position;
                            }
                        }
                        if (position != node.tokens.size()) {
                            addDiagnostic("P0029", "unexpected token in import statement",
                                          syntaxTokenLine(node.tokens[position]),
                                          syntaxTokenColumn(node.tokens[position]));
                        }
                    }
                } else if (node.type == NodeType::USING_STMT) {
                    const bool validUsingOriginal = node.tokens.size() == 2 &&
                        std::holds_alternative<LexerType::Keyword>(node.tokens[1]) &&
                        std::get<LexerType::Keyword>(node.tokens[1]).name == "original";
                    if (!validUsingOriginal) {
                        addDiagnostic("P0030", "expected 'using original'", node.sourceLine, node.sourceColumn);
                    }
                } else if (node.type == NodeType::FUNC_FWD_DECL_STMT) {
                    bool hasDeclFunc = false;
                    for (size_t i = 0; i + 1 < node.tokens.size(); ++i) {
                        if (std::holds_alternative<LexerType::Keyword>(node.tokens[i]) &&
                            std::get<LexerType::Keyword>(node.tokens[i]).name == "decl" &&
                            std::holds_alternative<LexerType::Keyword>(node.tokens[i + 1]) &&
                            std::get<LexerType::Keyword>(node.tokens[i + 1]).name == "func") {
                            hasDeclFunc = true;
                            break;
                        }
                    }
                    if (!hasDeclFunc) {
                        addDiagnostic("P0023", "expected 'decl func name(...)'", node.sourceLine, node.sourceColumn);
                    }
                } else if (node.type == NodeType::INTERFACE_METHOD_DECL_STMT) {
                    const auto signature = parseForwardFunctionSignature(node.tokens);
                    const bool hasDeclFunc = node.tokens.size() >= 2 &&
                        std::holds_alternative<LexerType::Keyword>(node.tokens[0]) &&
                        std::get<LexerType::Keyword>(node.tokens[0]).name == "decl" &&
                        std::holds_alternative<LexerType::Keyword>(node.tokens[1]) &&
                        std::get<LexerType::Keyword>(node.tokens[1]).name == "func";
                    if (!parentIsInterface) {
                        addDiagnostic("P0040", "interface method signatures must appear inside an interface",
                                      node.sourceLine, node.sourceColumn);
                    } else if (!hasDeclFunc || !signature.valid || signature.name.empty()) {
                        addDiagnostic("P0040", "interface methods must use 'decl func name(...)'",
                                      node.sourceLine, node.sourceColumn);
                    }
                } else if (node.type == NodeType::FOR_DECL_STMT) {
                    addDiagnostic("P0013", "unsupported or malformed 'for' header",
                                  node.sourceLine, node.sourceColumn);
                } else if (node.type == NodeType::IF_DECL_STMT ||
                           node.type == NodeType::FUNC_DECL_STMT ||
                           node.type == NodeType::CLASS_DECL_STMT ||
                           node.type == NodeType::INTERFACE_DECL_STMT ||
                           node.type == NodeType::IMPL_DECL_STMT ||
                           node.type == NodeType::TRY_DECL_STMT ||
                           node.type == NodeType::WITH_DECL_STMT ||
                           node.type == NodeType::USING_BLOCK_STMT ||
                           node.type == NodeType::USING_LLVM_BLOCK_STMT ||
                           node.type == NodeType::USING_VM_BLOCK_STMT) {
                    addDiagnostic("P0032", "expected a block for this declaration",
                                  node.sourceLine, node.sourceColumn);
                }
                continue;
            }

            const auto& blockPtr = std::get<std::unique_ptr<TreeParser::CodeBlock>>(element);
            if (!blockPtr) continue;
            const auto& block = *blockPtr;
            if (block.type == CodeBlockType::IMPL && !parentIsTopLevel) {
                addDiagnostic("P0038", "impl declarations must appear at module scope",
                              block.sourceLine, block.sourceColumn);
            }
            if (block.type == CodeBlockType::INTERFACE && !parentIsTopLevel) {
                addDiagnostic("P0040", "interface declarations must appear at module scope",
                              block.sourceLine, block.sourceColumn);
            }
            if (block.type == CodeBlockType::CATCH && !parentIsTry) {
                addDiagnostic("P0031", "'catch' block must follow a 'try' block",
                              block.sourceLine, block.sourceColumn);
            }
            if (block.type == CodeBlockType::FUNC && block.isClassMethod &&
                !block.isStaticMethod && !parentReceiverAlias.empty()) {
                const auto aliasCapture = std::find(
                    block.captureVars.begin(), block.captureVars.end(),
                    parentReceiverAlias
                );
                if (aliasCapture != block.captureVars.end() ||
                    block.hasDuplicateInjectedReceiverParameter) {
                    addDiagnostic("P0033",
                                  "class receiver alias is already injected; do not capture or redeclare it",
                                  block.sourceLine, block.sourceColumn);
                }
            }
            if (block.type == CodeBlockType::FUNC && block.funcName.empty() && !block.isOperatorOverload) {
                addDiagnostic("P0004", "function declarations require a name; anonymous functions are not supported",
                              block.sourceLine, block.sourceColumn);
            } else if (block.type == CodeBlockType::IF && block.conditionTokens.empty()) {
                addDiagnostic("P0011", "expected an expression inside 'if' header",
                              block.sourceLine, block.sourceColumn);
            } else if (block.type == CodeBlockType::FOR && !block.forHeaderValid) {
                addDiagnostic("P0013", "invalid or incomplete 'for' header",
                              block.sourceLine, block.sourceColumn);
            } else if (block.type == CodeBlockType::WITH && block.withExprTokens.empty()) {
                addDiagnostic("P0011", "expected an expression inside 'with' header",
                              block.sourceLine, block.sourceColumn);
            } else if (block.type == CodeBlockType::CLASS && block.className.empty()) {
                addDiagnostic("P0014", "class declarations require a name",
                              block.sourceLine, block.sourceColumn);
            } else if (block.type == CodeBlockType::INTERFACE && block.interfaceName.empty()) {
                addDiagnostic("P0040", "interface declarations require a name",
                              block.sourceLine, block.sourceColumn);
            } else if (block.type == CodeBlockType::IMPL && block.implTypeName.empty()) {
                addDiagnostic("P0014", "impl declarations require a target type",
                              block.sourceLine, block.sourceColumn);
            } else if (block.type == CodeBlockType::IMPL &&
                       !isSyntaxTypePath(block.implTypeName)) {
                addDiagnostic("P0014", "impl target must be a valid type name",
                              block.sourceLine, block.sourceColumn);
            }

            if (block.type == CodeBlockType::IMPL) {
                for (const auto& member : block.nodes) {
                    if (std::holds_alternative<TreeParser::Node>(member)) {
                        const auto& node = std::get<TreeParser::Node>(member);
                        const bool receiverAlias = node.type == NodeType::EXP_STMT &&
                            node.tokens.size() == 3 &&
                            std::holds_alternative<LexerType::Keyword>(node.tokens[0]) &&
                            std::get<LexerType::Keyword>(node.tokens[0]).name == "using" &&
                            isSyntaxOperator(node.tokens[1], "&") &&
                            isSyntaxIdentifier(node.tokens[2]);
                        if (!receiverAlias) {
                            addDiagnostic("P0032", "impl blocks may only contain function declarations",
                                          node.sourceLine, node.sourceColumn);
                        }
                        continue;
                    }
                    const auto& method = std::get<std::unique_ptr<TreeParser::CodeBlock>>(member);
                    if (method && method->type != CodeBlockType::FUNC) {
                        addDiagnostic("P0032", "impl blocks may only contain function declarations",
                                      method->sourceLine, method->sourceColumn);
                    }
                }
            }
            if (block.type == CodeBlockType::INTERFACE) {
                for (const auto& member : block.nodes) {
                    if (std::holds_alternative<TreeParser::Node>(member)) {
                        const auto& node = std::get<TreeParser::Node>(member);
                        const bool receiverAlias = node.type == NodeType::EXP_STMT &&
                            node.tokens.size() == 3 &&
                            std::holds_alternative<LexerType::Keyword>(node.tokens[0]) &&
                            std::get<LexerType::Keyword>(node.tokens[0]).name == "using" &&
                            isSyntaxOperator(node.tokens[1], "&") &&
                            isSyntaxIdentifier(node.tokens[2]);
                        if (!receiverAlias &&
                            node.type != NodeType::INTERFACE_METHOD_DECL_STMT) {
                            addDiagnostic("P0040", "interfaces may only contain method signatures and a receiver alias",
                                          node.sourceLine, node.sourceColumn);
                        }
                        continue;
                    }
                    const auto& method = std::get<std::unique_ptr<TreeParser::CodeBlock>>(member);
                    if (method) {
                        addDiagnostic("P0040", "interfaces contain signatures without method bodies",
                                      method->sourceLine, method->sourceColumn);
                    }
                }
            }
            if (block.type == CodeBlockType::FUNC && block.isClassMethod &&
                !block.ownerClassName.empty() && !block.isStaticMethod &&
                !block.isCompileTimeStatic && !block.paramNames.empty() &&
                block.paramNames.front() == "this" &&
                !block.paramTypeNames.empty() &&
                !block.paramTypeNames.front().empty() &&
                canonicalSyntaxTypeName(block.paramTypeNames.front()) !=
                    canonicalSyntaxTypeName(block.ownerClassName)) {
                addDiagnostic("P0039", "impl receiver parameter 'this' must use the implemented type",
                              block.sourceLine, block.sourceColumn);
            }

            if (block.type == CodeBlockType::IF && !block.conditionTokens.empty()) {
                validateExpression(block.conditionTokens);
            } else if (block.type == CodeBlockType::WITH && !block.withExprTokens.empty()) {
                validateExpression(block.withExprTokens);
            } else if (block.type == CodeBlockType::FOR && block.forHeaderValid) {
                switch (block.forKind) {
                    case TreeParser::CodeBlock::ForKind::COND:
                        validateExpression(block.condExprTokens);
                        break;
                    case TreeParser::CodeBlock::ForKind::LIST:
                        validateExpression(block.listExprTokens);
                        break;
                    case TreeParser::CodeBlock::ForKind::DICT:
                        validateExpression(block.dictExprTokens);
                        break;
                    case TreeParser::CodeBlock::ForKind::COUNT:
                        validateExpression(block.countStartTokens);
                        validateExpression(block.countEndTokens);
                        if (!block.countStepTokens.empty()) validateExpression(block.countStepTokens);
                        break;
                    case TreeParser::CodeBlock::ForKind::COUNT_SIMPLE:
                        validateExpression(block.countEndTokens);
                        break;
                    default:
                        break;
                }
            } else if (block.type == CodeBlockType::FUNC) {
                for (const auto& defaultValue : block.paramDefaults) {
                    if (!defaultValue.empty()) validateExpression(defaultValue);
                }
            } else if (block.type == CodeBlockType::TRY) {
                bool hasCatch = false;
                for (const auto& child : block.nodes) {
                    if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(child)) {
                        const auto& childBlock = std::get<std::unique_ptr<TreeParser::CodeBlock>>(child);
                        hasCatch = hasCatch || (childBlock && childBlock->type == CodeBlockType::CATCH);
                    }
                }
                if (!hasCatch) {
                    addDiagnostic("P0017", "'try' block requires a following 'catch' block",
                                  block.sourceLine, block.sourceColumn);
                }
            }
            self(self, block.nodes,
                 block.type == CodeBlockType::TRY,
                 block.type == CodeBlockType::CLASS ||
                    block.type == CodeBlockType::INTERFACE ||
                    block.type == CodeBlockType::IMPL,
                 (block.type == CodeBlockType::CLASS ||
                    block.type == CodeBlockType::INTERFACE ||
                    block.type == CodeBlockType::IMPL)
                     ? block.classDefaultAlias : std::string(),
                 block.type == CodeBlockType::INTERFACE,
                 false);
        }
    };
    // TreeParser wraps module statements in a synthetic BLOCK. Validate its
    // children at module scope; nested blocks still lose that privilege.
    if (ast.size() == 1 &&
        std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(ast.front())) {
        const auto& root = std::get<std::unique_ptr<TreeParser::CodeBlock>>(ast.front());
        if (root && root->type == CodeBlockType::BLOCK) {
            visit(visit, root->nodes, false, false, {}, false, true);
        } else {
            visit(visit, ast, false, false, {}, false, true);
        }
    } else {
        visit(visit, ast, false, false, {}, false, true);
    }
    return diagnostics;
}

} // namespace ZeusParser

#endif
