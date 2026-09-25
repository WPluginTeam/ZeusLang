#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "BuiltinRegistry.hpp"

namespace ZeusBuiltins {

enum class CompilationTarget {
    VirtualMachine,
    LLVM
};

struct CompilationPlan {
    std::vector<std::string> implementationModules;
    std::vector<std::string> unqualifiedModules;

    void prependMissingModules(std::vector<std::string>& packages) const {
        for (const auto& module : implementationModules) {
            if (std::find(packages.begin(), packages.end(), module) == packages.end()) {
                packages.insert(packages.begin(), module);
            }
        }
    }
};

inline CompilationPlan makeCompilationPlan(CompilationTarget target, bool usesBytesLiteral) {
    CompilationPlan plan;
    std::vector<std::pair<int, std::string>> exposedModules;
    for (const auto& module : implementationModules()) {
        const bool enabled = target == CompilationTarget::VirtualMachine
            ? module.autoImportToVM
            : module.autoImportToLLVM;
        const bool triggered = module.trigger != ImportTrigger::BytesLiteral || usesBytesLiteral;
        if (!enabled || !triggered) continue;

        plan.implementationModules.emplace_back(module.packageName);
        if (module.exposeGlobals) {
            exposedModules.emplace_back(module.exposureOrder, module.packageName);
        }
    }
    std::stable_sort(exposedModules.begin(), exposedModules.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; });
    for (auto& module : exposedModules) {
        plan.unqualifiedModules.push_back(std::move(module.second));
    }
    return plan;
}

} // namespace ZeusBuiltins
