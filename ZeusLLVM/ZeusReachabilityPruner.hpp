#pragma once

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ZeusEmitter.hpp"
#include "ZeusModuleGraph.hpp"
#include "ZeusModulePruner.hpp"

namespace ZeusLLVM {

struct ReachabilityPruneResult {
    bool ok = true;
    std::string error;
    size_t packageCount = 0;
    size_t activeModuleCount = 0;
    size_t prunedModuleChunks = 0;
    size_t prunedPackageFunctions = 0;
    size_t unreachablePackageFunctions = 0;
    size_t prunedNamespaceMarkers = 0;
    std::vector<std::string> activeModules;
    std::vector<std::string> prunedModules;
    std::map<std::string, std::set<std::string>> moduleReasons;
};

namespace ReachabilityPrunerDetail {

inline bool readIntegerArgument(
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
}

inline bool isFunctionChunk(const std::vector<IRType::IRInstr>& chunk) {
    return !chunk.empty() && chunk.front().op == VMOperator::FUNC_BEGIN;
}

inline bool isNamespaceMarker(const std::vector<IRType::IRInstr>& chunk) {
    return !chunk.empty() && chunk.front().metadata.kind ==
        IRType::IRMetadataKind::ModuleImportInit;
}

inline int namespaceAliasGlobalIndex(
    const std::vector<IRType::IRInstr>& chunk
) {
    int64_t aliasGlobal = -1;
    for (const auto& instruction : chunk) {
        if (instruction.op == VMOperator::IMPORT && instruction.args.size() > 3 &&
            readIntegerArgument(instruction, 3, aliasGlobal) && aliasGlobal >= 0 &&
            aliasGlobal <= std::numeric_limits<int>::max()) {
            return static_cast<int>(aliasGlobal);
        }
        if (instruction.op == VMOperator::STORE_VALUE_GLOBAL &&
            readIntegerArgument(instruction, 0, aliasGlobal) && aliasGlobal >= 0 &&
            aliasGlobal <= std::numeric_limits<int>::max()) {
            return static_cast<int>(aliasGlobal);
        }
    }
    return -1;
}

} // namespace ReachabilityPrunerDetail

inline ReachabilityPruneResult pruneUnreachablePackageCode(
    ZeusCompile::Result& compiled,
    const ModuleGraphResult& moduleGraph,
    const std::vector<std::string>& implicitModules,
    const std::vector<ZeusPackage::CompiledPackage*>& packages
) {
    ReachabilityPruneResult result;
    result.packageCount = packages.size();

    std::map<std::string, const ModuleNode*> moduleByName;
    for (const auto& module : moduleGraph.dependencyOrder) {
        moduleByName[module.packageName] = &module;
    }

    std::map<int, std::string> packageBySourceFileId;
    std::map<std::string, int> sourceFileIdByPackage;
    std::map<std::string, std::set<std::string>> moduleReasons;
    for (auto* package : packages) {
        if (package == nullptr) continue;
        packageBySourceFileId[package->sourceFileId] = package->packageName;
        sourceFileIdByPackage[package->packageName] = package->sourceFileId;
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
                const auto dependency =
                    moduleHasInitializationEffects.find(imported.packageName);
                if (dependency == moduleHasInitializationEffects.end() ||
                    !dependency->second) {
                    continue;
                }
                moduleHasInitializationEffects[module.packageName] = true;
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }

    auto retainModule = [&](const std::string& packageName,
                            const std::string& reason) {
        if (moduleByName.find(packageName) == moduleByName.end()) return false;
        moduleReasons[packageName].insert(reason);
        return true;
    };
    std::set<std::string> activeModules;
    auto activateModule = [&](const std::string& packageName,
                              const std::string& reason) {
        if (!retainModule(packageName, reason)) return false;
        return activeModules.insert(packageName).second;
    };

    std::vector<std::string> implicitQueue;
    for (const auto& packageName : implicitModules) {
        if (activateModule(packageName, "implicit")) {
            implicitQueue.push_back(packageName);
        }
    }
    for (size_t cursor = 0; cursor < implicitQueue.size(); ++cursor) {
        const auto module = moduleByName.find(implicitQueue[cursor]);
        if (module == moduleByName.end()) continue;
        for (const auto& imported : module->second->imports) {
            if (activateModule(imported.packageName, "implicit dependency")) {
                implicitQueue.push_back(imported.packageName);
            }
        }
    }

    for (const auto& imported : moduleGraph.rootImports) {
        const auto effects = moduleHasInitializationEffects.find(imported.packageName);
        if (effects != moduleHasInitializationEffects.end() && effects->second) {
            activateModule(imported.packageName, "root initialization effect");
        }
    }

    std::vector<int> sourceFileIds(compiled.program.size(), 0);
    std::vector<std::string> packageOwners(compiled.program.size());
    for (size_t ci = 0; ci < compiled.program.size(); ++ci) {
        if (compiled.program[ci].empty()) continue;
        const int sourceFileId = compiled.program[ci].front().sourceFileId;
        sourceFileIds[ci] = sourceFileId;
        const auto owner = packageBySourceFileId.find(sourceFileId);
        if (owner != packageBySourceFileId.end()) {
            packageOwners[ci] = owner->second;
        }
    }

    auto activatePackageForSymbol = [&](const std::string& symbol,
                                        const std::string& reason) {
        bool changed = false;
        if (symbol.empty()) return changed;
        for (const auto& module : moduleGraph.dependencyOrder) {
            const std::string prefix = module.packageName + ".";
            bool matches = symbol.rfind(prefix, 0) == 0;
            if (!matches) {
                for (const auto& classInfo : module.classes) {
                    if (symbol == classInfo.name) {
                        matches = true;
                        break;
                    }
                }
            }
            if (matches) changed = activateModule(module.packageName, reason) || changed;
        }
        return changed;
    };

    auto activateEffectfulDependencies = [&]() {
        bool changed = false;
        for (const auto& imported : moduleGraph.rootImports) {
            const auto effects = moduleHasInitializationEffects.find(imported.packageName);
            if (effects != moduleHasInitializationEffects.end() && effects->second) {
                changed = activateModule(
                    imported.packageName, "root initialization effect") || changed;
            }
        }
        std::vector<std::string> activeSnapshot(
            activeModules.begin(), activeModules.end());
        for (const auto& packageName : activeSnapshot) {
            const auto module = moduleByName.find(packageName);
            if (module == moduleByName.end()) continue;
            for (const auto& imported : module->second->imports) {
                const auto effects =
                    moduleHasInitializationEffects.find(imported.packageName);
                if (effects != moduleHasInitializationEffects.end() && effects->second) {
                    changed = activateModule(
                        imported.packageName, "transitive initialization effect") || changed;
                }
            }
        }
        return changed;
    };

    std::set<size_t> finalReachableFunctions;
    std::set<int> finalNamespaceAliasReads;
    std::map<size_t, ZeusLLVM::detail::FunctionSignature> finalFunctionSignatures;
    bool converged = false;

    const size_t iterationLimit = moduleGraph.dependencyOrder.size() + 2;
    for (size_t iteration = 0; iteration < iterationLimit; ++iteration) {
        std::set<size_t> rootChunks;
        for (size_t ci = 0; ci < compiled.program.size(); ++ci) {
            const auto& chunk = compiled.program[ci];
            if (chunk.empty() ||
                ReachabilityPrunerDetail::isFunctionChunk(chunk) ||
                ReachabilityPrunerDetail::isNamespaceMarker(chunk)) {
                continue;
            }
            const std::string& owner = packageOwners[ci];
            if (sourceFileIds[ci] == 0 || owner.empty() ||
                activeModules.find(owner) != activeModules.end()) {
                rootChunks.insert(ci);
            }
        }

        std::set<int> allowedFunctionSourceFileIds;
        allowedFunctionSourceFileIds.insert(0);
        for (const auto& packageName : activeModules) {
            const auto source = sourceFileIdByPackage.find(packageName);
            if (source != sourceFileIdByPackage.end()) {
                allowedFunctionSourceFileIds.insert(source->second);
            }
        }
        for (const size_t ci : rootChunks) {
            if (ci < sourceFileIds.size()) {
                allowedFunctionSourceFileIds.insert(sourceFileIds[ci]);
            }
        }

        ZeusLLVM::detail::Emitter emitter;
        emitter.programChunks = &compiled.program;
        if (!emitter.collectMetadata(compiled)) {
            result.ok = false;
            result.error = emitter.result.error;
            return result;
        }
        emitter.prescanGlobalTypes();
        emitter.collectReachableFunctionChunks(
            compiled.program, &rootChunks, &allowedFunctionSourceFileIds);

        bool changed = false;
        for (const size_t ci : emitter.reachableFunctionChunks) {
            if (ci < packageOwners.size() && !packageOwners[ci].empty()) {
                changed = activateModule(packageOwners[ci], "reachable function") || changed;
            }
        }

        std::set<size_t> scannedChunks = rootChunks;
        scannedChunks.insert(
            emitter.reachableFunctionChunks.begin(),
            emitter.reachableFunctionChunks.end());
        std::set<int> namespaceAliasReads;

        auto activatePackageForGlobal = [&](int64_t globalIndex, bool isRead) {
            if (globalIndex < 0 ||
                static_cast<uint64_t>(globalIndex) >= compiled.globalNames.size() ||
                globalIndex > std::numeric_limits<int>::max()) {
                return false;
            }
            bool globalChanged = activatePackageForSymbol(
                compiled.globalNames[static_cast<size_t>(globalIndex)],
                "reachable global");
            const int slot = static_cast<int>(globalIndex);
            const auto importedNamespace = emitter.namespacePackagesByGlobal.find(slot);
            if (isRead && importedNamespace != emitter.namespacePackagesByGlobal.end()) {
                namespaceAliasReads.insert(slot);
                globalChanged = activateModule(
                    importedNamespace->second, "namespace alias use") || globalChanged;
            }
            return globalChanged;
        };

        for (const size_t ci : scannedChunks) {
            if (ci >= compiled.program.size()) continue;
            const auto& chunk = compiled.program[ci];
            for (const auto& instruction : chunk) {
                if (!instruction.metadata.symbol.name.empty()) {
                    changed = activatePackageForSymbol(
                        instruction.metadata.symbol.name, "reachable symbol") || changed;
                }

                std::string directTarget;
                if (instruction.metadata.readDirectCall(directTarget)) {
                    changed = activatePackageForSymbol(
                        directTarget, "reachable direct call") || changed;
                }

                if (instruction.op == VMOperator::LOAD_VALUE &&
                    instruction.args.size() >= 2 &&
                    std::holds_alternative<std::string>(instruction.args[0]) &&
                    std::get<std::string>(instruction.args[0]) == "global") {
                    int64_t globalIndex = -1;
                    if (ReachabilityPrunerDetail::readIntegerArgument(
                            instruction, 1, globalIndex)) {
                        changed = activatePackageForGlobal(globalIndex, true) || changed;
                    }
                } else if (instruction.op == VMOperator::STORE_VALUE_GLOBAL ||
                           instruction.op == VMOperator::ASSIGN_VALUE_GLOBAL) {
                    int64_t globalIndex = -1;
                    if (ReachabilityPrunerDetail::readIntegerArgument(
                            instruction, 0, globalIndex)) {
                        changed = activatePackageForGlobal(globalIndex, false) || changed;
                    }
                }

                for (size_t argumentIndex = 0;
                     argumentIndex + 2 < instruction.args.size();
                     ++argumentIndex) {
                    if (!std::holds_alternative<std::string>(
                            instruction.args[argumentIndex]) ||
                        std::get<std::string>(instruction.args[argumentIndex]) !=
                            "__zeus_receiver_source" ||
                        !std::holds_alternative<std::string>(
                            instruction.args[argumentIndex + 1]) ||
                        std::get<std::string>(instruction.args[argumentIndex + 1]) !=
                            "global") {
                        continue;
                    }
                    int64_t globalIndex = -1;
                    if (ReachabilityPrunerDetail::readIntegerArgument(
                            instruction, argumentIndex + 2, globalIndex)) {
                        changed = activatePackageForGlobal(globalIndex, true) || changed;
                    }
                }

                std::string className;
                std::vector<std::string> typeArguments;
                std::string specializedClassName;
                if (instruction.metadata.readClassInstantiation(
                        className, typeArguments, &specializedClassName)) {
                    changed = activatePackageForSymbol(
                        specializedClassName.empty() ? className : specializedClassName,
                        "reachable class") || changed;
                }
            }
        }

        for (const auto& functionName : emitter.functionValueReferences) {
            changed = activatePackageForSymbol(
                functionName, "reachable function value") || changed;
        }
        changed = activateEffectfulDependencies() || changed;

        if (!changed) {
            finalReachableFunctions = emitter.reachableFunctionChunks;
            finalNamespaceAliasReads = std::move(namespaceAliasReads);
            finalFunctionSignatures = emitter.functionSignaturesByChunk;
            converged = true;
            break;
        }
    }

    if (!converged) {
        result.ok = false;
        result.error = "[emit-llvm] package reachability did not converge";
        return result;
    }

    for (const auto& packageName : activeModules) {
        result.activeModules.push_back(packageName);
    }
    result.activeModuleCount = activeModules.size();
    result.moduleReasons = moduleReasons;

    for (const auto& function : finalFunctionSignatures) {
        const size_t ci = function.first;
        if (ci >= packageOwners.size() || packageOwners[ci].empty() ||
            activeModules.find(packageOwners[ci]) == activeModules.end() ||
            function.second.isGenericTemplate ||
            finalReachableFunctions.find(ci) != finalReachableFunctions.end()) {
            continue;
        }
        ++result.unreachablePackageFunctions;
    }

    for (size_t ci = 0; ci < compiled.program.size(); ++ci) {
        auto& chunk = compiled.program[ci];
        if (chunk.empty()) continue;

        const std::string& owner = packageOwners[ci];
        if (!owner.empty() && activeModules.find(owner) == activeModules.end()) {
            if (ReachabilityPrunerDetail::isNamespaceMarker(chunk)) {
                ++result.prunedNamespaceMarkers;
            }
            if (ReachabilityPrunerDetail::isFunctionChunk(chunk)) {
                ++result.prunedPackageFunctions;
            } else {
                ++result.prunedModuleChunks;
            }
            chunk.clear();
            continue;
        }

        if (ReachabilityPrunerDetail::isNamespaceMarker(chunk)) {
            const int aliasGlobal =
                ReachabilityPrunerDetail::namespaceAliasGlobalIndex(chunk);
            if (aliasGlobal >= 0 &&
                finalNamespaceAliasReads.find(aliasGlobal) ==
                    finalNamespaceAliasReads.end()) {
                ++result.prunedNamespaceMarkers;
                chunk.clear();
            }
        }
    }

    for (const auto& module : moduleGraph.dependencyOrder) {
        if (activeModules.find(module.packageName) == activeModules.end()) {
            result.prunedModules.push_back(module.packageName);
        }
    }

    return result;
}

inline void reportReachabilityPruning(const ReachabilityPruneResult& result) {
    std::cout << "[emit-llvm] reachable import index retained "
              << result.activeModuleCount << "/" << result.packageCount
              << " modules; removed " << result.prunedModuleChunks
              << " module chunks, " << result.prunedPackageFunctions
              << " package functions and " << result.prunedNamespaceMarkers
              << " unused namespace markers; "
              << result.unreachablePackageFunctions
              << " active package functions are omitted by the emitter" << std::endl;
    if (!result.activeModules.empty()) {
        std::cout << "[emit-llvm] active modules:";
        for (const auto& name : result.activeModules) {
            std::cout << " " << name;
            const auto reasons = result.moduleReasons.find(name);
            if (reasons == result.moduleReasons.end() || reasons->second.empty()) {
                continue;
            }
            std::cout << "[";
            bool firstReason = true;
            for (const auto& reason : reasons->second) {
                if (!firstReason) std::cout << ",";
                std::cout << reason;
                firstReason = false;
            }
            std::cout << "]";
        }
        std::cout << std::endl;
    }
    if (!result.prunedModules.empty()) {
        std::cout << "[emit-llvm] pruned modules:";
        for (const auto& name : result.prunedModules) std::cout << " " << name;
        std::cout << std::endl;
    }
}

} // namespace ZeusLLVM
