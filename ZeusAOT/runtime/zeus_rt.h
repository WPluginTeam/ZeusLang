#pragma once

// Zeus AOT demo 入口头：runtime 的 main 在这里，程序体（zeus_entry）由 .ll 提供。
// ABI 函数声明见正式契约头 ZeusRuntimeLib/ZeusABI.hpp。

#include "../../ZeusRuntimeLib/ZeusABI.hpp"

#ifdef __cplusplus
extern "C" {
#endif

// 由生成的 .ll 定义：对应 .zeu 顶层代码。
int zeus_entry(void);

#ifdef __cplusplus
}
#endif
