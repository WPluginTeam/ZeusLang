#ifndef ZEUS_DIAGNOSTICS_HPP
#define ZEUS_DIAGNOSTICS_HPP

#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace ZeusDiagnostics {

struct Diagnostic {
    std::string code;
    std::string message;
    int line = 0;
    int column = 0;
};

inline std::string currentSourcePath;
inline std::string currentSourceCode;
inline size_t reportedCount = 0;

inline void setSourceContext(const std::string& path, const std::string& source) {
    currentSourcePath = path;
    currentSourceCode = source;
}

inline void noteReported() {
    ++reportedCount;
}

inline std::string sourceLine(const std::string& source, int requestedLine) {
    if (requestedLine <= 0) return {};
    size_t begin = 0;
    for (int line = 1; line < requestedLine; ++line) {
        begin = source.find('\n', begin);
        if (begin == std::string::npos) return {};
        ++begin;
    }
    size_t end = source.find('\n', begin);
    if (end == std::string::npos) end = source.size();
    if (end > begin && source[end - 1] == '\r') --end;
    return source.substr(begin, end - begin);
}

inline bool decodeUtf8(const std::string& text, size_t offset, unsigned int& codePoint, size_t& length) {
    if (offset >= text.size()) return false;
    const unsigned char first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        codePoint = first;
        length = 1;
        return true;
    }
    if ((first & 0xE0) == 0xC0 && offset + 1 < text.size()) {
        codePoint = ((first & 0x1F) << 6) | (static_cast<unsigned char>(text[offset + 1]) & 0x3F);
        length = 2;
        return true;
    }
    if ((first & 0xF0) == 0xE0 && offset + 2 < text.size()) {
        codePoint = ((first & 0x0F) << 12) |
                    ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(text[offset + 2]) & 0x3F);
        length = 3;
        return true;
    }
    if ((first & 0xF8) == 0xF0 && offset + 3 < text.size()) {
        codePoint = ((first & 0x07) << 18) |
                    ((static_cast<unsigned char>(text[offset + 1]) & 0x3F) << 12) |
                    ((static_cast<unsigned char>(text[offset + 2]) & 0x3F) << 6) |
                    (static_cast<unsigned char>(text[offset + 3]) & 0x3F);
        length = 4;
        return true;
    }
    codePoint = first;
    length = 1;
    return true;
}

inline int terminalWidth(unsigned int codePoint) {
    if (codePoint == 0) return 0;
    if (codePoint >= 0x0300 && codePoint <= 0x036F) return 0;
    if ((codePoint >= 0x1100 && codePoint <= 0x115F) ||
        (codePoint >= 0x2E80 && codePoint <= 0xA4CF) ||
        (codePoint >= 0xAC00 && codePoint <= 0xD7A3) ||
        (codePoint >= 0xF900 && codePoint <= 0xFAFF) ||
        (codePoint >= 0xFE10 && codePoint <= 0xFE6F) ||
        (codePoint >= 0xFF00 && codePoint <= 0xFF60) ||
        (codePoint >= 0xFFE0 && codePoint <= 0xFFE6) ||
        (codePoint >= 0x1F300 && codePoint <= 0x1FAFF)) {
        return 2;
    }
    return 1;
}

inline int terminalWidthBeforeColumn(const std::string& line, int oneBasedColumn) {
    const int targetCodePoints = oneBasedColumn > 0 ? oneBasedColumn - 1 : 0;
    int codePoints = 0;
    int width = 0;
    for (size_t i = 0; i < line.size() && codePoints < targetCodePoints;) {
        unsigned int codePoint = 0;
        size_t length = 1;
        decodeUtf8(line, i, codePoint, length);
        if (codePoint == '\t') {
            width += 4 - (width % 4);
        } else {
            width += terminalWidth(codePoint);
        }
        i += length;
        ++codePoints;
    }
    return width;
}

inline std::string render(
    const Diagnostic& diagnostic,
    const std::string& path,
    const std::string& source,
    const std::string& category = "error"
) {
    std::ostringstream output;
    output << (path.empty() ? "<source>" : path);
    if (diagnostic.line > 0) {
        output << ':' << diagnostic.line;
        if (diagnostic.column > 0) output << ':' << diagnostic.column;
    }
    output << ": " << category;
    if (!diagnostic.code.empty()) output << '[' << diagnostic.code << ']';
    output << ": " << diagnostic.message;

    const std::string excerpt = sourceLine(source, diagnostic.line);
    if (!excerpt.empty()) {
        output << "\n " << diagnostic.line << " | " << excerpt;
        if (diagnostic.column > 0) {
            output << "\n   | "
                   << std::string(static_cast<size_t>(terminalWidthBeforeColumn(excerpt, diagnostic.column)), ' ')
                   << '^';
        }
    }
    return output.str();
}

inline void append(
    std::vector<Diagnostic>& destination,
    const std::vector<Diagnostic>& source
) {
    destination.insert(destination.end(), source.begin(), source.end());
}

} // namespace ZeusDiagnostics

#endif
