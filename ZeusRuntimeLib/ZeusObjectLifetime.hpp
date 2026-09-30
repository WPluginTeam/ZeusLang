#pragma once

#include <cstdint>

using ZeusClassDestroyCallback = void (*)(void*);

extern "C" {

// Registers one native class allocation. Re-registering the same pointer is
// harmless, which lets a factory return an object created by another `new`.
void zeus_class_ref_register(void* object, ZeusClassDestroyCallback destroy) noexcept;

// Returns one only when object belongs to this lifetime manager and was
// retained successfully. Dynamic ZeusValue handles use this to own a lease.
int32_t zeus_class_ref_try_retain(void* object) noexcept;
void zeus_class_ref_retain(void* object) noexcept(false);
void zeus_class_ref_release(void* object) noexcept(false);

// Explicit delete runs the destructor immediately. The allocation remains as
// a tombstone until every alias releases its reference.
int32_t zeus_class_ref_destroy_now(void* object) noexcept(false);
void zeus_class_ref_require_live(void* object) noexcept(false);

}
