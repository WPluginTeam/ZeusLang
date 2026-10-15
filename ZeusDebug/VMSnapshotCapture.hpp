#pragma once

#include <string>
#include <utility>
#include <vector>

#include "VMSnapshot.hpp"
#include "../ZeusIR/IRType.hpp"
#include "../ZeusRuntimeLib/ZeusVMException.hpp"
#include "../ZeusRuntimeLib/ZeusVMUtils.hpp"

namespace ZeusDebug {

inline std::string snapshotValueString(const ZeusType::UniversalVariant& value) {
    if (const auto* objectHandle = std::get_if<ZeusType::ObjectHandle>(&value)) {
        return "<object#" + std::to_string(objectHandle->id) + ">";
    }
    if (const auto* variant = std::get_if<ZeusType::Variant>(&value)) {
        if (std::holds_alternative<ZeusType::Array>(*variant)) {
            const auto& array = std::get<ZeusType::Array>(*variant);
            const size_t size = array.impl ? array.impl->data.size() : 0;
            return "<array size=" + std::to_string(size) + ">";
        }
        if (std::holds_alternative<ZeusType::Object>(*variant)) {
            const auto& object = std::get<ZeusType::Object>(*variant);
            const size_t size = object.impl ? object.impl->data.size() : 0;
            return "<dictionary size=" + std::to_string(size) + ">";
        }
    }
    return ZeusVMUtils::valueToDebugString(value);
}

template <typename CellVector>
inline void appendSnapshotCells(
    std::vector<SnapshotSlot>& output,
    const CellVector& cells,
    const std::string& prefix,
    const std::vector<std::string>* names = nullptr
) {
    output.reserve(cells.size());
    for (size_t i = 0; i < cells.size(); ++i) {
        SnapshotSlot slot;
        slot.index = i;
        if (names && i < names->size() && !(*names)[i].empty()) {
            slot.name = (*names)[i];
        } else {
            slot.name = prefix + "[" + std::to_string(i) + "]";
        }
        slot.value = cells[i] ? snapshotValueString(cells[i]->readValue()) : "<empty>";
        output.push_back(std::move(slot));
    }
}

template <typename VMType>
inline std::string snapshotFunctionName(
    const VMType& vm,
    const std::vector<IRType::IRInstr>* code
) {
    for (const auto& prototype : vm.prototypes) {
        if (&prototype.body == code) {
            if (!prototype.ownerType.empty() && !prototype.functionName.empty()) {
                return prototype.ownerType + "." + prototype.functionName;
            }
            return prototype.functionName.empty() ? "<anonymous>" : prototype.functionName;
        }
    }
    if (vm.sharedPrototypes) {
        for (const auto& prototype : *vm.sharedPrototypes) {
            if (&prototype.body == code) {
                if (!prototype.ownerType.empty() && !prototype.functionName.empty()) {
                    return prototype.ownerType + "." + prototype.functionName;
                }
                return prototype.functionName.empty() ? "<anonymous>" : prototype.functionName;
            }
        }
    }
    return "<chunk>";
}

template <typename VMType>
inline VMSnapshotRecord captureVMSnapshot(
    const VMType& vm,
    const IRType::IRInstr& instruction,
    size_t pc
) {
    VMSnapshotRecord record;
    const VMException::SourceFileInfo* source =
        VMException::getSourceFile(instruction.sourceFileId);
    if (source && !source->filePath.empty()) {
        record.sourcePath = source->filePath;
    } else if (!vm.breakpointConfig.sourcePath.empty()) {
        record.sourcePath = vm.breakpointConfig.sourcePath;
    } else {
        record.sourcePath = VMException::sourceFilePath;
    }
    record.sourceFileId = instruction.sourceFileId;
    record.line = instruction.line;
    record.column = instruction.column;
    record.chunk = vm.currentChunk;
    record.pc = pc;
    record.instruction = instruction.toString();
    switch (vm.vmstat) {
    case ZeusType::VMStatus::Running: record.status = "running"; break;
    case ZeusType::VMStatus::Stopped: record.status = "stopped"; break;
    case ZeusType::VMStatus::Error: record.status = "error"; break;
    }

    record.frames.reserve(vm.callFrames.size());
    for (size_t i = 0; i < vm.callFrames.size(); ++i) {
        const auto& frame = vm.callFrames[i];
        SnapshotFrame snapshotFrame;
        snapshotFrame.depth = i;
        snapshotFrame.pc = frame.pc;
        snapshotFrame.codeSize = frame.code ? frame.code->size() : 0;
        snapshotFrame.stackBase = frame.stackBase;
        snapshotFrame.functionName = snapshotFunctionName(vm, frame.code);
        snapshotFrame.line = i + 1 < vm.callFrames.size()
            ? vm.callFrames[i + 1].savedLine
            : instruction.line;

        const auto& frameLocal = i + 1 < vm.callFrames.size()
            ? vm.callFrames[i + 1].savedLocal
            : vm.local;
        const auto& frameUpvalue = i + 1 < vm.callFrames.size()
            ? vm.callFrames[i + 1].savedUpvalue
            : vm.upvalue;
        appendSnapshotCells(snapshotFrame.locals, frameLocal, "local");
        appendSnapshotCells(snapshotFrame.upvalues, frameUpvalue, "upvalue");
        record.frames.push_back(std::move(snapshotFrame));
    }

    record.stack.reserve(vm.stack.data.size());
    for (size_t i = 0; i < vm.stack.data.size(); ++i) {
        SnapshotStackValue value;
        value.index = i;
        value.ownerFlags = i < vm.stack.owner.size() ? vm.stack.owner[i] : 0;
        value.value = snapshotValueString(vm.stack.data[i]);
        record.stack.push_back(std::move(value));
    }
    appendSnapshotCells(record.globals, vm.global, "global", &vm.globalNames);
    return record;
}

} // namespace ZeusDebug
