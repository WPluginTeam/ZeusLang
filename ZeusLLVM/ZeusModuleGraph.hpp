#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include "../ZeusParser/NativeTypeDeclarationResolver.hpp"
#include <system_error>
#include <utility>
#include <vector>

#include "../ZeusIR/IRImport.hpp"
#include "../ZeusParser/AstElementView.hpp"
#include "../ZeusParser/LexerParser.hpp"
#include "../ZeusParser/SyntaxValidator.hpp"
#include "../ZeusParser/TreeParser.hpp"
#include "../ZeusParser/BackendBlockSelector.hpp"
#include "../ZeusParser/PlatformBlockSelector.hpp"
#include "../ZeusParser/InterfaceConformance.hpp"
#include "../ZeusBuiltins/BuiltinRegistry.hpp"
#include "../ZeusParser/CHeaderScanner.hpp"

namespace ZeusLLVM {

struct ModuleImport {
    std::string packageName;
    std::string alias;
    bool namespaceMode = false;
    std::string nativeHeaderPath;
};

struct ModuleFunctionInterface {
    std::string name;
    std::string returnType;
    std::string ownerType;
    std::vector<std::string> parameterNames;
    std::vector<std::string> parameterTypes;
    std::vector<std::string> typeParameters;
    std::vector<std::string> ownerTypeParameters;
    bool isClassMethod = false;
    bool isStaticMethod = false;
};

struct ModuleClassInterface {
    std::string name;
    std::vector<std::pair<std::string, std::string>> fields;
};

struct ModuleNode {
    std::string packageName;
    std::string path;
    std::string source;
    bool hasTopLevelSideEffects = false;
    std::string topLevelSideEffectReason;
    bool hasExplicitExportList = false;
    std::vector<std::string> exportNames;
    std::vector<ModuleImport> imports;
    std::vector<ModuleFunctionInterface> functions;
    std::vector<ModuleClassInterface> classes;
    ZeusParser::InterfaceContractRegistry interfaceContracts;

    bool isExported(const std::string& name) const {
        return !hasExplicitExportList ||
            std::find(exportNames.begin(), exportNames.end(), name) != exportNames.end();
    }
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
        if (i >= 5 &&
            operatorText(tokens[i - 5]) == "@" &&
            isKeyword(tokens[i - 4], "native") &&
            operatorText(tokens[i - 3]) == "(" &&
            std::holds_alternative<LexerType::Value>(tokens[i - 2]) &&
            operatorText(tokens[i - 1]) == ")") {
            const auto& val = std::get<LexerType::Value>(tokens[i - 2]);
            if (std::holds_alternative<std::string>(val.data)) {
                item.nativeHeaderPath = std::get<std::string>(val.data);
            }
        }
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

inline std::pair<bool, std::vector<std::string>> readExportList(
    const std::string& source
) {
    const auto tokens = LexerParser::parse(source);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (!isKeyword(tokens[i], "using") || operatorText(tokens[i + 1]) != "{") continue;
        const size_t close = ZeusParser::matchingSyntaxDelimiter(tokens, i + 1);
        if (close == tokens.size()) return {true, {}};

        std::vector<std::string> names;
        for (size_t j = i + 2; j < close; ++j) {
            if (std::holds_alternative<LexerType::Keyword>(tokens[j])) {
                names.push_back(std::get<LexerType::Keyword>(tokens[j]).name);
            }
        }
        return {true, std::move(names)};
    }
    return {false, {}};
}

inline std::vector<ModuleFunctionInterface> readInterfaces(
    const std::string& source,
    const std::string& sourcePath = {}
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    auto tokens = ZeusParser::parseNativeTypeSource(source, sourcePath, diagnostics);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateSyntax(tokens));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(diagnostics.front(), sourcePath, source));
    }
    auto nodes = TreeParser::parse(tokens, &diagnostics);
    ZeusParser::selectPlatformBlocks(nodes);
    ZeusParser::selectBackendBlocks(nodes, ZeusParser::BackendTarget::LLVM);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateParsedSyntax(nodes));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(diagnostics.front(), sourcePath, source));
    }
    std::vector<ModuleFunctionInterface> functions;

    const auto collectFunctions = [&](const auto& self,
                                      const std::vector<TreeParser::NodeAndBlock>& elements) -> void {
        for (const auto& node : elements) {
            if (std::holds_alternative<TreeParser::Node>(node)) {
                const auto& nd = std::get<TreeParser::Node>(node);
                if (nd.type == NodeType::FUNC_FWD_DECL_STMT) {
                    const auto sig = ZeusParser::parseForwardFunctionSignature(nd.tokens);
                    if (sig.valid && !sig.name.empty()) {
                        ModuleFunctionInterface function;
                        function.name = sig.name;
                        function.returnType = sig.returnTypeName;
                        for (const auto& p : sig.parameters) {
                            function.parameterNames.push_back(p.name);
                            function.parameterTypes.push_back(p.typeName);
                        }
                        function.isStaticMethod = true;
                        functions.push_back(std::move(function));
                    }
                }
                continue;
            }
            TreeParser::AstElementView element(node);
            if (!element.isBlock()) continue;
            const auto* block = element.block();
            if (block == nullptr) continue;

            if (block->type == CodeBlockType::FUNC) {
                if (!block->funcName.empty()) {
                    ModuleFunctionInterface function;
                    function.name = block->funcName;
                    function.returnType = block->returnTypeName;
                    function.ownerType = block->ownerClassName;
                    if (const auto* builtin =
                            ZeusBuiltins::findTypeImplementation(function.ownerType)) {
                        function.ownerType = std::string(builtin->canonicalName);
                    }
                    function.parameterNames = block->paramNames;
                    function.parameterTypes = block->paramTypeNames;
                    function.typeParameters = block->typeParameters;
                    function.ownerTypeParameters = block->classTypeParameters;
                    function.isClassMethod = block->isClassMethod;
                    function.isStaticMethod =
                        block->isStaticMethod || block->isCompileTimeStatic;
                    if (function.isClassMethod && function.returnType.empty() &&
                        !TreeParser::hasExplicitReturnStatement(block->nodes)) {
                        function.returnType = "nil";
                    }
                    functions.push_back(std::move(function));
                }
                continue;
            }

            if (block->type == CodeBlockType::BLOCK ||
                block->type == CodeBlockType::CLASS ||
                block->type == CodeBlockType::IMPL) {
                self(self, block->nodes);
            }
        }
    };
    collectFunctions(collectFunctions, nodes);

    return functions;
}

inline std::vector<ModuleClassInterface> readClassInterfaces(
    const std::string& source,
    const std::string& sourcePath = {}
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    auto tokens = ZeusParser::parseNativeTypeSource(source, sourcePath, diagnostics);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateSyntax(tokens));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(diagnostics.front(), sourcePath, source));
    }
    auto nodes = TreeParser::parse(tokens, &diagnostics);
    ZeusParser::selectPlatformBlocks(nodes);
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
    auto tokens = ZeusParser::parseNativeTypeSource(source, sourcePath, diagnostics);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateSyntax(tokens));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(
            diagnostics.front(), sourcePath, source));
    }
    auto nodes = TreeParser::parse(tokens, &diagnostics);
    ZeusParser::selectPlatformBlocks(nodes);
    ZeusParser::selectBackendBlocks(nodes, ZeusParser::BackendTarget::LLVM);
    ZeusDiagnostics::append(diagnostics, ZeusParser::validateParsedSyntax(nodes));
    if (!diagnostics.empty()) {
        throw std::runtime_error(ZeusDiagnostics::render(
            diagnostics.front(), sourcePath, source));
    }
    return ZeusParser::collectInterfaceContracts(nodes);
}

inline bool initializerTokensMayHaveSideEffects(
    const std::vector<LexerType::Token>& tokens
) {
    size_t initializerStart = tokens.size();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (operatorText(tokens[i]) == "=") {
            initializerStart = i + 1;
            break;
        }
    }
    if (initializerStart == tokens.size()) return false;

    for (size_t i = initializerStart; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        if (std::holds_alternative<LexerType::Value>(token)) continue;
        if (isKeyword(token, "true") || isKeyword(token, "false") ||
            isKeyword(token, "nil") || isKeyword(token, "null")) {
            continue;
        }
        const std::string op = operatorText(token);
        if (op == "[" || op == "]" || op == "{" || op == "}" ||
            op == "," || op == ":") {
            continue;
        }
        return true;
    }
    return false;
}

inline bool hasTopLevelSideEffects(
    const std::string& source,
    const std::string& sourcePath,
    std::string* reason = nullptr
) {
    auto markEffect = [&](const std::string& value) {
        if (reason != nullptr) *reason = value;
        return true;
    };
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    const auto tokens = ZeusParser::parseNativeTypeSource(
        source, sourcePath, diagnostics);
    if (!diagnostics.empty()) return markEffect("source parse diagnostics");

    auto nodes = TreeParser::parse(tokens, &diagnostics);
    if (!diagnostics.empty()) return markEffect("tree parse diagnostics");
    ZeusParser::selectPlatformBlocks(nodes);
    ZeusParser::selectBackendBlocks(nodes, ZeusParser::BackendTarget::LLVM);

    const std::vector<TreeParser::NodeAndBlock>* topLevel = &nodes;
    if (nodes.size() == 1 &&
        std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(nodes.front())) {
        const auto& root = std::get<std::unique_ptr<TreeParser::CodeBlock>>(nodes.front());
        if (root && root->type == CodeBlockType::BLOCK) {
            topLevel = &root->nodes;
        }
    }

    for (const auto& element : *topLevel) {
        if (std::holds_alternative<TreeParser::Node>(element)) {
            const auto& statement = std::get<TreeParser::Node>(element);
            switch (statement.type) {
                case NodeType::IMPORT_STMT:
                case NodeType::USING_STMT:
                case NodeType::USING_EXPORTS_STMT:
                case NodeType::FUNC_FWD_DECL_STMT:
                case NodeType::FUNC_DECL_STMT:
                case NodeType::CLASS_DECL_STMT:
                case NodeType::INTERFACE_DECL_STMT:
                case NodeType::IMPL_DECL_STMT:
                case NodeType::INTERFACE_METHOD_DECL_STMT:
                    break;
                case NodeType::VAR_DECL_STMT:
                    if (!initializerTokensMayHaveSideEffects(statement.tokens)) break;
                    return markEffect("top-level variable initializer");
                default:
                    return markEffect(std::string("top-level statement ") +
                        Debug::toString(statement.type));
            }
            continue;
        }

        const auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(element);
        if (!block) continue;
        switch (block->type) {
            case CodeBlockType::FUNC:
            case CodeBlockType::CLASS:
            case CodeBlockType::INTERFACE:
            case CodeBlockType::IMPL:
                break;
            default:
                return markEffect(std::string("top-level block ") +
                    Debug::toString(block->type));
        }
    }
    return false;
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

    bool visit(std::string packageName, const std::string& importerPath, const std::string& nativeHeaderPath = "") {
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

        std::string source;
        std::string canonicalPath;
        if (!nativeHeaderPath.empty()) {
            std::filesystem::path hPath(nativeHeaderPath);
            if (hPath.is_relative()) {
                std::filesystem::path base = std::filesystem::path(importerPath).parent_path();
                hPath = base / hPath;
            }
            std::error_code ec;
            std::filesystem::path resolvedH = hPath.lexically_normal();
            if (!std::filesystem::exists(resolvedH)) {
                resolvedH = std::filesystem::absolute(hPath, ec);
            }
            if (!std::filesystem::exists(resolvedH)) {
                result.error = "[emit-llvm] @native header file not found: '" + nativeHeaderPath + "' (imported from '" + importerPath + "')";
                return false;
            }
            canonicalPath = normalizedPath(resolvedH.string());
            std::vector<ZeusParser::CFunctionDeclaration> decls;
            std::string scanError;
            if (!ZeusParser::CHeaderScanner::scanHeader(canonicalPath, decls, scanError)) {
                result.error = "[emit-llvm] " + scanError;
                return false;
            }
            source = ZeusParser::CHeaderScanner::generateZeusDeclarations(decls);
        } else {
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
        source.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        ModuleNode node;
        node.packageName = packageName;
        node.path = canonicalPath;
        node.source = std::move(source);
        node.hasTopLevelSideEffects =
            ModuleGraphDetail::hasTopLevelSideEffects(
                node.source, node.path, &node.topLevelSideEffectReason);
        const auto exports = ModuleGraphDetail::readExportList(node.source);
        node.hasExplicitExportList = exports.first;
        node.exportNames = exports.second;
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
            if (!visit(item.packageName, node.path, item.nativeHeaderPath)) return false;
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
        const auto rootTokens = ZeusParser::parseNativeTypeSource(rootSource, rootPath, rootDiagnostics);
        ZeusDiagnostics::append(rootDiagnostics, ZeusParser::validateSyntax(rootTokens));
        if (!rootDiagnostics.empty()) {
            result.error = ZeusDiagnostics::render(rootDiagnostics.front(), rootPath, rootSource);
            return std::move(result);
        }
        auto rootAst = TreeParser::parse(rootTokens, &rootDiagnostics);
        ZeusParser::selectPlatformBlocks(rootAst);
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
            if (!visit(item.packageName, rootPath, item.nativeHeaderPath)) break;
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
