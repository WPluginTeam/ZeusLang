#ifndef IRMetadataCodec_hpp
#define IRMetadataCodec_hpp

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../ZeusIR/IRType.hpp"

namespace IRBinary {
namespace IRMetadataCodec {

    inline bool writeRaw(std::ofstream& stream, const void* data, size_t size) {
        stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
        return static_cast<bool>(stream);
    }

    inline bool readRaw(std::ifstream& stream, void* data, size_t size) {
        stream.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
        return static_cast<size_t>(stream.gcount()) == size;
    }

    inline bool writeUint8(std::ofstream& stream, uint8_t value) {
        return writeRaw(stream, &value, sizeof(value));
    }

    inline bool writeUint32(std::ofstream& stream, uint32_t value) {
        return writeRaw(stream, &value, sizeof(value));
    }

    inline bool writeInt32(std::ofstream& stream, int32_t value) {
        return writeRaw(stream, &value, sizeof(value));
    }

    inline bool writeInt64(std::ofstream& stream, int64_t value) {
        return writeRaw(stream, &value, sizeof(value));
    }

    inline bool readUint8(std::ifstream& stream, uint8_t& value) {
        return readRaw(stream, &value, sizeof(value));
    }

    inline bool readUint32(std::ifstream& stream, uint32_t& value) {
        return readRaw(stream, &value, sizeof(value));
    }

    inline bool readInt32(std::ifstream& stream, int32_t& value) {
        return readRaw(stream, &value, sizeof(value));
    }

    inline bool readInt64(std::ifstream& stream, int64_t& value) {
        return readRaw(stream, &value, sizeof(value));
    }

    inline bool writeString(std::ofstream& stream, const std::string& value) {
        if (value.size() > std::numeric_limits<uint32_t>::max()) return false;
        const uint32_t size = static_cast<uint32_t>(value.size());
        return writeUint32(stream, size) &&
            (size == 0 || writeRaw(stream, value.data(), size));
    }

    inline bool readString(std::ifstream& stream, std::string& value) {
        uint32_t size = 0;
        if (!readUint32(stream, size)) return false;
        value.resize(size);
        return size == 0 || readRaw(stream, &value[0], size);
    }

    inline bool writeSymbol(std::ofstream& stream, const IRType::SymbolRef& symbol) {
        return writeUint8(stream, static_cast<uint8_t>(symbol.kind)) &&
            writeString(stream, symbol.name) &&
            writeInt32(stream, static_cast<int32_t>(symbol.index)) &&
            writeInt32(stream, static_cast<int32_t>(symbol.aux)) &&
            writeUint8(stream, symbol.isConst ? 1 : 0);
    }

    inline bool readSymbol(std::ifstream& stream, IRType::SymbolRef& symbol) {
        uint8_t kind = 0;
        uint8_t isConst = 0;
        int32_t index = -1;
        int32_t aux = -1;
        if (!readUint8(stream, kind) ||
            kind > static_cast<uint8_t>(IRType::SymbolRefKind::Class) ||
            !readString(stream, symbol.name) ||
            !readInt32(stream, index) || !readInt32(stream, aux) ||
            !readUint8(stream, isConst) || isConst > 1) {
            return false;
        }
        symbol.kind = static_cast<IRType::SymbolRefKind>(kind);
        symbol.index = index;
        symbol.aux = aux;
        symbol.isConst = isConst != 0;
        return true;
    }

    inline bool writeStrings(std::ofstream& stream,
                             const std::vector<std::string>& values) {
        if (values.size() > std::numeric_limits<uint32_t>::max() ||
            !writeUint32(stream, static_cast<uint32_t>(values.size()))) {
            return false;
        }
        for (const auto& value : values) {
            if (!writeString(stream, value)) return false;
        }
        return true;
    }

    inline bool readStrings(std::ifstream& stream,
                            std::vector<std::string>& values) {
        uint32_t count = 0;
        if (!readUint32(stream, count)) return false;
        values.clear();
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::string value;
            if (!readString(stream, value)) return false;
            values.push_back(std::move(value));
        }
        return true;
    }

    inline bool writeBools(std::ofstream& stream, const std::vector<bool>& values) {
        if (values.size() > std::numeric_limits<uint32_t>::max() ||
            !writeUint32(stream, static_cast<uint32_t>(values.size()))) {
            return false;
        }
        for (bool value : values) {
            if (!writeUint8(stream, value ? 1 : 0)) return false;
        }
        return true;
    }

    inline bool readBools(std::ifstream& stream, std::vector<bool>& values) {
        uint32_t count = 0;
        if (!readUint32(stream, count)) return false;
        values.clear();
        values.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t value = 0;
            if (!readUint8(stream, value) || value > 1) return false;
            values.push_back(value != 0);
        }
        return true;
    }

    inline bool isDefault(const IRType::SymbolRef& symbol) {
        return symbol.kind == IRType::SymbolRefKind::Unknown &&
            symbol.name.empty() && symbol.index == -1 && symbol.aux == -1 &&
            !symbol.isConst;
    }

    inline bool isDefault(const IRType::IRMetadata& metadata) {
        if (metadata.kind != IRType::IRMetadataKind::None || !metadata.tag.empty() ||
            !isDefault(metadata.symbol) || !metadata.typeName.empty() ||
            !metadata.typeNames.empty() || !metadata.parameterNames.empty() ||
            !metadata.parameterIsReference.empty() || !metadata.callArguments.empty() ||
            !metadata.callArgumentTypes.empty() ||
            !metadata.explicitTypeArguments.empty() || !metadata.fieldNames.empty() ||
            !metadata.classTypeParameters.empty() ||
            !metadata.fieldTypeExplicit.empty() ||
            !metadata.fieldConstness.empty() || !metadata.fieldDefaults.empty() ||
            !metadata.fieldInitializerNames.empty() ||
            !metadata.ownerType.empty() || !metadata.operatorSymbol.empty() ||
            !metadata.nativeSymbol.empty() || metadata.nativeReferenceABI != "raw" ||
            !metadata.moduleName.empty() || metadata.captureCount != 0 ||
            metadata.hasCaptureCount || metadata.isStaticMethod || metadata.isInline ||
            metadata.isNoSafepoint || !isDefault(metadata.subscriptWritebackTarget) ||
            metadata.isSubscriptValueWriteback || metadata.classAssignmentCopy || metadata.classCopySkipHook ||
            !metadata.classCopyType.empty() || !metadata.classCopyHook.empty()) {
            return false;
        }
        return true;
    }

    inline bool writeMetadata(std::ofstream& stream,
                              const IRType::IRMetadata& metadata) {
        if (!writeUint8(stream, static_cast<uint8_t>(metadata.kind)) ||
            !writeString(stream, metadata.tag) ||
            !writeSymbol(stream, metadata.symbol) ||
            !writeString(stream, metadata.typeName) ||
            !writeStrings(stream, metadata.typeNames) ||
            !writeStrings(stream, metadata.parameterNames) ||
            !writeBools(stream, metadata.parameterIsReference) ||
            metadata.callArguments.size() > std::numeric_limits<uint32_t>::max() ||
            !writeUint32(stream, static_cast<uint32_t>(metadata.callArguments.size()))) {
            return false;
        }
        for (const auto& argument : metadata.callArguments) {
            if (!writeInt32(stream, static_cast<int32_t>(argument.positionalIndex)) ||
                !writeString(stream, argument.namedParameter) ||
                !writeSymbol(stream, argument.source) ||
                !writeUint8(stream, argument.isAssignable ? 1 : 0)) {
                return false;
            }
        }
        if (!writeStrings(stream, metadata.callArgumentTypes) ||
            !writeStrings(stream, metadata.explicitTypeArguments) ||
            !writeStrings(stream, metadata.classTypeParameters) ||
            !writeStrings(stream, metadata.fieldNames) ||
            !writeBools(stream, metadata.fieldConstness) ||
            metadata.fieldDefaults.size() > std::numeric_limits<uint32_t>::max() ||
            !writeUint32(stream, static_cast<uint32_t>(metadata.fieldDefaults.size()))) {
            return false;
        }
        for (const auto& value : metadata.fieldDefaults) {
            if (!writeUint8(stream, static_cast<uint8_t>(value.kind)) ||
                !writeInt64(stream, value.integerValue) ||
                !writeUint8(stream, value.booleanValue ? 1 : 0) ||
                !writeString(stream, value.stringValue) ||
                !writeString(stream, value.bytesValue)) {
                return false;
            }
        }
        if (!writeBools(stream, metadata.fieldTypeExplicit) ||
            !writeStrings(stream, metadata.fieldInitializerNames)) return false;
        return writeString(stream, metadata.ownerType) &&
            writeString(stream, metadata.operatorSymbol) &&
            writeString(stream, metadata.nativeSymbol) &&
            writeString(stream, metadata.nativeReferenceABI) &&
            writeString(stream, metadata.moduleName) &&
            writeInt32(stream, static_cast<int32_t>(metadata.captureCount)) &&
            writeUint8(stream, metadata.hasCaptureCount ? 1 : 0) &&
            writeUint8(stream, metadata.isStaticMethod ? 1 : 0) &&
            writeUint8(stream, metadata.isInline ? 1 : 0) &&
            writeUint8(stream, metadata.isNoSafepoint ? 1 : 0) &&
            writeSymbol(stream, metadata.subscriptWritebackTarget) &&
            writeUint8(stream, metadata.isSubscriptValueWriteback ? 1 : 0) &&
            writeUint8(stream, metadata.classAssignmentCopy ? 1 : 0) &&
            writeUint8(stream, metadata.classCopySkipHook ? 1 : 0) &&
            writeString(stream, metadata.classCopyType) &&
            writeString(stream, metadata.classCopyHook);
    }

    inline bool readMetadata(
        std::ifstream& stream,
        IRType::IRMetadata& metadata,
        uint16_t version
    ) {
        uint8_t kind = 0;
        if (!readUint8(stream, kind) ||
            kind > static_cast<uint8_t>(IRType::IRMetadataKind::RawBytesRangeIndexStore) ||
            !readString(stream, metadata.tag) ||
            !readSymbol(stream, metadata.symbol) ||
            !readString(stream, metadata.typeName) ||
            !readStrings(stream, metadata.typeNames) ||
            !readStrings(stream, metadata.parameterNames) ||
            !readBools(stream, metadata.parameterIsReference)) {
            return false;
        }
        metadata.kind = static_cast<IRType::IRMetadataKind>(kind);

        uint32_t argumentCount = 0;
        if (!readUint32(stream, argumentCount)) return false;
        metadata.callArguments.clear();
        metadata.callArguments.reserve(argumentCount);
        for (uint32_t i = 0; i < argumentCount; ++i) {
            IRType::CallArgumentSource argument;
            int32_t positionalIndex = -1;
            uint8_t assignable = 0;
            if (!readInt32(stream, positionalIndex) ||
                !readString(stream, argument.namedParameter) ||
                !readSymbol(stream, argument.source) ||
                !readUint8(stream, assignable) || assignable > 1) {
                return false;
            }
            argument.positionalIndex = positionalIndex;
            argument.isAssignable = assignable != 0;
            metadata.callArguments.push_back(std::move(argument));
        }
        if (!readStrings(stream, metadata.callArgumentTypes) ||
            (version >= 5 && !readStrings(stream, metadata.explicitTypeArguments)) ||
            (version >= 6 && !readStrings(stream, metadata.classTypeParameters)) ||
            !readStrings(stream, metadata.fieldNames) ||
            !readBools(stream, metadata.fieldConstness)) {
            return false;
        }
        uint32_t defaultCount = 0;
        if (!readUint32(stream, defaultCount)) return false;
        metadata.fieldDefaults.clear();
        metadata.fieldDefaults.reserve(defaultCount);
        for (uint32_t i = 0; i < defaultCount; ++i) {
            IRType::ClassFieldDefault value;
            uint8_t kindValue = 0;
            uint8_t booleanValue = 0;
            if (!readUint8(stream, kindValue) ||
                kindValue > static_cast<uint8_t>(IRType::ClassFieldDefaultKind::Unsupported) ||
                !readInt64(stream, value.integerValue) ||
                !readUint8(stream, booleanValue) || booleanValue > 1 ||
                !readString(stream, value.stringValue) ||
                !readString(stream, value.bytesValue)) {
                return false;
            }
            value.kind = static_cast<IRType::ClassFieldDefaultKind>(kindValue);
            value.booleanValue = booleanValue != 0;
            metadata.fieldDefaults.push_back(std::move(value));
        }
        metadata.fieldTypeExplicit.clear();
        if (version >= 7 && !readBools(stream, metadata.fieldTypeExplicit)) {
            return false;
        }
        metadata.fieldInitializerNames.clear();
        if (version >= 9 && !readStrings(stream, metadata.fieldInitializerNames)) {
            return false;
        }

        uint8_t flags[4] = {};
        int32_t captureCount = 0;
        if (!readString(stream, metadata.ownerType) ||
            !readString(stream, metadata.operatorSymbol) ||
            !readString(stream, metadata.nativeSymbol) ||
            !readString(stream, metadata.nativeReferenceABI) ||
            !readString(stream, metadata.moduleName) ||
            !readInt32(stream, captureCount)) {
            return false;
        }
        for (auto& flag : flags) {
            if (!readUint8(stream, flag) || flag > 1) return false;
        }
        if (!readSymbol(stream, metadata.subscriptWritebackTarget)) return false;
        uint8_t subscriptWriteback = 0;
        if (!readUint8(stream, subscriptWriteback) || subscriptWriteback > 1) {
            return false;
        }
        metadata.captureCount = captureCount;
        metadata.hasCaptureCount = flags[0] != 0;
        metadata.isStaticMethod = flags[1] != 0;
        metadata.isInline = flags[2] != 0;
        metadata.isNoSafepoint = flags[3] != 0;
        metadata.isSubscriptValueWriteback = subscriptWriteback != 0;
        if (version >= 8) {
            uint8_t copy = 0, skipHook = 0;
            if (!readUint8(stream, copy) || copy > 1 ||
                !readUint8(stream, skipHook) || skipHook > 1 ||
                !readString(stream, metadata.classCopyType) ||
                !readString(stream, metadata.classCopyHook)) return false;
            metadata.classAssignmentCopy = copy != 0;
            metadata.classCopySkipHook = skipHook != 0;
        }
        return true;
    }

    inline bool writeSection(
        std::ofstream& stream,
        const std::vector<std::vector<IRType::IRInstr>>& program,
        uint32_t& offset,
        uint32_t& size
    ) {
        const std::streamoff start = static_cast<std::streamoff>(stream.tellp());
        if (start < 0 || static_cast<uint64_t>(start) >
                std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        offset = static_cast<uint32_t>(start);
        uint32_t count = 0;
        for (const auto& chunk : program) {
            for (const auto& instruction : chunk) {
                if (!isDefault(instruction.metadata)) {
                    if (count == std::numeric_limits<uint32_t>::max()) return false;
                    ++count;
                }
            }
        }
        if (!writeUint32(stream, count)) return false;
        for (uint32_t ci = 0; ci < program.size(); ++ci) {
            for (uint32_t ii = 0; ii < program[ci].size(); ++ii) {
                const auto& instruction = program[ci][ii];
                if (isDefault(instruction.metadata)) continue;
                if (!writeUint32(stream, ci) || !writeUint32(stream, ii) ||
                    !writeMetadata(stream, instruction.metadata)) {
                    return false;
                }
            }
        }
        const std::streamoff end = static_cast<std::streamoff>(stream.tellp());
        if (end < start || static_cast<uint64_t>(end) - static_cast<uint64_t>(start) >
                std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        size = static_cast<uint32_t>(static_cast<uint64_t>(end) -
                                     static_cast<uint64_t>(start));
        return true;
    }

    inline bool readSection(
        std::ifstream& stream,
        uint32_t sectionOffset,
        uint32_t sectionSize,
        uint16_t version,
        std::vector<std::vector<IRType::IRInstr>>& program
    ) {
        stream.seekg(sectionOffset, std::ios::beg);
        if (!stream) return false;
        uint32_t count = 0;
        if (!readUint32(stream, count)) return false;
        size_t totalInstructions = 0;
        for (const auto& chunk : program) totalInstructions += chunk.size();
        if (count > totalInstructions) return false;
        std::vector<std::vector<bool>> seen;
        seen.reserve(program.size());
        for (const auto& chunk : program) seen.emplace_back(chunk.size(), false);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t chunkIndex = 0;
            uint32_t instructionIndex = 0;
            if (!readUint32(stream, chunkIndex) || !readUint32(stream, instructionIndex) ||
                chunkIndex >= program.size() ||
                instructionIndex >= program[chunkIndex].size() ||
                seen[chunkIndex][instructionIndex]) {
                return false;
            }
            if (!readMetadata(
                    stream, program[chunkIndex][instructionIndex].metadata, version)) {
                return false;
            }
            seen[chunkIndex][instructionIndex] = true;
        }
        const std::streamoff end = stream.tellg();
        return end >= 0 && static_cast<uint64_t>(end) ==
            static_cast<uint64_t>(sectionOffset) + sectionSize;
    }

}
}

#endif // IRMetadataCodec_hpp
