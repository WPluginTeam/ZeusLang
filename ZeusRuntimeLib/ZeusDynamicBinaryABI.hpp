// ZeusRuntimeLib/ZeusDynamicBinaryABI.hpp
//
// boxed 二元运算 ABI。它把“数值/字符串运算”和“用户对象运算符分派”
// 放到同一个显式边界，避免 LLVM emitter 复制 VM 的分派细节。

#pragma once

#include "ZeusABI.hpp"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ZeusDynamicBinaryOp {
    ZEUS_BINARY_ADD = 0,
    ZEUS_BINARY_SUBTRACT = 1,
    ZEUS_BINARY_MULTIPLY = 2,
    ZEUS_BINARY_DIVIDE = 3,
    ZEUS_BINARY_MODULUS = 4,
    ZEUS_BINARY_LESS_THAN = 5,
    ZEUS_BINARY_EQUAL = 6,
    ZEUS_BINARY_NOT_EQUAL = 7
} ZeusDynamicBinaryOp;

typedef struct ZeusRuntimeContext ZeusRuntimeContext;

// userData 的生命周期由调用方管理。回调借用 lhs/rhs，并返回调用方拥有的
// 新 ZeusBox*；如果无法分派，返回 nil box。
typedef ZeusBox* (*ZeusObjectBinaryDispatchFn)(
    void* userData,
    int32_t opcode,
    const ZeusBox* lhs,
    const ZeusBox* rhs
);

ZeusRuntimeContext* zeus_runtime_context_create(
    void* userData,
    ZeusObjectBinaryDispatchFn dispatch
);
void zeus_runtime_context_release(ZeusRuntimeContext* context);

// lhs/rhs 均为借用引用，返回值由调用方拥有。对象左值只有在 context
// 提供 dispatch 时才会进入用户对象运算符；否则返回 nil box。
ZeusBox* zeus_dynamic_binary(
    ZeusRuntimeContext* context,
    int32_t opcode,
    const ZeusBox* lhs,
    const ZeusBox* rhs
);

#ifdef __cplusplus
}
#endif
