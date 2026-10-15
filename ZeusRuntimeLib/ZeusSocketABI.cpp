#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ZeusSocketABI.hpp"
#include "ZeusRuntimeDebug.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
using ZeusNativeSocket = SOCKET;
static constexpr ZeusNativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using ZeusNativeSocket = int;
static constexpr ZeusNativeSocket kInvalidSocket = -1;
#endif

namespace {

thread_local std::string g_socketLastError;

void clearLastError() noexcept {
    g_socketLastError.clear();
}

void setLastError(const char* message) noexcept {
    try {
        g_socketLastError = message ? message : "socket operation failed";
    } catch (...) {
        g_socketLastError.clear();
    }
}

void setLastError(const std::string& message) noexcept {
    try {
        g_socketLastError = message;
    } catch (...) {
        g_socketLastError.clear();
    }
}

int lastSocketErrorCode() noexcept {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

void closeNativeSocket(ZeusNativeSocket socket) noexcept {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}

void shutdownNativeSocket(ZeusNativeSocket socket) noexcept {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

bool initializeSocketPlatform() noexcept {
#ifdef _WIN32
    static std::once_flag initialized;
    static int startupResult = WSASYSNOTREADY;
    try {
        std::call_once(initialized, [] {
            WSADATA data{};
            startupResult = WSAStartup(MAKEWORD(2, 2), &data);
        });
    } catch (...) {
        setLastError("socket platform initialization failed");
        return false;
    }
    if (startupResult != 0) {
        setLastError("WSAStartup failed (error " + std::to_string(startupResult) + ")");
        return false;
    }
#endif
    return true;
}

struct RuntimeSocket {
    RuntimeSocket(ZeusNativeSocket nativeSocket, bool connected)
        : nativeSocket(nativeSocket), connected(connected) {
        ZeusRuntimeDebug::trackCreated(
            ZeusRuntimeDebug::ResourceKind::SocketRecord,
            this,
            "socket",
            __FILE__,
            static_cast<uint32_t>(__LINE__));
    }

    ~RuntimeSocket() {
        closeNativeSocket(nativeSocket.exchange(
            kInvalidSocket, std::memory_order_acq_rel));
        ZeusRuntimeDebug::trackDestroyed(
            ZeusRuntimeDebug::ResourceKind::SocketRecord, this);
    }

    void requestClose() noexcept {
        if (!closing.exchange(true, std::memory_order_acq_rel)) {
            connected.store(false, std::memory_order_release);
            const ZeusNativeSocket socket = nativeSocket.exchange(
                kInvalidSocket, std::memory_order_acq_rel);
            shutdownNativeSocket(socket);
            closeNativeSocket(socket);
        }
    }

    ZeusNativeSocket nativeHandle() const {
        if (closing.load(std::memory_order_acquire)) {
            throw std::runtime_error("socket is closed");
        }
        const ZeusNativeSocket socket = nativeSocket.load(std::memory_order_acquire);
        if (socket == kInvalidSocket) {
            throw std::runtime_error("socket is closed");
        }
        return socket;
    }

    std::atomic<ZeusNativeSocket> nativeSocket{kInvalidSocket};
    std::atomic<bool> connected{false};
    std::atomic<bool> closing{false};
};

class RuntimeSocketTable {
public:
    ~RuntimeSocketTable() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& entry : sockets_) entry.second->requestClose();
        sockets_.clear();
    }

    int64_t insert(ZeusNativeSocket nativeSocket, bool connected) {
        std::shared_ptr<RuntimeSocket> socket;
        try {
            socket = std::make_shared<RuntimeSocket>(nativeSocket, connected);
        } catch (...) {
            closeNativeSocket(nativeSocket);
            throw;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (nextId_ <= 0 || nextId_ == INT64_MAX) return -1;
        const int64_t id = nextId_++;
        sockets_.emplace(id, std::move(socket));
        return id;
    }

    std::shared_ptr<RuntimeSocket> find(int64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sockets_.find(id);
        return found == sockets_.end() ? nullptr : found->second;
    }

    std::shared_ptr<RuntimeSocket> remove(int64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = sockets_.find(id);
        if (found == sockets_.end()) return nullptr;
        auto socket = std::move(found->second);
        sockets_.erase(found);
        return socket;
    }

    uint64_t openCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint64_t>(sockets_.size());
    }

    uint64_t connectedCount() {
        std::lock_guard<std::mutex> lock(mutex_);
        uint64_t count = 0;
        for (const auto& entry : sockets_) {
            if (entry.second->connected.load(std::memory_order_acquire)) ++count;
        }
        return count;
    }

private:
    std::mutex mutex_;
    std::unordered_map<int64_t, std::shared_ptr<RuntimeSocket>> sockets_;
    int64_t nextId_ = 1;
};

RuntimeSocketTable& socketTable() {
    static RuntimeSocketTable table;
    return table;
}

std::string hostString(ZeusNativeString host) {
    if (!host) throw std::runtime_error("host must be a valid native string");
    uint64_t length = 0;
    std::memcpy(&length, host - sizeof(uint64_t), sizeof(length));
    const uint64_t maximumLength = static_cast<uint64_t>(
        std::numeric_limits<size_t>::max() - sizeof(uint64_t) - 1);
    if (length > maximumLength ||
        length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw std::runtime_error("host string length is invalid");
    }
    const char* characters = host;
    if (characters[static_cast<size_t>(length)] != '\0') {
        throw std::runtime_error("host string is missing its terminator");
    }
    std::string result(characters, static_cast<size_t>(length));
    if (result.find('\0') != std::string::npos) {
        throw std::runtime_error("host cannot contain a null byte");
    }
    return result;
}

std::shared_ptr<RuntimeSocket> findSocket(int64_t id) {
    auto socket = socketTable().find(id);
    if (!socket) throw std::runtime_error("invalid socket handle");
    if (socket->closing.load(std::memory_order_acquire)) {
        throw std::runtime_error("socket is closed");
    }
    return socket;
}

bool validPort(int64_t port) noexcept {
    return port >= 0 && port <= 65535;
}

} // namespace

extern "C" int64_t zeus_socket_create(void) noexcept {
    clearLastError();
    try {
        if (!initializeSocketPlatform()) return -1;
#ifdef _WIN32
        const ZeusNativeSocket nativeSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (nativeSocket == INVALID_SOCKET) {
#else
        const ZeusNativeSocket nativeSocket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (nativeSocket < 0) {
#endif
            setLastError("socket creation failed (error " +
                std::to_string(lastSocketErrorCode()) + ")");
            return -1;
        }
        const int64_t id = socketTable().insert(nativeSocket, false);
        if (id < 0) {
            setLastError("socket handle space exhausted");
        }
        return id;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("socket creation failed");
    }
    return -1;
}

extern "C" bool zeus_socket_connect(
    int64_t socket_id,
    ZeusNativeString host,
    int64_t port
) noexcept {
    clearLastError();
    try {
        if (!validPort(port)) throw std::runtime_error("port must be between 0 and 65535");
        const auto socket = findSocket(socket_id);
        const std::string hostText = hostString(host);
        const std::string portText = std::to_string(port);
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        const int resolveResult = getaddrinfo(
            hostText.c_str(), portText.c_str(), &hints, &addresses);
        if (resolveResult != 0 || !addresses) {
            throw std::runtime_error("failed to resolve host '" + hostText + "'");
        }

        const int connectResult = ::connect(
            socket->nativeHandle(),
            addresses->ai_addr,
            static_cast<int>(addresses->ai_addrlen));
        const int errorCode = connectResult == 0 ? 0 : lastSocketErrorCode();
        freeaddrinfo(addresses);
#ifdef _WIN32
        if (connectResult == SOCKET_ERROR) {
#else
        if (connectResult < 0) {
#endif
            throw std::runtime_error("connection failed (error " +
                std::to_string(errorCode) + ")");
        }
        socket->connected.store(true, std::memory_order_release);
        return true;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("connection failed");
    }
    return false;
}

extern "C" bool zeus_socket_bind(
    int64_t socket_id,
    ZeusNativeString host,
    int64_t port
) noexcept {
    clearLastError();
    try {
        if (!validPort(port)) throw std::runtime_error("port must be between 0 and 65535");
        const auto socket = findSocket(socket_id);
        const std::string hostText = hostString(host);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
#ifdef _WIN32
        address.sin_addr.s_addr = inet_addr(hostText.c_str());
#else
        if (::inet_pton(AF_INET, hostText.c_str(), &address.sin_addr) != 1) {
            throw std::runtime_error("host must be an IPv4 address");
        }
#endif
#ifdef _WIN32
        const int addressLength = static_cast<int>(sizeof(address));
#else
        const socklen_t addressLength = static_cast<socklen_t>(sizeof(address));
#endif
        const int result = ::bind(
            socket->nativeHandle(),
            reinterpret_cast<const sockaddr*>(&address),
            addressLength);
#ifdef _WIN32
        if (result == SOCKET_ERROR) {
#else
        if (result < 0) {
#endif
            throw std::runtime_error("bind failed (error " +
                std::to_string(lastSocketErrorCode()) + ")");
        }
        return true;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("bind failed");
    }
    return false;
}

extern "C" bool zeus_socket_listen(int64_t socket_id, int64_t backlog) noexcept {
    clearLastError();
    try {
        const auto socket = findSocket(socket_id);
        const int safeBacklog = static_cast<int>(std::clamp<int64_t>(backlog, 0, INT_MAX));
        if (::listen(socket->nativeHandle(), safeBacklog) != 0) {
            throw std::runtime_error("listen failed (error " +
                std::to_string(lastSocketErrorCode()) + ")");
        }
        return true;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("listen failed");
    }
    return false;
}

extern "C" int64_t zeus_socket_accept(int64_t socket_id) noexcept {
    clearLastError();
    try {
        const auto listener = findSocket(socket_id);
        sockaddr_in peer{};
#ifdef _WIN32
        int peerLength = sizeof(peer);
#else
        socklen_t peerLength = sizeof(peer);
#endif
        const ZeusNativeSocket client = ::accept(
            listener->nativeHandle(),
            reinterpret_cast<sockaddr*>(&peer),
            &peerLength);
#ifdef _WIN32
        if (client == INVALID_SOCKET) {
#else
        if (client < 0) {
#endif
            throw std::runtime_error("accept failed (error " +
                std::to_string(lastSocketErrorCode()) + ")");
        }
        if (listener->closing.load(std::memory_order_acquire)) {
            closeNativeSocket(client);
            throw std::runtime_error("listener closed while accepting");
        }
        const int64_t id = socketTable().insert(client, true);
        if (id < 0) {
            throw std::runtime_error("socket handle space exhausted");
        }
        return id;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("accept failed");
    }
    return -1;
}

extern "C" int64_t zeus_socket_send(
    int64_t socket_id,
    const uint8_t* data,
    int64_t length
) noexcept {
    clearLastError();
    try {
        if (length < 0 || (length > 0 && !data)) {
            throw std::runtime_error("invalid send buffer");
        }
        const auto socket = findSocket(socket_id);
        if (length == 0) return 0;
        const auto count = static_cast<size_t>(std::min<int64_t>(length, INT_MAX));
#ifdef _WIN32
        const int result = ::send(
            socket->nativeHandle(),
            reinterpret_cast<const char*>(data),
            static_cast<int>(count),
            0);
        if (result == SOCKET_ERROR) {
#else
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        const ssize_t result = ::send(socket->nativeHandle(), data, count, flags);
        if (result < 0) {
#endif
            throw std::runtime_error("send failed (error " +
                std::to_string(lastSocketErrorCode()) + ")");
        }
        return static_cast<int64_t>(result);
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("send failed");
    }
    return -1;
}

extern "C" int64_t zeus_socket_send_all(
    int64_t socket_id,
    const uint8_t* data,
    int64_t length
) noexcept {
    clearLastError();
    try {
        if (length < 0 || (length > 0 && !data)) {
            throw std::runtime_error("invalid send buffer");
        }
        const auto socket = findSocket(socket_id);
        int64_t sent = 0;
        while (sent < length) {
            const auto count = static_cast<size_t>(
                std::min<int64_t>(length - sent, INT_MAX));
#ifdef _WIN32
            const int result = ::send(
                socket->nativeHandle(),
                reinterpret_cast<const char*>(data + sent),
                static_cast<int>(count),
                0);
            if (result == SOCKET_ERROR) {
#else
            int flags = 0;
#ifdef MSG_NOSIGNAL
            flags |= MSG_NOSIGNAL;
#endif
            const ssize_t result = ::send(
                socket->nativeHandle(), data + sent, count, flags);
            if (result < 0) {
#endif
                throw std::runtime_error("send failed (error " +
                    std::to_string(lastSocketErrorCode()) + ")");
            }
            if (result == 0) throw std::runtime_error("send made no progress");
            sent += static_cast<int64_t>(result);
        }
        return sent;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("send all failed");
    }
    return -1;
}

extern "C" int64_t zeus_socket_recv(
    int64_t socket_id,
    uint8_t* output,
    int64_t capacity
) noexcept {
    clearLastError();
    try {
        if (capacity <= 0 || capacity > INT_MAX || !output) {
            throw std::runtime_error("receive capacity must be between 1 and INT_MAX");
        }
        const auto socket = findSocket(socket_id);
#ifdef _WIN32
        const int result = ::recv(
            socket->nativeHandle(),
            reinterpret_cast<char*>(output),
            static_cast<int>(capacity),
            0);
        if (result == SOCKET_ERROR) {
#else
        const ssize_t result = ::recv(
            socket->nativeHandle(),
            output,
            static_cast<size_t>(capacity),
            0);
        if (result < 0) {
#endif
            throw std::runtime_error("recv failed (error " +
                std::to_string(lastSocketErrorCode()) + ")");
        }
        return static_cast<int64_t>(result);
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("recv failed");
    }
    return -1;
}

extern "C" int64_t zeus_socket_recv_exact(
    int64_t socket_id,
    uint8_t* output,
    int64_t capacity
) noexcept {
    clearLastError();
    try {
        if (capacity < 0 || capacity > INT_MAX || (capacity > 0 && !output)) {
            throw std::runtime_error("receive size must be between 0 and INT_MAX");
        }
        if (capacity == 0) return 0;
        const auto socket = findSocket(socket_id);
        int64_t received = 0;
        while (received < capacity) {
#ifdef _WIN32
            const int result = ::recv(
                socket->nativeHandle(),
                reinterpret_cast<char*>(output + received),
                static_cast<int>(capacity - received),
                0);
            if (result == SOCKET_ERROR) {
#else
            const ssize_t result = ::recv(
                socket->nativeHandle(),
                output + received,
                static_cast<size_t>(capacity - received),
                0);
            if (result < 0) {
#endif
                throw std::runtime_error("recv failed (error " +
                    std::to_string(lastSocketErrorCode()) + ")");
            }
            if (result == 0) break;
            received += static_cast<int64_t>(result);
        }
        return received;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("recv exact failed");
    }
    return -1;
}

extern "C" bool zeus_socket_close(int64_t socket_id) noexcept {
    clearLastError();
    try {
        auto socket = socketTable().remove(socket_id);
        // Socket close is an idempotent operation. A concurrent owner may
        // already have removed the handle while another thread was blocked
        // in accept/recv on its shared RuntimeSocket record.
        if (!socket) return true;
        socket->requestClose();
        return true;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("socket close failed");
    }
    return false;
}

extern "C" bool zeus_socket_is_connected(
    int64_t socket_id,
    bool* connected
) noexcept {
    clearLastError();
    try {
        if (!connected) throw std::runtime_error("null connection-state output");
        const auto socket = findSocket(socket_id);
        *connected = socket->connected.load(std::memory_order_acquire);
        return true;
    } catch (const std::exception& error) {
        setLastError(error.what());
    } catch (...) {
        setLastError("could not query socket state");
    }
    return false;
}

extern "C" uint64_t zeus_socket_open_count(void) noexcept {
    try {
        return socketTable().openCount();
    } catch (...) {
        return 0;
    }
}

extern "C" uint64_t zeus_socket_connected_count(void) noexcept {
    try {
        return socketTable().connectedCount();
    } catch (...) {
        return 0;
    }
}

extern "C" ZeusNativeString zeus_socket_last_error(void) noexcept {
    try {
        return zeus_string_from_data(
            g_socketLastError.data(), static_cast<int64_t>(g_socketLastError.size()));
    } catch (...) {
        return nullptr;
    }
}
