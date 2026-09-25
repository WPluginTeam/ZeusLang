#pragma once

// ZeusBuiltins Core Infrastructure
// 包含注册表、全局指针和辅助函数

#include <string>
#include <vector>
#include <deque>
#include <variant>
#include <type_traits>
#include <functional>
#include <unordered_map>
#include <iostream>
#include <mutex>

#include "../../ZeusRuntimeLib/ZeusVariant.hpp"
#include "../ZeusVMOperator.hpp"
#include "../../ZeusRuntimeLib/ZeusCell.hpp"
#include "../../ZeusParser/LexerType.hpp"
#include "../../ZeusRuntimeLib/ZeusVMUtils.hpp"

#ifdef ZEUS_DEBUG_FILE_CHANNEL
#undef ZEUS_DEBUG_FILE_CHANNEL
#endif
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_BUILTIN
#include "../../ZeusDebug/Utils.hpp"

namespace ZeusBuiltins {

    // =====================================================================
    // 原生函数描述符
    // =====================================================================

    struct NativeFuncDescriptor {
        std::string name;
        ZeusType::NativeCallback fn;
        int minArity = 0;
        int maxArity = 0;  // <0 表示可变参数
    };

    // =====================================================================
    // 全局注册表
    // =====================================================================

    inline std::vector<NativeFuncDescriptor> g_registry;
    inline std::unordered_map<std::string, int> g_nameToId;

    // =====================================================================
    // 全局指针（由 VM 初始化时设置）
    // =====================================================================

    // 对象系统
    inline std::vector<ZeusType::ClassInfo>* g_vmClasses = nullptr;
    inline std::deque<ZeusType::ObjectInstance>* g_vmObjects = nullptr;
    inline std::vector<int>* g_vmFreeObjectSlots = nullptr;
    inline size_t* g_vmFreeObjectSlotsSize = nullptr;

    // Socket 系统
    inline std::vector<ZeusType::SocketData>* g_vmSockets = nullptr;
    inline std::vector<int>* g_vmFreeSocketSlots = nullptr;
    inline size_t* g_vmFreeSocketSlotsSize = nullptr;
    inline std::mutex* g_vmSocketsMutex = nullptr;

    // 闭包系统
    inline std::vector<int>* g_vmFreeClosureSlotsPtr = nullptr;
    inline size_t* g_vmClosuresSize = nullptr;
    inline std::function<size_t()> g_vmGetFreeClosureSlotsSize = nullptr;
    inline std::function<std::string()> g_vmDumpClosureDetails = nullptr;

    // 全局变量系统
    inline std::vector<std::string>* g_vmGlobalNames = nullptr;
    inline size_t* g_vmPrototypesSize = nullptr;

    // 引用计数管理函数
    inline std::function<void(int)> g_vmRetainObject = nullptr;
    inline std::function<void(int)> g_vmReleaseObject = nullptr;
    inline std::function<void(int)> g_vmRetainClosure = nullptr;
    inline std::function<void(int)> g_vmReleaseClosure = nullptr;

    // =====================================================================
    // 注册表操作函数
    // =====================================================================

    inline bool isVariadic(const NativeFuncDescriptor& d) {
        return d.maxArity < 0;
    }

    inline int registerBuiltin(const std::string& name,
                               ZeusType::NativeCallback fn,
                               int minArity,
                               int maxArity) {
        if (g_nameToId.find(name) != g_nameToId.end()) {
            return g_nameToId[name];
        }

        int id = (int)g_registry.size();
        g_registry.push_back(NativeFuncDescriptor{name, fn, minArity, maxArity});
        g_nameToId[name] = id;

        ZEUS_DEBUG_PRINT("[Builtin] register name=%s id=%d arity=%s%s",
            name.c_str(),
            id,
            (isVariadic(g_registry.back()) ? "*" : std::to_string(maxArity).c_str()),
            (minArity != (isVariadic(g_registry.back()) ? 0 : maxArity) 
                ? (" min=" + std::to_string(minArity)).c_str() : "")
        );

        return id;
    }

    inline int findId(const std::string& name) {
        auto it = g_nameToId.find(name);
        if (it == g_nameToId.end()) return -1;
        return it->second;
    }

    inline const NativeFuncDescriptor* get(int id) {
        if (id < 0 || id >= (int)g_registry.size()) return nullptr;
        return &g_registry[id];
    }

    // =====================================================================
    // 辅助工具函数
    // =====================================================================

    // 从 UniversalVariant 提取 Array 指针
    inline ZeusType::Array* getArrayPtr(ZeusType::UniversalVariant& uv) {
        if (auto* vp = std::get_if<ZeusType::Variant>(&uv)) {
            return std::get_if<ZeusType::Array>(vp);
        }
        return nullptr;
    }

    inline const ZeusType::Array* getArrayPtr(const ZeusType::UniversalVariant& uv) {
        if (auto* vp = std::get_if<ZeusType::Variant>(&uv)) {
            return std::get_if<ZeusType::Array>(vp);
        }
        return nullptr;
    }

    // 值转字符串（调试用）
    inline std::string _toStringOne(const ZeusType::UniversalVariant& v) {
        return ZeusVMUtils::valueToDebugString(v);
    }

}  // namespace ZeusBuiltins
