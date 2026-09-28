

#ifndef LexerParser_hpp
#define LexerParser_hpp

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>
#include <string>
#include <variant>
#include "LexerType.hpp"
#include "../ZeusDiagnostics.hpp"

namespace LexerParser {

    enum class TokenStatus {
        STRING_READING,
        BYTES_READING,
        INT_READING,
        FLOAT_READING,
        KEYWORD_READING,

        NOSTAT
    };

    std::vector<LexerType::Token> parse(
        const std::string& input,
        std::vector<ZeusDiagnostics::Diagnostic>* diagnostics = nullptr
    ) {
        std::vector<LexerType::Token> tokens;
        std::string        tempToken;

        std::vector<size_t> lineStarts{0};
        for (size_t i = 0; i < input.size(); ++i) {
            if (input[i] == '\n') lineStarts.push_back(i + 1);
        }

        auto setTokenPosition = [&](LexerType::TokenClass& token, size_t offset) {
            auto lineIt = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
            const size_t lineIndex = static_cast<size_t>(lineIt - lineStarts.begin() - 1);
            const size_t lineStart = lineStarts[lineIndex];
            int column = 1;
            for (size_t i = lineStart; i < offset; ++i) {
                const unsigned char byte = static_cast<unsigned char>(input[i]);
                if ((byte & 0xC0) != 0x80) ++column;
            }
            token.line = static_cast<int>(lineIndex + 1);
            token.column = column;
        };

        auto reportLexError = [&](const std::string& code, const std::string& message, size_t offset) {
            if (diagnostics == nullptr) return;
            ZeusDiagnostics::Diagnostic diagnostic;
            diagnostic.code = code;
            diagnostic.message = message;
            LexerType::TokenClass location;
            setTokenPosition(location, offset);
            diagnostic.line = location.line;
            diagnostic.column = location.column;
            diagnostics->push_back(std::move(diagnostic));
        };
        
        TokenStatus        stat = TokenStatus::NOSTAT;
        int currentLine = 1;  // Track current line number
        char bytesQuoteChar = '\0';  // 记录 bytes 字面量使用的引号类型

        size_t tokenStartOffset = 0;
        size_t blockCommentStart = 0;
        bool blockCommentOpen = false;
        int n = 0;
        while (n < input.size()) {
            char c = input[n++];
            char cNext = (n < input.size()) ? input[n] : '\0';

            /*
                无状态下判断读取
            */
            if (stat == TokenStatus::NOSTAT) {
                // Track newlines (do this AFTER processing to avoid off-by-one)
                if (c == '\n') {
                    currentLine++;
                    continue;
                }
                
                // 处理单行注释：跳过 // 直到行尾
                if (c == '/' && cNext == '/') {
                    while (n < input.size() && input[n] != '\n') {
                        n++;
                    }
                    // Skip the newline and increment line counter
                    if (n < input.size()) {
                        n++; // skip \n
                        currentLine++;
                    }
                    continue;
                }
                
                // 处理多行注释：跳过 /* ... */
                if (c == '/' && cNext == '*') {
                    blockCommentStart = static_cast<size_t>(n - 1);
                    blockCommentOpen = true;
                    ++n; // consume the opening '*'
                    while (n + 1 < input.size()) {
                        if (input[n] == '*' && input[n + 1] == '/') {
                            n += 2; // skip */
                            blockCommentOpen = false;
                            break;
                        }
                        if (input[n] == '\n') {
                            currentLine++;
                        }
                        n++;
                    }
                    continue;
                }

                // 识别 b"..." 或 b'...' 字节串字面量（必须在 isalpha 之前检查）
                if ((c == 'b' || c == 'B') && (cNext == '"' || cNext == '\'')) {
                    tokenStartOffset = static_cast<size_t>(n - 1);
                    bytesQuoteChar = cNext;  // 记录使用的引号类型
                    n++;
                    stat = TokenStatus::BYTES_READING;
                    continue;
                }

                if (c == '"') {
                    tokenStartOffset = static_cast<size_t>(n - 1);
                    stat = TokenStatus::STRING_READING;
                    // tempToken += c;
                    continue;
                }

                if (isalpha(c) || c == '_') {
                    tokenStartOffset = static_cast<size_t>(n - 1);
                    stat = TokenStatus::KEYWORD_READING;
                    tempToken += c;
                    continue;
                }

                if (isdigit(c)) {
                    tokenStartOffset = static_cast<size_t>(n - 1);
                    stat = TokenStatus::INT_READING;
                    tempToken += c;
                    continue;
                }

                if (ispunct(c)) {
                    if (c == '-' && cNext == '>') {
                        auto op = LexerType::Operator("->");
                        setTokenPosition(op, static_cast<size_t>(n - 1));
                        tokens.push_back(op);
                        n++;
                        continue;
                    }
                    if (c == '=' && cNext == '=') {
                        auto op = LexerType::Operator("==");
                        setTokenPosition(op, static_cast<size_t>(n - 1));
                        tokens.push_back(op);
                        n++;
                        continue;
                    }
                    if (c == '!' && cNext == '=') {
                        auto op = LexerType::Operator("!=");
                        setTokenPosition(op, static_cast<size_t>(n - 1));
                        tokens.push_back(op);
                        n++;
                        continue;
                    }
                    if (c == '<' && cNext == '=') {
                        auto op = LexerType::Operator("<=");
                        setTokenPosition(op, static_cast<size_t>(n - 1));
                        tokens.push_back(op);
                        n++;
                        continue;
                    }
                    if (c == '>' && cNext == '=') {
                        auto op = LexerType::Operator(">=");
                        setTokenPosition(op, static_cast<size_t>(n - 1));
                        tokens.push_back(op);
                        n++;
                        continue;
                    }
                    auto op = LexerType::Operator(std::string(1, c));
                    setTokenPosition(op, static_cast<size_t>(n - 1));
                    tokens.push_back(op);
                }
            }

            if (stat == TokenStatus::BYTES_READING) {
                if (c == bytesQuoteChar) {
                    // 字节串结束 (只有遇到与开始相同的引号才结束)
                    std::vector<uint8_t> bytesData;
                    for (char ch : tempToken) {
                        bytesData.push_back(static_cast<uint8_t>(ch));
                    }
                    auto bytesLit = LexerType::BytesLiteral(bytesData);
                    setTokenPosition(bytesLit, tokenStartOffset);
                    tokens.push_back(bytesLit);
                    tempToken.clear();
                    bytesQuoteChar = '\0';  // 重置引号类型
                    stat = TokenStatus::NOSTAT;
                } else if (c == '\n') {
                    // 跨行字节串：记录换行符并更新行号
                    tempToken += c;
                    currentLine++;
                } else if (c == '\\' && n < input.size()) {
                    // 字节串转义序列
                    char nextChar = input[n++];
                    switch (nextChar) {
                        case 'n':  tempToken += '\n'; break;
                        case 't':  tempToken += '\t'; break;
                        case 'r':  tempToken += '\r'; break;
                        case '\\': tempToken += '\\'; break;
                        case '"':  tempToken += '"';  break;
                        case '\'': tempToken += '\''; break;
                        case '0':  tempToken += '\0'; break;
                        case 'x': {
                            // \xHH 十六进制字节
                            if (n + 1 < input.size()) {
                                std::string hex = "";
                                hex += input[n++];
                                hex += input[n++];
                                try {
                                    int value = std::stoi(hex, nullptr, 16);
                                    tempToken += static_cast<char>(value);
                                } catch (...) {
                                    tempToken += "\\x" + hex;
                                }
                            }
                            break;
                        }
                        default:
                            tempToken += '\\';
                            tempToken += nextChar;
                            break;
                    }
                } else {
                    tempToken += c;
                }
                continue;
            }

            if (stat == TokenStatus::STRING_READING) {
                if (c == '"') {
                    // tempToken += c;
                    auto val = LexerType::Value(tempToken);
                    setTokenPosition(val, tokenStartOffset);
                    tokens.push_back(val);
                    tempToken.clear();
                    stat = TokenStatus::NOSTAT;
                } else if (c == '\n') {
                    // 跨行字符串：记录换行符并更新行号
                    tempToken += c;
                    currentLine++;
                } else if (c == '\\' && n < input.size()) {
                    // 处理转义序列
                    char nextChar = input[n++];
                    switch (nextChar) {
                        case 'n':  tempToken += '\n'; break;  // 换行
                        case 't':  tempToken += '\t'; break;  // 制表符
                        case 'r':  tempToken += '\r'; break;  // 回车
                        case '\\': tempToken += '\\'; break;  // 反斜杠
                        case '"':  tempToken += '"';  break;  // 双引号
                        case '\'': tempToken += '\''; break;  // 单引号
                        case '0':  tempToken += '\0'; break;  // 空字符
                        case 'b':  tempToken += '\b'; break;  // 退格
                        case 'f':  tempToken += '\f'; break;  // 换页
                        case 'v':  tempToken += '\v'; break;  // 垂直制表符
                        default:
                            // 未知转义序列，保留原样（或可以选择报错）
                            tempToken += '\\';
                            tempToken += nextChar;
                            break;
                    }
                } else {
                    tempToken += c;
                }
                continue;
            }

            if (stat == TokenStatus::INT_READING) {
                if (c == '.' && isdigit(cNext)) {
                    // 开始读取浮点数的小数部分
                    stat = TokenStatus::FLOAT_READING;
                    tempToken += c;
                    continue;  // 继续下一个字符，避免重复处理
                } else if (!isdigit(c)) {
                    // 整数结束，使用 BigInteger 直接从字符串构造
                    auto val = LexerType::Value(ZeusType::BigInteger(tempToken));
                    setTokenPosition(val, tokenStartOffset);
                    tokens.push_back(val);
                    tempToken.clear();
                    stat = TokenStatus::NOSTAT;
                    n--;
                    continue;
                } else {
                    tempToken += c;
                    continue;
                }
            }

            if (stat == TokenStatus::FLOAT_READING) {
                if (!isdigit(c)) {
                    // 浮点数结束
                    auto val = LexerType::Value(ZeusType::BigDecimal(tempToken));
                    setTokenPosition(val, tokenStartOffset);
                    tokens.push_back(val);
                    tempToken.clear();
                    stat = TokenStatus::NOSTAT;
                    n--;
                    continue;
                } else {
                    tempToken += c;
                    continue;
                }
            }

            if (stat == TokenStatus::KEYWORD_READING) {
                if (!isalnum(c) && c != '_') {
                    // 特殊处理:将 and 和 or 转换为运算符而非关键字
                    if (tempToken == "and" || tempToken == "or") {
                        auto op = LexerType::Operator(tempToken);
                        setTokenPosition(op, tokenStartOffset);
                        tokens.push_back(op);
                    } else {
                        auto kw = LexerType::Keyword(tempToken);
                        setTokenPosition(kw, tokenStartOffset);
                        tokens.push_back(kw);
                    }
                    tempToken.clear();
                    stat = TokenStatus::NOSTAT;
                    n--;
                } else {
                    tempToken += c;
                }
                continue;
            }
        }

        if (blockCommentOpen) {
            reportLexError("L0001", "unterminated block comment", blockCommentStart);
        }

        if (stat == TokenStatus::STRING_READING) {
            reportLexError("L0002", "unterminated string literal", tokenStartOffset);
        } else if (stat == TokenStatus::BYTES_READING) {
            reportLexError("L0003", "unterminated bytes literal", tokenStartOffset);
        } else if (stat == TokenStatus::KEYWORD_READING) {
            if (tempToken == "and" || tempToken == "or") {
                auto op = LexerType::Operator(tempToken);
                setTokenPosition(op, tokenStartOffset);
                tokens.push_back(op);
            } else {
                auto kw = LexerType::Keyword(tempToken);
                setTokenPosition(kw, tokenStartOffset);
                tokens.push_back(kw);
            }
        } else if (stat == TokenStatus::INT_READING) {
            auto value = LexerType::Value(ZeusType::BigInteger(tempToken));
            setTokenPosition(value, tokenStartOffset);
            tokens.push_back(value);
        } else if (stat == TokenStatus::FLOAT_READING) {
            auto value = LexerType::Value(ZeusType::BigDecimal(tempToken));
            setTokenPosition(value, tokenStartOffset);
            tokens.push_back(value);
        }

        return tokens;

    }


}

#endif
