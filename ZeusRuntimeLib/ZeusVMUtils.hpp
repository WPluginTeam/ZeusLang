// 简单的 VM 辅助工具函数：与执行循环无耦合，可被算术 / 比较等指令复用。
#pragma once

#include <variant>
#include <string>
#include <deque>
#include <climits>
#include "../ZeusParser/LexerType.hpp"      // LexerType::Value
#include "ZeusVariant.hpp"                // ZeusType::UniversalVariant

// 全局指针声明：访问VM的classes和objects（定义在ZeusBuiltins.hpp中）
inline std::vector<ZeusType::ClassInfo>* g_vmClassesPtr = nullptr;
inline std::deque<ZeusType::ObjectInstance>* g_vmObjectsPtr = nullptr;

namespace ZeusVMUtils {

inline int bigIntegerToInt(const ZeusType::BigInteger& big) {
    return ZeusType::bigIntegerToInt(big);
}

inline bool tryGetIntFromValue(const LexerType::Value& v, int& out) {
    if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
        out = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(v.data));
        return true;
    }
    return false;
}

inline bool tryGetInt(const ZeusType::UniversalVariant& u, int& out) {
    if (std::holds_alternative<ZeusType::BigInteger>(u)) {
        out = ZeusVMUtils::bigIntegerToInt(std::get<ZeusType::BigInteger>(u));
        return true;
    }
    if (std::holds_alternative<LexerType::Value>(u)) {
        return tryGetIntFromValue(std::get<LexerType::Value>(u), out);
    }
    return false;
}

// 检查 UniversalVariant 是否为 nil (ZeusType::Null)
inline bool isNull(const ZeusType::UniversalVariant& u) {
    if (!std::holds_alternative<ZeusType::Variant>(u)) return false;
    const auto& var = std::get<ZeusType::Variant>(u);
    return std::holds_alternative<ZeusType::Null>(var);
}

// 调试展开辅助（放在底部，依赖上方数组工具）

// 将 UniversalVariant 尝试转成 LexerType::Value（复制语义）
inline bool convertToValue(const ZeusType::UniversalVariant& u, LexerType::Value& out) {
    if (std::holds_alternative<LexerType::Value>(u)) { out = std::get<LexerType::Value>(u); return true; }
    if (std::holds_alternative<ZeusType::BigInteger>(u))   { out = LexerType::Value(std::get<ZeusType::BigInteger>(u)); return true; }
    if (std::holds_alternative<ZeusType::BigDecimal>(u))          { out = LexerType::Value(std::get<ZeusType::BigDecimal>(u)); return true; }
    if (std::holds_alternative<bool>(u))           { out = LexerType::Value(std::get<bool>(u)); return true; }
    if (std::holds_alternative<std::string>(u))    { out = LexerType::Value(std::get<std::string>(u)); return true; }
    return false; // 其它类型暂不支持
}

// 提取数值（BigInteger/BigDecimal），失败返回 false
inline bool extractNumber(const ZeusType::UniversalVariant& u, ZeusType::BigDecimal& out) {
    if (std::holds_alternative<LexerType::Value>(u)) {
        const auto& v = std::get<LexerType::Value>(u);
        if (std::holds_alternative<ZeusType::BigInteger>(v.data))   { out = ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(v.data));   return true; }
        if (std::holds_alternative<ZeusType::BigDecimal>(v.data)) { out = std::get<ZeusType::BigDecimal>(v.data);        return true; }
        return false;
    }
    if (std::holds_alternative<ZeusType::BigInteger>(u))   { out = ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(u));   return true; }
    if (std::holds_alternative<ZeusType::BigDecimal>(u)) { out = std::get<ZeusType::BigDecimal>(u);        return true; }
    return false;
}

// 将UniversalVariant转换为布尔值(用于逻辑运算)
inline bool toBool(const ZeusType::UniversalVariant& u) {
    // 处理直接的bool值
    if (std::holds_alternative<bool>(u)) {
        return std::get<bool>(u);
    }
    
    // 处理LexerType::Value中的bool
    if (std::holds_alternative<LexerType::Value>(u)) {
        const auto& v = std::get<LexerType::Value>(u);
        if (std::holds_alternative<bool>(v.data)) {
            return std::get<bool>(v.data);
        }
        // 数字: 0为false, 非0为true
        if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
            return std::get<ZeusType::BigInteger>(v.data) != ZeusType::BigInteger(0);
        }
        if (std::holds_alternative<ZeusType::BigDecimal>(v.data)) {
            return std::get<ZeusType::BigDecimal>(v.data) != ZeusType::BigDecimal(0);
        }
        // 字符串: 空字符串为false, 非空为true
        if (std::holds_alternative<std::string>(v.data)) {
            return !std::get<std::string>(v.data).empty();
        }
    }
    
    // 处理直接的数字类型
    if (std::holds_alternative<ZeusType::BigInteger>(u)) {
        return std::get<ZeusType::BigInteger>(u) != ZeusType::BigInteger(0);
    }
    if (std::holds_alternative<ZeusType::BigDecimal>(u)) {
        return std::get<ZeusType::BigDecimal>(u) != ZeusType::BigDecimal(0);
    }
    
    // 处理字符串
    if (std::holds_alternative<std::string>(u)) {
        return !std::get<std::string>(u).empty();
    }
    
    // 处理Variant类型(nil)
    if (std::holds_alternative<ZeusType::Variant>(u)) {
        const auto& var = std::get<ZeusType::Variant>(u);
        if (std::holds_alternative<ZeusType::Null>(var)) {
            return false; // nil为false
        }
        // 数组: 空数组为false, 非空为true
        if (std::holds_alternative<ZeusType::Array>(var)) {
            const auto& arr = std::get<ZeusType::Array>(var);
            return arr.impl && !arr.impl->data.empty();
        }
    }
    
    // 其他类型(对象、闭包等)默认为true
    return true;
}

inline bool isNumericValue(const LexerType::Value& v) {
    // P0：直接判分支，零分配（原来调 getStringType() 构造 std::string）
    return std::holds_alternative<ZeusType::BigInteger>(v.data) ||
           std::holds_alternative<ZeusType::BigDecimal>(v.data);
}

// P0：int 快路径 —— Value(int) 直取 int64（小整数零分配，大整数按位换算）
inline bool tryGetInt64FromValue(const LexerType::Value& v, int64_t& out) {
    if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
        return std::get<ZeusType::BigInteger>(v.data).tryToInt64(out);
    }
    return false;
}

// P0：UniversalVariant(int) 直取 int64，支持 Value 包装与裸 BigInteger
inline bool tryGetInt64(const ZeusType::UniversalVariant& u, int64_t& out) {
    if (std::holds_alternative<ZeusType::BigInteger>(u)) {
        return std::get<ZeusType::BigInteger>(u).tryToInt64(out);
    }
    if (std::holds_alternative<LexerType::Value>(u)) {
        return tryGetInt64FromValue(std::get<LexerType::Value>(u), out);
    }
    return false;
}

inline bool isArray(const ZeusType::UniversalVariant& u) {
    if (!std::holds_alternative<ZeusType::Variant>(u)) return false;
    const auto &var = std::get<ZeusType::Variant>(u);
    return std::holds_alternative<ZeusType::Array>(var);
}

inline ZeusType::Array* getArray(ZeusType::UniversalVariant& u) {
    if (!std::holds_alternative<ZeusType::Variant>(u)) return nullptr;
    auto &var = std::get<ZeusType::Variant>(u);
    if (!std::holds_alternative<ZeusType::Array>(var)) return nullptr;
    return &std::get<ZeusType::Array>(var);
}

inline const ZeusType::Array* getArrayConst(const ZeusType::UniversalVariant& u) {
    if (!std::holds_alternative<ZeusType::Variant>(u)) return nullptr;
    const auto &var = std::get<ZeusType::Variant>(u);
    if (!std::holds_alternative<ZeusType::Array>(var)) return nullptr;
    return &std::get<ZeusType::Array>(var);
}

inline size_t arrayLength(const ZeusType::UniversalVariant& u) {
    auto arr = getArrayConst(u);
    if (!arr) return 0;
    if (!arr->impl) return 0;
    return arr->impl->data.size();
}

inline ZeusType::UniversalVariant arrayRead(const ZeusType::UniversalVariant& u, int i) {
    auto arr = getArrayConst(u);
    if (!arr || !arr->impl) return LexerType::Value(0);
    if (i < 0 || (size_t)i >= arr->impl->data.size()) return LexerType::Value(0);
    return arr->impl->data[i];
}

inline bool arrayWrite(ZeusType::UniversalVariant& u, int i, const ZeusType::UniversalVariant& v) {
    auto arr = getArray(u);
    if (!arr || !arr->impl) return false;
    if (i < 0 || (size_t)i >= arr->impl->data.size()) return false;
    arr->impl->data[i] = v;
    return true;
}

inline ZeusType::UniversalVariant makeArray(int n, std::vector<ZeusType::UniversalVariant>& stack) {
    ZeusType::Array arr;
    arr.impl = std::make_shared<ZeusType::ArrayData>();
    if (n > 0) {
        arr.impl->data.resize(n);
        for (int i = n - 1; i >= 0; --i) {
            arr.impl->data[i] = stack.back();
            stack.pop_back();
        }
    }
    ZeusType::Variant var = arr;
    return ZeusType::UniversalVariant(var);
}

// ---------------- 调试展开 ----------------
inline std::string arrayToDebugString(const ZeusType::UniversalVariant& u);
inline std::string valueToDebugString(const ZeusType::UniversalVariant& u);

inline std::string arrayToDebugString(const ZeusType::UniversalVariant& u) {
    const ZeusType::Array* arr = getArrayConst(u);
    if (!arr || !arr->impl) {
        return "[]";
    }

    const auto& data = arr->impl->data;
    std::string out = "[";
    for (size_t i = 0; i < data.size(); ++i) {
        out += valueToDebugString(data[i]);
        if (i + 1 < data.size()) {
            out += ", ";
        }
    }
    out += "]";
    return out;
}

inline std::string valueToDebugString(const ZeusType::UniversalVariant& u) {
    if (std::holds_alternative<ZeusType::BigInteger>(u)) {
        return std::get<ZeusType::BigInteger>(u).toString();
    }
    if (std::holds_alternative<ZeusType::BigDecimal>(u)) {
        return std::get<ZeusType::BigDecimal>(u).toString();
    }
    if (std::holds_alternative<bool>(u)) {
        return std::get<bool>(u) ? "true" : "false";
    }
    if (std::holds_alternative<std::string>(u)) {
        return std::string("\"") + std::get<std::string>(u) + "\"";
    }
    if (std::holds_alternative<LexerType::Value>(u)) {
        return std::get<LexerType::Value>(u).toString();
    }
    // nil: UniversalVariant 不直接持有 Null，而是 Variant( Null )
    if (std::holds_alternative<ZeusType::Variant>(u)) {
        const ZeusType::Variant &varx = std::get<ZeusType::Variant>(u);
        if (std::holds_alternative<ZeusType::Null>(varx)) {
            return "nil";
        }
    }
    if (std::holds_alternative<ZeusType::ClosureHandle>(u)) {
        return std::string("<closure:#") + std::to_string(std::get<ZeusType::ClosureHandle>(u).id) + ">";
    }
    if (std::holds_alternative<ZeusType::NativeFuncHandle>(u)) {
        return std::string("<native:#") + std::to_string(std::get<ZeusType::NativeFuncHandle>(u).id) + ">";
    }
    if (std::holds_alternative<ZeusType::ClassHandle>(u)) {
        return std::string("<class:#") + std::to_string(std::get<ZeusType::ClassHandle>(u).id) + ">";
    }
    if (std::holds_alternative<ZeusType::LLVMClassHandle>(u)) {
        const auto& handle = std::get<ZeusType::LLVMClassHandle>(u);
        if (!handle.state) return "<object:invalid>";
        const bool alive = handle.state->object.load(std::memory_order_acquire) != nullptr;
        const std::string typeName = handle.state->typeName.empty()
            ? "object"
            : handle.state->typeName;
        return std::string("<") + typeName + ":#" +
               std::to_string(handle.state->debugId) +
               (alive ? ">" : ":deleted>");
    }
    if (std::holds_alternative<ZeusType::ObjectHandle>(u)) {
        const auto& objHandle = std::get<ZeusType::ObjectHandle>(u);
        std::string className = "object";
        
        // 尝试获取具体类名
        if (g_vmObjectsPtr && g_vmClassesPtr && objHandle.id >= 0 && objHandle.id < (int)g_vmObjectsPtr->size()) {
            const auto& obj = (*g_vmObjectsPtr)[objHandle.id];
            if (obj.classId >= 0 && obj.classId < (int)g_vmClassesPtr->size()) {
                className = (*g_vmClassesPtr)[obj.classId].name;
            }
        }
        
        return std::string("<") + className + ":#" + std::to_string(objHandle.id) + ">";
    }
    if (std::holds_alternative<ZeusType::SocketHandle>(u)) {
        return std::string("<socket:#") + std::to_string(std::get<ZeusType::SocketHandle>(u).id) + ">";
    }
    if (std::holds_alternative<ZeusType::Variant>(u)) {
        const ZeusType::Variant& var = std::get<ZeusType::Variant>(u);
        if (std::holds_alternative<ZeusType::Array>(var)) {
            return arrayToDebugString(u);
        }
        if (std::holds_alternative<ZeusType::Bytes>(var)) {
            const auto& bytes = std::get<ZeusType::Bytes>(var);
            std::string result = "b\"";
            for (size_t i = 0; i < bytes.data.size() && i < 32; ++i) {
                uint8_t byte = bytes.data[i];
                if (byte >= 32 && byte < 127 && byte != '\\' && byte != '"') {
                    result += static_cast<char>(byte);
                } else {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", byte);
                    result += buf;
                }
            }
            if (bytes.data.size() > 32) result += "...";
            result += "\"";
            return result;
        }
        // Null 情况已在外层统一成 "nil"
        if (std::holds_alternative<ZeusType::String>(var)) {
            return "<String>";
        }
        if (std::holds_alternative<ZeusType::Integer>(var)) {
            return "<Integer>";
        }
        if (std::holds_alternative<ZeusType::Float>(var)) {
            return "<Float>";
        }
        if (std::holds_alternative<ZeusType::Boolean>(var)) {
            return "<Boolean>";
        }
        if (std::holds_alternative<ZeusType::Object>(var)) {
            const ZeusType::Object& obj = std::get<ZeusType::Object>(var);
            if (!obj.impl) {
                return "{}";
            }
            std::string out = "{";
            bool first = true;
            // 按插入顺序输出
            for (const auto &k : obj.impl->order) {
                auto it = obj.impl->data.find(k);
                if (it == obj.impl->data.end()) continue; // 理论不该发生
                if (!first) out += ", ";
                first = false;
                out += k;
                out += ": ";
                out += valueToDebugString(it->second);
            }
            out += "}";
            return out;
        }
        if (std::holds_alternative<ZeusType::Function>(var)) {
            return "<Function>";
        }
    }

    if (std::holds_alternative<ZeusType::SharedClassHandle>(u)) {
        return std::string("<shared-class:") + std::get<ZeusType::SharedClassHandle>(u).name + ">";
    }

    if (std::holds_alternative<ZeusType::SharedObjectHandle>(u)) {
        return std::string("<shared-object:") + std::get<ZeusType::SharedObjectHandle>(u).name + ">";
    }

    return "<unknown>";
}

} // namespace ZeusVMUtils
