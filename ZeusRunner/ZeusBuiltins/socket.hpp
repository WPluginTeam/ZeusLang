// ZeusBuiltins/socket.hpp - Socket 操作内置函数
// Part of ZeusBuiltins modular refactoring

#pragma once

#include "core.hpp"

#ifdef _WIN32
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    typedef int SOCKET;
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
#endif

namespace ZeusBuiltins {

    // ===== Socket 操作函数 =====
    
    // __socket_create(): 创建新的socket
    inline ZeusType::UniversalVariant builtin_socket_create(const std::vector<ZeusType::UniversalVariant>& args) {
#ifdef _WIN32
        static bool wsaInitialized = false;
        if (!wsaInitialized) {
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw std::runtime_error("__socket_create: WSAStartup failed");
            }
            wsaInitialized = true;
        }
        SOCKET sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sockfd == INVALID_SOCKET) {
            throw std::runtime_error("__socket_create: socket creation failed");
        }
#else
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            throw std::runtime_error("__socket_create: socket creation failed");
        }
#endif

        if (!g_vmSockets) {
            throw std::runtime_error("__socket_create: g_vmSockets not initialized");
        }

        ZeusType::SocketData socketData;
        socketData.sockfd = static_cast<int>(sockfd);
        socketData.isConnected = false;
        socketData.remoteAddr = "";
        socketData.remotePort = 0;

        // 使用互斥锁保护 socket 集合操作
        std::lock_guard<std::mutex> lock(*g_vmSocketsMutex);

        int id = -1;
        if (g_vmFreeSocketSlots && !g_vmFreeSocketSlots->empty()) {
            id = g_vmFreeSocketSlots->back();
            g_vmFreeSocketSlots->pop_back();
            if (g_vmFreeSocketSlotsSize && *g_vmFreeSocketSlotsSize > 0) (*g_vmFreeSocketSlotsSize)--;
            (*g_vmSockets)[id] = std::move(socketData);
        } else {
            id = static_cast<int>(g_vmSockets->size());
            g_vmSockets->push_back(socketData);
        }

        return ZeusType::UniversalVariant(ZeusType::SocketHandle{id});
    }

    // __socket_connect(socket, host, port): 连接到指定主机和端口
    inline ZeusType::UniversalVariant builtin_socket_connect(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) {
            throw std::runtime_error("__socket_connect expects 3 arguments");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_connect: first argument must be SocketHandle");
        }

        int id = std::get<ZeusType::SocketHandle>(args[0]).id;

        if (!g_vmSockets || id < 0 || id >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_connect: invalid socket id");
        }

        LexerType::Value hostVal(0);
        if (!ZeusVMUtils::convertToValue(args[1], hostVal) || hostVal.getStringType() != "string") {
            throw std::runtime_error("__socket_connect: host must be string");
        }

        LexerType::Value portVal(0);
        if (!ZeusVMUtils::convertToValue(args[2], portVal) || portVal.getStringType() != "int") {
            throw std::runtime_error("__socket_connect: port must be number");
        }

        std::string host = std::get<std::string>(hostVal.data);
        int port = static_cast<int>(std::get<ZeusType::BigInteger>(portVal.data).toInt64());

        auto& socketData = (*g_vmSockets)[id];
        SOCKET sockfd = static_cast<SOCKET>(socketData.sockfd);

        // 使用 getaddrinfo 来解析主机名（支持 localhost 等主机名）
        struct addrinfo hints{}, *result = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        
        std::string portStr = std::to_string(port);
        int gai_result = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);

        if (gai_result != 0) {
            throw std::runtime_error("__socket_connect: failed to resolve host '" + host + "'");
        }

        int connectResult = connect(sockfd, result->ai_addr, (int)result->ai_addrlen);
        freeaddrinfo(result);

#ifdef _WIN32
        if (connectResult == SOCKET_ERROR) {
            int err = WSAGetLastError();
            throw std::runtime_error("__socket_connect: connection failed (error " + std::to_string(err) + ")");
        }
#else
        if (connectResult < 0) {
            throw std::runtime_error("__socket_connect: connection failed");
        }
#endif

        socketData.isConnected = true;
        socketData.remoteAddr = host;
        socketData.remotePort = port;

        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __socket_bind(socket, host, port): 绑定到指定主机和端口
    inline ZeusType::UniversalVariant builtin_socket_bind(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 3) {
            throw std::runtime_error("__socket_bind expects 3 arguments");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_bind: first argument must be SocketHandle");
        }

        int id = std::get<ZeusType::SocketHandle>(args[0]).id;

        if (!g_vmSockets || id < 0 || id >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_bind: invalid socket id");
        }

        LexerType::Value hostVal(0);
        if (!ZeusVMUtils::convertToValue(args[1], hostVal) || hostVal.getStringType() != "string") {
            throw std::runtime_error("__socket_bind: host must be string");
        }

        LexerType::Value portVal(0);
        if (!ZeusVMUtils::convertToValue(args[2], portVal) || portVal.getStringType() != "int") {
            throw std::runtime_error("__socket_bind: port must be number");
        }

        std::string host = std::get<std::string>(hostVal.data);
        int port = static_cast<int>(std::get<ZeusType::BigInteger>(portVal.data).toInt64());

        auto& socketData = (*g_vmSockets)[id];

        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);

#ifdef _WIN32
        serverAddr.sin_addr.s_addr = inet_addr(host.c_str());
#else
        inet_pton(AF_INET, host.c_str(), &serverAddr.sin_addr);
#endif

        SOCKET sockfd = static_cast<SOCKET>(socketData.sockfd);
        int result = bind(sockfd, (sockaddr*)&serverAddr, sizeof(serverAddr));

#ifdef _WIN32
        if (result == SOCKET_ERROR) {
            throw std::runtime_error("__socket_bind: bind failed");
        }
#else
        if (result < 0) {
            throw std::runtime_error("__socket_bind: bind failed");
        }
#endif

        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __socket_listen(socket, backlog): 开始监听连接
    inline ZeusType::UniversalVariant builtin_socket_listen(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("__socket_listen expects 2 arguments");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_listen: first argument must be SocketHandle");
        }

        int id = std::get<ZeusType::SocketHandle>(args[0]).id;

        if (!g_vmSockets || id < 0 || id >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_listen: invalid socket id");
        }

        LexerType::Value backlogVal(0);
        if (!ZeusVMUtils::convertToValue(args[1], backlogVal) || backlogVal.getStringType() != "int") {
            throw std::runtime_error("__socket_listen: backlog must be number");
        }

        int backlog = static_cast<int>(std::get<ZeusType::BigInteger>(backlogVal.data).toInt64());

        auto& socketData = (*g_vmSockets)[id];
        SOCKET sockfd = static_cast<SOCKET>(socketData.sockfd);
        int result = listen(sockfd, backlog);

#ifdef _WIN32
        if (result == SOCKET_ERROR) {
            throw std::runtime_error("__socket_listen: listen failed");
        }
#else
        if (result < 0) {
            throw std::runtime_error("__socket_listen: listen failed");
        }
#endif

        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __socket_accept(socket): 接受连接
    inline ZeusType::UniversalVariant builtin_socket_accept(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("__socket_accept expects 1 argument");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_accept: argument must be SocketHandle");
        }

        int id = std::get<ZeusType::SocketHandle>(args[0]).id;

        if (!g_vmSockets || id < 0 || id >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_accept: invalid socket id");
        }

        auto& socketData = (*g_vmSockets)[id];
        SOCKET sockfd = static_cast<SOCKET>(socketData.sockfd);

        sockaddr_in clientAddr{};

#ifdef _WIN32
        int clientAddrLen = sizeof(clientAddr);
#else
        socklen_t clientAddrLen = sizeof(clientAddr);
#endif

        // accept 是阻塞调用，不需要在锁内
        SOCKET clientSockfd = accept(sockfd, (sockaddr*)&clientAddr, &clientAddrLen);

#ifdef _WIN32
        if (clientSockfd == INVALID_SOCKET) {
            throw std::runtime_error("__socket_accept: accept failed");
        }
#else
        if (clientSockfd < 0) {
            throw std::runtime_error("__socket_accept: accept failed");
        }
#endif

        ZeusType::SocketData clientSocketData;
        clientSocketData.sockfd = static_cast<int>(clientSockfd);
        clientSocketData.isConnected = true;

        char clientIP[INET_ADDRSTRLEN];
#ifdef _WIN32
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
#else
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIP, INET_ADDRSTRLEN);
#endif
        clientSocketData.remoteAddr = std::string(clientIP);
        clientSocketData.remotePort = ntohs(clientAddr.sin_port);

        // 使用互斥锁保护 socket 集合操作
        std::lock_guard<std::mutex> lock(*g_vmSocketsMutex);

        int clientId = -1;
        if (g_vmFreeSocketSlots && !g_vmFreeSocketSlots->empty()) {
            clientId = g_vmFreeSocketSlots->back();
            g_vmFreeSocketSlots->pop_back();
            if (g_vmFreeSocketSlotsSize && *g_vmFreeSocketSlotsSize > 0) (*g_vmFreeSocketSlotsSize)--;
            (*g_vmSockets)[clientId] = std::move(clientSocketData);
        } else {
            clientId = static_cast<int>(g_vmSockets->size());
            g_vmSockets->push_back(clientSocketData);
        }

        return ZeusType::UniversalVariant(ZeusType::SocketHandle{clientId});
    }

    // __socket_send(socket, bytes): 发送字节数据
    inline ZeusType::UniversalVariant builtin_socket_send(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("__socket_send expects 2 arguments");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_send: first argument must be SocketHandle");
        }

        // 支持直接Bytes或Variant包装的Bytes
        const ZeusType::Bytes* bytes = nullptr;
        if (auto* bp = std::get_if<ZeusType::Bytes>(&args[1])) {
            bytes = bp;
        } else if (auto* vp = std::get_if<ZeusType::Variant>(&args[1])) {
            bytes = std::get_if<ZeusType::Bytes>(vp);
        }

        if (!bytes) {
            throw std::runtime_error("__socket_send: second argument must be Bytes");
        }

        int socketId = std::get<ZeusType::SocketHandle>(args[0]).id;

        if (!g_vmSockets || socketId < 0 || socketId >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_send: invalid socket id");
        }

        auto& socketData = (*g_vmSockets)[socketId];

        SOCKET sockfd = static_cast<SOCKET>(socketData.sockfd);
        int result = send(sockfd, reinterpret_cast<const char*>(bytes->data.data()), 
                         static_cast<int>(bytes->data.size()), 0);

#ifdef _WIN32
        if (result == SOCKET_ERROR) {
            throw std::runtime_error("__socket_send: send failed");
        }
#else
        if (result < 0) {
            throw std::runtime_error("__socket_send: send failed");
        }
#endif

        return ZeusType::UniversalVariant(LexerType::Value(ZeusType::BigInteger(result)));
    }

    // __socket_recv(socket, size): 接收指定大小的字节数据
    inline ZeusType::UniversalVariant builtin_socket_recv(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 2) {
            throw std::runtime_error("__socket_recv expects 2 arguments");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_recv: first argument must be SocketHandle");
        }

        int socketId = std::get<ZeusType::SocketHandle>(args[0]).id;

        if (!g_vmSockets || socketId < 0 || socketId >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_recv: invalid socket id");
        }

        LexerType::Value sizeVal(0);
        if (!ZeusVMUtils::convertToValue(args[1], sizeVal) || sizeVal.getStringType() != "int") {
            throw std::runtime_error("__socket_recv: size must be number");
        }

        int size = static_cast<int>(std::get<ZeusType::BigInteger>(sizeVal.data).toInt64());

        if (size <= 0) {
            throw std::runtime_error("__socket_recv: size must be positive");
        }

        auto& socketData = (*g_vmSockets)[socketId];
        SOCKET sockfd = static_cast<SOCKET>(socketData.sockfd);

        std::vector<uint8_t> buffer(size);
        int result = recv(sockfd, reinterpret_cast<char*>(buffer.data()), size, 0);

#ifdef _WIN32
        if (result == SOCKET_ERROR) {
            throw std::runtime_error("__socket_recv: recv failed");
        }
#else
        if (result < 0) {
            throw std::runtime_error("__socket_recv: recv failed");
        }
#endif

        buffer.resize(result);

        ZeusType::Bytes bytes;
        bytes.data = std::move(buffer);

        return ZeusType::UniversalVariant(ZeusType::Variant(bytes));
    }

    // __socket_close(socket): 关闭socket
    inline ZeusType::UniversalVariant builtin_socket_close(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("__socket_close expects 1 argument");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_close: argument must be SocketHandle");
        }

        int id = std::get<ZeusType::SocketHandle>(args[0]).id;

        // 使用互斥锁保护 socket 操作
        std::lock_guard<std::mutex> lock(*g_vmSocketsMutex);

        if (!g_vmSockets || id < 0 || id >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_close: invalid socket id");
        }

        auto& socketData = (*g_vmSockets)[id];
        SOCKET sockfd = static_cast<SOCKET>(socketData.sockfd);

#ifdef _WIN32
        closesocket(sockfd);
#else
        close(sockfd);
#endif

        socketData.isConnected = false;
        socketData.sockfd = -1;

        // Add closed socket slot to free list for reuse
        if (g_vmFreeSocketSlots) {
            g_vmFreeSocketSlots->push_back(id);
            if (g_vmFreeSocketSlotsSize) (*g_vmFreeSocketSlotsSize)++;
        }

        return ZeusType::UniversalVariant(LexerType::Value(true));
    }

    // __socket_is_connected(socket): 检查socket是否已连接
    inline ZeusType::UniversalVariant builtin_socket_is_connected(const std::vector<ZeusType::UniversalVariant>& args) {
        if (args.size() != 1) {
            throw std::runtime_error("__socket_is_connected expects 1 argument");
        }

        if (!std::holds_alternative<ZeusType::SocketHandle>(args[0])) {
            throw std::runtime_error("__socket_is_connected: argument must be SocketHandle");
        }

        int id = std::get<ZeusType::SocketHandle>(args[0]).id;

        if (!g_vmSockets || id < 0 || id >= static_cast<int>(g_vmSockets->size())) {
            throw std::runtime_error("__socket_is_connected: invalid socket id");
        }

        const auto& socketData = (*g_vmSockets)[id];

        return ZeusType::UniversalVariant(LexerType::Value(socketData.isConnected));
    }

    // 注册 Socket 相关内置函数
    inline void registerSocketBuiltins() {
        registerBuiltin("__socket_create", builtin_socket_create, 0, 0);
        registerBuiltin("__socket_connect", builtin_socket_connect, 3, 3);
        registerBuiltin("__socket_bind", builtin_socket_bind, 3, 3);
        registerBuiltin("__socket_listen", builtin_socket_listen, 2, 2);
        registerBuiltin("__socket_accept", builtin_socket_accept, 1, 1);
        registerBuiltin("__socket_send", builtin_socket_send, 2, 2);
        registerBuiltin("__socket_recv", builtin_socket_recv, 2, 2);
        registerBuiltin("__socket_close", builtin_socket_close, 1, 1);
        registerBuiltin("__socket_is_connected", builtin_socket_is_connected, 1, 1);
    }

} // namespace ZeusBuiltins
