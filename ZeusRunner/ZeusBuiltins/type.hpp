#pragma once

// ZeusBuiltins Type Functions
// 类型转换和类型检查函数：str, int, float, type, len

#include "core.hpp"

namespace ZeusBuiltins {

    // =====================================================================
    // 工具函数
    // =====================================================================

    // len: 获取字符串、数组或字节串的长度
    inline ZeusType::UniversalVariant builtin_len(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("len expects exactly 1 argument");
        }

        const auto& a0 = args[0];
        
        // 字符串
        if (const std::string* sp = std::get_if<std::string>(&a0)) {
            return ZeusType::UniversalVariant(ZeusType::BigInteger((int)sp->size()));
        }

        if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&a0)) {
            if (std::holds_alternative<std::string>(vp->data)) {
                const auto& s = std::get<std::string>(vp->data);
                return ZeusType::UniversalVariant(ZeusType::BigInteger((int)s.size()));
            }
        }
        
        // 数组
        if (const ZeusType::Array* ap = getArrayPtr(a0)) {
            if (ap->impl) {
                return ZeusType::UniversalVariant(ZeusType::BigInteger((int)ap->impl->data.size()));
            }

            return ZeusType::UniversalVariant(ZeusType::BigInteger(0));
        }

        // 字节串（Variant 包装）
        if (const ZeusType::Variant* vp = std::get_if<ZeusType::Variant>(&a0)) {
            if (const ZeusType::Bytes* bp = std::get_if<ZeusType::Bytes>(&(*vp))) {
                return ZeusType::UniversalVariant(ZeusType::BigInteger((int)bp->data.size()));
            }
        }
        
        // 字节串（直接）
        if (const ZeusType::Bytes* bp = std::get_if<ZeusType::Bytes>(&a0)) {
            return ZeusType::UniversalVariant(ZeusType::BigInteger((int)bp->data.size()));
        }
        
        throw std::runtime_error("len unsupported type (expected string, array, or bytes)");
    }

    // =====================================================================
    // 类型转换函数
    // =====================================================================

    // __builtin_str: 底层字符串转换（不带引号）
    // 注意：这是内部函数，用户应使用 Zeus 层面的 str() 函数
    inline ZeusType::UniversalVariant builtin_str(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.empty()) {
            return ZeusType::UniversalVariant(std::string(""));
        }
        
        const auto& v = args[0];
        std::string result;
        
        if (std::holds_alternative<ZeusType::BigInteger>(v)) {
            result = std::get<ZeusType::BigInteger>(v).toString();
        }
        else if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
            result = std::get<ZeusType::BigDecimal>(v).toString();
        }
        else if (std::holds_alternative<bool>(v)) {
            result = std::get<bool>(v) ? "true" : "false";
        }
        else if (std::holds_alternative<std::string>(v)) {
            result = std::get<std::string>(v);
        }
        else if (std::holds_alternative<LexerType::Value>(v)) {
            const auto& val = std::get<LexerType::Value>(v);

            if (std::holds_alternative<std::string>(val.data)) {
                result = std::get<std::string>(val.data);
            } else if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                result = std::get<ZeusType::BigInteger>(val.data).toString();
            } else if (std::holds_alternative<ZeusType::BigDecimal>(val.data)) {
                result = std::get<ZeusType::BigDecimal>(val.data).toString();
            } else if (std::holds_alternative<bool>(val.data)) {
                result = std::get<bool>(val.data) ? "true" : "false";
            } else {
                result = val.toString();
            }
        }
        else if (std::holds_alternative<ZeusType::Variant>(v)) {
            const auto& var = std::get<ZeusType::Variant>(v);

            if (std::holds_alternative<ZeusType::Null>(var)) {
                result = "nil";
            } else if (std::holds_alternative<ZeusType::Array>(var)) {
                result = ZeusVMUtils::arrayToDebugString(v);
            } else {
                result = ZeusVMUtils::valueToDebugString(v);
            }
        }
        else if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
            result = "<closure:#" + std::to_string(std::get<ZeusType::ClosureHandle>(v).id) + ">";
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

            result = "<" + className + ":#" + std::to_string(objHandle.id) + ">";
        }
        else if (std::holds_alternative<ZeusType::ClassHandle>(v)) {
            result = "<class:#" + std::to_string(std::get<ZeusType::ClassHandle>(v).id) + ">";
        }
        else if (std::holds_alternative<ZeusType::NativeFuncHandle>(v)) {
            result = "<native:#" + std::to_string(std::get<ZeusType::NativeFuncHandle>(v).id) + ">";
        }
        else {
            result = ZeusVMUtils::valueToDebugString(v);
        }
        
        return ZeusType::UniversalVariant(result);
    }

    // int: 将值转换为 BigInteger
    inline ZeusType::UniversalVariant builtin_int(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("int expects exactly 1 argument");
        }

        const auto& v = args[0];
        
        if (std::holds_alternative<ZeusType::BigInteger>(v)) {
            return v;
        }
        
        if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&v)) {
            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                return ZeusType::UniversalVariant(std::get<ZeusType::BigInteger>(vp->data));
            }

            if (std::holds_alternative<ZeusType::BigDecimal>(vp->data)) {
                const auto& decimal = std::get<ZeusType::BigDecimal>(vp->data);
                return ZeusType::UniversalVariant(ZeusType::BigInteger(decimal.toString()));
            }

            if (std::holds_alternative<std::string>(vp->data)) {
                const auto& s = std::get<std::string>(vp->data);

                try {
                    return ZeusType::UniversalVariant(ZeusType::BigInteger(s));
                } catch (...) {
                    throw std::runtime_error("int: cannot convert string '" + s + "' to integer");
                }
            }
        }
        
        if (const std::string* sp = std::get_if<std::string>(&v)) {
            try {
                return ZeusType::UniversalVariant(ZeusType::BigInteger(*sp));
            } catch (...) {
                throw std::runtime_error("int: cannot convert string '" + *sp + "' to integer");
            }
        }
        
        if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
            const auto& decimal = std::get<ZeusType::BigDecimal>(v);
            return ZeusType::UniversalVariant(ZeusType::BigInteger(decimal.toString()));
        }
        
        throw std::runtime_error("int: unsupported type conversion");
    }

    // float: 将值转换为 BigDecimal
    inline ZeusType::UniversalVariant builtin_float(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("float expects exactly 1 argument");
        }

        const auto& v = args[0];
        
        if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
            return v;
        }
        
        if (const LexerType::Value* vp = std::get_if<LexerType::Value>(&v)) {
            if (std::holds_alternative<ZeusType::BigDecimal>(vp->data)) {
                return ZeusType::UniversalVariant(std::get<ZeusType::BigDecimal>(vp->data));
            }

            if (std::holds_alternative<ZeusType::BigInteger>(vp->data)) {
                const auto& integer = std::get<ZeusType::BigInteger>(vp->data);
                return ZeusType::UniversalVariant(ZeusType::BigDecimal(integer.toString()));
            }

            if (std::holds_alternative<std::string>(vp->data)) {
                const auto& s = std::get<std::string>(vp->data);

                try {
                    return ZeusType::UniversalVariant(ZeusType::BigDecimal(s));
                } catch (...) {
                    throw std::runtime_error("float: cannot convert string '" + s + "' to decimal");
                }
            }
        }
        
        if (const std::string* sp = std::get_if<std::string>(&v)) {
            try {
                return ZeusType::UniversalVariant(ZeusType::BigDecimal(*sp));
            } catch (...) {
                throw std::runtime_error("float: cannot convert string '" + *sp + "' to decimal");
            }
        }
        
        if (std::holds_alternative<ZeusType::BigInteger>(v)) {
            const auto& integer = std::get<ZeusType::BigInteger>(v);
            return ZeusType::UniversalVariant(ZeusType::BigDecimal(integer.toString()));
        }
        
        throw std::runtime_error("float: unsupported type conversion");
    }

    // type: 返回值的类型名称
    inline ZeusType::UniversalVariant builtin_type(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("type expects exactly 1 argument");
        }

        const auto& v = args[0];
        std::string typeName;
        
        if (std::holds_alternative<ZeusType::BigInteger>(v)) {
            typeName = "int";
        }
        else if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
            typeName = "float";
        }
        else if (std::holds_alternative<bool>(v)) {
            typeName = "bool";
        }
        else if (std::holds_alternative<std::string>(v)) {
            typeName = "string";
        }
        else if (std::holds_alternative<LexerType::Value>(v)) {
            const auto& val = std::get<LexerType::Value>(v);

            if (std::holds_alternative<std::string>(val.data)) {
                typeName = "string";
            } else if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                typeName = "int";
            } else if (std::holds_alternative<ZeusType::BigDecimal>(val.data)) {
                typeName = "float";
            } else if (std::holds_alternative<bool>(val.data)) {
                typeName = "bool";
            } else {
                typeName = "value";
            }
        }
        else if (std::holds_alternative<ZeusType::Variant>(v)) {
            const auto& var = std::get<ZeusType::Variant>(v);

            if (std::holds_alternative<ZeusType::Null>(var)) {
                typeName = "nil";
            } else if (std::holds_alternative<ZeusType::Array>(var)) {
                typeName = "array";
            } else if (std::holds_alternative<ZeusType::Object>(var)) {
                typeName = "object";
            } else if (std::holds_alternative<ZeusType::Bytes>(var)) {
                typeName = "bytes";
            } else {
                typeName = "variant";
            }
        }
        else if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
            typeName = "closure";
        }
        else if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
            const auto& objHandle = std::get<ZeusType::ObjectHandle>(v);

            if (g_vmObjects && g_vmClasses && objHandle.id >= 0 && objHandle.id < (int)g_vmObjects->size()) {
                const auto& obj = (*g_vmObjects)[objHandle.id];

                if (obj.classId >= 0 && obj.classId < (int)g_vmClasses->size()) {
                    typeName = (*g_vmClasses)[obj.classId].name;
                } else {
                    typeName = "object";
                }
            } else {
                typeName = "object";
            }
        }
        else if (std::holds_alternative<ZeusType::LLVMClassHandle>(v)) {
            const auto& classHandle = std::get<ZeusType::LLVMClassHandle>(v);
            typeName = classHandle.state && !classHandle.state->typeName.empty()
                ? classHandle.state->typeName
                : "object";
        }
        else if (std::holds_alternative<ZeusType::ClassHandle>(v)) {
            typeName = "class";
        }
        else if (std::holds_alternative<ZeusType::NativeFuncHandle>(v)) {
            typeName = "native_function";
        }
        else if (std::holds_alternative<ZeusType::NamespaceHandle>(v)) {
            typeName = "namespace";
        }
        else if (std::holds_alternative<ZeusType::Bytes>(v)) {
            typeName = "bytes";
        }
        else {
            typeName = "unknown";
        }
        
        return ZeusType::UniversalVariant(typeName);
    }

    // =====================================================================
    // 注册函数
    // =====================================================================

    inline void registerTypeBuiltins() {
        registerBuiltin("__builtin_str", builtin_str, 1, 1);  // 底层实现，Zeus 层会重新定义 str()
        registerBuiltin("type", builtin_type, 1, 1);
        registerBuiltin("len", builtin_len, 1, 1);
        registerBuiltin("int", builtin_int, 1, 1);
        registerBuiltin("float", builtin_float, 1, 1);
    }

}  // namespace ZeusBuiltins
