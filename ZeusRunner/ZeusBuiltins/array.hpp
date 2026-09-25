// ZeusBuiltins/array.hpp - 数组操作内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"

namespace ZeusBuiltins {

    // ===== Array 操作辅助函数 =====
    
    // 获取可变 Array 指针
    inline ZeusType::Array* getMutableArrayPtr(ZeusType::UniversalVariant& uv) {
        if (ZeusType::Variant* vp = std::get_if<ZeusType::Variant>(&uv)) {
            return std::get_if<ZeusType::Array>(vp);
        }
        return nullptr;
    }

    // __array_push(array, value): 向数组末尾添加元素
    inline ZeusType::UniversalVariant builtin_array_push(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__array_push expects 2 arguments");

        // 使用const引用拿到数组参数,然后修改其内部impl
        const auto& arrArg = args[0];
        const ZeusType::Array* arrPtr = getArrayPtr(arrArg);

        if (!arrPtr) {
            throw std::runtime_error("__array_push: first argument must be an array");
        }

        if (!arrPtr->impl) {
            throw std::runtime_error("__array_push: array has no internal data");
        }

        // Array的impl是shared_ptr,数据是共享的,直接修改
        arrPtr->impl->data.push_back(args[1]);

        // 返回新长度
        return ZeusType::UniversalVariant(ZeusType::BigInteger((int)arrPtr->impl->data.size()));
    }

    // __array_pop(array): 移除并返回数组末尾元素
    inline ZeusType::UniversalVariant builtin_array_pop(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__array_pop expects 1 argument");

        const auto& arrArg = args[0];
        const ZeusType::Array* arrPtr = getArrayPtr(arrArg);

        if (!arrPtr || !arrPtr->impl || arrPtr->impl->data.empty()) {
            throw std::runtime_error("__array_pop: cannot pop from empty array");
        }

        ZeusType::UniversalVariant result = arrPtr->impl->data.back();
        arrPtr->impl->data.pop_back();
        return result;
    }

    // __array_get(array, index): 获取数组指定索引的元素
    inline ZeusType::UniversalVariant builtin_array_get(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) throw std::runtime_error("__array_get expects 2 arguments");

        const auto& arrArg = args[0];
        const ZeusType::Array* arrPtr = getArrayPtr(arrArg);

        if (!arrPtr || !arrPtr->impl) {
            throw std::runtime_error("__array_get: first argument must be an array");
        }

        int index = 0;
        ZeusVMUtils::tryGetInt(args[1], index);

        if (index < 0 || index >= (int)arrPtr->impl->data.size()) {
            throw std::runtime_error("__array_get: index out of range");
        }

        return arrPtr->impl->data[index];
    }

    // __array_set(array, index, value): 设置数组指定索引的元素
    inline ZeusType::UniversalVariant builtin_array_set(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__array_set expects 3 arguments");

        const auto& arrArg = args[0];
        const ZeusType::Array* arrPtr = getArrayPtr(arrArg);

        if (!arrPtr || !arrPtr->impl) {
            throw std::runtime_error("__array_set: first argument must be an array");
        }

        int index = 0;
        ZeusVMUtils::tryGetInt(args[1], index);

        if (index < 0 || index >= (int)arrPtr->impl->data.size()) {
            throw std::runtime_error("__array_set: index out of range");
        }

        arrPtr->impl->data[index] = args[2];
        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __array_clear(array): 清空数组
    inline ZeusType::UniversalVariant builtin_array_clear(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) throw std::runtime_error("__array_clear expects 1 argument");

        const auto& arrArg = args[0];
        const ZeusType::Array* arrPtr = getArrayPtr(arrArg);

        if (!arrPtr || !arrPtr->impl) {
            throw std::runtime_error("__array_clear: argument must be an array");
        }

        arrPtr->impl->data.clear();
        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __array_slice(array, start, end): 获取数组切片
    inline ZeusType::UniversalVariant builtin_array_slice(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) throw std::runtime_error("__array_slice expects 3 arguments");

        const auto& arrArg = args[0];
        const ZeusType::Array* arrPtr = getArrayPtr(arrArg);

        if (!arrPtr || !arrPtr->impl) {
            throw std::runtime_error("__array_slice: first argument must be an array");
        }

        int start = 0;
        int end = arrPtr->impl->data.size();

        ZeusVMUtils::tryGetInt(args[1], start);
        ZeusVMUtils::tryGetInt(args[2], end);

        if (start < 0) start = 0;
        if (end > (int)arrPtr->impl->data.size()) end = arrPtr->impl->data.size();
        if (start > end) start = end;

        ZeusType::Array result;
        result.impl = std::make_shared<ZeusType::ArrayData>();
        for (int i = start; i < end; ++i) {
            result.impl->data.push_back(arrPtr->impl->data[i]);
        }

        return ZeusType::UniversalVariant(ZeusType::Variant(result));
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
