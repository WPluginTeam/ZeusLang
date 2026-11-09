// ZeusBuiltins/all.hpp - 统一包含所有 ZeusBuiltins 模块
// Part of ZeusBuiltins modular refactoring

#pragma once

// 核心基础设施（必须首先包含）
#include "core.hpp"

// 输入输出
#include "io.hpp"

// 类型转换
#include "type.hpp"

// 数组操作
#include "array.hpp"
#include "dict.hpp"

// 字节数组操作
#include "bytes.hpp"

// 字符串操作
#include "strings.hpp"

// 对象和全局变量
#include "object.hpp"
#include "allocation.hpp"

// Network protocol glue. Sockets themselves are not here: zeus.socket loads the
// platform library at run time, so nothing in the runtime carries a socket
// implementation.
#include "crypto.hpp"
#include "websocket.hpp"
#include "NativeInjectionRegistry.hpp"

// 线程和同步
#include "thread.hpp"

// 调试和统计
#include "debug.hpp"

namespace ZeusBuiltins {

    // 初始化所有内置函数
    inline void initAllBuiltins() {
        // IO 和基础函数
        registerIOBuiltins();
        
        // 类型转换
        registerTypeBuiltins();
        
        // 数组操作
        registerArrayBuiltins();
        
        // 字节数组
        registerBytesBuiltins();
        
        // 字符串操作
        registerStringBuiltins();
        
        // 对象和全局变量
        registerObjectBuiltins();

        // 线程和同步
        registerThreadBuiltins();
        
        // 调试
        registerDebugBuiltins();

        // Keep legacy native function IDs stable; append new dictionary ABI last.
        registerDictBuiltins();
        registerAllocationBuiltins();
    }

} // namespace ZeusBuiltins
