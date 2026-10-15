// ZeusBuiltins/array.hpp - 数组操作内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusContainerOps.hpp"

namespace ZeusBuiltins {

    // __array_push(array, value): 向数组末尾添加元素
    inline ZeusType::UniversalVariant builtin_array_push(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__array_push expects 2 arguments");
        return ZeusContainerOps::arrayPush(args[0], args[1]);
    }

    // __array_pop(array): 移除并返回数组末尾元素
    inline ZeusType::UniversalVariant builtin_array_pop(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__array_pop expects 1 argument");

        return ZeusContainerOps::arrayPop(args[0]);
    }

    // __array_get(array, index): 获取数组指定索引的元素
    inline ZeusType::UniversalVariant builtin_array_get(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__array_get expects 2 arguments");

        return ZeusContainerOps::arrayGet(args[0], args[1]);
    }

    // __array_set(array, index, value): 设置数组指定索引的元素
    inline ZeusType::UniversalVariant builtin_array_set(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__array_set expects 3 arguments");

        return ZeusContainerOps::arraySet(args[0], args[1], args[2]);
    }

    // __array_clear(array): 清空数组
    inline ZeusType::UniversalVariant builtin_array_clear(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__array_clear expects 1 argument");

        return ZeusContainerOps::arrayClear(args[0]);
    }

    // __array_slice(array, start, end): 获取数组切片
    inline ZeusType::UniversalVariant builtin_array_slice(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__array_slice expects 3 arguments");

        return ZeusContainerOps::arraySlice(args[0], args[1], args[2]);
    }

    // 注册数组相关内置函数
    inline void registerArrayBuiltins() {
        registerBuiltin("__array_push", builtin_array_push, 2, 2);
        registerBuiltin("__array_pop", builtin_array_pop, 1, 1);
        registerBuiltin("__array_get", builtin_array_get, 2, 2);
        registerBuiltin("__array_set", builtin_array_set, 3, 3);
        registerBuiltin("__array_clear", builtin_array_clear, 1, 1);
        registerBuiltin("__array_slice", builtin_array_slice, 3, 3);
    }

} // namespace ZeusBuiltins
