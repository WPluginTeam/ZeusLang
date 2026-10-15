#pragma once

#include "ZeusABI.hpp"
#ifndef ZEUS_GC_VM_ONLY
#include "ZeusObjectLifetime.hpp"
#endif
#include "ZeusVariant.hpp"

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_set>
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
using HasExternalRootsFunction = bool (*)(void* payload, std::size_t internalReferences);

void registerSharedNode(
    const std::shared_ptr<void>& node,
    TraceFunction trace,
    BreakCyclesFunction breakCycles,
    HasExternalRootsFunction hasExternalRoots = nullptr,
    std::size_t nonGraphOwners = 0
);

#ifdef ZEUS_GC_VM_ONLY
inline void* containerClassObject(
    const ZeusType::UniversalVariant& value
) noexcept {
    (void)value;
    return nullptr;
}
#else
inline void* containerClassObject(
    const ZeusType::UniversalVariant& value
) noexcept {
    if (const auto* named =
            std::get_if<std::shared_ptr<ZeusType::NamedArg>>(&value)) {
        return *named ? containerClassObject((*named)->value) : nullptr;
    }
    const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&value);
    if (!handle || !handle->state) return nullptr;
    if (handle->state->lifetime) return handle->state->lifetime.get();
    return handle->state->object.load(std::memory_order_acquire);
}
#endif

// A container node owns one graph edge for each directly stored LLVM class
// value. Nested Array/Object nodes own their own class edges.
inline void retainContainerClassEdge(
    const ZeusType::UniversalVariant& value
) {
#ifdef ZEUS_GC_VM_ONLY
    (void)value;
#else
    if (void* object = containerClassObject(value)) {
        zeus_class_ref_retain_edge(object);
    }
#endif
}

inline void releaseContainerClassEdge(
    const ZeusType::UniversalVariant& value
) {
#ifdef ZEUS_GC_VM_ONLY
    (void)value;
#else
    if (void* object = containerClassObject(value)) {
        zeus_class_ref_release_edge(object);
    }
#endif
}

inline void releaseContainerClassEdgeNoexcept(
    const ZeusType::UniversalVariant& value
) noexcept {
    try {
        releaseContainerClassEdge(value);
    } catch (...) {
        std::abort();
    }
}

inline void appendContainerValue(
    std::vector<ZeusType::UniversalVariant>& values,
    const ZeusType::UniversalVariant& value
) {
    retainContainerClassEdge(value);
    try {
        values.push_back(value);
    } catch (...) {
        releaseContainerClassEdgeNoexcept(value);
        throw;
    }
}

inline void replaceContainerValue(
    ZeusType::UniversalVariant& target,
    const ZeusType::UniversalVariant& value
) {
    ZeusType::UniversalVariant previous = target;
    retainContainerClassEdge(value);
    try {
        target = value;
    } catch (...) {
        try {
            target = std::move(previous);
        } catch (...) {
            std::abort();
        }
        releaseContainerClassEdgeNoexcept(value);
        throw;
    }
    releaseContainerClassEdgeNoexcept(previous);
}

inline void storeObjectContainerValue(
    ZeusType::ObjectData& object,
    const std::string& key,
    const ZeusType::UniversalVariant& value
) {
    auto found = object.data.find(key);
    if (found != object.data.end()) {
        replaceContainerValue(found->second, value);
        return;
    }

    bool edgeRetained = false;
    try {
        retainContainerClassEdge(value);
        edgeRetained = true;
        auto inserted = object.data.emplace(key, value);
        if (!inserted.second) {
            releaseContainerClassEdgeNoexcept(value);
            edgeRetained = false;
            replaceContainerValue(inserted.first->second, value);
            return;
        }
        try {
            object.appendOrderKey(key);
        } catch (...) {
            object.data.erase(inserted.first);
            releaseContainerClassEdgeNoexcept(value);
            edgeRetained = false;
            throw;
        }
        edgeRetained = false;
    } catch (...) {
        if (edgeRetained) releaseContainerClassEdgeNoexcept(value);
        throw;
    }
}

inline void traceArrayDataNode(void* payload, const Visitor& visitor) {
    auto* array = static_cast<ZeusType::ArrayData*>(payload);
    for (const auto& value : array->data) visitor.visitVariant(&value);
}

inline void breakArrayDataCycles(void* payload) {
    auto* array = static_cast<ZeusType::ArrayData*>(payload);
    std::vector<ZeusType::UniversalVariant> values;
    values.swap(array->data);
    array->valueRefCount.store(0, std::memory_order_relaxed);
    for (const auto& value : values) releaseContainerClassEdgeNoexcept(value);
}

inline void traceObjectDataNode(void* payload, const Visitor& visitor) {
    auto* object = static_cast<ZeusType::ObjectData*>(payload);
    for (const auto& item : object->data) visitor.visitVariant(&item.second);
}

inline void breakObjectDataCycles(void* payload) {
    auto* object = static_cast<ZeusType::ObjectData*>(payload);
    std::unordered_map<std::string, ZeusType::UniversalVariant> values;
    // Drop borrowed key views before their owning map is moved out.
    object->orderPositions.clear();
    object->order.clear();
    values.swap(object->data);
    object->deletedOrderCount = 0;
    object->valueRefCount.store(0, std::memory_order_relaxed);
    for (const auto& item : values) {
        releaseContainerClassEdgeNoexcept(item.second);
    }
}

inline std::shared_ptr<ZeusType::ArrayData> makeArrayData() {
    auto data = std::shared_ptr<ZeusType::ArrayData>(
        new ZeusType::ArrayData(),
        [](ZeusType::ArrayData* array) noexcept {
            if (!array) return;
            std::vector<ZeusType::UniversalVariant> values;
            values.swap(array->data);
            for (const auto& value : values) {
                releaseContainerClassEdgeNoexcept(value);
            }
            delete array;
        }
    );
    registerSharedNode(
        std::static_pointer_cast<void>(data),
        traceArrayDataNode,
        breakArrayDataCycles
    );
    return data;
}

inline std::shared_ptr<ZeusType::ObjectData> makeObjectData() {
    auto data = std::shared_ptr<ZeusType::ObjectData>(
        new ZeusType::ObjectData(),
        [](ZeusType::ObjectData* object) noexcept {
            if (!object) return;
            std::unordered_map<std::string, ZeusType::UniversalVariant> values;
            // Drop borrowed key views before their owning map is moved out.
            object->orderPositions.clear();
            object->order.clear();
            values.swap(object->data);
            object->deletedOrderCount = 0;
            for (const auto& item : values) {
                releaseContainerClassEdgeNoexcept(item.second);
            }
            delete object;
        }
    );
    registerSharedNode(
        std::static_pointer_cast<void>(data),
        traceObjectDataNode,
        breakObjectDataCycles
    );
    return data;
}

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

// Marks from explicit roots and externally owned shared_ptr references, then
// breaks the edges of every unreachable managed node. Shared references held
// by registered nodes are subtracted before deciding whether an owner is a root.
void collect(const std::vector<std::shared_ptr<void>>& additionalRoots = {});
// Check the shared-node allocation threshold at a compiler/runtime safe point.
void collectIfNeeded();
// Number of registered nodes whose owner is still alive.
std::size_t liveNodeCount();

// Managed graph nodes include LLVM closure records, capture cells, arrays, and
// dictionaries. External shared_ptr owners (VM stacks/cells and iterators) are
// roots; outgoing references inside registered nodes are graph edges.

// The helpers below stay the VM adapter for the VM's own object and closure
// tables. They are refcount bookkeeping for VM handles; the shared collector
// above owns native closure nodes and is independent of them.
template <typename VMType>
inline void retainVariantIfObject(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.retainObject(std::get<ZeusType::ObjectHandle>(v));
    }
}

template <typename VMType>
inline void releaseVariantIfObject(VMType& vm, const ZeusType::UniversalVariant& v) {
    if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
        vm.releaseObject(std::get<ZeusType::ObjectHandle>(v));
    }
}

template <typename VMType>
inline void retainVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v);

template <typename VMType>
inline void releaseVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v);

template <typename VMType>
inline void retainVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v) {
    std::lock_guard<std::recursive_mutex> graph(graphMutex());
    std::unordered_set<const void*> active;
    std::function<void(const ZeusType::UniversalVariant&)> visit;
    visit = [&](const ZeusType::UniversalVariant& value) {
        if (std::holds_alternative<ZeusType::ObjectHandle>(value)) {
            vm.retainObject(std::get<ZeusType::ObjectHandle>(value));
            return;
        }
        if (std::holds_alternative<ZeusType::ClosureHandle>(value)) {
            vm.retainClosure(std::get<ZeusType::ClosureHandle>(value).id);
            return;
        }
        if (const auto* named = std::get_if<std::shared_ptr<ZeusType::NamedArg>>(&value)) {
            if (*named && active.insert(named->get()).second) {
                visit((*named)->value);
                active.erase(named->get());
            }
            return;
        }
        const auto* wrapped = std::get_if<ZeusType::Variant>(&value);
        if (!wrapped) return;
        std::visit([&](const auto& inner) {
            using T = std::decay_t<decltype(inner)>;
            if constexpr (std::is_same_v<T, ZeusType::Array>) {
                if (!inner.impl) return;
                inner.impl->valueRefCount.fetch_add(1, std::memory_order_relaxed);
                if (!active.insert(inner.impl.get()).second) return;
                for (const auto& item : inner.impl->data) visit(item);
                active.erase(inner.impl.get());
            } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
                if (!inner.impl) return;
                inner.impl->valueRefCount.fetch_add(1, std::memory_order_relaxed);
                if (!active.insert(inner.impl.get()).second) return;
                for (const auto& item : inner.impl->data) visit(item.second);
                active.erase(inner.impl.get());
            }
        }, *wrapped);
    };
    visit(v);
}

template <typename VMType>
inline void releaseVariantDeep(VMType& vm, const ZeusType::UniversalVariant& v) {
    std::lock_guard<std::recursive_mutex> graph(graphMutex());
    std::unordered_set<const void*> active;
    std::function<void(const ZeusType::UniversalVariant&)> visit;
    visit = [&](const ZeusType::UniversalVariant& value) {
        if (std::holds_alternative<ZeusType::ObjectHandle>(value)) {
            vm.releaseObject(std::get<ZeusType::ObjectHandle>(value));
            return;
        }
        if (std::holds_alternative<ZeusType::ClosureHandle>(value)) {
            vm.releaseClosure(std::get<ZeusType::ClosureHandle>(value).id);
            return;
        }
        if (const auto* named = std::get_if<std::shared_ptr<ZeusType::NamedArg>>(&value)) {
            if (*named && active.insert(named->get()).second) {
                visit((*named)->value);
                active.erase(named->get());
            }
            return;
        }
        const auto* wrapped = std::get_if<ZeusType::Variant>(&value);
        if (!wrapped) return;
        std::visit([&](const auto& inner) {
            using T = std::decay_t<decltype(inner)>;
            if constexpr (std::is_same_v<T, ZeusType::Array>) {
                if (!inner.impl) return;
                if (active.insert(inner.impl.get()).second) {
                    for (const auto& item : inner.impl->data) visit(item);
                    active.erase(inner.impl.get());
                }
                inner.impl->valueRefCount.fetch_sub(1, std::memory_order_relaxed);
            } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
                if (!inner.impl) return;
                if (active.insert(inner.impl.get()).second) {
                    for (const auto& item : inner.impl->data) visit(item.second);
                    active.erase(inner.impl.get());
                }
                inner.impl->valueRefCount.fetch_sub(1, std::memory_order_relaxed);
            }
        }, *wrapped);
    };
    visit(v);
}

} // namespace ZeusGC
