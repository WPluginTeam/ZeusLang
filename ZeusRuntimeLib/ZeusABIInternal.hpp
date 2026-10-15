// ZeusRuntimeLib/ZeusABIInternal.hpp
//
// ABI 实现之间共享的装箱布局。这个文件不属于生成代码的公共 C ABI，
// 只允许 ZeusRuntimeLib 内部实现 include。

#pragma once

#include "ZeusABI.hpp"
#include "ZeusObjectLifetime.hpp"
#include "ZeusVariant.hpp"
#include "ZeusGC.hpp"
#include "ZeusRuntimeDebug.hpp"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <utility>

// C ABI 对外只暴露不透明的 ZeusBox；运行库内部需要看到其真实值。
//
// A rooted box is a GC root: it keeps every managed node reachable from its
// value alive, so its root link is tied to the box itself. An unrooted box is
// a graph edge (a capture cell payload, a closure record entry) and must not
// keep its own subgraph alive, otherwise cycles could never be collected.
// `v` is declared first so the root link is unlinked before the variant dies.
struct ZeusBox;

struct ZeusBox {
    ZeusType::UniversalVariant v;
    ZeusGC::RootLink gcRoot;

    static void* operator new(std::size_t size) {
        return allocate(size, "<unspecified>", 0, "ZeusBox");
    }
    static void* operator new(std::size_t size, const char* sourceFile, int sourceLine) {
        return allocate(size, sourceFile, sourceLine, "ZeusBox");
    }
    static void* operator new(
        std::size_t size, const char* sourceFile, int sourceLine, const char* label
    ) {
        return allocate(size, sourceFile, sourceLine, label);
    }
    static void operator delete(void* pointer) noexcept {
        if (!pointer) return;
        ZeusRuntimeDebug::trackDestroyed(ZeusRuntimeDebug::ResourceKind::ZeusBox, pointer);
        ::operator delete(pointer);
    }
    static void operator delete(void* pointer, std::size_t) noexcept {
        operator delete(pointer);
    }
    static void operator delete(void* pointer, const char*, int) noexcept {
        operator delete(pointer);
    }
    static void operator delete(void* pointer, const char*, int, const char*) noexcept {
        operator delete(pointer);
    }

    ZeusBox() : rooted_(true) { gcRoot.attach(&v); }
    ZeusBox(const ZeusBox& other) : v(other.v), rooted_(true) {
        gcRoot.attach(&v);
        acquireClassReference();
    }
    ZeusBox(const ZeusBox& other, bool root) : v(other.v), rooted_(root) {
        if (rooted_) gcRoot.attach(&v);
        acquireClassReference();
    }
    ZeusBox(ZeusBox&& other)
        : v(std::move(other.v)), rooted_(other.rooted_),
          ownsClassReference_(other.ownsClassReference_),
          graphReference_(other.graphReference_) {
        if (rooted_) gcRoot.attach(&v);
        other.ownsClassReference_ = false;
    }
    ~ZeusBox() noexcept {
        releaseClassReference();
    }

    ZeusBox& operator=(const ZeusBox& other) {
        if (this != &other) assignVariant(other.v);
        return *this;
    }
    ZeusBox& operator=(ZeusBox&& other) {
        if (this != &other) {
            void* sourceObject = classReferenceObject(other.v);
            const bool sourceOwned = other.ownsClassReference_;
            const bool sourceGraphReference = other.graphReference_;
            ZeusType::UniversalVariant moved = std::move(other.v);
            other.ownsClassReference_ = false;
            assignVariant(std::move(moved));
            if (sourceOwned) {
                releaseClassReference(sourceObject, sourceGraphReference);
            }
        }
        return *this;
    }

    void assignVariant(ZeusType::UniversalVariant value) {
        void* replacementObject = classReferenceObject(value);
        if (replacementObject) {
            retainClassReference(replacementObject, !rooted_);
        }
        releaseClassReference();
        v = std::move(value);
        ownsClassReference_ = replacementObject != nullptr;
        graphReference_ = !rooted_;
    }

private:
    static void* classReferenceObject(const ZeusType::UniversalVariant& value) noexcept {
        const auto* handle = std::get_if<ZeusType::LLVMClassHandle>(&value);
        if (!handle || !handle->state) return nullptr;
        if (handle->state->lifetime) return handle->state->lifetime.get();
        return handle->state->object.load(std::memory_order_acquire);
    }

    static void retainClassReference(void* object, bool graphReference) {
        if (!object) return;
        if (graphReference) zeus_class_ref_retain_edge(object);
        else zeus_class_ref_retain(object);
    }

    static void releaseClassReference(void* object, bool graphReference) noexcept {
        if (!object) return;
        try {
            if (graphReference) zeus_class_ref_release_edge(object);
            else zeus_class_ref_release(object);
        } catch (...) {
            std::abort();
        }
    }

    void acquireClassReference() {
        void* object = classReferenceObject(v);
        if (!object) return;
        graphReference_ = !rooted_;
        retainClassReference(object, graphReference_);
        ownsClassReference_ = true;
    }

    void releaseClassReference() noexcept {
        if (!ownsClassReference_) return;
        void* object = classReferenceObject(v);
        const bool graphReference = graphReference_;
        ownsClassReference_ = false;
        releaseClassReference(object, graphReference);
    }

    bool rooted_ = false;
    bool ownsClassReference_ = false;
    bool graphReference_ = false;

    static void* allocate(
        std::size_t size,
        const char* sourceFile,
        int sourceLine,
        const char* label
    ) {
        void* pointer = ::operator new(size);
        ZeusRuntimeDebug::trackCreated(
            ZeusRuntimeDebug::ResourceKind::ZeusBox,
            pointer,
            label,
            sourceFile,
            sourceLine < 0 ? 0u : static_cast<uint32_t>(sourceLine),
            size
        );
        return pointer;
    }
};

#define ZEUS_NEW_BOX(...) new (__FILE__, __LINE__) ZeusBox(__VA_ARGS__)
#define ZEUS_NEW_BOX_NAMED(label, ...) \
    new (__FILE__, __LINE__, label) ZeusBox(__VA_ARGS__)

// Internal capture-cell values are graph edges rather than collector roots.
void zeus_value_clone_internal(ZeusValue* out, const ZeusValue* value);
void zeus_value_release_internal(ZeusValue* value);
bool zeus_value_slot_is_gc_edge(const ZeusValue* slot);
