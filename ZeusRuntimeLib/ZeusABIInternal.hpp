// ZeusRuntimeLib/ZeusABIInternal.hpp
//
// ABI 实现之间共享的装箱布局。这个文件不属于生成代码的公共 C ABI，
// 只允许 ZeusRuntimeLib 内部实现 include。

#pragma once

#include "ZeusABI.hpp"
#include "ZeusVariant.hpp"
#include "ZeusGC.hpp"

#include <utility>

// C ABI 对外只暴露不透明的 ZeusBox；运行库内部需要看到其真实值。
//
// A rooted box is a GC root: it keeps every managed node reachable from its
// value alive, so its root link is tied to the box itself. An unrooted box is
// a graph edge (a capture cell payload, a closure record entry) and must not
// keep its own subgraph alive, otherwise cycles could never be collected.
// `v` is declared first so the root link is unlinked before the variant dies.
struct ZeusBox {
    ZeusType::UniversalVariant v;
    ZeusGC::RootLink gcRoot;

    ZeusBox() { gcRoot.attach(&v); }
    ZeusBox(const ZeusBox& other) : v(other.v) { gcRoot.attach(&v); }
    ZeusBox(const ZeusBox& other, bool root) : v(other.v) {
        if (root) gcRoot.attach(&v);
    }
    ZeusBox(ZeusBox&& other) : v(std::move(other.v)) { gcRoot.attach(&v); }
    ZeusBox& operator=(const ZeusBox& other) {
        if (this != &other) v = other.v;
        return *this;
    }
    ZeusBox& operator=(ZeusBox&& other) {
        if (this != &other) v = std::move(other.v);
        return *this;
    }
};

// Internal capture-cell values are graph edges rather than collector roots.
void zeus_value_clone_internal(ZeusValue* out, const ZeusValue* value);
void zeus_value_release_internal(ZeusValue* value);
