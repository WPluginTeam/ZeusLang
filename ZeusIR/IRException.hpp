
// 轻量 IR 异常：不抛出 C++ runtime_error，只通过 _throw 改写编译状态并统一打印

#pragma once

#include <string>
#include <sstream>
#include <iostream>
#include <vector>
#include <map>
#include "IRType.hpp"
#include "../ZeusParser/TreeParser.hpp"
#include "../ZeusDiagnostics.hpp"

namespace IRExceptionNS {

    // 错误类别枚举
    enum class ErrorCategory {
        LEXER,          // 词法错误
        PARSER,         // 语法错误
        IR_COMPILER,    // IR编译错误
        TYPE_ERROR,     // 类型错误
        NAME_ERROR,     // 名称错误
        IMPORT_ERROR,   // 导入错误
        INTERNAL_ERROR  // 内部错误
    };

    // 错误码枚举 (1000-1999)
    enum class ErrorCode {
        // 通用错误 (1000-1099)
        E1000_UNKNOWN_ERROR = 1000,
        E1001_INTERNAL_ERROR = 1001,
        
        // 词法错误 (1100-1199)
        E1100_INVALID_TOKEN = 1100,
        E1101_UNTERMINATED_STRING = 1101,
        E1102_INVALID_NUMBER = 1102,
        
        // 语法错误 (1200-1299)
        E1200_UNEXPECTED_TOKEN = 1200,
        E1201_MISSING_PAREN = 1201,
        E1202_MISSING_BRACE = 1202,
        E1203_INVALID_SYNTAX = 1203,
        
        // IR编译错误 (1300-1399)
        E1300_UNDEFINED_VARIABLE = 1300,
        E1301_CONST_ASSIGNMENT = 1301,
        E1302_INVALID_OPERATOR = 1302,
        E1303_FUNCTION_NOT_FOUND = 1303,
        E1304_INVALID_EXPRESSION = 1304,
        E1305_DUPLICATE_DECLARATION = 1305,
        
        // 导入错误 (1800-1899)
        E1800_MODULE_NOT_FOUND = 1800,
        E1801_IMPORT_CYCLE = 1801,
        E1802_PACKAGE_NOT_FOUND = 1802,
    };

    // 错误类别转字符串
    inline std::string categoryToString(ErrorCategory cat) {
        switch (cat) {
            case ErrorCategory::LEXER: return "Lexer";
            case ErrorCategory::PARSER: return "Parser";
            case ErrorCategory::IR_COMPILER: return "Compiler";
            case ErrorCategory::TYPE_ERROR: return "Type";
            case ErrorCategory::NAME_ERROR: return "Name";
            case ErrorCategory::IMPORT_ERROR: return "Import";
            default: return "Internal";
        }
    }

    struct IRException {
        std::string message;
        int line = 0;
        TreeParser::Node errorPosition; // 用于 token 串打印
        
        // 新增字段
        ErrorCode code = ErrorCode::E1000_UNKNOWN_ERROR;
        ErrorCategory category = ErrorCategory::IR_COMPILER;
        std::string detailedMessage;
        std::string suggestion;

        IRException() = default;

        IRException(const std::string& msg, int ln, const TreeParser::Node& node)
            : message(msg), line(ln), errorPosition(node) {}

        // 新增构造函数：带错误码和类别
        IRException(ErrorCode ec, ErrorCategory cat, const std::string& msg, 
                   int ln, const TreeParser::Node& node)
            : code(ec), category(cat), message(msg), line(ln), errorPosition(node) {}
        
        // 设置详细信息
        IRException& setDetails(const std::string& details) {
            detailedMessage = details;
            return *this;
        }
        
        // 设置修复建议
        IRException& setSuggestion(const std::string& suggest) {
            suggestion = suggest;
            return *this;
        }

        static std::string tokensToString(const TreeParser::Node& n) {
            std::ostringstream oss;
            for (const auto& tk : n.tokens) {
                oss << std::visit([](auto&& arg){ return arg.toString(); }, tk) << ' ';
            }
            return oss.str();
        }

        void _throw(IRType::IRCompilerStatus& stat) {
            stat = IRType::IRCompilerStatus::Stopped;

            std::ostringstream oss;
            int actualLine = errorPosition.sourceLine;
            int actualColumn = errorPosition.sourceColumn;
            if (!errorPosition.tokens.empty()) {
                std::visit([&](const auto& token) {
                    if (actualLine <= 0) actualLine = token.line;
                    if (actualColumn <= 0) actualColumn = token.column;
                }, errorPosition.tokens.front());
            }
            if (actualLine <= 0) actualLine = line;
            const std::string codeLabel =
                categoryToString(category) + " E" + std::to_string(static_cast<int>(code));
            oss << ZeusDiagnostics::render(
                { codeLabel, message, actualLine, actualColumn },
                ZeusDiagnostics::currentSourcePath,
                ZeusDiagnostics::currentSourceCode
            ) << "\n";
            
            // Token信息（作为代码上下文）
            std::string tokens = tokensToString(errorPosition);
            if (!tokens.empty()) {
                oss << "    | " << tokens << "\n";
            }
            
            // 详细信息
            if (!detailedMessage.empty()) {
                oss << "  Details: " << detailedMessage << "\n";
            }
            
            // 修复建议
            if (!suggestion.empty()) {
                oss << "  Suggestion: " << suggestion << "\n";
            }

            std::cout << oss.str() << std::endl;
            ZeusDiagnostics::noteReported();
        }
    };

}

// ==================== 便捷错误构造宏 ====================

// 简化宏：构造临时 IRException 并调用 _throw（不进行 C++ 异常抛出）
#define IR_THROW_NODE(LINE, NODE, MSG, STATUS_REF)                 \
    do {                                                           \
        IRExceptionNS::IRException _ire(MSG, (LINE), (NODE));      \
        _ire._throw(STATUS_REF);                                   \
    } while(0)

// 带错误码的版本
#define IR_THROW_WITH_CODE(CODE, CATEGORY, LINE, NODE, MSG, STATUS_REF)  \
    do {                                                                 \
        IRExceptionNS::IRException _ire((CODE), (CATEGORY), (MSG), (LINE), (NODE)); \
        _ire._throw(STATUS_REF);                                         \
    } while(0)

// 未定义变量错误
#define IR_THROW_UNDEFINED_VAR(LINE, NODE, VAR_NAME, STATUS_REF)         \
    do {                                                                 \
        IRExceptionNS::IRException _ire(                                 \
            IRExceptionNS::ErrorCode::E1300_UNDEFINED_VARIABLE,         \
            IRExceptionNS::ErrorCategory::NAME_ERROR,                   \
            "Undefined variable '" + std::string(VAR_NAME) + "'",       \
            (LINE), (NODE)                                              \
        );                                                              \
        _ire.setSuggestion("Check for typos or declare the variable before using it."); \
        _ire._throw(STATUS_REF);                                        \
    } while(0)

// 常量赋值错误
#define IR_THROW_CONST_ASSIGNMENT(LINE, NODE, VAR_NAME, STATUS_REF)      \
    do {                                                                 \
        IRExceptionNS::IRException _ire(                                 \
            IRExceptionNS::ErrorCode::E1301_CONST_ASSIGNMENT,           \
            IRExceptionNS::ErrorCategory::IR_COMPILER,                  \
            "Cannot assign to constant '" + std::string(VAR_NAME) + "'", \
            (LINE), (NODE)                                              \
        );                                                              \
        _ire.setSuggestion("Use 'let' instead of 'const' if you need to modify this variable."); \
        _ire._throw(STATUS_REF);                                        \
    } while(0)

// 无效语法错误
#define IR_THROW_INVALID_SYNTAX(LINE, NODE, MSG, STATUS_REF)             \
    do {                                                                 \
        IRExceptionNS::IRException _ire(                                 \
            IRExceptionNS::ErrorCode::E1203_INVALID_SYNTAX,             \
            IRExceptionNS::ErrorCategory::PARSER,                       \
            (MSG),                                                      \
            (LINE), (NODE)                                              \
        );                                                              \
        _ire._throw(STATUS_REF);                                        \
    } while(0)
