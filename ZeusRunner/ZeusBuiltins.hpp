// ZeusBuiltins.hpp - Zeus 内置函数注册表
// 重构后：只负责注册函数，具体实现分散在 ZeusBuiltins/*.hpp 模块中

#pragma once

// 包含所有模块化的内置函数实现
#include "ZeusBuiltins/all.hpp"

namespace ZeusBuiltins {

    // ===== 主函数注册入口 =====
    // 调用所有模块的注册函数
    inline void initCoreBuiltins() {
        initAllBuiltins();
    }

    // ===== 调试辅助 =====
    // 打印全部 delayPrint 缓存（不清空）—— 仅供 C++ 调用
    inline void printAll() {
        for (const auto& line : g_delayPrintBuffer) {
            std::cout << line << '\n';
        }
    }

}  // namespace ZeusBuiltins
