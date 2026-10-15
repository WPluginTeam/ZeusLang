#pragma once

#include <cstdint>

using ZeusClassDestroyCallback = void (*)(void*);

extern "C" {

// Registers one native class allocation. Re-registering the same pointer is
// harmless, which lets a factory return an object created by another `new`.
void zeus_class_ref_register(
    void* object,
    ZeusClassDestroyCallback destroy,
    const char* debug_type = nullptr
) noexcept;

// Registers a class payload inside a tracked Zeus malloc allocation.
void zeus_class_ref_register_embedded(
    void* object,
    ZeusClassDestroyCallback destroy,
    const char* type_name,
    uint64_t storage_size
) noexcept(false);
// 0: fresh storage, 1: live object, 2: cleared slot ready for reuse.
int32_t zeus_class_ref_embedded_slot_state(
    void* object,
    const char* type_name,
    uint64_t storage_size
) noexcept(false);
// Class slots keep their payloads at unique addresses across clear/reuse so an
// old native pointer cannot become an alias to a later occupant.
void* zeus_class_ref_embedded_slot_object(
    void* slot,
    const char* type_name,
    uint64_t storage_size
) noexcept(false);
void zeus_class_ref_embedded_slot_assign(
    void* slot,
    void* value,
    ZeusClassDestroyCallback destroy,
    const char* type_name,
    uint64_t storage_size
) noexcept(false);
void* zeus_class_ref_embedded_slot_create(
    void* slot,
    ZeusClassDestroyCallback destroy,
    const char* type_name,
    uint64_t storage_size
) noexcept(false);
void zeus_class_ref_embedded_slot_clear(
    void* slot,
    const char* type_name,
    uint64_t storage_size
) noexcept(false);
void zeus_class_ref_embedded_set_initialized(void* object, int32_t initialized)
    noexcept(false);
void zeus_class_ref_require_embedded_live(
    void* object,
    const char* type_name,
    uint64_t storage_size
) noexcept(false);
void zeus_class_ref_destroy_embedded_in_allocation(void* allocation)
    noexcept(false);

// Returns one only when object belongs to this lifetime manager and was
// retained successfully. Dynamic ZeusValue handles use this to own a lease.
int32_t zeus_class_ref_try_retain(void* object) noexcept;
// `try_retain`/`release_lease` protect the tombstone allocation owned by a
// managed LLVM class state without counting that lease as an external root.
void zeus_class_ref_release_lease(void* object) noexcept;
uint64_t zeus_class_ref_external_reference_count(void* object) noexcept;
void zeus_class_ref_retain(void* object) noexcept(false);
void zeus_class_ref_require_managed_live(void* object) noexcept(false);
void zeus_class_ref_retain_owned(void* object) noexcept(false);
void zeus_class_ref_release(void* object) noexcept(false);
// Native class fields own references too, but those references are graph edges
// and must not keep an otherwise unreachable class cycle alive.
void zeus_class_ref_retain_edge(void* object) noexcept(false);
void zeus_class_ref_release_edge(void* object) noexcept(false);

// Explicit delete runs the destructor immediately. The allocation remains as
// a tombstone until every alias releases its reference.
int32_t zeus_class_ref_destroy_now(void* object) noexcept(false);
void zeus_class_ref_require_live(void* object) noexcept(false);

}

// One copy transaction preserves cycles and shared value-field subgraphs.
using ZeusClassCopyCallback = void* (*)(void*, void*);
extern "C" {
void* zeus_class_copy_begin() noexcept(false);
void* zeus_class_copy_find(void* context, void* source) noexcept(false);
void zeus_class_copy_remember(void* context, void* source, void* target) noexcept(false);
void zeus_class_copy_end(void* context) noexcept(false);
void zeus_class_copy_register(const char* type_name, ZeusClassCopyCallback callback) noexcept(false);
void* zeus_class_copy_object(void* source, void* context) noexcept(false);
}

extern "C" {
void zeus_class_copy_register_default(const char* type_name, ZeusClassCopyCallback callback) noexcept(false);
void* zeus_class_copy_default_object(void* source, void* context) noexcept(false);
}

extern "C" {
void zeus_class_copy_remember_allocated(void* context, void* source, void* target) noexcept(false);
void* zeus_class_copy_owned(void* source, int32_t skip_hook) noexcept(false);
}
