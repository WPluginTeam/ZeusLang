#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../ZeusIR/IRImport.hpp"
#include "../ZeusParser/AstElementView.hpp"
#include "../ZeusParser/LexerParser.hpp"
#include "../ZeusParser/TreeParser.hpp"
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

struct ModuleNode {
    std::string packageName;
    std::string path;
    std::string source;
    std::vector<ModuleImport> imports;
    std::vector<ModuleFunctionInterface> functions;
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

        imports.push_back(std::move(item));
    }

    return imports;
}

inline std::vector<ModuleFunctionInterface> readInterfaces(const std::string& source) {
    const auto tokens = LexerParser::parse(source);
    const auto nodes = TreeParser::parse(tokens);
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

    bool visit(const std::string& packageName, const std::string& importerPath) {
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
        const std::string resolvedPath = IRImport::resolvePackagePath(packageName);
        IRImport::currentWorkingDir = oldWorkingDirectory;
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
        node.functions = readInterfaces(node.source);

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
