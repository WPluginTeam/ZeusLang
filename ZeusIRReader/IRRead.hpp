// Read IR program from binary file
#ifndef IRRead_hpp
#define IRRead_hpp

#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <iostream>
#include <cstring>

#include "IRWrite.hpp"

namespace IRBinary {

    inline bool readRaw(std::ifstream &ifs, void* data, size_t sz) {
        ifs.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(sz));
        return static_cast<size_t>(ifs.gcount()) == sz;
    }

    inline bool readUint8(std::ifstream &ifs, uint8_t &v) { return readRaw(ifs, &v, sizeof(v)); }
    inline bool readUint16(std::ifstream &ifs, uint16_t &v){ return readRaw(ifs, &v, sizeof(v)); }
    inline bool readUint32(std::ifstream &ifs, uint32_t &v){ return readRaw(ifs, &v, sizeof(v)); }
    inline bool readInt32(std::ifstream &ifs, int32_t &v){ return readRaw(ifs, &v, sizeof(v)); }
    inline bool readFloat(std::ifstream &ifs, float &v){ return readRaw(ifs, &v, sizeof(v)); }

    inline bool readString(std::ifstream &ifs, std::string &out) {
        uint32_t len = 0; if (!readUint32(ifs, len)) return false;
        if (len == 0) { out.clear(); return true; }
        out.resize(len);
        if (!readRaw(ifs, out.data(), len)) return false;
        return true;
    }

    inline bool readBytes(std::ifstream &ifs, ZeusType::Bytes &out) {
        uint32_t len = 0; if (!readUint32(ifs, len)) return false;
        out.data.clear();
        if (len == 0) return true;
        out.data.resize(len);
        if (!readRaw(ifs, out.data.data(), len)) return false;
        return true;
    }

    inline bool readValue(std::ifstream &ifs, LexerType::Value &out, uint8_t tag) {
        switch (tag) {
            case ARG_VALUE_INT: {
                int32_t iv; if (!readInt32(ifs, iv)) return false; out = LexerType::Value((int)iv); return true; }
            case ARG_VALUE_FLOAT: {
                float fv; if (!readFloat(ifs, fv)) return false; out = LexerType::Value(fv); return true; }
            case ARG_VALUE_STRING: {
                std::string sv; if (!readString(ifs, sv)) return false; out = LexerType::Value(sv); return true; }
            case ARG_VALUE_BOOL: {
                uint8_t bv; if (!readUint8(ifs, bv)) return false; out = LexerType::Value((bool)(bv != 0)); return true; }
            default: return false; }
    }

    inline bool readStringListAt(std::ifstream& ifs, uint32_t offset, std::vector<std::string>& out) {
        ifs.seekg(offset, std::ios::beg);
        if (!ifs) return false;
        uint32_t count = 0;
        if (!readUint32(ifs, count)) return false;
        out.clear();
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            std::string s;
            if (!readString(ifs, s)) return false;
            out.push_back(std::move(s));
        }
        return true;
    }

    inline bool readSectionTable(std::ifstream& ifs,
                                 std::vector<SectionEntry>& out,
                                 uint32_t tableOffset,
                                 uint32_t expectedCount) {
        ifs.seekg(tableOffset, std::ios::beg);
        if (!ifs) return false;
        uint32_t count = 0;
        if (!readUint32(ifs, count)) return false;
        if (expectedCount != 0 && count != expectedCount) {
            // tolerate mismatch
        }
        out.clear();
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            SectionEntry ent;
            if (!readUint16(ifs, ent.type)) return false;
            if (!readUint16(ifs, ent.reserved)) return false;
            if (!readUint32(ifs, ent.offset)) return false;
            if (!readUint32(ifs, ent.size)) return false;
            out.push_back(ent);
        }
        return true;
    }

    inline bool readFooter(std::ifstream& ifs, uint32_t& sectionCount, uint32_t& tableOffset) {
        static const unsigned char FOOTER_MAGIC[4] = { 'Z', 'S', 'E', 'C' };
        ifs.seekg(0, std::ios::end);
        std::streamoff size = ifs.tellg();
        if (size < 12) return false;
        ifs.seekg(size - 12, std::ios::beg);
        unsigned char magic[4];
        if (!readRaw(ifs, magic, 4)) return false;
        if (std::memcmp(magic, FOOTER_MAGIC, 4) != 0) return false;
        if (!readUint32(ifs, sectionCount)) return false;
        if (!readUint32(ifs, tableOffset)) return false;
        return true;
    }

    inline void convertLegacyNilSentinel(IRType::IRInstr& instr) {
        size_t literalArgIndex = 0;
        if (instr.op == VMOperator::HOTSPOT) {
            // Older optimized PUSH instructions stored the literal in args[1].
            literalArgIndex = 1;
        } else if (instr.op != VMOperator::PUSH) {
            return;
        }

        if (literalArgIndex < instr.args.size() &&
            std::holds_alternative<LexerType::Value>(instr.args[literalArgIndex])) {
            const auto& value = std::get<LexerType::Value>(instr.args[literalArgIndex]);
            if (std::holds_alternative<std::string>(value.data) &&
                std::get<std::string>(value.data) == "__NIL__") {
                instr.args[literalArgIndex] = IRType::NullLiteral{};
            }
        }
    }

    inline std::vector<std::vector<IRType::IRInstr>> readIRProgram(
        const std::string& path,
        std::string* errorMsg = nullptr,
        std::vector<std::string>* outGlobalNames = nullptr,
        IRMeta* outMeta = nullptr) {
        std::vector<std::vector<IRType::IRInstr>> prog;
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs) { if (errorMsg) *errorMsg = "Cannot open file for read: " + path; return prog; }
        unsigned char magic[4]; if (!readRaw(ifs, magic, 4)) { if (errorMsg) *errorMsg = "File too short (magic)"; return prog; }
        if (std::memcmp(magic, MAGIC, 4) != 0) { if (errorMsg) *errorMsg = "Bad magic"; return prog; }
        uint16_t ver; if (!readUint16(ifs, ver)) { if (errorMsg) *errorMsg = "File too short (version)"; return prog; }

        if (ver == 1) {
            uint32_t chunkCount; if (!readUint32(ifs, chunkCount)) { if (errorMsg) *errorMsg = "File too short (chunkCount)"; return prog; }
            prog.reserve(chunkCount);
            for (uint32_t ci = 0; ci < chunkCount; ++ci) {
                uint32_t instrCount; if (!readUint32(ifs, instrCount)) { if (errorMsg) *errorMsg = "Truncated reading instrCount"; prog.clear(); return {}; }
                std::vector<IRType::IRInstr> chunk; chunk.reserve(instrCount);
                for (uint32_t i = 0; i < instrCount; ++i) {
                    uint16_t opv; if (!readUint16(ifs, opv)) { if (errorMsg) *errorMsg = "Truncated reading op"; prog.clear(); return {}; }
                    IRType::IRInstr instr; instr.op = static_cast<VMOperator>(opv); instr.index = (int)i;
                    uint8_t argc; if (!readUint8(ifs, argc)) { if (errorMsg) *errorMsg = "Truncated reading argc"; prog.clear(); return {}; }
                    for (uint8_t a = 0; a < argc; ++a) {
                        uint8_t tag; if (!readUint8(ifs, tag)) { if (errorMsg) *errorMsg = "Truncated reading arg tag"; prog.clear(); return {}; }
                        if (tag == ARG_STRING) { std::string s; if (!readString(ifs, s)) { if (errorMsg) *errorMsg = "Truncated reading string"; prog.clear(); return {}; } instr.args.push_back(s); }
                        else if (tag == ARG_STRING_CONST_KEYWORD) instr.args.push_back(std::string("const"));
                        else if (tag == ARG_STRING_MUTABLE_KEYWORD) instr.args.push_back(std::string("mutable"));
                        else if (tag == ARG_VALUE_BYTES) { ZeusType::Bytes b; if (!readBytes(ifs, b)) { if (errorMsg) *errorMsg = "Truncated reading bytes"; prog.clear(); return {}; } instr.args.push_back(std::move(b)); }
                        else { LexerType::Value v(0); if (!readValue(ifs, v, tag)) { if (errorMsg) *errorMsg = "Bad value tag"; prog.clear(); return {}; } instr.args.push_back(v); }
                    }
                    convertLegacyNilSentinel(instr);
                    chunk.push_back(std::move(instr));
                }
                prog.push_back(std::move(chunk));
            }
            return prog;
        }

        if (ver < 2 || ver > VERSION) { if (errorMsg) *errorMsg = "Unsupported version"; return prog; }

        IRMeta meta;
        if (!readString(ifs, meta.compilerName)) { if (errorMsg) *errorMsg = "File too short (compilerName)"; return prog; }
        if (!readString(ifs, meta.buildTime)) { if (errorMsg) *errorMsg = "File too short (buildTime)"; return prog; }
        if (!readString(ifs, meta.buildVersion)) { if (errorMsg) *errorMsg = "File too short (buildVersion)"; return prog; }
        if (outMeta) *outMeta = meta;

        std::streamoff programStart = ifs.tellg();

        uint32_t footerCount = 0;
        uint32_t tableOffset = 0;
        std::vector<SectionEntry> sections;
        std::vector<std::string> symbols;
        std::vector<std::string> globals;
        SectionEntry metadataSection;
        bool hasMetadataSection = false;

        if (readFooter(ifs, footerCount, tableOffset)) {
            if (readSectionTable(ifs, sections, tableOffset, footerCount)) {
                for (const auto& ent : sections) {
                    if (ent.type == SECTION_SYMBOLS) {
                        if (!readStringListAt(ifs, ent.offset, symbols)) {
                            if (errorMsg) *errorMsg = "Failed to read symbols";
                            return {};
                        }
                    } else if (ent.type == SECTION_GLOBAL_NAMES) {
                        if (!readStringListAt(ifs, ent.offset, globals)) {
                            if (errorMsg) *errorMsg = "Failed to read global names";
                            return {};
                        }
                    } else if (ent.type == SECTION_INSTRUCTION_METADATA) {
                        metadataSection = ent;
                        hasMetadataSection = true;
                    }
                }
            }
        }

        if (ver >= 4 && !hasMetadataSection) {
            if (errorMsg) *errorMsg = "Version 4 IR is missing its instruction metadata section";
            return {};
        }

        if (outGlobalNames) *outGlobalNames = globals;

        ifs.seekg(programStart, std::ios::beg);
        uint32_t chunkCount; if (!readUint32(ifs, chunkCount)) { if (errorMsg) *errorMsg = "File too short (chunkCount)"; return prog; }
        prog.reserve(chunkCount);
        for (uint32_t ci = 0; ci < chunkCount; ++ci) {
            uint32_t instrCount; if (!readUint32(ifs, instrCount)) { if (errorMsg) *errorMsg = "Truncated reading instrCount"; prog.clear(); return {}; }
            std::vector<IRType::IRInstr> chunk; chunk.reserve(instrCount);
            for (uint32_t i = 0; i < instrCount; ++i) {
                uint16_t opv; if (!readUint16(ifs, opv)) { if (errorMsg) *errorMsg = "Truncated reading op"; prog.clear(); return {}; }
                IRType::IRInstr instr; instr.op = static_cast<VMOperator>(opv); instr.index = (int)i;
                uint8_t argc; if (!readUint8(ifs, argc)) { if (errorMsg) *errorMsg = "Truncated reading argc"; prog.clear(); return {}; }
                for (uint8_t a = 0; a < argc; ++a) {
                    uint8_t tag; if (!readUint8(ifs, tag)) { if (errorMsg) *errorMsg = "Truncated reading arg tag"; prog.clear(); return {}; }
                    if (tag == ARG_STRING_SYMREF) {
                        uint32_t idx = 0; if (!readUint32(ifs, idx)) { if (errorMsg) *errorMsg = "Bad symbol index"; prog.clear(); return {}; }
                        if (idx >= symbols.size()) { if (errorMsg) *errorMsg = "Symbol index out of range"; prog.clear(); return {}; }
                        instr.args.push_back(symbols[idx]);
                    } else if (tag == ARG_STRING) {
                        std::string s; if (!readString(ifs, s)) { if (errorMsg) *errorMsg = "Truncated reading string"; prog.clear(); return {}; } instr.args.push_back(s);
                    } else if (tag == ARG_STRING_CONST_KEYWORD) {
                        instr.args.push_back(std::string("const"));
                    } else if (tag == ARG_STRING_MUTABLE_KEYWORD) {
                        instr.args.push_back(std::string("mutable"));
                    } else if (tag == ARG_VALUE_BYTES) {
                        ZeusType::Bytes b;
                        if (!readBytes(ifs, b)) { if (errorMsg) *errorMsg = "Truncated reading bytes"; prog.clear(); return {}; }
                        instr.args.push_back(std::move(b));
                    } else if (tag == ARG_VALUE_NULL && ver >= 3) {
                        instr.args.push_back(IRType::NullLiteral{});
                    } else {
                        LexerType::Value v(0); if (!readValue(ifs, v, tag)) { if (errorMsg) *errorMsg = "Bad value tag"; prog.clear(); return {}; } instr.args.push_back(v);
                    }
                }
                if (ver == 2) convertLegacyNilSentinel(instr);
                chunk.push_back(std::move(instr));
            }
            prog.push_back(std::move(chunk));
        }

        if (hasMetadataSection && !IRMetadataCodec::readSection(
                ifs, metadataSection.offset, metadataSection.size, ver, prog)) {
            if (errorMsg) *errorMsg = "Failed to read IR instruction metadata";
            return {};
        }

        return prog;
    }

    inline std::vector<IRType::IRInstr> readIR(const std::string& path, std::string* errorMsg = nullptr) {
        std::vector<std::string> globals;
        auto prog = readIRProgram(path, errorMsg, &globals, nullptr);
        if (prog.empty()) return {};
        if (prog.size() == 1) return prog.front();
        return prog.front();
    }

} // namespace IRBinary

#endif // IRRead_hpp
