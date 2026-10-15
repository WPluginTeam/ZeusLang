#pragma once

#include "array.hpp"
#include "core.hpp"
#include "crypto.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace ZeusBuiltins {

inline const ZeusType::Bytes* websocketBytesArgument(
    const ZeusType::UniversalVariant& argument
) {
    return cryptoBytesArgument(argument);
}

inline ZeusType::UniversalVariant builtin_websocket_mask(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 2U) {
        throw std::runtime_error("__websocket_mask expects 2 arguments");
    }
    const ZeusType::Bytes* payload = websocketBytesArgument(args[0]);
    const ZeusType::Bytes* mask = websocketBytesArgument(args[1]);
    if (!payload || !mask || mask->data.size() != 4U) {
        throw std::runtime_error("__websocket_mask expects bytes and a four-byte mask");
    }
    ZeusType::Bytes output;
    output.data.resize(payload->data.size());
    for (size_t i = 0; i < payload->data.size(); ++i) {
        output.data[i] = static_cast<uint8_t>(payload->data[i] ^ mask->data[i % 4U]);
    }
    return ZeusType::UniversalVariant(ZeusType::Variant(std::move(output)));
}

inline ZeusType::UniversalVariant builtin_websocket_join(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1U) {
        throw std::runtime_error("__websocket_join expects 1 argument");
    }
    const ZeusType::Array* chunks = getArrayPtr(args[0]);
    if (!chunks || !chunks->impl) {
        throw std::runtime_error("__websocket_join argument must be a list");
    }

    size_t total = 0;
    for (const auto& item : chunks->impl->data) {
        const ZeusType::Bytes* bytes = websocketBytesArgument(item);
        if (!bytes) throw std::runtime_error("__websocket_join list items must be bytes");
        if (bytes->data.size() > std::numeric_limits<size_t>::max() - total) {
            throw std::length_error("WebSocket message size overflow");
        }
        total += bytes->data.size();
    }

    ZeusType::Bytes output;
    output.data.reserve(total);
    for (const auto& item : chunks->impl->data) {
        const ZeusType::Bytes* bytes = websocketBytesArgument(item);
        output.data.insert(output.data.end(), bytes->data.begin(), bytes->data.end());
    }
    return ZeusType::UniversalVariant(ZeusType::Variant(std::move(output)));
}

} // namespace ZeusBuiltins
