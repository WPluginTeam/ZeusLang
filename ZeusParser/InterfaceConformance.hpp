#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "FunctionTypeSyntax.hpp"
#include "SyntaxValidator.hpp"
#include "TreeParser.hpp"
#include "../ZeusDiagnostics.hpp"

namespace ZeusParser {

struct InterfaceMethodContract {
    FunctionSignatureSyntax signature;
    bool isInstanceMethod = false;
    int line = 0;
    int column = 0;
};

struct InterfaceContractRegistry {
    std::unordered_map<std::string, std::vector<InterfaceMethodContract>> contractsByName;
    std::unordered_map<std::string, std::string> bindingOwners;
    std::unordered_set<std::string> ambiguousNames;
};

inline InterfaceContractRegistry collectInterfaceContracts(
    const std::vector<TreeParser::NodeAndBlock>& ast
) {
    InterfaceContractRegistry contractsByName;
    const auto collect = [&](const auto& self,
                             const std::vector<TreeParser::NodeAndBlock>& nodes) -> void {
        for (const auto& element : nodes) {
            if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(element)) {
                continue;
            }
            const auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(element);
            if (!block) continue;
            if (block->type == CodeBlockType::INTERFACE && !block->interfaceName.empty()) {
                auto& contracts = contractsByName.contractsByName[block->interfaceName];
                for (const auto& member : block->nodes) {
                    if (!std::holds_alternative<TreeParser::Node>(member)) continue;
                    const auto& node = std::get<TreeParser::Node>(member);
                    if (node.type != NodeType::INTERFACE_METHOD_DECL_STMT) continue;

                    const auto signature = parseForwardFunctionSignature(node.tokens);
                    if (!signature.valid || signature.name.empty()) continue;
                    InterfaceMethodContract contract;
                    contract.signature = signature;
                    contract.isInstanceMethod = !signature.isStaticMethod &&
                        !block->classDefaultAlias.empty();
                    contract.line = node.sourceLine;
                    contract.column = node.sourceColumn;
                    contracts.push_back(std::move(contract));
                }
            }
            self(self, block->nodes);
        }
    };
    collect(collect, ast);
    return contractsByName;
}

inline void bindImportedInterfaceContracts(
    InterfaceContractRegistry& destination,
    const std::string& packageName,
    const std::string& importAlias,
    bool namespaceMode,
    const InterfaceContractRegistry& exportedContracts
) {
    std::string alias = importAlias;
    if (alias.empty()) {
        alias = packageName;
        const size_t dot = alias.find_last_of('.');
        if (dot != std::string::npos) alias = alias.substr(dot + 1);
    }

    const auto bind = [&](const std::string& name,
                          const std::vector<InterfaceMethodContract>& contracts) {
        const auto owner = destination.bindingOwners.find(name);
        if (owner != destination.bindingOwners.end()) {
            if (owner->second != packageName) {
                destination.ambiguousNames.insert(name);
            }
            return;
        }
        destination.bindingOwners.emplace(name, packageName);
        destination.contractsByName.emplace(name, contracts);
    };

    for (const auto& entry : exportedContracts.contractsByName) {
        bind(packageName + "." + entry.first, entry.second);
        if (namespaceMode) {
            bind(alias + "." + entry.first, entry.second);
        } else {
            bind(entry.first, entry.second);
        }
    }
}

inline std::vector<ZeusDiagnostics::Diagnostic> validateInterfaceConformance(
    const std::vector<TreeParser::NodeAndBlock>& ast,
    const InterfaceContractRegistry& importedInterfaces = {}
) {
    std::vector<ZeusDiagnostics::Diagnostic> diagnostics;
    std::unordered_map<std::string, const TreeParser::CodeBlock*> interfaces;
    std::vector<const TreeParser::CodeBlock*> classes;
    std::unordered_map<std::string, std::vector<const TreeParser::CodeBlock*>> implMethodsByType;

    const auto collect = [&](const auto& self,
                             const std::vector<TreeParser::NodeAndBlock>& nodes) -> void {
        for (const auto& element : nodes) {
            if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(element)) {
                continue;
            }
            const auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(element);
            if (!block) continue;
            if (block->type == CodeBlockType::INTERFACE && !block->interfaceName.empty()) {
                auto inserted = interfaces.emplace(block->interfaceName, block.get());
                if (!inserted.second) {
                    diagnostics.push_back({
                        "interface",
                        "interface '" + block->interfaceName + "' is declared more than once",
                        block->sourceLine,
                        block->sourceColumn
                    });
                }
            } else if (block->type == CodeBlockType::CLASS && !block->className.empty()) {
                classes.push_back(block.get());
            } else if (block->type == CodeBlockType::IMPL && !block->implTypeName.empty()) {
                for (const auto& member : block->nodes) {
                    if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(member)) {
                        continue;
                    }
                    const auto& method =
                        std::get<std::unique_ptr<TreeParser::CodeBlock>>(member);
                    if (method && method->type == CodeBlockType::FUNC) {
                        implMethodsByType[block->implTypeName].push_back(method.get());
                    }
                }
            }
            self(self, block->nodes);
        }
    };
    collect(collect, ast);

    InterfaceContractRegistry contractsByInterface;
    for (const auto& entry : interfaces) {
        const auto* interfaceBlock = entry.second;
        auto& contracts = contractsByInterface.contractsByName[interfaceBlock->interfaceName];
        std::unordered_set<std::string> seenMethods;
        for (const auto& member : interfaceBlock->nodes) {
            if (!std::holds_alternative<TreeParser::Node>(member)) continue;
            const auto& node = std::get<TreeParser::Node>(member);
            if (node.type != NodeType::INTERFACE_METHOD_DECL_STMT) continue;

            const auto signature = parseForwardFunctionSignature(node.tokens);
            if (!signature.valid || signature.name.empty()) continue;
            InterfaceMethodContract contract;
            contract.signature = signature;
            contract.isInstanceMethod = !signature.isStaticMethod &&
                !interfaceBlock->classDefaultAlias.empty();
            contract.line = node.sourceLine;
            contract.column = node.sourceColumn;
            if (!seenMethods.insert(signature.name).second) {
                diagnostics.push_back({
                    "interface",
                    "interface '" + interfaceBlock->interfaceName +
                        "' declares method '" + signature.name + "' more than once",
                    node.sourceLine,
                    node.sourceColumn
                });
            } else {
                contracts.push_back(std::move(contract));
            }
        }
    }

    for (const auto& imported : importedInterfaces.contractsByName) {
        if (interfaces.find(imported.first) == interfaces.end()) {
            contractsByInterface.contractsByName.emplace(imported.first, imported.second);
        }
    }

    const auto canonicalType = [](const std::string& typeName) {
        if (typeName.empty()) return std::string();
        if (typeName.front() == '&') {
            return std::string("&") + canonicalSyntaxTypeName(typeName.substr(1));
        }
        return canonicalSyntaxTypeName(typeName);
    };
    const auto isInstanceMethod = [](const TreeParser::CodeBlock& method) {
        return !method.inferredInstanceAlias.empty() &&
            !method.isStaticMethod && !method.isCompileTimeStatic;
    };
    const auto methodMatches = [&](const InterfaceMethodContract& contract,
                                   const TreeParser::CodeBlock& method) {
        const bool methodIsInstance = isInstanceMethod(method);
        if (contract.isInstanceMethod != methodIsInstance) return false;
        if (contract.signature.hasVariadicArgs != !method.variadicArgs.empty() ||
            contract.signature.hasVariadicKwargs != !method.variadicKwargs.empty()) {
            return false;
        }

        const size_t receiverOffset = methodIsInstance ? 1 : 0;
        if (method.paramNames.size() < receiverOffset ||
            method.paramNames.size() - receiverOffset !=
                contract.signature.parameters.size()) {
            return false;
        }

        for (size_t i = 0; i < contract.signature.parameters.size(); ++i) {
            const size_t methodIndex = i + receiverOffset;
            const std::string methodType = methodIndex < method.paramTypeNames.size()
                ? method.paramTypeNames[methodIndex] : std::string();
            const bool methodIsReference =
                methodIndex < method.paramIsReference.size() &&
                method.paramIsReference[methodIndex];
            const auto& expected = contract.signature.parameters[i];
            if (canonicalType(methodType) != canonicalType(expected.typeName) ||
                methodIsReference != expected.isReference) {
                return false;
            }
        }
        return canonicalType(method.returnTypeName) ==
            canonicalType(contract.signature.returnTypeName);
    };

    for (const auto* classBlock : classes) {
        std::unordered_set<std::string> seenInterfaces;
        for (const auto& interfaceName : classBlock->implementedInterfaces) {
            if (!seenInterfaces.insert(interfaceName).second) {
                diagnostics.push_back({
                    "interface",
                    "class '" + classBlock->className + "' lists interface '" +
                        interfaceName + "' more than once",
                    classBlock->sourceLine,
                    classBlock->sourceColumn
                });
                continue;
            }

            if (interfaces.find(interfaceName) == interfaces.end() &&
                importedInterfaces.ambiguousNames.find(interfaceName) !=
                    importedInterfaces.ambiguousNames.end()) {
                diagnostics.push_back({
                    "interface",
                    "class '" + classBlock->className +
                        "' implements ambiguous imported interface '" + interfaceName + "'",
                    classBlock->sourceLine,
                    classBlock->sourceColumn
                });
                continue;
            }

            const auto contractsIt = contractsByInterface.contractsByName.find(interfaceName);
            if (contractsIt == contractsByInterface.contractsByName.end()) {
                diagnostics.push_back({
                    "interface",
                    "class '" + classBlock->className + "' implements unknown interface '" +
                        interfaceName + "'",
                    classBlock->sourceLine,
                    classBlock->sourceColumn
                });
                continue;
            }

            for (const auto& contract : contractsIt->second) {
                bool hasMethodName = false;
                bool hasMatchingMethod = false;
                const TreeParser::CodeBlock* incompatibleMethod = nullptr;
                const auto considerMethod = [&](const TreeParser::CodeBlock* method) {
                    if (method == nullptr || method->type != CodeBlockType::FUNC ||
                        method->funcName != contract.signature.name) return;
                    hasMethodName = true;
                    if (incompatibleMethod == nullptr) incompatibleMethod = method;
                    if (methodMatches(contract, *method)) {
                        hasMatchingMethod = true;
                    }
                };
                const TreeParser::CodeBlock* finalClassMethod = nullptr;
                const TreeParser::CodeBlock* finalNamedClassMethod = nullptr;
                for (const auto& member : classBlock->nodes) {
                    if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(member)) {
                        continue;
                    }
                    const auto* method =
                        std::get<std::unique_ptr<TreeParser::CodeBlock>>(member).get();
                    if (method == nullptr || method->type != CodeBlockType::FUNC ||
                        method->funcName != contract.signature.name) {
                        continue;
                    }
                    hasMethodName = true;
                    finalNamedClassMethod = method;
                    if (isInstanceMethod(*method) == contract.isInstanceMethod) {
                        // Class method maps are keyed by name within the static/instance
                        // table, and later declarations overwrite earlier ones.
                        finalClassMethod = method;
                    }
                }
                if (finalClassMethod != nullptr) {
                    incompatibleMethod = finalClassMethod;
                    hasMatchingMethod = methodMatches(contract, *finalClassMethod);
                } else if (finalNamedClassMethod != nullptr) {
                    incompatibleMethod = finalNamedClassMethod;
                }
                // A same-category class method occupies the same runtime method
                // slot as an impl method, so an impl candidate cannot rescue an
                // incompatible effective class declaration.
                if (!hasMatchingMethod && finalClassMethod == nullptr) {
                    const auto implMethods = implMethodsByType.find(classBlock->className);
                    if (implMethods != implMethodsByType.end()) {
                        for (const auto* method : implMethods->second) {
                            considerMethod(method);
                            if (hasMatchingMethod) break;
                        }
                    }
                }

                if (hasMatchingMethod) continue;
                const std::string message = !hasMethodName
                    ? "class '" + classBlock->className + "' is missing method '" +
                        contract.signature.name + "' required by interface '" + interfaceName + "'"
                    : "class '" + classBlock->className + "' has an incompatible declaration for method '" +
                        contract.signature.name + "' required by interface '" + interfaceName + "'";
                diagnostics.push_back({
                    "interface",
                    message,
                    incompatibleMethod != nullptr
                        ? incompatibleMethod->sourceLine : classBlock->sourceLine,
                    incompatibleMethod != nullptr
                        ? incompatibleMethod->sourceColumn : classBlock->sourceColumn
                });
            }
        }
    }

    return diagnostics;
}

} // namespace ZeusParser
