// Write IR program to binary file
#ifndef IRWrite_hpp
#define IRWrite_hpp

#include <vector>
#include <string>
#include <fstream>
#include <cstdint>
#include <cstring>
#include <optional>
#include <iostream>
#include <unordered_map>
#include <ctime>
#include <cstdlib>

#include "../ZeusIR/IRType.hpp"

namespace IRBinary {

    // Magic + version
    static const unsigned char MAGIC[4] = { 'Z', 'I', 'R', '1' };
    static const uint16_t VERSION = 3;

    // Arg type tags（尾部追加：老 .zir 里不可能出现新 tag，前向兼容）
    enum ArgTag : uint8_t {
        ARG_STRING = 0,
        ARG_VALUE_INT = 1,
        ARG_VALUE_FLOAT = 2,
        ARG_VALUE_STRING = 3,
        ARG_VALUE_BOOL = 4,
        ARG_STRING_CONST_KEYWORD = 5,
        ARG_STRING_MUTABLE_KEYWORD = 6,
        ARG_STRING_SYMREF = 7,
        ARG_VALUE_BYTES = 8,
        ARG_VALUE_NULL = 9,
    };

    enum SectionType : uint16_t {
        SECTION_SYMBOLS = 1,
        SECTION_GLOBAL_NAMES = 2,
    };

    struct SectionEntry {
        uint16_t type = 0;
        uint16_t reserved = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct IRMeta {
        std::string compilerName;
        std::string buildTime;
        std::string buildVersion;
    };

    inline void writeRaw(std::ofstream &ofs, const void* data, size_t sz) {
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(sz));
    }

    inline void writeUint8(std::ofstream &ofs, uint8_t v) { writeRaw(ofs, &v, sizeof(v)); }
    inline void writeUint16(std::ofstream &ofs, uint16_t v) { writeRaw(ofs, &v, sizeof(v)); }
    inline void writeUint32(std::ofstream &ofs, uint32_t v) { writeRaw(ofs, &v, sizeof(v)); }
    inline void writeInt32(std::ofstream &ofs, int32_t v) { writeRaw(ofs, &v, sizeof(v)); }
    inline void writeFloat(std::ofstream &ofs, float v) { writeRaw(ofs, &v, sizeof(v)); }

    inline void writeString(std::ofstream &ofs, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        writeUint32(ofs, len);
        if (len > 0) {
            writeRaw(ofs, s.data(), len);
        }
    }

    inline void writeBytes(std::ofstream &ofs, const ZeusType::Bytes& b) {
        uint32_t len = static_cast<uint32_t>(b.data.size());
        writeUint32(ofs, len);
        if (len > 0) {
            writeRaw(ofs, b.data.data(), len);
        }
    }

    inline IRMeta getDefaultMeta() {
        IRMeta meta;
        const char* host = std::getenv("COMPUTERNAME");
        if (!host || !*host) host = std::getenv("HOSTNAME");
        if (!host || !*host) host = "unknown-host";
        meta.compilerName = host;

        std::time_t now = std::time(nullptr);
        char buf[64] = {0};
        std::tm tmv{};
    #ifdef _WIN32
        localtime_s(&tmv, &now);
    #else
        localtime_r(&now, &tmv);
    #endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
        meta.buildTime = buf;

        meta.buildVersion = "ZeusIR v3";
        return meta;
    }

    inline void writeValue(std::ofstream &ofs, const LexerType::Value& v) {
        const auto& data = v.data;
        if (std::holds_alternative<ZeusType::BigInteger>(data)) {
            writeUint8(ofs, ARG_VALUE_INT);
            int32_t iv = (int32_t)std::get<ZeusType::BigInteger>(data).toInt64();
            writeInt32(ofs, iv);
        } else if (std::holds_alternative<ZeusType::BigDecimal>(data)) {
            writeUint8(ofs, ARG_VALUE_FLOAT);
            float fv = (float)std::get<ZeusType::BigDecimal>(data).toDouble();
            writeFloat(ofs, fv);
        } else if (std::holds_alternative<std::string>(data)) {
            writeUint8(ofs, ARG_VALUE_STRING);
            writeString(ofs, std::get<std::string>(data));
        } else if (std::holds_alternative<bool>(data)) {
            writeUint8(ofs, ARG_VALUE_BOOL);
            uint8_t bv = std::get<bool>(data) ? 1 : 0;
            writeUint8(ofs, bv);
        }
    }

    inline bool writeIRProgram(const std::vector<std::vector<IRType::IRInstr>>& program,
                               const std::string& path,
                               const std::vector<std::string>* globalNames = nullptr,
                               const IRMeta* meta = nullptr,
                               std::string* errorMsg = nullptr) {
        std::ofstream ofs(path, std::ios::binary);
        if (!ofs) { if (errorMsg) *errorMsg = "Cannot open file for write: " + path; return false; }

        IRMeta useMeta = meta ? *meta : getDefaultMeta();

        std::vector<std::string> symbols;
        std::unordered_map<std::string, uint32_t> symbolIndex;
        symbolIndex.reserve(256);

        auto getSymbolIndex = [&](const std::string& s) -> uint32_t {
            auto it = symbolIndex.find(s);
            if (it != symbolIndex.end()) return it->second;
            uint32_t idx = static_cast<uint32_t>(symbols.size());
            symbols.push_back(s);
            symbolIndex.emplace(s, idx);
            return idx;
        };

        for (const auto& chunk : program) {
            for (const auto& instr : chunk) {
                for (const auto& a : instr.args) {
                    if (std::holds_alternative<std::string>(a)) {
                        const std::string &s = std::get<std::string>(a);
                        if (s == "const" || s == "mutable") continue;
                        getSymbolIndex(s);
                    }
                }
            }
        }

        writeRaw(ofs, MAGIC, 4);
        writeUint16(ofs, VERSION);
        writeString(ofs, useMeta.compilerName);
        writeString(ofs, useMeta.buildTime);
        writeString(ofs, useMeta.buildVersion);

        writeUint32(ofs, static_cast<uint32_t>(program.size()));
        for (const auto& chunk : program) {
            writeUint32(ofs, static_cast<uint32_t>(chunk.size()));
            for (const auto& instr : chunk) {
                uint16_t opv = static_cast<uint16_t>(instr.op);
                writeUint16(ofs, opv);
                uint8_t ac = static_cast<uint8_t>(instr.args.size());
                writeUint8(ofs, ac);
                for (const auto& a : instr.args) {
                    if (std::holds_alternative<std::string>(a)) {
                        const std::string &s = std::get<std::string>(a);
                        if (s == "const") writeUint8(ofs, ARG_STRING_CONST_KEYWORD);
                        else if (s == "mutable") writeUint8(ofs, ARG_STRING_MUTABLE_KEYWORD);
                        else {
                            writeUint8(ofs, ARG_STRING_SYMREF);
                            uint32_t idx = getSymbolIndex(s);
                            writeUint32(ofs, idx);
                        }
                    } else if (std::holds_alternative<LexerType::Value>(a)) {
                        writeValue(ofs, std::get<LexerType::Value>(a));
                    } else if (std::holds_alternative<ZeusType::Bytes>(a)) {
                        writeUint8(ofs, ARG_VALUE_BYTES);
                        writeBytes(ofs, std::get<ZeusType::Bytes>(a));
                    } else if (std::holds_alternative<IRType::NullLiteral>(a)) {
                        writeUint8(ofs, ARG_VALUE_NULL);
                    } else {
                        if (errorMsg) *errorMsg = "Unknown IRArgs variant";
                        return false;
                    }
                }
            }
        }

        std::vector<SectionEntry> sections;

        auto writeStringListSection = [&](SectionType type, const std::vector<std::string>& list) {
            SectionEntry ent;
            ent.type = type;
            ent.offset = static_cast<uint32_t>(ofs.tellp());
            writeUint32(ofs, static_cast<uint32_t>(list.size()));
            for (const auto& s : list) {
                writeString(ofs, s);
            }
            ent.size = static_cast<uint32_t>(static_cast<uint32_t>(ofs.tellp()) - ent.offset);
            sections.push_back(ent);
        };

        writeStringListSection(SECTION_SYMBOLS, symbols);
        if (globalNames) {
            writeStringListSection(SECTION_GLOBAL_NAMES, *globalNames);
        }

        uint32_t sectionTableOffset = static_cast<uint32_t>(ofs.tellp());
        writeUint32(ofs, static_cast<uint32_t>(sections.size()));
        for (const auto& ent : sections) {
            writeUint16(ofs, ent.type);
            writeUint16(ofs, ent.reserved);
            writeUint32(ofs, ent.offset);
            writeUint32(ofs, ent.size);
        }

        static const unsigned char FOOTER_MAGIC[4] = { 'Z', 'S', 'E', 'C' };
        writeRaw(ofs, FOOTER_MAGIC, 4);
        writeUint32(ofs, static_cast<uint32_t>(sections.size()));
        writeUint32(ofs, sectionTableOffset);

        return true;
    }

    inline bool writeIR(const std::vector<IRType::IRInstr>& ir,
                        const std::string& path,
                        std::string* errorMsg = nullptr) {
        std::vector<std::vector<IRType::IRInstr>> program;
        program.push_back(ir);
        return writeIRProgram(program, path, nullptr, nullptr, errorMsg);
    }

} // namespace IRBinary

#endif // IRWrite_hpp
