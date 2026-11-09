#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "../ZeusDiagnostics.hpp"
#include "../ZeusIR/IRType.hpp"

namespace ZeusPackage {

struct PrivateGlobalSlot {
    std::string packageName;
    std::string globalName;
    int index = -1;
};

inline std::vector<ZeusDiagnostics::Diagnostic> findPrivateGlobalReferences(
    const std::vector<std::vector<IRType::IRInstr>>& program,
    const std::vector<PrivateGlobalSlot>& privateGlobals,
    const std::string& currentPackageName,
    const std::vector<std::string>& currentGlobalNames
) {
    const auto ownsName = [&](const std::string& name) {
        return std::find(currentGlobalNames.begin(), currentGlobalNames.end(), name) !=
            currentGlobalNames.end();
    };

    const auto makeDiagnostic = [](const std::string& packageName,
                                   const std::string& globalName,
                                   const IRType::IRInstr& instruction) {
        return ZeusDiagnostics::Diagnostic{
            "P0043",
            "name '" + globalName + "' is not exported by module '" + packageName + "'",
            instruction.line,
            instruction.column
        };
    };

    for (const auto& chunk : program) {
        for (const auto& instruction : chunk) {
            if (instruction.op != VMOperator::LOAD_VALUE) continue;

            std::string referencedName;
            bool hasResolvedGlobalSlot = false;
            if (instruction.args.size() == 1 &&
                std::holds_alternative<std::string>(instruction.args[0])) {
                referencedName = std::get<std::string>(instruction.args[0]);
            } else if (instruction.args.size() >= 2 &&
                       std::holds_alternative<std::string>(instruction.args[0]) &&
                       std::holds_alternative<LexerType::Value>(instruction.args[1])) {
                const std::string& scope = std::get<std::string>(instruction.args[0]);
                if (scope != "global") continue;

                if (instruction.args.size() >= 3 &&
                    std::holds_alternative<std::string>(instruction.args[2])) {
                    referencedName = std::get<std::string>(instruction.args[2]);
                }

                const auto& indexValue = std::get<LexerType::Value>(instruction.args[1]);
                if (std::holds_alternative<ZeusType::BigInteger>(indexValue.data)) {
                    const int64_t index = std::get<ZeusType::BigInteger>(indexValue.data).toInt64();
                    if (index >= std::numeric_limits<int>::min() &&
                        index <= std::numeric_limits<int>::max()) {
                        for (const auto& privateGlobal : privateGlobals) {
                            if (privateGlobal.index != static_cast<int>(index) ||
                                privateGlobal.packageName == currentPackageName ||
                                (!referencedName.empty() && ownsName(referencedName))) {
                                continue;
                            }
                            return {makeDiagnostic(
                                privateGlobal.packageName,
                                referencedName.empty() ? privateGlobal.globalName : referencedName,
                                instruction
                            )};
                        }
                        hasResolvedGlobalSlot = index >= 0 || index <= -2;
                    }
                }
            }

            if (hasResolvedGlobalSlot || referencedName.empty() || ownsName(referencedName)) {
                continue;
            }

            for (const auto& privateGlobal : privateGlobals) {
                if (privateGlobal.packageName == currentPackageName) continue;
                const std::string qualifiedName =
                    privateGlobal.packageName + "." + privateGlobal.globalName;
                if (referencedName == privateGlobal.globalName ||
                    referencedName == qualifiedName) {
                    return {makeDiagnostic(
                        privateGlobal.packageName, referencedName, instruction
                    )};
                }
            }
        }
    }

    return {};
}

} // namespace ZeusPackage
