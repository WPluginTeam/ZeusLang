#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../ZeusIR/IRImport.hpp"
#include "../ZeusParser/AstElementView.hpp"
#include "../ZeusParser/LexerParser.hpp"
#include "../ZeusParser/SyntaxValidator.hpp"
#include "../ZeusParser/TreeParser.hpp"
#include "../ZeusParser/BackendBlockSelector.hpp"
#include "../ZeusParser/InterfaceConformance.hpp"
#include "../ZeusBuiltins/BuiltinRegistry.hpp"

namespace ZeusLLVM {

struct ModuleImport {
    std::string packageName;
    std::string alias;
    bool namespaceMode = false;
};

struct ModuleFunctionInterface {
    std::string name;
    std::string returnType;
    std::vector<std::string> parameterTypes;
};

struct ModuleClassInterface {
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;
};

struct ModuleNode {
    std::string packageName;
    std::string path;
    std::string source;
    std::vector<ModuleImport> imports;
    std::vector<ModuleFunctionInterface> functions;
    std::vector<ModuleClassInterface> classes;
    ZeusParser::InterfaceContractRegistry interfaceContracts;
};

struct ModuleGraphResult {
    std::vector<ModuleNode> dependencyOrder;
    std::vector<ModuleImport> rootImports;
    std::string error;

    bool ok() const { return error.empty(); }
};

namespace ModuleGraphDetail {

inline bool isKeyword(const LexerType::Token& token, const std::string& name) {
    return std::holds_alternative<LexerType::Keyword>(token) &&
           std::get<LexerType::Keyword>(token).name == name;
}

inline std::string operatorText(const LexerType::Token& token) {
    if (!std::holds_alternative<LexerType::Operator>(token)) return {};
    return std::get<LexerType::Operator>(token).toString();
}

inline std::vector<ModuleImport> readImports(const std::string& source) {
    const auto tokens = LexerParser::parse(source);
    std::vector<ModuleImport> imports;

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!isKeyword(tokens[i], "import")) continue;

        ModuleImport item;
        bool started = false;
        size_t j = i + 1;
        for (; j < tokens.size(); ++j) {
            const std::string op = operatorText(tokens[j]);
            if (op == "[" || op == ";") break;
            if (op == ".") {
                item.packageName += ".";
            } else if (std::holds_alternative<LexerType::Keyword>(tokens[j])) {
                item.packageName += std::get<LexerType::Keyword>(tokens[j]).name;
                started = true;
            } else if (std::holds_alternative<LexerType::Value>(tokens[j])) {
                item.packageName += std::get<LexerType::Value>(tokens[j]).toString();
                started = true;
            } else {
                if (started) break;
            }
        }

        if (item.packageName.empty()) continue;

        if (j < tokens.size() && operatorText(tokens[j]) == "[") {
            bool sawAmpersand = false;
            for (++j; j < tokens.size() && operatorText(tokens[j]) != "]"; ++j) {
                if (operatorText(tokens[j]) == "&") {
                    item.namespaceMode = true;
                    sawAmpersand = true;
                    continue;
                }
                if (!sawAmpersand) continue;
                if (std::holds_alternative<LexerType::Keyword>(tokens[j])) {
                    item.alias = std::get<LexerType::Keyword>(tokens[j]).name;
                } else if (std::holds_alternative<LexerType::Value>(tokens[j])) {
                    item.alias = std::get<LexerType::Value>(tokens[j]).toString();
                }
            }
        }

        item.packageName = IRImport::canonicalPackageName(item.packageName);
        imports.push_back(std::move(item));
    }

    return imports;
}

inline std::vector<ModuleFunctionInterface> readInterfaces(
    const std::string& source,
    const std::string& sourcePath = {}
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    const auto tokens = LexerParser::parse(source, &diagnostics);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateSyntax(tokens));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(diagnostics.front(), sourcePath, source));
    }
    auto nodes = TreeParser::parse(tokens, &diagnostics);
    ZeusParser::selectBackendBlocks(nodes, ZeusParser::BackendTarget::LLVM);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateParsedSyntax(nodes));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(diagnostics.front(), sourcePath, source));
    }
    std::vector<ModuleFunctionInterface> functions;

    const auto collectTopLevelFunctions = [&](const auto& self,
                                              const std::vector<TreeParser::NodeAndBlock>& elements) -> void {
        for (const auto& node : elements) {
            TreeParser::AstElementView element(node);
            if (!element.isBlock()) continue;
            const auto* block = element.block();
            if (block == nullptr) continue;

            if (block->type == CodeBlockType::FUNC) {
                if (!block->funcName.empty()) {
                    ModuleFunctionInterface function;
                    function.name = block->funcName;
                    function.returnType = block->returnTypeName;
                    function.parameterTypes = block->paramTypeNames;
                    functions.push_back(std::move(function));
                }
                continue;
            }

            if (block->type == CodeBlockType::BLOCK) {
                self(self, block->nodes);
            }
        }
    };
    collectTopLevelFunctions(collectTopLevelFunctions, nodes);

    return functions;
}

inline std::vector<ModuleClassInterface> readClassInterfaces(
    const std::string& source,
    const std::string& sourcePath = {}
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    const auto tokens = LexerParser::parse(source, &diagnostics);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateSyntax(tokens));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(diagnostics.front(), sourcePath, source));
    }
    auto nodes = TreeParser::parse(tokens, &diagnostics);
    ZeusParser::selectBackendBlocks(nodes, ZeusParser::BackendTarget::LLVM);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateParsedSyntax(nodes));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(diagnostics.front(), sourcePath, source));
    }

    std::vector<ModuleClassInterface> classes;
    const auto collectClasses = [&](const auto& self,
                                    const std::vector<TreeParser::NodeAndBlock>& elements) -> void {
        for (const auto& node : elements) {
            TreeParser::AstElementView element(node);
            if (!element.isBlock()) continue;
            const auto* block = element.block();
            if (block == nullptr) continue;

            if (block->type == CodeBlockType::CLASS && !block->className.empty()) {
                ModuleClassInterface classInterface;
                classInterface.name = block->className;
                for (const auto& child : block->nodes) {
                    if (!std::holds_alternative<TreeParser::Node>(child)) continue;
                    const auto& field = std::get<TreeParser::Node>(child);
                    if (field.type != NodeType::VAR_DECL_STMT) continue;

                    std::string fieldName;
                    std::string fieldType;
                    bool readingType = false;
                    for (const auto& token : field.tokens) {
                        if (ModuleGraphDetail::operatorText(token) == "=") break;
                        if (ModuleGraphDetail::operatorText(token) == ":") {
                            readingType = true;
                            continue;
                        }
                        if (!std::holds_alternative<LexerType::Keyword>(token)) continue;
                        const std::string& name =
                            std::get<LexerType::Keyword>(token).name;
                        if (name == "let" || name == "var" || name == "const") continue;
                        if (readingType) {
                            fieldType = name;
                            readingType = false;
                        } else if (fieldName.empty()) {
                            fieldName = name;
                        }
                    }
                    if (!fieldName.empty() && !fieldType.empty()) {
                        classInterface.fields.emplace_back(fieldName, fieldType);
                    }
                }
                classes.push_back(std::move(classInterface));
            }

            if (block->type == CodeBlockType::BLOCK) {
                self(self, block->nodes);
            }
        }
    };
    collectClasses(collectClasses, nodes);
    return classes;
}

inline ZeusParser::InterfaceContractRegistry readInterfaceContracts(
    const std::string& source,
    const std::string& sourcePath = {}
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    const auto tokens = LexerParser::parse(source, &diagnostics);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateSyntax(tokens));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(
            diagnostics.front(), sourcePath, source));
    }
    auto nodes = TreeParser::parse(tokens, &diagnostics);
    ZeusParser::selectBackendBlocks(nodes, ZeusParser::BackendTarget::LLVM);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateParsedSyntax(nodes));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(
            diagnostics.front(), sourcePath, source));
    }
    return ZeusParser::collectInterfaceContracts(nodes);
}

class Builder {
    ModuleGraphResult result;
    std::map<std::string, int> visitState;
    std::map<std::string, size_t> packageToNode;
    std::map<std::string, std::string> pathToPackage;
    std::vector<std::string> importStack;

    static std::string normalizedPath(const std::string& path) {
        std::error_code ec;
        std::filesystem::path value = std::filesystem::weakly_canonical(path, ec);
        if (ec) value = std::filesystem::absolute(path, ec);
        return value.lexically_normal().string();
    }

    bool visit(std::string packageName, const std::string& importerPath) {
        packageName = IRImport::canonicalPackageName(packageName);
        auto stateIt = visitState.find(packageName);
        if (stateIt != visitState.end()) {
            if (stateIt->second == 2) return true;
            if (stateIt->second == 1) {
                result.error = "[emit-llvm] module import cycle: ";
                for (const auto& name : importStack) result.error += name + " -> ";
                result.error += packageName;
                return false;
            }
        }

        const std::string oldWorkingDirectory = IRImport::currentWorkingDir;
        IRImport::setWorkingDirectory(importerPath);
        std::string resolvedPath = IRImport::resolvePackagePath(packageName);
        IRImport::currentWorkingDir = oldWorkingDirectory;
        if (!resolvedPath.empty()) {
            const auto* implementation = ZeusBuiltins::findImplementationModule(packageName);
            if (implementation != nullptr && !implementation->llvmSourceFile.empty()) {
                const std::filesystem::path llvmPath =
                    std::filesystem::path(resolvedPath).parent_path() /
                    std::string(implementation->llvmSourceFile);
                if (!std::filesystem::exists(llvmPath)) {
                    result.error = "[emit-llvm] LLVM source for built-in implementation module '" +
                                   packageName + "' was not found at '" + llvmPath.string() + "'";
                    return false;
                }
                resolvedPath = llvmPath.string();
            }
        }
        if (resolvedPath.empty()) {
            const std::string moduleKind = ZeusBuiltins::isImplementationModule(packageName)
                ? "built-in implementation module '"
                : "module '";
            result.error = "[emit-llvm] " + moduleKind + packageName + "' was not found (imported from '" +
                           importerPath + "')";
            return false;
        }

        const std::string canonicalPath = normalizedPath(resolvedPath);

        auto existingPath = pathToPackage.find(canonicalPath);
        if (existingPath != pathToPackage.end() && existingPath->second != packageName) {
            result.error = "[emit-llvm] one module file is imported under two names: '" +
                           existingPath->second + "' and '" + packageName + "'";
            return false;
        }

        std::ifstream file(resolvedPath, std::ios::binary);
        if (!file) {
            result.error = "[emit-llvm] cannot read module '" + packageName + "' at '" + resolvedPath + "'";
            return false;
        }
        std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        ModuleNode node;
        node.packageName = packageName;
        node.path = canonicalPath;
        node.source = std::move(source);
        node.imports = readImports(node.source);
        try {
            node.functions = readInterfaces(node.source, node.path);
            node.classes = readClassInterfaces(node.source, node.path);
            node.interfaceContracts = readInterfaceContracts(node.source, node.path);
        } catch (const std::exception& ex) {
            result.error = ex.what();
            return false;
        }

        visitState[packageName] = 1;
        importStack.push_back(packageName);
        for (const auto& item : node.imports) {
            if (!visit(item.packageName, node.path)) return false;
        }
        importStack.pop_back();
        visitState[packageName] = 2;

        packageToNode[packageName] = result.dependencyOrder.size();
        pathToPackage[canonicalPath] = packageName;
        result.dependencyOrder.push_back(std::move(node));
        return true;
    }

public:
    ModuleGraphResult build(
        const std::string& rootPath,
        const std::string& rootSource,
        const std::vector<std::string>& implicitPackages
    ) {
        result = ModuleGraphResult{};
        visitState.clear();
        packageToNode.clear();
        pathToPackage.clear();
        importStack.clear();

        std::vector<ZeusDiagnostics::Diagnostic> rootDiagnostics;
        const auto rootTokens = LexerParser::parse(rootSource, &rootDiagnostics);
        ZeusDiagnostics::append(rootDiagnostics, ZeusParser::validateSyntax(rootTokens));
        if (!rootDiagnostics.empty()) {
            result.error = ZeusDiagnostics::render(rootDiagnostics.front(), rootPath, rootSource);
            return std::move(result);
        }
        auto rootAst = TreeParser::parse(rootTokens, &rootDiagnostics);
        ZeusParser::selectBackendBlocks(rootAst, ZeusParser::BackendTarget::LLVM);
        ZeusDiagnostics::append(rootDiagnostics, ZeusParser::validateParsedSyntax(rootAst));
        if (!rootDiagnostics.empty()) {
            result.error = ZeusDiagnostics::render(rootDiagnostics.front(), rootPath, rootSource);
            return std::move(result);
        }
        result.rootImports = readImports(rootSource);

        for (const auto& packageName : implicitPackages) {
            const auto existing = std::find_if(
                result.rootImports.begin(), result.rootImports.end(),
                [&](const ModuleImport& imported) { return imported.packageName == packageName; }
            );
            if (existing == result.rootImports.end()) {
                result.rootImports.push_back(ModuleImport{packageName, {}, false});
            }
        }

        for (const auto& item : result.rootImports) {
            if (!visit(item.packageName, rootPath)) break;
        }

        return std::move(result);
    }
};

} // namespace ModuleGraphDetail

inline ModuleGraphResult buildModuleGraph(
    const std::string& rootPath,
    const std::string& rootSource,
    const std::vector<std::string>& implicitPackages = {}
) {
    return ModuleGraphDetail::Builder{}.build(rootPath, rootSource, implicitPackages);
}

} // namespace ZeusLLVM
