#pragma once

#include "ZeusABI.hpp"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ZeusThreadTask)(void* context);
typedef void (*ZeusThreadContextDestroy)(void* context);

int64_t zeus_thread_create(void);
/* Ownership of context transfers on success; destroy_context runs once after task exits. */
bool zeus_thread_start_task(
    int64_t thread_id,
    ZeusThreadTask task,
    void* context,
    ZeusThreadContextDestroy destroy_context
);
/* arguments is nil or a Zeus array; its values are copied for the worker. */
bool zeus_thread_start_closure(
    int64_t thread_id,
    const ZeusValue* closure,
    const ZeusValue* arguments
);
bool zeus_thread_join(int64_t thread_id);
bool zeus_thread_is_alive(int64_t thread_id);
int64_t zeus_thread_count(void);
int64_t zeus_thread_active_count(void);
bool zeus_thread_join_all(void);
bool zeus_thread_eliminate(int64_t thread_id);
bool zeus_thread_set_name(int64_t thread_id, ZeusNativeString name);
ZeusNativeString zeus_thread_get_name(int64_t thread_id);
int64_t zeus_thread_current_id(void);
bool zeus_thread_should_stop(void);
bool zeus_thread_sleep(int64_t milliseconds);
bool zeus_thread_reset(void);

/* Process-wide mutex pool shared by VM adapters and AOT code. Ids are
 * positive; every operation on an unknown id returns false. */
int64_t zeus_mutex_create(void);
bool zeus_mutex_lock(int64_t mutex_id);
bool zeus_mutex_trylock(int64_t mutex_id);
bool zeus_mutex_unlock(int64_t mutex_id);
bool zeus_mutex_destroy(int64_t mutex_id);

#ifdef __cplusplus
}
#endif
