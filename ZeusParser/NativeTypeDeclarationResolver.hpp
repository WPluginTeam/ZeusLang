#pragma once

#include "NativeTypeDeclarationSyntax.hpp"
#include "CompileTimeStaticValues.hpp"
#include "../ZeusTyping/Types.hpp"
#include "../ZeusIR/IRImport.hpp"
#include "../ZeusBuiltins/NativeTypeIDs.hpp"

namespace ZeusParser {
namespace NativeTypeDeclarationDetail {

inline std::string numberedType(const std::string& number) {
    const auto* type = ZeusBuiltins::findNativeTypeIDValue(number);
    return type == nullptr ? std::string() : std::string(type->canonicalName);
}

struct ModuleBindings {
    Bindings aliases;
    CompileTimeStaticValues::Values staticValues;
    std::set<std::string> hiddenAliases;
};

class Resolver {
    private:
        std::map<std::string, ModuleBindings> modules;
        std::set<std::string> visiting;

        ModuleBindings dependency(const Import& imported,
                                  const std::string& sourcePath,
                                  const Tokens& ownerTokens,
                                  std::vector<ZeusDiagnostics::Diagnostic>& diagnostics) {
            const std::string previousDirectory = IRImport::currentWorkingDir;
            IRImport::setWorkingDirectory(sourcePath);
            const std::string path = IRImport::resolvePackagePath(imported.package);
            IRImport::currentWorkingDir = previousDirectory;
            if (path.empty()) return {};
            const auto cached = modules.find(path);
            if (cached != modules.end()) return cached->second;
            if (!visiting.insert(path).second) return {};
            auto tokens = LexerParser::parse(IRImport::readPackageContent(path));
            std::vector<ZeusDiagnostics::Diagnostic> dependencyDiagnostics;
            ModuleBindings bindings = resolve(tokens, path, dependencyDiagnostics, false);
            for (const auto& diagnostic : dependencyDiagnostics) {
                error(diagnostics, ownerTokens, imported.token,
                      path + ":" + std::to_string(diagnostic.line) + ": " + diagnostic.message);
            }
            visiting.erase(path);
            modules.emplace(path, bindings);
            return bindings;
        }

    public:
        ModuleBindings resolve(Tokens& tokens, const std::string& sourcePath,
                               std::vector<ZeusDiagnostics::Diagnostic>& diagnostics,
                               bool rewrite = true) {
            ModuleBindings visible;
            std::set<std::string> localAliases;
            bool explicitExports = false;
            const auto exportedNames = exports(tokens, explicitExports);
            std::set<std::string> valueDeclarations;
            int declarationDepth = 0;
            for (size_t i = 0; i < tokens.size(); ++i) {
                const std::string keyword = word(tokens, i);
                if (declarationDepth == 0 &&
                    (keyword == "var" || keyword == "let" || keyword == "const" ||
                     keyword == "func" || keyword == "class")) {
                    size_t nameIndex = i + 1;
                    if ((keyword == "var" || keyword == "let") &&
                        word(tokens, nameIndex) == "const") ++nameIndex;
                    if (word(tokens, nameIndex) == "shared") ++nameIndex;
                    valueDeclarations.insert(word(tokens, nameIndex));
                }
                if (op(tokens, i, "{")) ++declarationDepth;
                if (op(tokens, i, "}")) --declarationDepth;
                if (rewrite && (keyword == "__i8" || keyword == "__i16" ||
                                keyword == "__i32" || keyword == "__double")) {
                    error(diagnostics, tokens, i,
                          "internal native type names are not source syntax; declare a native type alias");
                }
            }
            const auto bind = [&](Bindings& destination, const std::string& name,
                                  const std::string& type, size_t token) {
                if (!destination.emplace(name, type).second) {
                    error(diagnostics, tokens, token,
                          "duplicate native type binding '" + name + "'");
                }
            };
            const auto bindStaticValue = [&](CompileTimeStaticValues::Values& destination,
                                             const std::string& name,
                                             const LexerType::Value& value,
                                             size_t token) {
                if (!destination.emplace(name, value).second) {
                    error(diagnostics, tokens, token,
                          "duplicate compile-time static value import '" + name + "'");
                }
            };
            for (auto imported : imports(tokens)) {
                imported.package = IRImport::canonicalPackageName(imported.package);
                ModuleBindings bindings = dependency(imported, sourcePath, tokens, diagnostics);
                const auto bindImports = [&](Bindings& target, const Bindings& source) {
                    for (const auto& entry : source) {
                        bind(target, imported.namespaceMode
                            ? imported.alias + "." + entry.first : entry.first,
                            entry.second, imported.token);
                    }
                };
                bindImports(visible.aliases, bindings.aliases);
                for (const auto& entry : bindings.staticValues) {
                    const std::string name = imported.namespaceMode
                        ? imported.alias + "." + entry.first : entry.first;
                    const std::string root = name.substr(0, name.find('.'));
                    if (valueDeclarations.count(root) == 0) {
                        bindStaticValue(visible.staticValues, name, entry.second,
                                        imported.token);
                    }
                }
                for (const auto& name : bindings.hiddenAliases) {
                    visible.hiddenAliases.insert(imported.namespaceMode
                        ? imported.alias + "." + name : name);
                }
            }

            const auto localStaticValues = CompileTimeStaticValues::resolveDeclarations(
                tokens, visible.staticValues);
            for (const auto& entry : localStaticValues) {
                visible.staticValues.insert_or_assign(entry.first, entry.second);
            }

            std::vector<bool> removed(tokens.size(), false);
            int depth = 0;
            for (size_t i = 0; i < tokens.size(); ++i) {
                if (op(tokens, i, "{")) ++depth;
                if (op(tokens, i, "}")) --depth;
                const auto declaration = parseNativeTypeDeclaration(tokens, i);
                if (!declaration.matched) continue;
                for (size_t j = declaration.beginToken; j < declaration.endToken; ++j)
                    removed[j] = true;
                ZeusDiagnostics::append(diagnostics, declaration.diagnostics);
                if (depth != 0) {
                    error(diagnostics, tokens, i,
                          "native type declarations are only allowed at module scope");
                } else if (declaration.valid()) {
                    std::string type;
                    if (declaration.valueKind == NativeTypeDeclarationValueKind::NumericID) {
                        type = numberedType(declaration.numericID);
                    } else if (declaration.valueKind ==
                               NativeTypeDeclarationValueKind::StaticValueReference) {
                        const auto found = visible.staticValues.find(
                            declaration.staticValueReference);
                        if (found != visible.staticValues.end()) {
                            const auto* integer = std::get_if<ZeusType::BigInteger>(
                                &found->second.data);
                            if (integer != nullptr) type = numberedType(integer->toString());
                        }
                    }
                    if (type.empty()) {
                        error(diagnostics, tokens, declaration.valueToken,
                              "unknown native type: use a registered numeric ID or a compile-time static integer constant");
                    } else if (ZeusBuiltins::findTypeImplementation(declaration.name) ||
                               (ZeusBuiltins::findNativeTypeIDCanonical(declaration.name) != nullptr &&
                                ZeusBuiltins::findNativeTypeIDCanonical(declaration.name)->valueABI ==
                                    ZeusBuiltins::NativeValueABI::NativePointer) ||
                               ZeusTypeInfer::parseTypeName(declaration.name) != ZeusTypeInfer::InferredType::Unknown) {
                        error(diagnostics, tokens, declaration.nameToken,
                              "native type alias cannot shadow builtin type '" + declaration.name + "'");
                    } else {
                        bind(visible.aliases, declaration.name, type, declaration.nameToken);
                        localAliases.insert(declaration.name);
                    }
                }
                if (declaration.endToken > i) i = declaration.endToken - 1;
            }

            if (rewrite) {
                Tokens retained;
                for (size_t i = 0; i < tokens.size(); ++i) if (!removed[i]) retained.push_back(tokens[i]);
                tokens = std::move(retained);
                normalize(tokens, visible.aliases, localAliases,
                          visible.hiddenAliases, diagnostics);
            }
            ModuleBindings exported;
            for (const auto& name : localAliases) {
                if (!explicitExports || exportedNames.count(name))
                    exported.aliases.emplace(name, visible.aliases.at(name));
                else exported.hiddenAliases.insert(name);
            }
            for (const auto& entry : localStaticValues) {
                if (!explicitExports || exportedNames.count(entry.first))
                    exported.staticValues.emplace(entry.first, entry.second);
            }
            return exported;
        }

        static void normalize(Tokens& tokens, const Bindings& aliases,
                              const std::set<std::string>& localAliases,
                              const std::set<std::string>& hiddenAliases,
                              std::vector<ZeusDiagnostics::Diagnostic>& diagnostics) {
            const auto normalizeType = [&](const auto& self, size_t& cursor) -> void {
                const bool referencePrefix = op(tokens, cursor, "&");
                if (referencePrefix) ++cursor;
                const size_t start = cursor;
                std::string name;
                if (!parseGenericTypeName(tokens, cursor, name)) return;
                const auto alias = aliases.find(name);
                if (alias != aliases.end()) {
                    const std::string& target = alias->second;
                    if (!target.empty() && target.front() == '&') {
                        if (referencePrefix) {
                            error(diagnostics, tokens, start,
                                  "reference type alias already includes a reference prefix");
                        } else {
                            const auto& source = std::get<LexerType::Keyword>(tokens[start]);
                            LexerType::Operator reference("&");
                            reference.line = source.line;
                            reference.column = source.column;
                            LexerType::Keyword typeName(target.substr(1));
                            typeName.line = source.line;
                            typeName.column = source.column;
                            tokens.erase(tokens.begin() + start, tokens.begin() + cursor);
                            tokens.insert(tokens.begin() + start, std::move(typeName));
                            tokens.insert(tokens.begin() + start, std::move(reference));
                            cursor = start + 2;
                        }
                    } else {
                        std::get<LexerType::Keyword>(tokens[start]).name = target;
                        tokens.erase(tokens.begin() + start + 1, tokens.begin() + cursor);
                        cursor = start + 1;
                    }
                } else if (hiddenAliases.count(name)) {
                    error(diagnostics, tokens, start,
                          "native type alias '" + name + "' is not exported by its module");
                }
                if (op(tokens, cursor, "<")) {
                    ++cursor;
                    while (cursor < tokens.size()) {
                        const size_t before = cursor;
                        self(self, cursor);
                        if (cursor == before) break;
                        if (op(tokens, cursor, ",")) ++cursor;
                        else break;
                    }
                    if (op(tokens, cursor, ">")) ++cursor;
                }
            };
            bool variableHeader = false;
            bool functionHeader = false;
            std::set<std::string> aliasRoots;
            for (const auto& alias : aliases)
                aliasRoots.insert(alias.first.substr(0, alias.first.find('.')));
            for (size_t i = 0; i < tokens.size(); ++i) {
                const std::string keyword = word(tokens, i);
                if (keyword == "var" || keyword == "let" || keyword == "const" ||
                    keyword == "func" || keyword == "class" || keyword == "interface") {
                    const std::string declared = word(tokens, i + 1);
                    if (aliasRoots.count(declared)) error(diagnostics, tokens, i + 1,
                        "declaration cannot shadow native type alias '" + declared + "'");
                }
                if (keyword == "var" || keyword == "let") variableHeader = true;
                if (keyword == "func") functionHeader = true;
                if (functionHeader && aliases.count(keyword) &&
                    op(tokens, i + 1, ":")) {
                    error(diagnostics, tokens, i,
                          "parameter cannot shadow native type alias '" + keyword + "'");
                }
                if (op(tokens, i, "=") || op(tokens, i, ";") || op(tokens, i, "{")) variableHeader = false;
                if (op(tokens, i, ";") || op(tokens, i, "{")) functionHeader = false;
                if (keyword == "sizeof" && op(tokens, i + 1, "(")) {
                    size_t cursor = i + 2;
                    normalizeType(normalizeType, cursor);
                }
                if (op(tokens, i, "->") ||
                    (op(tokens, i, ":") && (variableHeader || functionHeader)) ||
                    keyword == "impl") {
                    size_t cursor = i + 1;
                    normalizeType(normalizeType, cursor);
                }
                if (op(tokens, i, "<")) {
                    size_t after = i;
                    std::vector<std::string> arguments;
                    if (parseGenericTypeArguments(tokens, i, after, arguments) &&
                        op(tokens, after, "(")) {
                        size_t cursor = i + 1;
                        do {
                            normalizeType(normalizeType, cursor);
                            if (!op(tokens, cursor, ",")) break;
                            ++cursor;
                        } while (cursor < tokens.size());
                    }
                }
                if (keyword == "using" && op(tokens, i + 1, "{")) {
                    size_t cursor = i + 2;
                    while (cursor < tokens.size() && !op(tokens, cursor, "}")) {
                        if (localAliases.count(word(tokens, cursor))) {
                            tokens.erase(tokens.begin() + cursor);
                            if (op(tokens, cursor, ",")) tokens.erase(tokens.begin() + cursor);
                            else if (cursor > i + 2 && op(tokens, cursor - 1, ",")) {
                                tokens.erase(tokens.begin() + cursor - 1);
                                --cursor;
                            }
                        } else ++cursor;
                    }
                }
            }
        }
};

} // namespace NativeTypeDeclarationDetail

inline void normalizeNativeTypeDeclarations(
    std::vector<LexerType::Token>& tokens,
    const std::string& sourcePath,
    std::vector<ZeusDiagnostics::Diagnostic>& diagnostics
) {
    NativeTypeDeclarationDetail::Resolver resolver;
    resolver.resolve(tokens, sourcePath, diagnostics);
}

inline std::vector<LexerType::Token> parseNativeTypeSource(
    const std::string& source, const std::string& sourcePath,
    std::vector<ZeusDiagnostics::Diagnostic>& diagnostics
) {
    auto tokens = LexerParser::parse(source, &diagnostics);
    if (diagnostics.empty()) normalizeNativeTypeDeclarations(tokens, sourcePath, diagnostics);
    return tokens;
}

} // namespace ZeusParser
