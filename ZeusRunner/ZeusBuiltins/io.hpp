#pragma once

// ZeusBuiltins I/O Functions
// print, input 等输入输出函数

#include "core.hpp"

namespace ZeusBuiltins {

    // 调试缓存：delayPrint 收集的日志
    inline std::vector<std::string> g_delayPrintBuffer;

    // =====================================================================
    // 辅助函数：递归展开打印单个值
    // =====================================================================

    inline void _printValue(const ZeusType::UniversalVariant& v, int depth = 0) {
        if (std::holds_alternative<ZeusType::BigInteger>(v)) {
            std::cout << std::get<ZeusType::BigInteger>(v).toString();
        }
        else if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
            std::cout << std::get<ZeusType::BigDecimal>(v).toString();
        }
        else if (std::holds_alternative<bool>(v)) {
            std::cout << (std::get<bool>(v) ? "true" : "false");
        }
        else if (std::holds_alternative<std::string>(v)) {
            std::cout << std::get<std::string>(v);
        }
        else if (std::holds_alternative<LexerType::Value>(v)) {
            std::cout << std::get<LexerType::Value>(v).toString();
        }
        else if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
            std::cout << "<closure:#" << std::get<ZeusType::ClosureHandle>(v).id << ">";
        }
        else if (std::holds_alternative<ZeusType::NativeClosureHandle>(v)) {
            std::cout << "<closure>";
        }
        else if (std::holds_alternative<ZeusType::NativeFuncHandle>(v)) {
            std::cout << "<native:#" << std::get<ZeusType::NativeFuncHandle>(v).id << ">";
        }
        else if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
            const auto& objHandle = std::get<ZeusType::ObjectHandle>(v);
            std::string className = "object";

            if (g_vmObjects && g_vmClasses && objHandle.id >= 0 && objHandle.id < (int)g_vmObjects->size()) {
                const auto& obj = (*g_vmObjects)[objHandle.id];

                if (obj.classId >= 0 && obj.classId < (int)g_vmClasses->size()) {
                    className = (*g_vmClasses)[obj.classId].name;
                }
            }

            std::cout << "<" << className << ":#" << objHandle.id << ">";
        }
        else if (std::holds_alternative<ZeusType::Variant>(v)) {
            const auto& variant = std::get<ZeusType::Variant>(v);
            
            if (std::holds_alternative<ZeusType::Null>(variant)) {
                std::cout << "nil";
            }
            else if (std::holds_alternative<ZeusType::Array>(variant)) {
                const auto& arr = std::get<ZeusType::Array>(variant);

                if (!arr.impl || arr.impl->data.empty()) {
                    std::cout << "[]";
                } else {
                    std::cout << "[";

                    for (size_t i = 0; i < arr.impl->data.size(); ++i) {
                        _printValue(arr.impl->data[i], depth + 1);

                        if (i + 1 < arr.impl->data.size()) std::cout << ", ";
                    }

                    std::cout << "]";
                }
            }
            else if (std::holds_alternative<ZeusType::Object>(variant)) {
                const auto& obj = std::get<ZeusType::Object>(variant);

                if (!obj.impl || obj.impl->data.empty()) {
                    std::cout << "{}";
                } else {
                    std::cout << "{";

                    bool first = true;
                    for (size_t i = 0; i < obj.impl->order.size(); ++i) {
                        if (!obj.impl->isLiveOrderEntry(i)) continue;
                        const std::string& key = obj.impl->order[i];
                        auto found = obj.impl->data.find(key);
                        if (found == obj.impl->data.end()) continue;
                        if (!first) std::cout << ", ";
                        first = false;
                        std::cout << key << ": ";
                        _printValue(found->second, depth + 1);
                    }

                    std::cout << "}";
                }
            }
            else if (std::holds_alternative<ZeusType::Bytes>(variant)) {
                const auto& bytes = std::get<ZeusType::Bytes>(variant);
                std::cout << "b\"";
                const size_t maxPrintLen = 60;
                size_t charCount = 0;
                bool truncated = false;
                
                for (size_t i = 0; i < bytes.data.size(); ++i) {
                    if (charCount >= maxPrintLen) {
                        truncated = true;
                        break;
                    }
                    
                    uint8_t byte = bytes.data[i];

                    if (byte >= 32 && byte < 127 && byte != '\\' && byte != '"') {
                        std::cout << static_cast<char>(byte);
                        charCount++;
                    } else {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\x%02x", byte);
                        std::cout << buf;
                        charCount += 4;
                    }
                }
                
                if (truncated) {
                    std::cout << "...";
                }

                std::cout << "\"";
            }
            else {
                std::cout << "<variant>";
            }
        }
        else if (std::holds_alternative<ZeusType::Bytes>(v)) {
            const auto& bytes = std::get<ZeusType::Bytes>(v);
            std::cout << "b\"";
            const size_t maxPrintLen = 60;
            size_t charCount = 0;
            bool truncated = false;
            
            for (size_t i = 0; i < bytes.data.size(); ++i) {
                if (charCount >= maxPrintLen) {
                    truncated = true;
                    break;
                }
                
                uint8_t byte = bytes.data[i];

                if (byte >= 32 && byte < 127 && byte != '\\' && byte != '"') {
                    std::cout << static_cast<char>(byte);
                    charCount++;
                } else {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", byte);
                    std::cout << buf;
                    charCount += 4;
                }
            }
            
            if (truncated) {
                std::cout << "...";
            }

            std::cout << "\"";
        }
        else {
            std::cout << "<unknown>";
        }
    }

    // =====================================================================
    // I/O 函数实现
    // =====================================================================

    // print: 打印所有参数
    inline ZeusType::UniversalVariant builtin_print(const std::vector<ZeusType::UniversalVariant>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            _printValue(args[i]);

            if (i + 1 < args.size()) std::cout << ' ';
        }

        std::cout << '\n';
        return ZeusType::UniversalVariant(LexerType::Value(ZeusType::BigInteger(0)));
    }

    // raw_print: 原始打印（字符串不加引号）
    inline ZeusType::UniversalVariant builtin_raw_print(const std::vector<ZeusType::UniversalVariant>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            const auto &v = args[i];

            if (const std::string* sp = std::get_if<std::string>(&v)) {
                std::cout << *sp;
            }
            else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&v)) {
                if (std::holds_alternative<std::string>(vp->data)) {
                    std::cout << std::get<std::string>(vp->data);
                } else {
                    std::cout << ZeusVMUtils::valueToDebugString(v);
                }
            } else {
                std::cout << ZeusVMUtils::valueToDebugString(v);
            }

            std::cout << " ";
        }

        std::cout << '\n';
        return ZeusType::UniversalVariant(LexerType::Value(ZeusType::BigInteger(0)));
    }

    // delayPrint: 打印并缓存（用于调试）
    inline ZeusType::UniversalVariant builtin_delayPrint(const std::vector<ZeusType::UniversalVariant>& args) {
        std::string line;

        for (size_t i = 0; i < args.size(); ++i) {
            std::string part = _toStringOne(args[i]);

            if (i > 0) {
                line += ' ';
            }

            line += part;
        }

        ZEUS_DEBUG_PRINT("%s", line.c_str());
        g_delayPrintBuffer.push_back(line);

        return ZeusType::UniversalVariant(LexerType::Value(0));
    }

    // _write: 直接输出字符串，不添加任何格式（用于print函数内部）
    inline ZeusType::UniversalVariant builtin_write(const std::vector<ZeusType::UniversalVariant>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            const auto &v = args[i];

            if (const std::string* sp = std::get_if<std::string>(&v)) {
                std::cout << *sp;
            }
            else if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&v)) {
                if (std::holds_alternative<std::string>(vp->data)) {
                    std::cout << std::get<std::string>(vp->data);
                } else {
                    std::cout << ZeusVMUtils::valueToDebugString(v);
                }
            } else {
                std::cout << ZeusVMUtils::valueToDebugString(v);
            }
        }

        return ZeusType::UniversalVariant(LexerType::Value(ZeusType::BigInteger(0)));
    }

    // input: 从标准输入读取一行
    inline ZeusType::UniversalVariant builtin_input(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 0) {
            throw std::runtime_error("input expects no arguments");
        }

        std::string line;

        if (std::getline(std::cin, line)) {
            return ZeusType::UniversalVariant(line);
        }
        
        return ZeusType::UniversalVariant(std::string(""));
    }

    // =====================================================================
    // 注册函数
    // =====================================================================

    inline void registerIOBuiltins() {
        registerBuiltin("__builtin_print", builtin_raw_print, 0, -1);
        registerBuiltin("raw_print", builtin_print, 0, -1);
        registerBuiltin("_write", builtin_write, 0, -1);
        registerBuiltin("delayPrint", builtin_delayPrint, 0, -1);
        registerBuiltin("input", builtin_input, 0, 0);
    }

}  // namespace ZeusBuiltins
