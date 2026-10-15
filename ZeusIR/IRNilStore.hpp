#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "IRType.hpp"

namespace IRNilStore {

    struct PendingStore {
        size_t chunkIndex = 0;
        size_t pushIndex = 0;
        size_t storeIndex = 0;
    };

    inline bool readIndex(
        const IRType::IRInstr& instruction,
        size_t argumentIndex,
        int64_t& index
    ) {
        if (argumentIndex >= instruction.args.size() ||
            !std::holds_alternative<LexerType::Value>(instruction.args[argumentIndex])) {
            return false;
        }

        const auto& value = std::get<LexerType::Value>(instruction.args[argumentIndex]);
        return std::holds_alternative<ZeusType::BigInteger>(value.data) &&
            std::get<ZeusType::BigInteger>(value.data).tryToInt64(index);
    }

    inline bool isNilPush(const IRType::IRInstr& instruction) {
        return instruction.op == VMOperator::PUSH &&
            instruction.args.size() == 1 &&
            std::holds_alternative<IRType::NullLiteral>(instruction.args.front());
    }

    inline bool isFunctionBody(const std::vector<IRType::IRInstr>& chunk) {
        return std::any_of(chunk.begin(), chunk.end(), [](const auto& instruction) {
            return instruction.op == VMOperator::FUNC_BEGIN;
        });
    }

    inline void markDead(
        const PendingStore& pending,
        std::vector<std::vector<bool>>& remove,
        const std::vector<std::vector<IRType::IRInstr>>& program,
        IRType::IRInstr& replacement
    ) {
        remove[pending.chunkIndex][pending.pushIndex] = true;
        remove[pending.chunkIndex][pending.storeIndex] = true;
        const auto& initializer = program[pending.chunkIndex][pending.storeIndex];
        replacement.op = initializer.op;
        replacement.args = initializer.args;
    }

    inline void eliminateOverwrittenInitializers(
        std::vector<std::vector<IRType::IRInstr>>& program
    ) {
        std::vector<std::vector<bool>> remove(program.size());
        for (size_t i = 0; i < program.size(); ++i) {
            remove[i].resize(program[i].size(), false);
        }

        std::unordered_map<int64_t, PendingStore> pendingGlobals;

        for (size_t chunkIndex = 0; chunkIndex < program.size(); ++chunkIndex) {
            auto& chunk = program[chunkIndex];
            std::unordered_map<int64_t, PendingStore> pendingLocals;
            const bool functionBody = isFunctionBody(chunk);
            auto& pendingStores = functionBody ? pendingLocals : pendingGlobals;

            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                auto& instruction = chunk[pc];
                int64_t slotIndex = -1;

                if (instruction.op == VMOperator::LOAD_VALUE) {
                    if (instruction.args.size() >= 2 &&
                        std::holds_alternative<std::string>(instruction.args[0]) &&
                        readIndex(instruction, 1, slotIndex)) {
                        const auto& scope = std::get<std::string>(instruction.args[0]);
                        if ((functionBody && scope == "local") ||
                            (!functionBody && scope == "global")) {
                            pendingStores.erase(slotIndex);
                        }
                    } else {
                        pendingStores.clear();
                    }
                    continue;
                }

                const bool writesLocal =
                    instruction.op == VMOperator::STORE_VALUE_LOCAL ||
                    instruction.op == VMOperator::ASSIGN_VALUE_LOCAL;
                const bool writesGlobal =
                    instruction.op == VMOperator::STORE_VALUE_GLOBAL ||
                    instruction.op == VMOperator::ASSIGN_VALUE_GLOBAL;
                const bool writesTrackedScope = functionBody ? writesLocal : writesGlobal;

                if (writesTrackedScope) {
                    if (!readIndex(instruction, 0, slotIndex)) {
                        pendingStores.clear();
                        continue;
                    }

                    const auto prior = pendingStores.find(slotIndex);
                    if (prior != pendingStores.end()) {
                        markDead(prior->second, remove, program, instruction);
                        pendingStores.erase(prior);
                    }

                    if (pc > 0 && isNilPush(chunk[pc - 1])) {
                        pendingStores[slotIndex] = PendingStore{
                            chunkIndex,
                            pc - 1,
                            pc
                        };
                    }
                    continue;
                }

                switch (instruction.op) {
                    case VMOperator::PUSH:
                    case VMOperator::PUSH_NAMED:
                    case VMOperator::LOAD_NATIVE:
                    case VMOperator::IR_ANNOTATION:
                    case VMOperator::HOTSPOT:
                    case VMOperator::FUNC_BEGIN:
                    case VMOperator::FUNC_END:
                        break;

                    default:
                        pendingStores.clear();
                        break;
                }
            }

            if (functionBody) pendingLocals.clear();
        }

        for (size_t chunkIndex = 0; chunkIndex < program.size(); ++chunkIndex) {
            std::vector<IRType::IRInstr> compacted;
            compacted.reserve(program[chunkIndex].size());
            for (size_t pc = 0; pc < program[chunkIndex].size(); ++pc) {
                if (!remove[chunkIndex][pc]) {
                    compacted.push_back(std::move(program[chunkIndex][pc]));
                }
            }
            program[chunkIndex] = std::move(compacted);
        }
    }

}
