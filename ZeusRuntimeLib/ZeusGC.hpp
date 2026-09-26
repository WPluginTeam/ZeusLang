#pragma once

#include "ZeusABI.hpp"
#include "ZeusVariant.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <variant>
#include <vector>

namespace ZeusGC {

// A trace callback reports every outgoing dynamic value and managed node.
// Nodes are weakly registered, so a node stays alive exactly as long as the
// shared_ptrs that reach it; the collector only adds cycle breaking on top.
struct Visitor {
    void* context = nullptr;
    void (*value)(void*, const ZeusValue*) = nullptr;
    void (*variant)(void*, const ZeusType::UniversalVariant*) = nullptr;
    void (*node)(void*, const std::shared_ptr<void>*) = nullptr;

    void visitValue(const ZeusValue* item) const {
        if (value && item) value(context, item);
    }
    void visitVariant(const ZeusType::UniversalVariant* item) const {
        if (variant && item) variant(context, item);
    }
    void visitNode(const std::shared_ptr<void>& item) const {
        if (node && item) node(context, &item);
    }
};

using TraceFunction = void (*)(void* payload, const Visitor& visitor);
// breakCycles must drop every outgoing edge of the node. A node that keeps one
// edge after the call can never become unreachable again and leaks forever.
using BreakCyclesFunction = void (*)(void* payload);

void registerSharedNode(
    const std::shared_ptr<void>& node,
    TraceFunction trace,
    BreakCyclesFunction breakCycles
);
void rootNode(const std::shared_ptr<void>& node);
void unrootNode(const std::shared_ptr<void>& node);

// Roots are intrusive links owned by the rooting object, so a root can never
// outlive the value it protects and the hot path allocates nothing.
class RootLink;

struct RootRegistry {
    static void attach(RootLink& link, const ZeusType::UniversalVariant* target);
    static void retract(RootLink& link);
    static std::vector<const ZeusType::UniversalVariant*> snapshot();
};

class RootLink {
public:
    RootLink() noexcept = default;
    ~RootLink() noexcept;
    RootLink(const RootLink&) = delete;
    RootLink& operator=(const RootLink&) = delete;

    void attach(const ZeusType::UniversalVariant* target);
    void retract() noexcept;

private:
    friend struct RootRegistry;
    RootLink* next_ = nullptr;
    const ZeusType::UniversalVariant* target_ = nullptr;
    bool attached_ = false;
};

// Roots a variant for the rest of the process. Use it only for storage whose
// lifetime already outlives every collection point (runtime global/local slots).
void addPermanentRootVariant(const ZeusType::UniversalVariant* value);

// A collection must never reclaim a node that is still being built, so nodes
// are registered inside an allocation scope and any collection triggered while
// one is open is deferred to the outermost exit.
class AllocationScope {
public:
    AllocationScope() noexcept;
    ~AllocationScope();
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;
};

// Serializes graph mutation against collection. The lock order is always
// graph lock -> node lock; never hold a node lock while calling into the GC.
std::recursive_mutex& graphMutex();

// VM adapter hooks: inspect managed-node values using the VM's own object and
// closure marker, then pass discovered native nodes as additional GC roots.
void traceNodeValues(
    const std::shared_ptr<void>& root,
    const std::function<void(const ZeusType::UniversalVariant&)>& visitor
);
void traceRoots(
    const std::function<void(const ZeusType::UniversalVariant&)>& visitor
);

// Marks from the root set and breaks the edges of every unreachable managed
// node. Roots are rooted boxes, permanent slot roots and capture-cell handles;
// `additionalRoots` carries nodes the caller reached but cannot register
// itself (the VM passes the native closures it found while marking).
void collect(const std::vector<std::shared_ptr<void>>& additionalRoots = {});
// Number of registered nodes whose owner is still alive.
std::size_t liveNodeCount();

// Scope: nodes are LLVM closure records and capture cells. Cycles that run
// through boxed containers are not collected, because container payloads are
// not registered as nodes.

// The helpers below stay the VM adapter for the VM's own object and closure
// tables. They are refcount bookkeeping for VM handles; the shared collector
// above owns native closure nodes and is independent of them.
template <typename VMType>
inline void retainVariantIfObject(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.retainObject(std::get<ZeusType::ObjectHandle>(v).id);
    }
}

template <typename VMType>
inline void releaseVariantIfObject(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.releaseObject(std::get<ZeusType::ObjectHandle>(v).id);
    }
}

template <typename VMType>
inline void retainVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v);

template <typename VMType>
inline void releaseVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v);

template <typename VMType>
inline void retainVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.retainObject(std::get<ZeusType::ObjectHandle>(v).id);
        return;
    }
    if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
        vm.retainClosure(std::get<ZeusType::ClosureHandle>(v).id);
        return;
    }
    if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(v)) {
        auto ptr = std::get<std::shared_ptr<ZeusType::NamedArg>>(v);
        if (ptr) retainVariantDeep(vm, ptr->value);
        return;
    }
    if (!std::holds_alternative<ZeusType::Variant>(v)) return;

    const auto& var = std::get<ZeusType::Variant>(v);
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ZeusType::Array>) {
            if (!x.impl) return;
            for (const auto& it : x.impl->data) retainVariantDeep(vm, it);
        } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
            if (!x.impl) return;
            for (const auto& kv : x.impl->data) retainVariantDeep(vm, kv.second);
        }
    }, var);
}

template <typename VMType>
inline void releaseVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.releaseObject(std::get<ZeusType::ObjectHandle>(v).id);
        return;
    }
    if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
        vm.releaseClosure(std::get<ZeusType::ClosureHandle>(v).id);
        return;
    }
    if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(v)) {
        auto ptr = std::get<std::shared_ptr<ZeusType::NamedArg>>(v);
        if (ptr) releaseVariantDeep(vm, ptr->value);
        return;
    }
    if (!std::holds_alternative<ZeusType::Variant>(v)) return;

    const auto& var = std::get<ZeusType::Variant>(v);
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ZeusType::Array>) {
            if (!x.impl) return;
            for (const auto& it : x.impl->data) releaseVariantDeep(vm, it);
        } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
            if (!x.impl) return;
            for (const auto& kv : x.impl->data) releaseVariantDeep(vm, kv.second);
        }
    }, var);
}

} // namespace ZeusGC
