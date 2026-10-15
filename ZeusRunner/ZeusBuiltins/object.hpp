// ZeusBuiltins/object.hpp - 对象和全局变量操作内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"

namespace ZeusBuiltins {

    // ===== 对象操作函数 =====
    
    // getattr(obj, attrName): 获取对象属性
    inline ZeusType::UniversalVariant builtin_getattr(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 2) {
            throw std::runtime_error("getattr expects at least 2 arguments");
        }

        // 第一个参数必须是 ObjectHandle
        if (!std::holds_alternative<ZeusType::ObjectHandle>(args[0])) {
            throw std::runtime_error("getattr: first argument must be object");
        }

        int objId = std::get<ZeusType::ObjectHandle>(args[0]).id;

        // 第二个参数必须是字符串（属性名）
        std::string attrName;
        if (std::holds_alternative<std::string>(args[1])) {
            attrName = std::get<std::string>(args[1]);
        } else if (std::holds_alternative<LexerType::Value>(args[1])) {
            const auto& v = std::get<LexerType::Value>(args[1]);
            if (std::holds_alternative<std::string>(v.data)) {
                attrName = std::get<std::string>(v.data);
            }
        }

        if (attrName.empty()) {
            throw std::runtime_error("getattr: attribute name must be string");
        }

        if (!g_vmObjects || objId < 0 || objId >= (int)g_vmObjects->size()) {
            throw std::runtime_error("getattr: invalid object id");
        }

        const auto& obj = (*g_vmObjects)[objId];
        const bool hasClassInfo = g_vmClasses && obj.classId >= 0 &&
            obj.classId < static_cast<int>(g_vmClasses->size());
        const auto extension = obj.extensionValues.find(attrName);
        const bool hasField = hasClassInfo
            ? ZeusType::objectHasField(obj, (*g_vmClasses)[obj.classId], attrName)
            : extension != obj.extensionValues.end();
        if (!hasField) {
            // 如果没有第三个参数（默认值），抛出异常
            if (args.size() < 3) {
                throw std::runtime_error("getattr: attribute '" + attrName + "' not found");
            }
            // 返回默认值
            return args[2];
        }

        if (hasClassInfo) {
            return ZeusType::readObjectField(obj, (*g_vmClasses)[obj.classId], attrName);
        }
        return extension->second;
    }

    // setattr(obj, attrName, value): 设置对象属性
    inline ZeusType::UniversalVariant builtin_setattr(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() < 3) {
            throw std::runtime_error("setattr expects 3 arguments");
        }

        // 第一个参数必须是 ObjectHandle
        if (!std::holds_alternative<ZeusType::ObjectHandle>(args[0])) {
            throw std::runtime_error("setattr: first argument must be object");
        }

        int objId = std::get<ZeusType::ObjectHandle>(args[0]).id;

        // 第二个参数必须是字符串（属性名）
        std::string attrName;
        if (std::holds_alternative<std::string>(args[1])) {
            attrName = std::get<std::string>(args[1]);
        } else if (std::holds_alternative<LexerType::Value>(args[1])) {
            const auto& v = std::get<LexerType::Value>(args[1]);
            if (std::holds_alternative<std::string>(v.data)) {
                attrName = std::get<std::string>(v.data);
            }
        }

        if (attrName.empty()) {
            throw std::runtime_error("setattr: attribute name must be string");
        }

        if (!g_vmObjects || objId < 0 || objId >= (int)g_vmObjects->size()) {
            throw std::runtime_error("setattr: invalid object id");
        }

        auto& obj = (*g_vmObjects)[objId];
        if (g_vmClasses && obj.classId >= 0 && obj.classId < static_cast<int>(g_vmClasses->size())) {
            ZeusType::writeObjectField(obj, (*g_vmClasses)[obj.classId], attrName, args[2]);
        } else {
            obj.extensionValues[attrName] = args[2];
        }
        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // VM::invokeNative performs the shallow ObjectInstance copy so it can use
    // the VM's object table and retain each copied field edge.
    inline ZeusType::UniversalVariant builtin_object_clone(
        const std::vector<ZeusType::UniversalVariant>& args
    ) {
        if (args.size() != 1) {
            throw std::runtime_error("__zeus_object_clone expects 1 argument");
        }
        return args[0];
    }

    inline ZeusType::UniversalVariant builtin_object_assign(
        const std::vector<ZeusType::UniversalVariant>& args
    ) {
        if (args.size() != 2) {
            throw std::runtime_error("__zeus_object_assign expects 2 arguments");
        }
        return args[0];
    }

    inline ZeusType::UniversalVariant builtin_object_invalidate(
        const std::vector<ZeusType::UniversalVariant>& args
    ) {
        if (args.size() != 1) {
            throw std::runtime_error("__zeus_object_invalidate expects 1 argument");
        }
        return ZeusType::UniversalVariant(ZeusType::Variant(ZeusType::Null()));
    }

    // ===== 全局变量操作函数 =====
    // 注意: 这些函数的实际实现在 VM 的 CALL_NATIVE 分支中，这里只是占位符
    
    // __getglobal(index): 读取全局变量数组中的值
    inline ZeusType::UniversalVariant builtin_getglobal(const std::vector<ZeusType::UniversalVariant>& args) {
        // 实际实现在 VM::executeInstruction 的 CALL_NATIVE 中
        return ZeusType::UniversalVariant(LexerType::Value(0));
    }

    // __setglobal(index, value): 设置全局变量数组中的值
    inline ZeusType::UniversalVariant builtin_setglobal(const std::vector<ZeusType::UniversalVariant>& args) {
        // 实际实现在 VM::executeInstruction 的 CALL_NATIVE 中
        return ZeusType::UniversalVariant(LexerType::Value(0));
    }

    // __loadglobal(name): 通过符号名读取全局变量
    inline ZeusType::UniversalVariant builtin_loadglobal(const std::vector<ZeusType::UniversalVariant>& args) {
        // 实际实现在 VM::executeInstruction 的 CALL_NATIVE 中
        return ZeusType::UniversalVariant(LexerType::Value(0));
    }

    // __storeglobal(name, value): 通过符号名设置全局变量
    inline ZeusType::UniversalVariant builtin_storeglobal(const std::vector<ZeusType::UniversalVariant>& args) {
        // 实际实现在 VM::executeInstruction 的 CALL_NATIVE 中
        return ZeusType::UniversalVariant(LexerType::Value(0));
    }

    // __get_module(name): 获取模块代理对象
    inline ZeusType::UniversalVariant builtin_get_module(const std::vector<ZeusType::UniversalVariant>& args) {
        // 实际实现在 VM::executeInstruction 的 CALL_NATIVE 中
        return ZeusType::UniversalVariant(LexerType::Value(0));
    }

    // ===== 字典操作函数 =====
    
    // hasKey(dict, key): 检查字典是否包含指定的键
    // 注意: 这个函数的实际实现需要访问VM的objects数组
    // 这里只是占位符,实际实现在VM中
    inline ZeusType::UniversalVariant builtin_hasKey(const std::vector<ZeusType::UniversalVariant>& args) {
        // 实际实现在 VM::executeInstruction 的 CALL_NATIVE 中
        // 这里返回false作为占位
        return ZeusType::UniversalVariant(false);
    }

    // 注册对象和全局变量相关内置函数
    inline void registerObjectBuiltins() {
        // getattr/setattr 保留全局版本（常用）
        registerBuiltin("getattr", builtin_getattr, 2, 2);
        registerBuiltin("setattr", builtin_setattr, 3, 3);
        
        // 对象操作（供cppfunctions.object使用）
        registerBuiltin("__objget", builtin_getattr, 3, 3);  // 零拷贝对象字段访问
        registerBuiltin("__objset", builtin_setattr, 4, 4);  // 零拷贝对象字段修改
        registerBuiltin("__zeus_object_clone", builtin_object_clone, 1, 1);
        registerBuiltin("__zeus_object_assign", builtin_object_assign, 2, 2);
        registerBuiltin("__zeus_object_invalidate", builtin_object_invalidate, 1, 1);
        
        // 全局变量操作（供cppfunctions.global使用）
        registerBuiltin("__getglobal", builtin_getglobal, 1, 1);
        registerBuiltin("__setglobal", builtin_setglobal, 2, 2);
        registerBuiltin("__loadglobal", builtin_loadglobal, 1, 1);
        registerBuiltin("__storeglobal", builtin_storeglobal, 2, 2);
        registerBuiltin("__get_module", builtin_get_module, 1, 1);
        
        // 字典操作（供cppfunctions.dict使用）
        registerBuiltin("hasKey", builtin_hasKey, 2, 2);
    }

} // namespace ZeusBuiltins
