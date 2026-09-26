// ZeusBuiltins/gc.hpp - 共享收集器的观测内置函数
//
// These builtins are VM independent on purpose: the AOT runtime registers them
// through the standalone ZeusValue ABI so generated code can force a
// collection and read the live managed-node count without a VM instance.

#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusGC.hpp"

namespace ZeusBuiltins {

    // __gc_native_nodes(): 已注册的原生闭包节点数（存活）
    inline ZeusType::UniversalVariant builtin_gc_native_nodes(
        const std::vector<ZeusType::UniversalVariant>& args) {
        return ZeusType::UniversalVariant(
            LexerType::Value(static_cast<int64_t>(ZeusGC::liveNodeCount())));
    }

    // __gc_collect(): 立刻跑一次共享收集器，返回回收后的节点数
    inline ZeusType::UniversalVariant builtin_gc_collect(
        const std::vector<ZeusType::UniversalVariant>& args) {
        ZeusGC::collect();
        return ZeusType::UniversalVariant(
            LexerType::Value(static_cast<int64_t>(ZeusGC::liveNodeCount())));
    }

    inline void registerGcBuiltins() {
        registerBuiltin("__gc_native_nodes", builtin_gc_native_nodes, 0, 0);
        registerBuiltin("__gc_collect", builtin_gc_collect, 0, 0);
    }

} // namespace ZeusBuiltins
