#pragma once

#include "core.hpp"
#include "../../ZeusRuntimeLib/ZeusSocketABI.hpp"

#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ZeusBuiltins {

inline void throwSocketABIError(const char* operation, const char* fallback) {
    const ZeusNativeString detail = zeus_socket_last_error();
    std::string message = operation ? operation : "socket";
    message += ": ";
    uint64_t length = 0;
    if (detail) std::memcpy(&length, detail - sizeof(uint64_t), sizeof(length));
    if (detail && length > 0 &&
        length <= static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        message.append(detail, static_cast<size_t>(length));
    } else {
        message += fallback ? fallback : "operation failed";
    }
    throw std::runtime_error(message);
}

inline std::vector<uint64_t> socketNativeStringBuffer(const std::string& text) {
    if (text.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
        text.size() > std::numeric_limits<size_t>::max() - sizeof(uint64_t) - 1) {
        throw std::length_error("socket host string is too large");
    }
    const size_t byteCount = sizeof(uint64_t) + text.size() + 1;
    const size_t wordCount =
        byteCount / sizeof(uint64_t) + (byteCount % sizeof(uint64_t) != 0);
    std::vector<uint64_t> buffer(wordCount, 0);
    buffer[0] = static_cast<uint64_t>(text.size());
    char* characters = reinterpret_cast<char*>(buffer.data()) + sizeof(uint64_t);
    if (!text.empty()) std::memcpy(characters, text.data(), text.size());
    characters[text.size()] = '\0';
    return buffer;
}

inline ZeusNativeString socketNativeStringData(
    const std::vector<uint64_t>& buffer
) {
    if (buffer.empty()) throw std::invalid_argument("socket native string buffer is empty");
    return reinterpret_cast<const char*>(buffer.data()) + sizeof(uint64_t);
}

inline int64_t socketIdArgument(
    const std::vector<ZeusType::UniversalVariant>& args,
    size_t index,
    const char* operation
) {
    if (index >= args.size() ||
        !std::holds_alternative<ZeusType::SocketHandle>(args[index])) {
        throw std::runtime_error(std::string(operation) +
            ": argument must be SocketHandle");
    }
    return std::get<ZeusType::SocketHandle>(args[index]).id;
}

inline std::string socketStringArgument(
    const ZeusType::UniversalVariant& argument,
    const char* operation
) {
    LexerType::Value value(0);
    if (!ZeusVMUtils::convertToValue(argument, value) ||
        value.getStringType() != "string") {
        throw std::runtime_error(std::string(operation) + ": host must be string");
    }
    return std::get<std::string>(value.data);
}

inline int64_t socketIntegerArgument(
    const ZeusType::UniversalVariant& argument,
    const char* operation,
    const char* expected
) {
    LexerType::Value value(0);
    if (!ZeusVMUtils::convertToValue(argument, value) ||
        value.getStringType() != "int") {
        throw std::runtime_error(std::string(operation) + ": " + expected + " must be number");
    }
    return std::get<ZeusType::BigInteger>(value.data).toInt64();
}

inline const ZeusType::Bytes* socketBytesArgument(
    const ZeusType::UniversalVariant& argument
) {
    if (const auto* bytes = std::get_if<ZeusType::Bytes>(&argument)) return bytes;
    if (const auto* boxed = std::get_if<ZeusType::Variant>(&argument)) {
        return std::get_if<ZeusType::Bytes>(boxed);
    }
    return nullptr;
}

inline ZeusType::UniversalVariant builtin_socket_create(
    const std::vector<ZeusType::UniversalVariant>&
) {
    const int64_t id = zeus_socket_create();
    if (id < 0) throwSocketABIError("__socket_create", "socket creation failed");
    return ZeusType::UniversalVariant(ZeusType::SocketHandle{id});
}

inline ZeusType::UniversalVariant builtin_socket_connect(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 3) throw std::runtime_error("__socket_connect expects 3 arguments");
    const int64_t id = socketIdArgument(args, 0, "__socket_connect");
    const std::string host = socketStringArgument(args[1], "__socket_connect");
    const int64_t port = socketIntegerArgument(args[2], "__socket_connect", "port");
    const std::vector<uint64_t> hostBuffer = socketNativeStringBuffer(host);
    if (!zeus_socket_connect(id, socketNativeStringData(hostBuffer), port)) {
        throwSocketABIError("__socket_connect", "connection failed");
    }
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline ZeusType::UniversalVariant builtin_socket_bind(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 3) throw std::runtime_error("__socket_bind expects 3 arguments");
    const int64_t id = socketIdArgument(args, 0, "__socket_bind");
    const std::string host = socketStringArgument(args[1], "__socket_bind");
    const int64_t port = socketIntegerArgument(args[2], "__socket_bind", "port");
    const std::vector<uint64_t> hostBuffer = socketNativeStringBuffer(host);
    if (!zeus_socket_bind(id, socketNativeStringData(hostBuffer), port)) {
        throwSocketABIError("__socket_bind", "bind failed");
    }
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline ZeusType::UniversalVariant builtin_socket_listen(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) throw std::runtime_error("__socket_listen expects 2 arguments");
    const int64_t id = socketIdArgument(args, 0, "__socket_listen");
    const int64_t backlog = socketIntegerArgument(args[1], "__socket_listen", "backlog");
    if (!zeus_socket_listen(id, backlog)) {
        throwSocketABIError("__socket_listen", "listen failed");
    }
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline ZeusType::UniversalVariant builtin_socket_accept(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1) throw std::runtime_error("__socket_accept expects 1 argument");
    const int64_t id = socketIdArgument(args, 0, "__socket_accept");
    const int64_t clientId = zeus_socket_accept(id);
    if (clientId < 0) throwSocketABIError("__socket_accept", "accept failed");
    return ZeusType::UniversalVariant(ZeusType::SocketHandle{clientId});
}

inline ZeusType::UniversalVariant builtin_socket_send(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) throw std::runtime_error("__socket_send expects 2 arguments");
    const int64_t id = socketIdArgument(args, 0, "__socket_send");
    const ZeusType::Bytes* bytes = socketBytesArgument(args[1]);
    if (!bytes) throw std::runtime_error("__socket_send: second argument must be Bytes");
    if (bytes->data.size() > static_cast<size_t>(INT64_MAX)) {
        throw std::runtime_error("__socket_send: byte buffer is too large");
    }
    const int64_t result = zeus_socket_send(
        id,
        bytes->data.empty() ? nullptr : bytes->data.data(),
        static_cast<int64_t>(bytes->data.size()));
    if (result < 0) throwSocketABIError("__socket_send", "send failed");
    return ZeusType::UniversalVariant(
        LexerType::Value(ZeusType::BigInteger(result)));
}

inline ZeusType::UniversalVariant builtin_socket_send_all(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) {
        throw std::runtime_error("__socket_send_all expects 2 arguments");
    }
    const int64_t id = socketIdArgument(args, 0, "__socket_send_all");
    const ZeusType::Bytes* bytes = socketBytesArgument(args[1]);
    if (!bytes) throw std::runtime_error("__socket_send_all: second argument must be Bytes");
    if (bytes->data.size() > static_cast<size_t>(INT64_MAX)) {
        throw std::runtime_error("__socket_send_all: byte buffer is too large");
    }
    const int64_t result = zeus_socket_send_all(
        id,
        bytes->data.empty() ? nullptr : bytes->data.data(),
        static_cast<int64_t>(bytes->data.size()));
    if (result < 0) throwSocketABIError("__socket_send_all", "send failed");
    return ZeusType::UniversalVariant(
        LexerType::Value(ZeusType::BigInteger(result)));
}

inline ZeusType::UniversalVariant builtin_socket_recv(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) throw std::runtime_error("__socket_recv expects 2 arguments");
    const int64_t id = socketIdArgument(args, 0, "__socket_recv");
    const int64_t capacity = socketIntegerArgument(args[1], "__socket_recv", "size");
    if (capacity <= 0 || capacity > INT_MAX) {
        throw std::runtime_error("__socket_recv: size must be between 1 and INT_MAX");
    }

    ZeusType::Bytes bytes;
    bytes.data.resize(static_cast<size_t>(capacity));
    const int64_t result = zeus_socket_recv(id, bytes.data.data(), capacity);
    if (result < 0) throwSocketABIError("__socket_recv", "recv failed");
    bytes.data.resize(static_cast<size_t>(result));
    return ZeusType::UniversalVariant(ZeusType::Variant(std::move(bytes)));
}

inline ZeusType::UniversalVariant builtin_socket_recv_exact(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2) {
        throw std::runtime_error("__socket_recv_exact expects 2 arguments");
    }
    const int64_t id = socketIdArgument(args, 0, "__socket_recv_exact");
    const int64_t capacity = socketIntegerArgument(args[1], "__socket_recv_exact", "size");
    if (capacity < 0 || capacity > INT_MAX) {
        throw std::runtime_error("__socket_recv_exact: size must be between 0 and INT_MAX");
    }

    ZeusType::Bytes bytes;
    bytes.data.resize(static_cast<size_t>(capacity));
    const int64_t result = zeus_socket_recv_exact(
        id,
        bytes.data.empty() ? nullptr : bytes.data.data(),
        capacity);
    if (result < 0) throwSocketABIError("__socket_recv_exact", "recv failed");
    bytes.data.resize(static_cast<size_t>(result));
    return ZeusType::UniversalVariant(ZeusType::Variant(std::move(bytes)));
}

inline ZeusType::UniversalVariant builtin_socket_close(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1) throw std::runtime_error("__socket_close expects 1 argument");
    const int64_t id = socketIdArgument(args, 0, "__socket_close");
    if (!zeus_socket_close(id)) throwSocketABIError("__socket_close", "close failed");
    return ZeusType::UniversalVariant(LexerType::Value(true));
}

inline ZeusType::UniversalVariant builtin_socket_is_connected(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1) {
        throw std::runtime_error("__socket_is_connected expects 1 argument");
    }
    const int64_t id = socketIdArgument(args, 0, "__socket_is_connected");
    bool connected = false;
    if (!zeus_socket_is_connected(id, &connected)) {
        throwSocketABIError("__socket_is_connected", "could not query socket state");
    }
    return ZeusType::UniversalVariant(LexerType::Value(connected));
}

inline void registerSocketBuiltins() {
    registerBuiltin("__socket_create", builtin_socket_create, 0, 0);
    registerBuiltin("__socket_connect", builtin_socket_connect, 3, 3);
    registerBuiltin("__socket_bind", builtin_socket_bind, 3, 3);
    registerBuiltin("__socket_listen", builtin_socket_listen, 2, 2);
    registerBuiltin("__socket_accept", builtin_socket_accept, 1, 1);
    registerBuiltin("__socket_send", builtin_socket_send, 2, 2);
    registerBuiltin("__socket_send_all", builtin_socket_send_all, 2, 2);
    registerBuiltin("__socket_recv", builtin_socket_recv, 2, 2);
    registerBuiltin("__socket_recv_exact", builtin_socket_recv_exact, 2, 2);
    registerBuiltin("__socket_close", builtin_socket_close, 1, 1);
    registerBuiltin("__socket_is_connected", builtin_socket_is_connected, 1, 1);
}

} // namespace ZeusBuiltins
