// ZeusRuntimeLib/ZeusABIInternal.hpp
//
// ABI 实现之间共享的装箱布局。这个文件不属于生成代码的公共 C ABI，
// 只允许 ZeusRuntimeLib 内部实现 include。

#pragma once

#include "ZeusABI.hpp"
#include "ZeusVariant.hpp"

// C ABI 对外只暴露不透明的 ZeusBox；运行库内部需要看到其真实值。
struct ZeusBox {
    ZeusType::UniversalVariant v;
};
