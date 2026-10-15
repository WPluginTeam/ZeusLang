#pragma once

#include "core.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#pragma push_macro("INTERFACE")
#pragma push_macro("WIN32_LEAN_AND_MEAN")
#pragma push_macro("NOMINMAX")
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma pop_macro("NOMINMAX")
#pragma pop_macro("WIN32_LEAN_AND_MEAN")
#pragma pop_macro("INTERFACE")
#pragma comment(lib, "bcrypt.lib")
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ZeusBuiltins {

inline int64_t cryptoIntegerArgument(
    const ZeusType::UniversalVariant& argument,
    const char* operation
) {
    LexerType::Value value(0);
    if (!ZeusVMUtils::convertToValue(argument, value) ||
        value.getStringType() != "int") {
        throw std::runtime_error(std::string(operation) + ": length must be an integer");
    }
    return std::get<ZeusType::BigInteger>(value.data).toInt64();
}

inline const ZeusType::Bytes* cryptoBytesArgument(
    const ZeusType::UniversalVariant& argument
) {
    if (const auto* bytes = std::get_if<ZeusType::Bytes>(&argument)) return bytes;
    if (const auto* boxed = std::get_if<ZeusType::Variant>(&argument)) {
        return std::get_if<ZeusType::Bytes>(boxed);
    }
    return nullptr;
}

inline std::string cryptoStringArgument(
    const ZeusType::UniversalVariant& argument,
    const char* operation
) {
    LexerType::Value value(0);
    if (!ZeusVMUtils::convertToValue(argument, value) ||
        value.getStringType() != "string") {
        throw std::runtime_error(std::string(operation) + ": argument must be a string");
    }
    return std::get<std::string>(value.data);
}

inline void cryptoFillRandom(uint8_t* output, size_t length) {
    if (length == 0) return;
    if (!output) throw std::invalid_argument("random output buffer is null");
#ifdef _WIN32
    if (length > static_cast<size_t>(std::numeric_limits<ULONG>::max())) {
        throw std::length_error("random request is too large");
    }
    const NTSTATUS status = BCryptGenRandom(
        nullptr,
        output,
        static_cast<ULONG>(length),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) throw std::runtime_error("BCryptGenRandom failed");
#else
    const int descriptor = ::open("/dev/urandom", O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("cannot open /dev/urandom");
    size_t offset = 0;
    while (offset < length) {
        const ssize_t count = ::read(descriptor, output + offset, length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            ::close(descriptor);
            throw std::runtime_error("cannot read secure random bytes");
        }
        offset += static_cast<size_t>(count);
    }
    ::close(descriptor);
#endif
}

inline uint32_t cryptoRotateLeft(uint32_t value, unsigned count) noexcept {
    return (value << count) | (value >> (32U - count));
}

inline ZeusType::Bytes cryptoSha1(const ZeusType::Bytes& input) {
    if (input.data.size() > (std::numeric_limits<size_t>::max() - 72U)) {
        throw std::length_error("SHA-1 input is too large");
    }
    const uint64_t bitLength = static_cast<uint64_t>(input.data.size()) * 8U;
    std::vector<uint8_t> padded = input.data;
    padded.push_back(0x80U);
    while ((padded.size() % 64U) != 56U) padded.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) {
        padded.push_back(static_cast<uint8_t>((bitLength >> shift) & 0xffU));
    }

    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xefcdab89U;
    uint32_t h2 = 0x98badcfeU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xc3d2e1f0U;

    for (size_t offset = 0; offset < padded.size(); offset += 64U) {
        uint32_t words[80]{};
        for (size_t i = 0; i < 16U; ++i) {
            const size_t at = offset + i * 4U;
            words[i] = (static_cast<uint32_t>(padded[at]) << 24U) |
                (static_cast<uint32_t>(padded[at + 1U]) << 16U) |
                (static_cast<uint32_t>(padded[at + 2U]) << 8U) |
                static_cast<uint32_t>(padded[at + 3U]);
        }
        for (size_t i = 16U; i < 80U; ++i) {
            words[i] = cryptoRotateLeft(
                words[i - 3U] ^ words[i - 8U] ^ words[i - 14U] ^ words[i - 16U], 1U);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (size_t i = 0; i < 80U; ++i) {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20U) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999U;
            } else if (i < 40U) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (i < 60U) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }
            const uint32_t temp = cryptoRotateLeft(a, 5U) + f + e + k + words[i];
            e = d;
            d = c;
            c = cryptoRotateLeft(b, 30U);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    ZeusType::Bytes output;
    output.data.reserve(20U);
    const uint32_t words[] = {h0, h1, h2, h3, h4};
    for (const uint32_t word : words) {
        output.data.push_back(static_cast<uint8_t>((word >> 24U) & 0xffU));
        output.data.push_back(static_cast<uint8_t>((word >> 16U) & 0xffU));
        output.data.push_back(static_cast<uint8_t>((word >> 8U) & 0xffU));
        output.data.push_back(static_cast<uint8_t>(word & 0xffU));
    }
    return output;
}

inline std::string cryptoBase64Encode(const ZeusType::Bytes& input) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (input.data.size() > (std::numeric_limits<size_t>::max() - 2U) / 4U * 3U) {
        throw std::length_error("Base64 input is too large");
    }
    std::string output;
    output.reserve(((input.data.size() + 2U) / 3U) * 4U);
    for (size_t i = 0; i < input.data.size(); i += 3U) {
        const uint32_t a = input.data[i];
        const uint32_t b = i + 1U < input.data.size() ? input.data[i + 1U] : 0U;
        const uint32_t c = i + 2U < input.data.size() ? input.data[i + 2U] : 0U;
        const uint32_t value = (a << 16U) | (b << 8U) | c;
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(i + 1U < input.data.size()
            ? alphabet[(value >> 6U) & 0x3fU]
            : '=');
        output.push_back(i + 2U < input.data.size()
            ? alphabet[value & 0x3fU]
            : '=');
    }
    return output;
}

inline ZeusType::Bytes cryptoBase64Decode(const std::string& input) {
    auto decode = [](unsigned char value) -> int {
        if (value >= 'A' && value <= 'Z') return value - 'A';
        if (value >= 'a' && value <= 'z') return value - 'a' + 26;
        if (value >= '0' && value <= '9') return value - '0' + 52;
        if (value == '+') return 62;
        if (value == '/') return 63;
        return -1;
    };
    if ((input.size() % 4U) != 0U) {
        throw std::invalid_argument("Base64 length must be a multiple of four");
    }

    ZeusType::Bytes output;
    output.data.reserve((input.size() / 4U) * 3U);
    for (size_t i = 0; i < input.size(); i += 4U) {
        const bool last = i + 4U == input.size();
        const int a = decode(static_cast<unsigned char>(input[i]));
        const int b = decode(static_cast<unsigned char>(input[i + 1U]));
        if (a < 0 || b < 0) throw std::invalid_argument("invalid Base64 character");
        const bool padC = input[i + 2U] == '=';
        const bool padD = input[i + 3U] == '=';
        if ((padC && !padD) || ((padC || padD) && !last)) {
            throw std::invalid_argument("invalid Base64 padding");
        }

        const int c = padC ? 0 : decode(static_cast<unsigned char>(input[i + 2U]));
        const int d = padD ? 0 : decode(static_cast<unsigned char>(input[i + 3U]));
        if (c < 0 || d < 0) throw std::invalid_argument("invalid Base64 character");
        if (padC && (b & 0x0f) != 0) {
            throw std::invalid_argument("non-canonical Base64 padding");
        }
        if (!padC && padD && (c & 0x03) != 0) {
            throw std::invalid_argument("non-canonical Base64 padding");
        }

        const uint32_t value = (static_cast<uint32_t>(a) << 18U) |
            (static_cast<uint32_t>(b) << 12U) |
            (static_cast<uint32_t>(c) << 6U) |
            static_cast<uint32_t>(d);
        output.data.push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
        if (!padC) output.data.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
        if (!padD) output.data.push_back(static_cast<uint8_t>(value & 0xffU));
    }
    return output;
}

inline ZeusType::UniversalVariant builtin_crypto_random_bytes(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1U) {
        throw std::runtime_error("__crypto_random_bytes expects 1 argument");
    }
    const int64_t length = cryptoIntegerArgument(args[0], "__crypto_random_bytes");
    if (length < 0 || length > 65536) {
        throw std::runtime_error("__crypto_random_bytes length must be 0..65536");
    }
    ZeusType::Bytes output;
    output.data.resize(static_cast<size_t>(length));
    cryptoFillRandom(output.data.data(), output.data.size());
    return ZeusType::UniversalVariant(ZeusType::Variant(std::move(output)));
}

inline ZeusType::UniversalVariant builtin_crypto_sha1(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1U) throw std::runtime_error("__crypto_sha1 expects 1 argument");
    const ZeusType::Bytes* input = cryptoBytesArgument(args[0]);
    if (!input) throw std::runtime_error("__crypto_sha1 argument must be bytes");
    return ZeusType::UniversalVariant(ZeusType::Variant(cryptoSha1(*input)));
}

inline ZeusType::UniversalVariant builtin_crypto_base64_encode(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1U) {
        throw std::runtime_error("__crypto_base64_encode expects 1 argument");
    }
    const ZeusType::Bytes* input = cryptoBytesArgument(args[0]);
    if (!input) throw std::runtime_error("__crypto_base64_encode argument must be bytes");
    return ZeusType::UniversalVariant(cryptoBase64Encode(*input));
}

inline ZeusType::UniversalVariant builtin_crypto_base64_decode(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (args.size() != 1U) {
        throw std::runtime_error("__crypto_base64_decode expects 1 argument");
    }
    const std::string input = cryptoStringArgument(args[0], "__crypto_base64_decode");
    return ZeusType::UniversalVariant(
        ZeusType::Variant(cryptoBase64Decode(input)));
}

} // namespace ZeusBuiltins
