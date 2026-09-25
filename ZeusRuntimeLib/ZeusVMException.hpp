
#ifndef ZeusVMException_hpp
#define ZeusVMException_hpp

#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include "../ZeusIR/IRType.hpp"
#include "ZeusVariant.hpp"
#include "ZeusVMOperator.hpp"

// Forward declaration
class VM;

namespace VMException {

    // 错误类别枚举
    enum class ErrorCategory {
        VM_RUNTIME,     // VM运行时错误
        TYPE_ERROR,     // 类型错误
        NAME_ERROR,     // 名称错误
        ATTRIBUTE_ERROR,// 属性错误
        INDEX_ERROR,    // 索引错误
        INTERNAL_ERROR  // 内部错误
    };

    // 错误码枚举 (1400-1999)
    enum class ErrorCode {
        // 类型错误 (1400-1499)
        E1400_TYPE_MISMATCH = 1400,
        E1401_NOT_CALLABLE = 1401,
        E1402_NOT_ITERABLE = 1402,
        E1403_INVALID_ARGUMENT_TYPE = 1403,
        E1404_EXPECTED_OBJECT = 1404,
        E1405_EXPECTED_DICT = 1405,
        E1406_EXPECTED_LIST = 1406,
        E1407_EXPECTED_STRING = 1407,
        E1408_EXPECTED_NUMBER = 1408,
        
        // 名称错误 (1500-1599)
        E1500_UNDEFINED_NAME = 1500,
        E1501_UNDEFINED_ATTRIBUTE = 1501,
        E1502_UNDEFINED_METHOD = 1502,
        
        // 属性错误 (1600-1699)
        E1600_ATTRIBUTE_NOT_FOUND = 1600,
        E1601_NO_SUCH_ATTRIBUTE = 1601,
        E1602_ATTRIBUTE_READ_ONLY = 1602,
        
        // 索引错误 (1700-1799)
        E1700_INDEX_OUT_OF_RANGE = 1700,
        E1701_INVALID_INDEX_TYPE = 1701,
        E1702_KEY_NOT_FOUND = 1702,
        
        // IR/编译错误在VM中遇到 (1300-1399)
        E1301_CONST_ASSIGNMENT = 1301,
        
        // VM运行时错误 (1900-1999)
        E1900_STACK_OVERFLOW = 1900,
        E1901_STACK_UNDERFLOW = 1901,
        E1902_NULL_REFERENCE = 1902,
        E1903_DIVISION_BY_ZERO = 1903,
        E1904_INVALID_OPERATION = 1904,
        E1905_INSUFFICIENT_ARGS = 1905,
        
        // 通用错误
        E1000_UNKNOWN_ERROR = 1000,
    };

    // 错误类别转字符串
    inline std::string categoryToString(ErrorCategory cat) {
        switch (cat) {
            case ErrorCategory::VM_RUNTIME: return "Runtime";
            case ErrorCategory::TYPE_ERROR: return "Type";
            case ErrorCategory::NAME_ERROR: return "Name";
            case ErrorCategory::ATTRIBUTE_ERROR: return "Attribute";
            case ErrorCategory::INDEX_ERROR: return "Index";
            default: return "Internal";
        }
    }

    // 调用栈帧结构
    struct StackFrame {
        std::string functionName;
        int line = 0;
        int chunkIndex = -1;
        std::string sourceCode;          // 错误发生的代码行
        std::map<std::string, std::string> localVars;  // 局部变量快照（可选）
        
        std::string toString() const {
            std::ostringstream oss;
            if (!functionName.empty()) {
                oss << "in " << functionName;
            } else {
                oss << "in <top-level>";
            }
            if (line > 0) {
                oss << " (line " << line << ")";
            }
            return oss.str();
        }
    };

    // 线程名解析器类型（用于获取当前线程名）
    using ThreadNameResolverFunc = std::function<std::string()>;
    inline static ThreadNameResolverFunc threadNameResolver = nullptr;
    
    // 设置线程名解析器
    inline static void setThreadNameResolver(ThreadNameResolverFunc resolver) {
        threadNameResolver = resolver;
    }
    
    // 获取当前线程名
    inline static std::string getCurrentThreadName() {
        if (threadNameResolver) {
            return threadNameResolver();
        }
        return "thread-main";
    }

    // 源文件信息结构
    struct SourceFileInfo {
        std::string filePath;
        std::vector<std::string> lines;
    };

    // 多源文件支持：按 ID 索引的源文件信息
    inline static std::vector<SourceFileInfo> sourceFiles;
    
    // 当前源文件 ID（用于编译时）
    inline static int currentSourceFileId = 0;
    
    // 兼容旧接口：全局源代码行缓存(供VM设置) - 使用主文件
    inline static std::vector<std::string>& sourceLines = []() -> std::vector<std::string>& {
        if (sourceFiles.empty()) {
            sourceFiles.push_back(SourceFileInfo{});
        }
        return sourceFiles[0].lines;
    }();
    
    // 兼容旧接口：全局源文件路径(供VM设置) - 使用主文件
    inline static std::string& sourceFilePath = []() -> std::string& {
        if (sourceFiles.empty()) {
            sourceFiles.push_back(SourceFileInfo{});
        }
        return sourceFiles[0].filePath;
    }();
    
    // 注册新的源文件，返回文件 ID
    inline static int registerSourceFile(const std::string& path, const std::string& source) {
        SourceFileInfo info;
        info.filePath = path;
        
        std::istringstream iss(source);
        std::string line;
        while (std::getline(iss, line)) {
            info.lines.push_back(line);
        }
        
        int id = (int)sourceFiles.size();
        sourceFiles.push_back(std::move(info));
        return id;
    }
    
    // 获取源文件信息
    inline static const SourceFileInfo* getSourceFile(int id) {
        if (id >= 0 && id < (int)sourceFiles.size()) {
            return &sourceFiles[id];
        }
        return nullptr;
    }
    
    // 清理所有源文件信息（编译开始时调用）
    inline static void clearSourceFiles() {
        sourceFiles.clear();
        sourceFiles.push_back(SourceFileInfo{});  // 保留主文件槽位
        currentSourceFileId = 0;
    }
    
    inline static void setSourceCode(const std::string& source) {
        if (sourceFiles.empty()) {
            sourceFiles.push_back(SourceFileInfo{});
        }
        sourceFiles[0].lines.clear();
        std::istringstream iss(source);
        std::string line;
        while (std::getline(iss, line)) {
            sourceFiles[0].lines.push_back(line);
        }
    }

    inline static void setSourceFilePath(const std::string& path) {
        if (sourceFiles.empty()) {
            sourceFiles.push_back(SourceFileInfo{});
        }
        sourceFiles[0].filePath = path;
    }

    // 用于存储最后抛出的错误信息（供 try-catch 使用）
    struct LastErrorInfo {
        std::string errorType;      // 错误类型名称
        std::string message;        // 错误消息
        std::string formattedMsg;   // 格式化后的完整错误消息（用于无处理器时输出）
        int line = 0;               // 行号
        bool hasError = false;      // 是否有错误
    };
    inline static LastErrorInfo lastError;

    struct VMException {
        std::string message;
        IRType::IRInstr errorPosition;
        int line = 0;  // deprecated
        int chunkIndex = -1;
        
        // 新增字段
        ErrorCode code = ErrorCode::E1000_UNKNOWN_ERROR;
        ErrorCategory category = ErrorCategory::VM_RUNTIME;
        std::string detailedMessage;
        std::string suggestion;
        std::vector<StackFrame> callStack;  // 调用栈

        VMException(const std::string& msg, int ln = 0, IRType::IRInstr pos = {})
            : message(msg), errorPosition(pos), line(ln), chunkIndex(-1) {}

        // 新增构造函数：带错误码和类别
        VMException(ErrorCode ec, ErrorCategory cat, const std::string& msg, 
                   int ln = 0, IRType::IRInstr pos = {})
            : code(ec), category(cat), message(msg), errorPosition(pos), line(ln), chunkIndex(-1) {}
        
        // 设置详细信息
        VMException& setDetails(const std::string& details) {
            detailedMessage = details;
            return *this;
        }
        
        // 设置修复建议
        VMException& setSuggestion(const std::string& suggest) {
            suggestion = suggest;
            return *this;
        }
        
        // 添加调用栈帧
        VMException& addStackFrame(const StackFrame& frame) {
            callStack.push_back(frame);
            return *this;
        }
        
        // 添加调用栈帧（便捷方法）
        VMException& addStackFrame(const std::string& funcName, int ln, 
                                   const std::string& code = "") {
            StackFrame frame;
            frame.functionName = funcName;
            frame.line = ln;
            frame.sourceCode = code;
            callStack.push_back(frame);
            return *this;
        }

        void _throw(ZeusType::VMStatus& vmstat, int currentChunk = -1) {
            vmstat = ZeusType::VMStatus::Error;
            
            // 保存错误信息供 try-catch 使用
            int actualLine = (errorPosition.line > 0) ? errorPosition.line : line;
            lastError.errorType = categoryToString(category) + "Error";
            lastError.message = message;
            lastError.line = actualLine;
            lastError.hasError = true;
            
            std::ostringstream oss;
            
            // 获取当前线程名
            std::string threadName = getCurrentThreadName();
            
            // 错误头：[类别 Error E错误码] (线程名)
            oss << "[" << categoryToString(category) << " Error E" 
                << static_cast<int>(code) << "] ";
            oss << "(" << threadName << ") ";
            
            // 获取正确的源文件信息
            int sourceFileId = errorPosition.sourceFileId;
            const SourceFileInfo* srcFile = getSourceFile(sourceFileId);
            const std::string& filePath = srcFile ? srcFile->filePath : sourceFilePath;
            const std::vector<std::string>& srcLines = srcFile ? srcFile->lines : sourceFiles[0].lines;
            
            // 文件和位置信息
            if (!filePath.empty()) {
                oss << "File \"" << filePath << "\"";
                if (actualLine > 0) {
                    oss << ", Line " << actualLine;
                }
                oss << ": ";
            } else if (actualLine > 0) {
                oss << "Line " << actualLine << ": ";
            } else if (currentChunk >= 0) {
                oss << "Chunk #" << currentChunk << ": ";
            }
            
            // 主要错误信息
            oss << message << "\n";
            
            // 显示源代码行
            if (actualLine > 0 && actualLine <= (int)srcLines.size()) {
                oss << "    | " << srcLines[actualLine - 1] << "\n";
            } else if (!srcLines.empty() && actualLine > 0) {
                // 行号可能因为自动导入产生偏移,尝试显示最后几行来帮助定位
                oss << "    (Line number may be offset due to auto-imports)\n";
                oss << "    Last few lines of source:\n";
                int start = std::max(1, (int)srcLines.size() - 2);
                for (int i = start; i <= (int)srcLines.size(); ++i) {
                    oss << "    " << i << " | " << srcLines[i - 1] << "\n";
                }
            }
            
            // 详细信息
            if (!detailedMessage.empty()) {
                oss << "  Details: " << detailedMessage << "\n";
            }
            
            // 调用栈（如果有多个栈帧）
            if (callStack.size() > 1) {
                oss << "\nCall Stack (most recent first):\n";
                for (int i = callStack.size() - 1; i >= 0; --i) {
                    oss << "  [" << (callStack.size() - i) << "] " 
                        << callStack[i].toString() << "\n";
                    if (!callStack[i].sourceCode.empty()) {
                        oss << "      | " << callStack[i].sourceCode << "\n";
                    }
                }
            } else if (!callStack.empty()) {
                // 单个栈帧，显示为上下文
                const auto& frame = callStack[0];
                if (!frame.functionName.empty()) {
                    oss << "  Context: " << frame.toString() << "\n";
                }
            }
            
            // 局部变量（仅最顶层栈帧，且有变量时）
            if (!callStack.empty() && !callStack.back().localVars.empty()) {
                oss << "\nLocal variables:\n";
                for (const auto& [name, value] : callStack.back().localVars) {
                    oss << "  " << name << " = " << value << "\n";
                }
            }
            
            // 修复建议
            if (!suggestion.empty()) {
                oss << "\nSuggestion: " << suggestion << "\n";
            }
            
            // IR信息作为次要信息(仅在有指令且没有其他详细信息时显示)
            if (!errorPosition.toString().empty() && detailedMessage.empty()) {
                oss << "  (at IR instruction: " << errorPosition.toString() << ")\n";
            }
            
            // 保存格式化后的消息，不立即打印（由 runChunk 判断是否有处理器后决定是否输出）
            lastError.formattedMsg = oss.str();
        }
    };

    // ==================== 便捷错误构造函数 ====================

    // 类型错误：期望特定类型
    inline VMException typeError_ExpectedType(
        const std::string& expectedType,
        const std::string& actualType,
        int line,
        IRType::IRInstr instr = {}
    ) {
        VMException ex(
            ErrorCode::E1400_TYPE_MISMATCH,
            ErrorCategory::TYPE_ERROR,
            "Expected " + expectedType + " but got " + actualType,
            line, instr
        );
        ex.setDetails("Type mismatch: expected " + expectedType + ", actual " + actualType);
        ex.setSuggestion("Check the type of the value being used.");
        return ex;
    }

    // 属性错误：属性不存在
    inline VMException attributeError_NotFound(
        const std::string& attrName,
        const std::string& objType,
        int line,
        IRType::IRInstr instr = {}
    ) {
        VMException ex(
            ErrorCode::E1600_ATTRIBUTE_NOT_FOUND,
            ErrorCategory::ATTRIBUTE_ERROR,
            "Object of type '" + objType + "' has no attribute '" + attrName + "'",
            line, instr
        );
        ex.setDetails("Attempting to access non-existent attribute '" + attrName + "'");
        return ex;
    }

    // 参数类型错误
    inline VMException typeError_InvalidArgument(
        const std::string& funcName,
        int argIndex,
        const std::string& expectedType,
        const std::string& actualType,
        int line,
        IRType::IRInstr instr = {}
    ) {
        VMException ex(
            ErrorCode::E1403_INVALID_ARGUMENT_TYPE,
            ErrorCategory::TYPE_ERROR,
            funcName + "(): argument " + std::to_string(argIndex + 1) + 
            " must be " + expectedType + ", not " + actualType,
            line, instr
        );
        ex.setDetails("Expected: " + expectedType + "\nGot: " + actualType);
        ex.setSuggestion("Check the argument type passed to " + funcName + "()");
        return ex;
    }

    // 栈不足错误
    inline VMException stackError_Insufficient(
        const std::string& operation,
        int required,
        int available,
        int line,
        IRType::IRInstr instr = {}
    ) {
        VMException ex(
            ErrorCode::E1901_STACK_UNDERFLOW,
            ErrorCategory::VM_RUNTIME,
            operation + " requires " + std::to_string(required) + 
            " items on stack, but only " + std::to_string(available) + " available",
            line, instr
        );
        ex.setDetails("Stack underflow during " + operation);
        return ex;
    }

    // 索引越界错误
    inline VMException indexError_OutOfRange(
        int index,
        int size,
        int line,
        IRType::IRInstr instr = {}
    ) {
        VMException ex(
            ErrorCode::E1700_INDEX_OUT_OF_RANGE,
            ErrorCategory::INDEX_ERROR,
            "Index " + std::to_string(index) + " out of range (size: " + 
            std::to_string(size) + ")",
            line, instr
        );
        ex.setDetails("Valid range: 0 to " + std::to_string(size - 1));
        ex.setSuggestion("Check the index value and ensure it's within bounds.");
        return ex;
    }

    // 键不存在错误
    inline VMException keyError_NotFound(
        const std::string& key,
        int line,
        IRType::IRInstr instr = {}
    ) {
        VMException ex(
            ErrorCode::E1702_KEY_NOT_FOUND,
            ErrorCategory::INDEX_ERROR,
            "Key '" + key + "' not found in dictionary",
            line, instr
        );
        ex.setSuggestion("Check if the key exists using hasKey() before accessing it.");
        return ex;
    }

    // 期望对象句柄错误
    inline VMException typeError_ExpectedObject(
        const std::string& operation,
        const std::string& actualType,
        int line,
        IRType::IRInstr instr = {}
    ) {
        VMException ex(
            ErrorCode::E1404_EXPECTED_OBJECT,
            ErrorCategory::TYPE_ERROR,
            operation + " requires an object, but got " + actualType,
            line, instr
        );
        ex.setDetails("Expected: ObjectHandle\nGot: " + actualType);
        return ex;
    }

} // namespace VMException

#endif
