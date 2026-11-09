#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusMemoryABI.hpp"

namespace ZeusBuiltins {

    inline ZeusType::UniversalVariant builtin_malloc(
        const std::vector<ZeusType::UniversalVariant>& args
    ) {
        int64_t size = 0;
        if (args.size() != 1 || !ZeusVMUtils::tryGetInt64(args[0], size)) {
            throw std::runtime_error("__zeus_malloc requires an int size");
        }
        // 负数/超限尺寸按运行时层的约定是“分配失败、返回地址 0”（见 mem.zeu 与
        // ZeusMemoryABI.cpp）。这里不再单独抛错，否则 VM 与 AOT 的行为会分叉。
        return LexerType::Value(__zeus_malloc(size));
    }

    inline ZeusType::UniversalVariant builtin_free(
        const std::vector<ZeusType::UniversalVariant>& args
    ) {
        int64_t bits = 0;
        if (args.size() != 1 || !ZeusVMUtils::tryGetInt64(args[0], bits)) {
            throw std::runtime_error("__zeus_free requires an int allocation address");
        }
        void* address = reinterpret_cast<void*>(static_cast<uintptr_t>(bits));
        if (address && zeus_memory_find_allocation(address, 0) != address) {
            throw std::runtime_error("__zeus_free requires a live allocation base address");
        }
        return LexerType::Value(__zeus_free(bits));
    }

    inline void registerAllocationBuiltins() {
        registerBuiltin("__zeus_malloc", builtin_malloc, 1, 1);
        registerBuiltin("__zeus_free", builtin_free, 1, 1);
    }
}
