// ZeusRuntimeLib/ZeusDynamicABI.hpp
//
// 动态对象的显式 C ABI：array/dict 仍然是 ZeusBox*，但不再要求调用方
// 通过 zeus_push/zeus_pop 传递。LLVM lowering 可以直接传参数和返回值。

#pragma once

#include "ZeusABI.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// 构造函数会消费 values 中的所有 ZeusBox*；调用方不能再释放这些指针。
// array 的 values 按元素顺序排列，dict 的 values 按 key0,value0,key1,value1...
// 排列。失败时会消费输入并返回 nil box。
ZeusBox* zeus_array_build(int64_t count, ZeusBox* const* values);
ZeusBox* zeus_dict_build(int64_t pairCount, ZeusBox* const* keyValues);

// 查询函数借用输入对象，返回调用方拥有的新 ZeusBox*。
int64_t zeus_array_length(const ZeusBox* array);
ZeusBox* zeus_array_load(const ZeusBox* array, int64_t index);
ZeusBox* zeus_dict_load(const ZeusBox* dict, const ZeusBox* key);
ZeusBox* zeus_dict_keys(const ZeusBox* dict);

// 写入函数借用容器，消费 value；dict_store 同时消费 key。
// 返回值是写入值的独立副本，便于保持 VM 中 STORE 指令的“结果留栈”语义。
ZeusBox* zeus_array_store(ZeusBox* array, int64_t index, ZeusBox* value);
ZeusBox* zeus_dict_store(ZeusBox* dict, ZeusBox* key, ZeusBox* value);

typedef struct ZeusDynamicIterator ZeusDynamicIterator;

// 迭代器复制并持有容器的 shared_ptr 快照；源 ZeusBox 可以在迭代器存在时
// 释放。next 返回 1 表示产生一项，返回 0 表示结束；输出 box 均由调用方拥有。
ZeusDynamicIterator* zeus_array_iter_begin(const ZeusBox* array);
int32_t zeus_array_iter_next(ZeusDynamicIterator* iterator, ZeusBox** value);

ZeusDynamicIterator* zeus_dict_iter_begin(const ZeusBox* dict);
int32_t zeus_dict_iter_next(
    ZeusDynamicIterator* iterator,
    ZeusBox** key,
    ZeusBox** value
);

void zeus_dynamic_iter_release(ZeusDynamicIterator* iterator);

// ZeusValue-facing container ABI used by LLVM lowering. Arguments are borrowed;
// returned values are initialized by these functions. The current storage
// implementation adapts to the shared VM container representation internally.
void zeus_value_array_build(
    ZeusValue* out,
    int64_t count,
    const ZeusValue* const* values
);
void zeus_value_dict_build(
    ZeusValue* out,
    int64_t pair_count,
    const ZeusValue* const* key_values
);
void zeus_value_array_length(ZeusValue* out, const ZeusValue* array);
void zeus_value_array_load(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* index
);
void zeus_value_array_store(
    ZeusValue* out,
    const ZeusValue* array,
    const ZeusValue* index,
    const ZeusValue* value
);
void zeus_value_dict_keys(ZeusValue* out, const ZeusValue* dict);
void zeus_value_dict_load(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* key
);
void zeus_value_dict_store(
    ZeusValue* out,
    const ZeusValue* dict,
    const ZeusValue* key,
    const ZeusValue* value
);

#ifdef __cplusplus
}
#endif
