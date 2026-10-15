#pragma once

#include "ZeusABI.hpp"

#ifdef __cplusplus
extern "C" {
#define ZEUS_SOCKET_NOEXCEPT noexcept
#else
#define ZEUS_SOCKET_NOEXCEPT
#endif

// Socket handles are process-wide runtime IDs. Closed IDs are never reused.
int64_t zeus_socket_create(void) ZEUS_SOCKET_NOEXCEPT;
bool zeus_socket_connect(
    int64_t socket_id,
    ZeusNativeString host,
    int64_t port
) ZEUS_SOCKET_NOEXCEPT;
bool zeus_socket_bind(
    int64_t socket_id,
    ZeusNativeString host,
    int64_t port
) ZEUS_SOCKET_NOEXCEPT;
bool zeus_socket_listen(int64_t socket_id, int64_t backlog) ZEUS_SOCKET_NOEXCEPT;
int64_t zeus_socket_accept(int64_t socket_id) ZEUS_SOCKET_NOEXCEPT;
int64_t zeus_socket_send(
    int64_t socket_id,
    const uint8_t* data,
    int64_t length
) ZEUS_SOCKET_NOEXCEPT;
int64_t zeus_socket_send_all(
    int64_t socket_id,
    const uint8_t* data,
    int64_t length
) ZEUS_SOCKET_NOEXCEPT;
int64_t zeus_socket_recv(
    int64_t socket_id,
    uint8_t* output,
    int64_t capacity
) ZEUS_SOCKET_NOEXCEPT;
int64_t zeus_socket_recv_exact(
    int64_t socket_id,
    uint8_t* output,
    int64_t capacity
) ZEUS_SOCKET_NOEXCEPT;
bool zeus_socket_close(int64_t socket_id) ZEUS_SOCKET_NOEXCEPT;
bool zeus_socket_is_connected(int64_t socket_id, bool* connected) ZEUS_SOCKET_NOEXCEPT;

uint64_t zeus_socket_open_count(void) ZEUS_SOCKET_NOEXCEPT;
uint64_t zeus_socket_connected_count(void) ZEUS_SOCKET_NOEXCEPT;
ZeusNativeString zeus_socket_last_error(void) ZEUS_SOCKET_NOEXCEPT;

#ifdef __cplusplus
}
#endif

#undef ZEUS_SOCKET_NOEXCEPT
