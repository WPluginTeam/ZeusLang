#pragma once

#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "../ZeusIR/IRImport.hpp"
#include "../ZeusIR/IRScope.hpp"
#include "../ZeusIR/IRType.hpp"
#include "../ZeusPackage/ZeusPackageLoader.hpp"
#include "ZeusModuleGraph.hpp"

namespace ZeusLLVM {

namespace ModulePrunerDetail {

inline int tokenLine(const LexerType::Token& token) {
    return std::visit([](const auto& value) { return value.line; }, token);
}

inline std::set<std::string> collectModuleIdentifiers(const std::string& source) {
    const auto tokens = LexerParser::parse(source);
    std::vector<bool> importDeclaration(tokens.size(), false);

    for (size_t i = 0; i < tokens.size(); ++i) {
        if (!ModuleGraphDetail::isKeyword(tokens[i], "import")) continue;

        const int importLine = tokenLine(tokens[i]);
        size_t end = i + 1;
        while (end < tokens.size()) {
            const std::string op = ModuleGraphDetail::operatorText(tokens[end]);
            if (op == "[") {
                int depth = 0;
                do {
                    const std::string nestedOp =
                        ModuleGraphDetail::operatorText(tokens[end]);
                    if (nestedOp == "[") ++depth;
                    else if (nestedOp == "]") --depth;
                    ++end;
                } while (end < tokens.size() && depth > 0);
                break;
            }
            if (op == ";") {
                ++end;
                break;
            }
            if (tokenLine(tokens[end]) != importLine) break;
            if (op == "." ||
                std::holds_alternative<LexerType::Keyword>(tokens[end]) ||
                std::holds_alternative<LexerType::Value>(tokens[end])) {
                ++end;
                continue;
            }
            break;
        }

        for (size_t token = i; token < end; ++token) {
            importDeclaration[token] = true;
        }
        if (end > i) i = end - 1;
    }

    std::set<std::string> identifiers;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (importDeclaration[i]) continue;
        if (std::holds_alternative<LexerType::Keyword>(tokens[i])) {
            identifiers.insert(std::get<LexerType::Keyword>(tokens[i]).name);
        } else if (std::holds_alternative<LexerType::Value>(tokens[i])) {
            const auto& value = std::get<LexerType::Value>(tokens[i]);
            if (std::holds_alternative<std::string>(value.data)) {
                identifiers.insert(std::get<std::string>(value.data));
            }
        }
    }
    return identifiers;
}

inline std::string importAlias(const ModuleImport& imported) {
    if (!imported.alias.empty()) return imported.alias;
    const size_t dot = imported.packageName.find_last_of('.');
    return dot == std::string::npos
        ? imported.packageName
        : imported.packageName.substr(dot + 1);
}

inline bool importReferencesPackage(
    const ModuleImport& imported,
    const ZeusPackage::CompiledPackage& dependency,
    const std::set<std::string>& identifiers
) {
    if (imported.namespaceMode &&
        identifiers.find(importAlias(imported)) != identifiers.end()) {
        return true;
    }
    for (const auto& globalName : dependency.globalNames) {
        if (dependency.isExported(globalName) &&
            identifiers.find(globalName) != identifiers.end()) {
            return true;
        }
    }
    return false;
}

} // namespace ModulePrunerDetail

struct ModulePruneResult {
    size_t packageCount = 0;
    size_t prunedModuleBodies = 0;
    size_t prunedNamespaceImports = 0;
    std::vector<std::string> prunedModuleNames;
    std::set<std::string> symbolRoots;
    std::set<std::string> sideEffectRoots;
    std::set<std::string> implicitRoots;
    std::vector<std::string> retainedImportEdges;
    std::vector<std::string> prunedImportEdges;
};

inline ModulePruneResult pruneUnusedModuleBodies(
    std::vector<std::vector<IRType::IRInstr>>& program,
    const ModuleGraphResult& moduleGraph,
    const std::vector<std::string>& implicitModules,
    const std::vector<ZeusPackage::CompiledPackage*>& packages,
    const std::map<std::string, int>& packageGlobalOffsets
) {
    ModulePruneResult result;
    result.packageCount = packages.size();

    std::map<std::string, const ModuleNode*> moduleByName;
    for (const auto& module : moduleGraph.dependencyOrder) {
        moduleByName[module.packageName] = &module;
    }

    std::map<std::string, ZeusPackage::CompiledPackage*> packageByName;
    for (auto* package : packages) {
        if (package != nullptr) packageByName[package->packageName] = package;
    }

    std::map<std::string, bool> moduleHasInitializationEffects;
    for (const auto& module : moduleGraph.dependencyOrder) {
        moduleHasInitializationEffects[module.packageName] =
            module.hasTopLevelSideEffects;
    }
    for (size_t pass = 0; pass < moduleGraph.dependencyOrder.size(); ++pass) {
        bool changed = false;
        for (const auto& module : moduleGraph.dependencyOrder) {
            if (moduleHasInitializationEffects[module.packageName]) continue;
            for (const auto& imported : module.imports) {
                const auto dependencyEffects =
                    moduleHasInitializationEffects.find(imported.packageName);
                if (dependencyEffects == moduleHasInitializationEffects.end() ||
                    !dependencyEffects->second) {
                    continue;
                }
                moduleHasInitializationEffects[module.packageName] = true;
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }

    std::map<std::string, std::set<std::string>> identifiersByModule;
    for (const auto& module : moduleGraph.dependencyOrder) {
        identifiersByModule[module.packageName] =
            ModulePrunerDetail::collectModuleIdentifiers(module.source);
    }

    std::set<std::tuple<std::string, std::string, std::string>>
        usedNamespaceAliases;
    for (const auto& module : moduleGraph.dependencyOrder) {
        const auto identifiers = identifiersByModule.find(module.packageName);
        if (identifiers == identifiersByModule.end()) continue;
        for (const auto& imported : module.imports) {
            if (!imported.namespaceMode ||
                identifiers->second.find(
                    ModulePrunerDetail::importAlias(imported)) ==
                    identifiers->second.end()) {
                continue;
            }
            usedNamespaceAliases.emplace(
                module.packageName,
                imported.packageName,
                ModulePrunerDetail::importAlias(imported));
        }
    }

    std::set<std::string> retainedModules;
    std::vector<std::string> modulesToVisit;
    std::set<std::string> fullClosureModules;
    std::set<std::string> rootImportNames;
    for (const auto& imported : moduleGraph.rootImports) {
        rootImportNames.insert(imported.packageName);
    }

    auto retainModule = [&](const std::string& packageName) {
        if (moduleByName.find(packageName) == moduleByName.end()) return;
        if (retainedModules.insert(packageName).second) {
            modulesToVisit.push_back(packageName);
        }
    };

    for (const auto& packageName : implicitModules) {
        result.implicitRoots.insert(packageName);
        retainModule(packageName);
        if (moduleByName.find(packageName) != moduleByName.end()) {
            fullClosureModules.insert(packageName);
        }
    }
    for (size_t cursor = 0; cursor < modulesToVisit.size(); ++cursor) {
        const auto module = moduleByName.find(modulesToVisit[cursor]);
        if (module == moduleByName.end() ||
            fullClosureModules.find(module->first) == fullClosureModules.end()) {
            continue;
        }
        for (const auto& imported : module->second->imports) {
            if (moduleByName.find(imported.packageName) == moduleByName.end()) continue;
            if (fullClosureModules.insert(imported.packageName).second) {
                retainModule(imported.packageName);
            }
        }
    }

    std::map<int, std::string> namespaceAliasPackages;
    for (const auto& imported : moduleGraph.rootImports) {
        if (!imported.namespaceMode) continue;
        std::string alias = imported.alias;
        if (alias.empty()) {
            alias = imported.packageName;
            const size_t dot = alias.find_last_of('.');
            if (dot != std::string::npos) alias = alias.substr(dot + 1);
        }
        const auto slot = IRScope::globals.find(alias);
        if (slot != IRScope::globals.end()) {
            namespaceAliasPackages[slot->second] = imported.packageName;
        }
    }

    auto readIntegerArgument = [](
        const IRType::IRInstr& instruction,
        size_t argumentIndex,
        int64_t& value
    ) {
        if (argumentIndex >= instruction.args.size() ||
            !std::holds_alternative<LexerType::Value>(instruction.args[argumentIndex])) {
            return false;
        }
        const auto& literal = std::get<LexerType::Value>(instruction.args[argumentIndex]);
        return std::holds_alternative<ZeusType::BigInteger>(literal.data) &&
            std::get<ZeusType::BigInteger>(literal.data).tryToInt64(value);
    };

    auto retainPackageForSymbol = [&](const std::string& symbol) {
        if (symbol.empty()) return;
        for (const auto& module : moduleGraph.dependencyOrder) {
            const std::string prefix = module.packageName + ".";
            if (symbol.rfind(prefix, 0) == 0) {
                retainModule(module.packageName);
                continue;
            }
            for (const auto& classInfo : module.classes) {
                if (symbol == classInfo.name) {
                    retainModule(module.packageName);
                    break;
                }
            }
        }
    };

    auto retainPackageForGlobal = [&](int64_t globalIndex) {
        if (globalIndex < 0 || globalIndex > std::numeric_limits<int>::max()) return;
        const auto alias = namespaceAliasPackages.find(static_cast<int>(globalIndex));
        if (alias != namespaceAliasPackages.end()) {
            result.symbolRoots.insert(alias->second);
            retainModule(alias->second);
        }
        for (auto* package : packages) {
            if (package == nullptr) continue;
            const auto offset = packageGlobalOffsets.find(package->packageName);
            if (offset == packageGlobalOffsets.end()) continue;
            const int64_t first = offset->second;
            const int64_t last = first + static_cast<int64_t>(package->globalNames.size());
            if (globalIndex >= first && globalIndex < last) {
                result.symbolRoots.insert(package->packageName);
                retainModule(package->packageName);
                break;
            }
        }
    };

    for (const auto& module : moduleGraph.dependencyOrder) {
        const auto initializationEffects =
            moduleHasInitializationEffects.find(module.packageName);
        if (initializationEffects != moduleHasInitializationEffects.end() &&
            initializationEffects->second &&
            rootImportNames.find(module.packageName) != rootImportNames.end()) {
            result.sideEffectRoots.insert(module.packageName);
            retainModule(module.packageName);
        }
    }

    for (const auto& chunk : program) {
        for (const auto& instruction : chunk) {
            if (instruction.op == VMOperator::LOAD_VALUE &&
                !instruction.args.empty() &&
                std::holds_alternative<std::string>(instruction.args[0]) &&
                std::get<std::string>(instruction.args[0]) == "global") {
                int64_t globalIndex = -1;
                if (readIntegerArgument(instruction, 1, globalIndex)) {
                    retainPackageForGlobal(globalIndex);
                }
            } else if (instruction.op == VMOperator::STORE_VALUE_GLOBAL ||
                       instruction.op == VMOperator::ASSIGN_VALUE_GLOBAL) {
                int64_t globalIndex = -1;
                if (readIntegerArgument(instruction, 0, globalIndex)) {
                    retainPackageForGlobal(globalIndex);
                }
            }

            for (size_t i = 0; i + 2 < instruction.args.size(); ++i) {
                if (!std::holds_alternative<std::string>(instruction.args[i]) ||
                    std::get<std::string>(instruction.args[i]) !=
                        "__zeus_receiver_source" ||
                    !std::holds_alternative<std::string>(instruction.args[i + 1]) ||
                    std::get<std::string>(instruction.args[i + 1]) != "global") {
                    continue;
                }
                int64_t globalIndex = -1;
                if (readIntegerArgument(instruction, i + 2, globalIndex)) {
                    retainPackageForGlobal(globalIndex);
                }
            }

            std::string directTarget;
            if (instruction.metadata.readDirectCall(directTarget)) {
                for (const auto& module : moduleGraph.dependencyOrder) {
                    if (directTarget.rfind(module.packageName + ".", 0) == 0) {
                        result.symbolRoots.insert(module.packageName);
                    }
                }
                retainPackageForSymbol(directTarget);
            }
            if (!instruction.metadata.symbol.name.empty()) {
                for (const auto& module : moduleGraph.dependencyOrder) {
                    if (instruction.metadata.symbol.name.rfind(
                            module.packageName + ".", 0) == 0) {
                        result.symbolRoots.insert(module.packageName);
                    }
                }
            }
            retainPackageForSymbol(instruction.metadata.symbol.name);

            std::string className;
            std::vector<std::string> typeArguments;
            std::string specializedClassName;
            if (instruction.metadata.readClassInstantiation(
                    className, typeArguments, &specializedClassName)) {
                const std::string& instantiatedClass = specializedClassName.empty()
                    ? className : specializedClassName;
                for (const auto& module : moduleGraph.dependencyOrder) {
                    const std::string prefix = module.packageName + ".";
                    if (instantiatedClass.rfind(prefix, 0) == 0) {
                        result.symbolRoots.insert(module.packageName);
                    }
                }
                retainPackageForSymbol(instantiatedClass);
            }
        }
    }

    while (!modulesToVisit.empty()) {
        const std::string packageName = std::move(modulesToVisit.back());
        modulesToVisit.pop_back();
        const auto module = moduleByName.find(packageName);
        if (module == moduleByName.end()) continue;

        const auto identifiers = identifiersByModule.find(packageName);
        for (const auto& imported : module->second->imports) {
            const auto dependencyPackage = packageByName.find(imported.packageName);
            if (dependencyPackage == packageByName.end()) continue;
            const bool referenced = identifiers != identifiersByModule.end() &&
                ModulePrunerDetail::importReferencesPackage(
                    imported, *dependencyPackage->second, identifiers->second);
            const auto dependencyEffects =
                moduleHasInitializationEffects.find(imported.packageName);
            const bool hasInitializationEffects =
                dependencyEffects != moduleHasInitializationEffects.end() &&
                dependencyEffects->second;
            const bool requiredByImplicitModule =
                fullClosureModules.find(packageName) != fullClosureModules.end();
            if (referenced || hasInitializationEffects || requiredByImplicitModule) {
                std::string reasons;
                if (referenced) reasons = "symbol";
                if (hasInitializationEffects) {
                    if (!reasons.empty()) reasons += ",";
                    reasons += "initialization";
                }
                if (requiredByImplicitModule) {
                    if (!reasons.empty()) reasons += ",";
                    reasons += "implicit";
                }
                result.retainedImportEdges.push_back(
                    packageName + " -> " + imported.packageName +
                    " [" + reasons + "]");
                retainModule(imported.packageName);
            } else {
                result.prunedImportEdges.push_back(
                    packageName + " -> " + imported.packageName);
            }
        }
    }

    for (auto* package : packages) {
        if (package == nullptr ||
            fullClosureModules.find(package->packageName) != fullClosureModules.end()) {
            continue;
        }
        for (auto& chunk : package->chunks) {
            if (chunk.empty()) continue;
            ZeusPackage::ImportInfo imported;
            if (!ZeusPackage::readImportInfo(chunk.front(), imported) ||
                imported.importMode != 1) {
                continue;
            }
            const auto aliasKey = std::make_tuple(
                package->packageName,
                imported.packageName,
                ZeusPackage::defaultImportAlias(imported));
            if (usedNamespaceAliases.find(aliasKey) != usedNamespaceAliases.end()) {
                continue;
            }
            auto* modeValue = std::get_if<LexerType::Value>(&chunk.front().args[2]);
            if (modeValue == nullptr ||
                !std::holds_alternative<ZeusType::BigInteger>(modeValue->data)) {
                continue;
            }
            modeValue->data = ZeusType::BigInteger(0);
            ++result.prunedNamespaceImports;
        }
    }

    for (auto* package : packages) {
        if (package == nullptr || retainedModules.find(package->packageName) !=
                retainedModules.end()) {
            continue;
        }
        const auto module = moduleByName.find(package->packageName);
        if (module == moduleByName.end()) continue;
        package->chunks.clear();
        ++result.prunedModuleBodies;
        result.prunedModuleNames.push_back(package->packageName);
    }

    for (auto& chunk : program) {
        if (chunk.empty() || chunk.front().op != VMOperator::IMPORT ||
            chunk.front().args.size() < 3 ||
            !std::holds_alternative<LexerType::Value>(chunk.front().args[0]) ||
            !std::holds_alternative<std::string>(
                std::get<LexerType::Value>(chunk.front().args[0]).data)) {
            continue;
        }
        const std::string packageName = IRImport::canonicalPackageName(
            std::get<std::string>(
                std::get<LexerType::Value>(chunk.front().args[0]).data));
        if (retainedModules.find(packageName) != retainedModules.end()) continue;
        auto* modeValue = std::get_if<LexerType::Value>(&chunk.front().args[2]);
        if (modeValue == nullptr ||
            !std::holds_alternative<ZeusType::BigInteger>(modeValue->data)) {
            continue;
        }
        const int64_t importMode = std::get<ZeusType::BigInteger>(modeValue->data).toInt64();
        if (importMode != 1) continue;
        modeValue->data = ZeusType::BigInteger(0);
        ++result.prunedNamespaceImports;
    }

    return result;
}

inline void reportModulePruning(
    const ModulePruneResult& result,
    const ModuleGraphResult& moduleGraph
) {
    std::cout << "[emit-llvm] import index retained "
              << (result.packageCount - result.prunedModuleBodies) << "/"
              << result.packageCount << " module bodies; removed "
              << result.prunedModuleBodies << " unused bodies and "
              << result.prunedNamespaceImports
              << " namespace initializers; symbol roots="
              << result.symbolRoots.size() << ", side-effect roots="
              << result.sideEffectRoots.size() << ", implicit roots="
              << result.implicitRoots.size() << std::endl;
    if (!result.prunedModuleNames.empty()) {
        std::cout << "[emit-llvm] pruned modules:";
        for (const auto& name : result.prunedModuleNames) std::cout << " " << name;
        std::cout << std::endl;
    }
    if (!result.prunedImportEdges.empty()) {
        std::cout << "[emit-llvm] pruned unused import edges "
                  << result.prunedImportEdges.size() << ":";
        for (const auto& edge : result.prunedImportEdges) {
            std::cout << " " << edge;
        }
        std::cout << std::endl;
    }
    if (!result.retainedImportEdges.empty()) {
        std::cout << "[emit-llvm] retained import edges "
                  << result.retainedImportEdges.size() << ":";
        for (const auto& edge : result.retainedImportEdges) {
            std::cout << " " << edge;
        }
        std::cout << std::endl;
    }
    std::cout << "[emit-llvm] symbol roots:";
    for (const auto& name : result.symbolRoots) std::cout << " " << name;
    std::cout << "\n[emit-llvm] side-effect roots:";
    for (const auto& module : moduleGraph.dependencyOrder) {
        if (result.sideEffectRoots.find(module.packageName) ==
            result.sideEffectRoots.end()) continue;
        const std::string reason = module.topLevelSideEffectReason.empty()
            ? "transitive import initialization"
            : module.topLevelSideEffectReason;
        std::cout << " " << module.packageName << "["
                  << reason << "]";
    }
    std::cout << "\n[emit-llvm] implicit roots:";
    for (const auto& name : result.implicitRoots) std::cout << " " << name;
    std::cout << std::endl;
}

} // namespace ZeusLLVM
