#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ZeusDebug {

struct SnapshotSlot {
    size_t index = 0;
    std::string name;
    std::string value;
};

struct SnapshotStackValue {
    size_t index = 0;
    unsigned int ownerFlags = 0;
    std::string value;
};

struct SnapshotFrame {
    size_t depth = 0;
    size_t pc = 0;
    size_t codeSize = 0;
    size_t stackBase = 0;
    int line = -1;
    std::string functionName;
    std::vector<SnapshotSlot> locals;
    std::vector<SnapshotSlot> upvalues;
};

struct VMSnapshotRecord {
    std::string sourcePath;
    int sourceFileId = 0;
    int line = -1;
    int column = 0;
    int chunk = -1;
    size_t pc = 0;
    std::string instruction;
    std::string status;
    std::vector<SnapshotFrame> frames;
    std::vector<SnapshotStackValue> stack;
    std::vector<SnapshotSlot> globals;
};

inline std::string jsonEscape(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (unsigned char ch : value) {
        switch (ch) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20) {
                const char digits[] = "0123456789abcdef";
                output << "\\u00" << digits[(ch >> 4) & 0x0f] << digits[ch & 0x0f];
            } else {
                output << static_cast<char>(ch);
            }
        }
    }
    output << '"';
    return output.str();
}

inline void writeSnapshotSlot(std::ostream& output, const SnapshotSlot& slot) {
    output << "{\"index\":" << slot.index
           << ",\"name\":" << jsonEscape(slot.name)
           << ",\"value\":" << jsonEscape(slot.value) << '}';
}

inline void writeSnapshotSlots(std::ostream& output, const std::vector<SnapshotSlot>& slots) {
    output << '[';
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i != 0) output << ',';
        writeSnapshotSlot(output, slots[i]);
    }
    output << ']';
}

inline void writeSnapshotRecord(std::ostream& output, const VMSnapshotRecord& record, size_t sequence) {
    output << "{\n  \"schemaVersion\":1,\n"
           << "  \"sequence\":" << sequence << ",\n"
           << "  \"capturePoint\":\"before_instruction\",\n"
           << "  \"source\":{\"path\":" << jsonEscape(record.sourcePath)
           << ",\"fileId\":" << record.sourceFileId
           << ",\"line\":" << record.line
           << ",\"column\":" << record.column << "},\n"
           << "  \"instruction\":{\"chunk\":" << record.chunk
           << ",\"pc\":" << record.pc
           << ",\"text\":" << jsonEscape(record.instruction) << "},\n"
           << "  \"status\":" << jsonEscape(record.status) << ",\n"
           << "  \"frames\":[";

    for (size_t i = 0; i < record.frames.size(); ++i) {
        if (i != 0) output << ',';
        const SnapshotFrame& frame = record.frames[i];
        output << "{\"depth\":" << frame.depth
               << ",\"function\":" << jsonEscape(frame.functionName)
               << ",\"pc\":" << frame.pc
               << ",\"codeSize\":" << frame.codeSize
               << ",\"stackBase\":" << frame.stackBase
               << ",\"line\":" << frame.line
               << ",\"locals\":";
        writeSnapshotSlots(output, frame.locals);
        output << ",\"upvalues\":";
        writeSnapshotSlots(output, frame.upvalues);
        output << '}';
    }

    output << "],\n  \"stack\":[";
    for (size_t i = 0; i < record.stack.size(); ++i) {
        if (i != 0) output << ',';
        const SnapshotStackValue& value = record.stack[i];
        output << "{\"index\":" << value.index
               << ",\"ownerFlags\":" << value.ownerFlags
               << ",\"value\":" << jsonEscape(value.value) << '}';
    }

    output << "],\n  \"globals\":";
    writeSnapshotSlots(output, record.globals);
    output << "\n}\n";
}

class VMSnapshotWriter {
public:
    VMSnapshotWriter() = default;
    explicit VMSnapshotWriter(std::string directory)
        : directory_(std::filesystem::path(std::move(directory))) {}

    void setDirectory(std::string directory) {
        directory_ = std::filesystem::path(std::move(directory));
    }

    bool write(
        const VMSnapshotRecord& record,
        std::string& writtenPath,
        std::string& error
    ) {
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (ec) {
            error = "cannot create snapshot directory '" + directory_.string() + "': " + ec.message();
            return false;
        }

        for (;;) {
            std::ostringstream filename;
            filename << "vm-snapshot-" << std::setw(6) << std::setfill('0') << nextSequence_ << ".json";
            const std::filesystem::path finalPath = directory_ / filename.str();
            const std::filesystem::path temporaryPath(finalPath.string() + ".tmp");

            const bool finalExists = std::filesystem::exists(finalPath, ec);
            if (ec) {
                error = "cannot inspect snapshot path '" + finalPath.string() + "': " + ec.message();
                return false;
            }
            if (finalExists) {
                ++nextSequence_;
                continue;
            }

            std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "cannot open snapshot file '" + temporaryPath.string() + "'";
                return false;
            }
            writeSnapshotRecord(output, record, nextSequence_);
            output.flush();
            if (!output) {
                output.close();
                std::filesystem::remove(temporaryPath, ec);
                error = "failed while writing snapshot file '" + temporaryPath.string() + "'";
                return false;
            }
            output.close();
            if (!output) {
                std::filesystem::remove(temporaryPath, ec);
                error = "failed while closing snapshot file '" + temporaryPath.string() + "'";
                return false;
            }

            std::filesystem::rename(temporaryPath, finalPath, ec);
            if (ec) {
                const std::string renameError = ec.message();
                std::error_code cleanupError;
                std::filesystem::remove(temporaryPath, cleanupError);
                error = "cannot publish snapshot file '" + finalPath.string() + "': " + renameError;
                return false;
            }

            writtenPath = finalPath.string();
            ++nextSequence_;
            return true;
        }
    }

private:
    std::filesystem::path directory_ = "zeus-snapshots";
    size_t nextSequence_ = 1;
};

} // namespace ZeusDebug
