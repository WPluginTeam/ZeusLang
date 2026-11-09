#pragma once

#include <string>
#include <utility>
#include <vector>

#include "../ZeusIR/IRType.hpp"

namespace ZeusPackage {

// LLVM resolves namespace member bindings from this metadata. Both root and
// imported modules use the same marker and the final merged alias slot.
inline std::vector<IRType::IRInstr> makeStaticNamespaceImportInitChunk(
    const IRType::IRInstr& importInstruction,
    const std::string& packageName,
    int aliasGlobalIndex,
    const std::vector<std::string>& exportedNames
) {
    IRType::IRInstr marker = importInstruction;
    marker.metadata = IRType::IRMetadata::moduleImportInit(packageName);
    marker.args.push_back(LexerType::Value(ZeusType::BigInteger(aliasGlobalIndex)));
    std::vector<IRType::IRInstr> chunk;
    chunk.push_back(std::move(marker));
    for (const auto& name : exportedNames) {
        chunk.push_back(IRType::IRInstr{
            VMOperator::PUSH, {LexerType::Value(name)}
        });
    }
    return chunk;
}

} // namespace ZeusPackage
