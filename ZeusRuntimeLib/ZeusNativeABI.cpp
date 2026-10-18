#include "ZeusABI.hpp"

#include "ZeusABIInternal.hpp"
#include "ZeusProtocolABI.hpp"
#include "../ZeusBuiltins/BuiltinNativeABI.hpp"
#include "../ZeusRunner/ZeusBuiltins/regex.hpp"
#include "../ZeusRunner/ZeusBuiltins/bytes.hpp"
#include "../ZeusRunner/ZeusBuiltins/gc.hpp"
#include "../ZeusRunner/ZeusBuiltins/ThreadNativeBuiltins.hpp"
#include "../ZeusRunner/ZeusBuiltins/socket.hpp"
#include "../ZeusRunner/ZeusBuiltins/crypto.hpp"
#include "../ZeusRunner/ZeusBuiltins/websocket.hpp"
#include "../ZeusRunner/ZeusBuiltins/type.hpp"
#include "../ZeusRunner/ZeusBuiltins/strings.hpp"
#include "../ZeusRunner/ZeusBuiltins/time.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void registerStandaloneSocketBuiltins() {
    ZeusBuiltins::registerSocketBuiltins();
}

[[noreturn]] void nativeCallPanic(const char* name, const char* reason) {
    throw std::runtime_error(std::string("[ZeusValue native ABI] ") +
        (name ? name : "<null builtin>") + ": " +
        (reason ? reason : "unknown error"));
}

// Returns the text of a boxed native String instance, or nullptr when the value
// is not a native string. The caller converts it before invoking a builtin.
ZeusNativeString nativeStringTextOf(const ZeusValue* value,
                                         const ZeusType::UniversalVariant& boxed) {
    if (!value || value->flags != ZEUS_VALUE_FLAG_OWNS_BOX) return nullptr;
    const auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&boxed);
    if (!classHandle || !classHandle->state ||
        classHandle->state->typeName != "String") {
        return nullptr;
    }
    return zeus_value_intern_string(value);
}

using RegexNativeCallback = ZeusType::UniversalVariant (*)(
    const std::vector<ZeusType::UniversalVariant>&);

void invokeRegexNativeValue(
    ZeusValue* out,
    const char* symbol,
    std::initializer_list<const ZeusValue*> values,
    RegexNativeCallback callback
) {
    if (!out) nativeCallPanic(symbol, "null result pointer");
    zeus_value_init_nil(out);

    try {
        std::vector<ZeusType::UniversalVariant> arguments;
        arguments.reserve(values.size());
        for (const ZeusValue* value : values) {
            std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                zeus_value_to_box(value), &zeus_box_release);
            const ZeusNativeString text = nativeStringTextOf(value, box->v);
            if (text) {
                uint64_t length = 0;
                std::memcpy(&length, text - sizeof(uint64_t), sizeof(length));
                if (length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                    throw std::length_error("String argument is too large");
                }
                arguments.emplace_back(std::string(text, static_cast<size_t>(length)));
            } else {
                arguments.push_back(box->v);
            }
        }

        ZeusBox result;
        result.assignVariant(callback(arguments));
        zeus_value_from_box(out, &result);
    } catch (const std::exception& error) {
        nativeCallPanic(symbol, error.what());
    } catch (...) {
        nativeCallPanic(symbol, "callback threw a non-standard exception");
    }
}

void initializeStandaloneBuiltins() {
    static std::once_flag initialized;
    std::call_once(initialized, [] {
        ZeusBuiltins::registerTypeBuiltins();
        ZeusBuiltins::registerStringBuiltins();
        ZeusBuiltins::registerBytesBuiltins();
        ZeusBuiltins::registerGcBuiltins();
        // Thread sleep/id/stop stay reachable through the standalone ZeusValue
        // call ABI; mutex operations use the direct zeus_mutex_* ABI instead.
        ZeusBuiltins::registerStandaloneThreadBuiltins();
        // Both VM callbacks and standalone ZeusValue callbacks call the same
        // process-wide Socket C ABI for handle and OS resource management.
        registerStandaloneSocketBuiltins();
        // The time builtins need no VM state, so imported packages that use them
        // work in AOT binaries too.
        ZeusBuiltins::registerTimeBuiltins();
    });
}

} // namespace

void zeus_value_native_call(
    ZeusValue* out,
    const char* builtin_name,
    int32_t argc,
    const ZeusValue* const* values
) {
    if (!out) nativeCallPanic(builtin_name, "null result pointer");
    zeus_value_init_nil(out);

    if (!builtin_name || !*builtin_name) {
        nativeCallPanic(builtin_name, "empty builtin name");
    }
    if (argc < 0 || (argc > 0 && !values)) {
        nativeCallPanic(builtin_name, "invalid argument array");
    }
    if (!ZeusBuiltins::hasStandaloneValueCallABI(builtin_name)) {
        nativeCallPanic(builtin_name, "callback is not registered for standalone ZeusValue calls");
    }

    try {
        initializeStandaloneBuiltins();
        const int id = ZeusBuiltins::findId(builtin_name);
        const auto* descriptor = ZeusBuiltins::get(id);
        if (!descriptor) nativeCallPanic(builtin_name, "builtin is not registered");
        if (argc < descriptor->minArity ||
            (descriptor->maxArity >= 0 && argc > descriptor->maxArity)) {
            nativeCallPanic(builtin_name, "argument count does not match builtin signature");
        }

        std::vector<ZeusType::UniversalVariant> arguments;
        arguments.reserve(static_cast<size_t>(argc));
        for (int32_t i = 0; i < argc; ++i) {
            std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> box(
                zeus_value_to_box(values[i]), &zeus_box_release);
            // Builtins work on universal values. A native String instance only
            // carries its text behind the registered toString method, so it is
            // normalized here instead of in every builtin.
            const ZeusNativeString text = nativeStringTextOf(values[i], box->v);
            if (text) {
                uint64_t length = 0;
                std::memcpy(&length, text - sizeof(uint64_t), sizeof(length));
                if (length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
                    throw std::length_error("String argument is too large");
                }
                arguments.push_back(ZeusType::UniversalVariant(
                    std::string(text, static_cast<size_t>(length))));
            } else {
                arguments.push_back(box->v);
            }
        }

        ZeusBox result;
        result.assignVariant(descriptor->fn(arguments));
        zeus_value_from_box(out, &result);
    } catch (const std::exception& error) {
        nativeCallPanic(builtin_name, error.what());
    } catch (...) {
        nativeCallPanic(builtin_name, "callback threw a non-standard exception");
    }
}

extern "C" {

void zeus_re_compile(
    ZeusValue* out,
    const ZeusValue* pattern,
    const ZeusValue* flags
) {
    invokeRegexNativeValue(out, "zeus_re_compile", {pattern, flags},
        ZeusBuiltins::RegexBuiltinDetail::builtinCompile);
}

void zeus_re_matches(
    ZeusValue* out,
    const ZeusValue* pattern,
    const ZeusValue* flags,
    const ZeusValue* text,
    const ZeusValue* mode
) {
    invokeRegexNativeValue(out, "zeus_re_matches", {pattern, flags, text, mode},
        ZeusBuiltins::RegexBuiltinDetail::builtinMatches);
}

void zeus_re_match_at(
    ZeusValue* out,
    const ZeusValue* records,
    const ZeusValue* index
) {
    invokeRegexNativeValue(out, "zeus_re_match_at", {records, index},
        ZeusBuiltins::RegexBuiltinDetail::builtinMatchAt);
}

void zeus_re_findall(
    ZeusValue* out,
    const ZeusValue* pattern,
    const ZeusValue* flags,
    const ZeusValue* text
) {
    invokeRegexNativeValue(out, "zeus_re_findall", {pattern, flags, text},
        ZeusBuiltins::RegexBuiltinDetail::builtinFindall);
}

void zeus_re_split(
    ZeusValue* out,
    const ZeusValue* pattern,
    const ZeusValue* flags,
    const ZeusValue* text,
    const ZeusValue* maxsplit
) {
    invokeRegexNativeValue(out, "zeus_re_split", {pattern, flags, text, maxsplit},
        ZeusBuiltins::RegexBuiltinDetail::builtinSplit);
}

void zeus_re_sub(
    ZeusValue* out,
    const ZeusValue* pattern,
    const ZeusValue* flags,
    const ZeusValue* replacement,
    const ZeusValue* text,
    const ZeusValue* count
) {
    invokeRegexNativeValue(out, "zeus_re_sub",
        {pattern, flags, replacement, text, count},
        ZeusBuiltins::RegexBuiltinDetail::builtinSub);
}

void zeus_re_subn(
    ZeusValue* out,
    const ZeusValue* pattern,
    const ZeusValue* flags,
    const ZeusValue* replacement,
    const ZeusValue* text,
    const ZeusValue* count
) {
    invokeRegexNativeValue(out, "zeus_re_subn",
        {pattern, flags, replacement, text, count},
        ZeusBuiltins::RegexBuiltinDetail::builtinSubn);
}

void zeus_re_escape(ZeusValue* out, const ZeusValue* pattern) {
    invokeRegexNativeValue(out, "zeus_re_escape", {pattern},
        ZeusBuiltins::RegexBuiltinDetail::builtinEscape);
}

} // extern "C"

namespace {

thread_local std::string protocolNativeError;

template <typename Operation>
ZeusNativeBytes callProtocolBytesNative(Operation&& operation) noexcept {
    protocolNativeError.clear();
    try {
        return operation();
    } catch (const std::exception& error) {
        protocolNativeError = error.what();
    } catch (...) {
        protocolNativeError = "protocol native function failed";
    }
    return zeus_bytes_from_data(nullptr, 0);
}

template <typename Operation>
ZeusNativeString callProtocolStringNative(Operation&& operation) noexcept {
    protocolNativeError.clear();
    try {
        return operation();
    } catch (const std::exception& error) {
        protocolNativeError = error.what();
    } catch (...) {
        protocolNativeError = "protocol native function failed";
    }
    return zeus_string_from_data("", 0);
}

ZeusType::Bytes copyProtocolBytes(ZeusNativeBytes data) {
    const int64_t length = zeus_bytes_len(data);
    if (length < 0) throw std::invalid_argument("native Bytes length is invalid");
    ZeusType::Bytes output;
    output.data.resize(static_cast<size_t>(length));
    if (length > 0) {
        std::memcpy(output.data.data(), data, static_cast<size_t>(length));
    }
    return output;
}

ZeusNativeBytes protocolBytesResult(const ZeusType::Bytes& data) {
    if (data.data.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        throw std::length_error("protocol byte result is too large");
    }
    return zeus_bytes_from_data(
        data.data.empty() ? nullptr : data.data.data(),
        static_cast<int64_t>(data.data.size()));
}

std::string copyProtocolString(ZeusNativeString value) {
    zeus_native_string_validate(value);
    const int64_t length = zeus_string_length(value);
    if (length < 0) throw std::invalid_argument("native String length is invalid");
    return std::string(value, static_cast<size_t>(length));
}

} // namespace

extern "C" ZeusNativeBytes zeus_crypto_random_bytes(int64_t length) {
    return callProtocolBytesNative([&] {
        if (length < 0 || length > 65536) {
            throw std::invalid_argument("random byte request must be between 0 and 65536");
        }
        ZeusType::Bytes output;
        output.data.resize(static_cast<size_t>(length));
        ZeusBuiltins::cryptoFillRandom(output.data.data(), output.data.size());
        return protocolBytesResult(output);
    });
}

extern "C" ZeusNativeBytes zeus_crypto_sha1(ZeusNativeBytes data) {
    return callProtocolBytesNative([&] {
        return protocolBytesResult(ZeusBuiltins::cryptoSha1(copyProtocolBytes(data)));
    });
}

extern "C" ZeusNativeString zeus_crypto_base64_encode(ZeusNativeBytes data) {
    return callProtocolStringNative([&] {
        const std::string encoded =
            ZeusBuiltins::cryptoBase64Encode(copyProtocolBytes(data));
        if (encoded.size() > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
            throw std::length_error("Base64 result is too large");
        }
        return zeus_string_from_data(
            encoded.data(), static_cast<int64_t>(encoded.size()));
    });
}

extern "C" ZeusNativeBytes zeus_crypto_base64_decode(ZeusNativeString data) {
    return callProtocolBytesNative([&] {
        return protocolBytesResult(
            ZeusBuiltins::cryptoBase64Decode(copyProtocolString(data)));
    });
}

extern "C" ZeusNativeBytes zeus_websocket_mask(
    ZeusNativeBytes payload,
    ZeusNativeBytes mask
) {
    return callProtocolBytesNative([&] {
        const ZeusType::Bytes payloadBytes = copyProtocolBytes(payload);
        const ZeusType::Bytes maskBytes = copyProtocolBytes(mask);
        if (maskBytes.data.size() != 4U) {
            throw std::invalid_argument("WebSocket frame mask must contain four bytes");
        }
        ZeusType::Bytes output;
        output.data.resize(payloadBytes.data.size());
        for (size_t index = 0; index < payloadBytes.data.size(); ++index) {
            output.data[index] = static_cast<uint8_t>(
                payloadBytes.data[index] ^ maskBytes.data[index % 4U]);
        }
        return protocolBytesResult(output);
    });
}

extern "C" ZeusNativeBytes zeus_websocket_join(const ZeusValue* chunks) {
    return callProtocolBytesNative([&] {
        if (!chunks) throw std::invalid_argument("WebSocket fragments are missing");
        std::unique_ptr<ZeusBox, decltype(&zeus_box_release)> boxed(
            zeus_value_to_box(chunks), &zeus_box_release);
        if (!boxed) {
            throw std::invalid_argument("WebSocket fragments could not be boxed");
        }
        const ZeusType::Array* array = ZeusBuiltins::getArrayPtr(boxed->v);
        if (!array || !array->impl) {
            throw std::invalid_argument("WebSocket fragments must be a list");
        }
        size_t total = 0;
        for (const auto& item : array->impl->data) {
            const ZeusType::Bytes* bytes =
                ZeusBuiltins::websocketBytesArgument(item);
            if (!bytes) throw std::invalid_argument("WebSocket fragments must contain bytes");
            if (bytes->data.size() > std::numeric_limits<size_t>::max() - total) {
                throw std::length_error("WebSocket message size overflow");
            }
            total += bytes->data.size();
        }
        ZeusType::Bytes output;
        output.data.reserve(total);
        for (const auto& item : array->impl->data) {
            const ZeusType::Bytes* bytes =
                ZeusBuiltins::websocketBytesArgument(item);
            output.data.insert(output.data.end(), bytes->data.begin(), bytes->data.end());
        }
        return protocolBytesResult(output);
    });
}

extern "C" ZeusNativeString zeus_protocol_last_error(void) {
    return zeus_string_from_data(
        protocolNativeError.data(),
        static_cast<int64_t>(protocolNativeError.size()));
}
