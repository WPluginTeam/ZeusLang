#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>

namespace ZeusDebug {

struct BreakpointConfig {
    std::unordered_set<int> lines;
    std::string sourcePath;
    std::string snapshotDirectory;

    bool enabled() const {
        return !lines.empty();
    }

    bool contains(int sourceFileId, int line) const {
        return sourceFileId == 0 && lines.find(line) != lines.end();
    }
};

inline bool parseBreakpointList(
    std::string_view text,
    BreakpointConfig& config,
    std::string& error
) {
    size_t tokenStart = 0;
    while (tokenStart <= text.size()) {
        const size_t comma = text.find(',', tokenStart);
        const size_t tokenEnd = comma == std::string_view::npos ? text.size() : comma;
        size_t begin = tokenStart;
        size_t end = tokenEnd;

        while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
        while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
        if (begin == end) {
            error = "breakpoint list contains an empty line number";
            return false;
        }

        int line = 0;
        for (size_t i = begin; i < end; ++i) {
            const char ch = text[i];
            if (ch < '0' || ch > '9') {
                error = "breakpoint line numbers must be positive integers";
                return false;
            }
            const int digit = ch - '0';
            if (line > (std::numeric_limits<int>::max() - digit) / 10) {
                error = "breakpoint line number is too large";
                return false;
            }
            line = line * 10 + digit;
        }
        if (line <= 0) {
            error = "breakpoint line numbers must be positive integers";
            return false;
        }

        config.lines.insert(line);
        if (comma == std::string_view::npos) break;
        tokenStart = comma + 1;
    }
    return true;
}

inline std::string defaultSnapshotDirectory(const std::string& sourcePath) {
    return sourcePath.empty() ? "zeus-snapshots" : sourcePath + ".snapshots";
}

template <typename CallFrame, typename IRInstruction>
inline bool isNewSourcePosition(CallFrame& frame, const IRInstruction& instruction) {
    if (instruction.line <= 0) {
        frame.debugLastPc = frame.pc;
        return false;
    }
    const bool isNewPosition = !frame.debugHasSourcePosition ||
        frame.debugLastSourceFileId != instruction.sourceFileId ||
        frame.debugLastLine != instruction.line ||
        frame.debugLastPc >= frame.pc;
    frame.debugLastSourceFileId = instruction.sourceFileId;
    frame.debugLastLine = instruction.line;
    frame.debugLastPc = frame.pc;
    frame.debugHasSourcePosition = true;
    return isNewPosition;
}

} // namespace ZeusDebug
