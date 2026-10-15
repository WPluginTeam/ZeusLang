#pragma once

#include <string>
#include <vector>

#include "TreeParser.hpp"
#include "../ZeusDiagnostics.hpp"

namespace ZeusParser {

enum class BackendTarget {
    VirtualMachine,
    LLVM
};

inline bool isBackendTargetBlock(CodeBlockType type) {
    return type == CodeBlockType::USING_LLVM || type == CodeBlockType::USING_VM;
}

inline bool isBackendOwnerBoundary(CodeBlockType type) {
    return type == CodeBlockType::FUNC || type == CodeBlockType::CLASS ||
           type == CodeBlockType::INTERFACE || type == CodeBlockType::IMPL;
}

inline void collectBackendBlocks(
    const std::vector<TreeParser::NodeAndBlock>& nodes,
    std::vector<const TreeParser::CodeBlock*>& llvmBlocks,
    std::vector<const TreeParser::CodeBlock*>& vmBlocks
) {
    for (const auto& item : nodes) {
        if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(item)) continue;
        const auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(item);
        if (!block) continue;
        if (isBackendOwnerBoundary(block->type)) continue;
        if (block->type == CodeBlockType::USING_LLVM) {
            llvmBlocks.push_back(block.get());
            continue;
        }
        if (block->type == CodeBlockType::USING_VM) {
            vmBlocks.push_back(block.get());
            continue;
        }
        collectBackendBlocks(block->nodes, llvmBlocks, vmBlocks);
    }
}

inline void selectBackendBlocks(
    std::vector<TreeParser::NodeAndBlock>& nodes,
    BackendTarget target,
    std::vector<ZeusDiagnostics::Diagnostic>* warnings = nullptr
) {
    std::vector<const TreeParser::CodeBlock*> llvmBlocks;
    std::vector<const TreeParser::CodeBlock*> vmBlocks;
    collectBackendBlocks(nodes, llvmBlocks, vmBlocks);

    if (warnings != nullptr) {
        if (target == BackendTarget::VirtualMachine) {
            for (size_t i = vmBlocks.size(); i < llvmBlocks.size(); ++i) {
                warnings->push_back({
                    "backend",
                    "using LLVM block has no matching using VM block in this function/scope; skipped for VM target",
                    llvmBlocks[i]->sourceLine,
                    llvmBlocks[i]->sourceColumn
                });
            }
        } else {
            for (size_t i = llvmBlocks.size(); i < vmBlocks.size(); ++i) {
                warnings->push_back({
                    "backend",
                    "using VM block has no matching using LLVM block in this function/scope; skipped for LLVM target",
                    vmBlocks[i]->sourceLine,
                    vmBlocks[i]->sourceColumn
                });
            }
        }
    }

    const auto filter = [&](auto&& self, std::vector<TreeParser::NodeAndBlock>& scope) -> void {
        for (auto it = scope.begin(); it != scope.end();) {
            if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(*it)) {
                ++it;
                continue;
            }
            auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(*it);
            if (!block) {
                ++it;
                continue;
            }
            if (block->type == CodeBlockType::INTERFACE) {
                ++it;
                continue;
            }

            const bool isLLVMBlock = block->type == CodeBlockType::USING_LLVM;
            const bool isVMBlock = block->type == CodeBlockType::USING_VM;
            if ((target == BackendTarget::LLVM && isVMBlock) ||
                (target == BackendTarget::VirtualMachine && isLLVMBlock)) {
                it = scope.erase(it);
                continue;
            }

            if (isBackendTargetBlock(block->type) || isBackendOwnerBoundary(block->type)) {
                selectBackendBlocks(block->nodes, target, warnings);
            } else {
                self(self, block->nodes);
            }
            ++it;
        }
    };
    filter(filter, nodes);
}

} // namespace ZeusParser
