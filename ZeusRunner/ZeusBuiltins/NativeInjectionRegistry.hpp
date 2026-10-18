#pragma once

#include "crypto.hpp"
#include "websocket.hpp"
#include "regex.hpp"
#include "ThreadNativeBuiltins.hpp"

#include <string_view>
#include <vector>

namespace ZeusBuiltins {

struct NativeInjectionDescriptor {
    std::string_view declarationName;
    std::string_view symbol;
    ZeusType::NativeCallback callback;
    int minArity;
    int maxArity;
};

inline ZeusType::UniversalVariant nativeProtocolLastError(
    const std::vector<ZeusType::UniversalVariant>& args
) {
    if (!args.empty()) {
        throw std::runtime_error("__protocol_last_error expects no arguments");
    }
    return ZeusType::UniversalVariant(std::string());
}

inline const std::vector<NativeInjectionDescriptor>& nativeInjections() {
    static const std::vector<NativeInjectionDescriptor> injections = {
        {"__crypto_random_bytes", "zeus_crypto_random_bytes",
         builtin_crypto_random_bytes, 1, 1},
        {"__crypto_sha1", "zeus_crypto_sha1", builtin_crypto_sha1, 1, 1},
        {"__crypto_base64_encode", "zeus_crypto_base64_encode",
         builtin_crypto_base64_encode, 1, 1},
        {"__crypto_base64_decode", "zeus_crypto_base64_decode",
         builtin_crypto_base64_decode, 1, 1},
        {"__websocket_mask", "zeus_websocket_mask",
         builtin_websocket_mask, 2, 2},
        {"__websocket_join", "zeus_websocket_join",
         builtin_websocket_join, 1, 1},
        {"__protocol_last_error", "zeus_protocol_last_error",
         nativeProtocolLastError, 0, 0},
        {"__mutex_create", "zeus_mutex_create", builtin_mutex_create, 0, 0},
        {"__mutex_lock", "zeus_mutex_lock", builtin_mutex_lock, 1, 1},
        {"__mutex_trylock", "zeus_mutex_trylock", builtin_mutex_trylock, 1, 1},
        {"__mutex_unlock", "zeus_mutex_unlock", builtin_mutex_unlock, 1, 1},
        {"__mutex_destroy", "zeus_mutex_destroy", builtin_mutex_destroy, 1, 1},
        {"__re_compile", "zeus_re_compile",
         RegexBuiltinDetail::builtinCompile, 2, 2},
        {"__re_matches", "zeus_re_matches",
         RegexBuiltinDetail::builtinMatches, 4, 4},
        {"__re_match_at", "zeus_re_match_at",
         RegexBuiltinDetail::builtinMatchAt, 2, 2},
        {"__re_findall", "zeus_re_findall",
         RegexBuiltinDetail::builtinFindall, 3, 3},
        {"__re_split", "zeus_re_split",
         RegexBuiltinDetail::builtinSplit, 4, 4},
        {"__re_sub", "zeus_re_sub",
         RegexBuiltinDetail::builtinSub, 5, 5},
        {"__re_subn", "zeus_re_subn",
         RegexBuiltinDetail::builtinSubn, 5, 5},
        {"__re_escape", "zeus_re_escape",
         RegexBuiltinDetail::builtinEscape, 1, 1}
    };
    return injections;
}

inline int nativeInjectionHandle(std::string_view symbol) {
    const auto& injections = nativeInjections();
    for (size_t index = 0; index < injections.size(); ++index) {
        if (injections[index].symbol == symbol) {
            return -static_cast<int>(index) - 1;
        }
    }
    return 0;
}

inline const NativeInjectionDescriptor* nativeInjectionFromHandle(
    int handle
) noexcept {
    if (handle >= 0) return nullptr;
    const size_t index = static_cast<size_t>(-static_cast<int64_t>(handle) - 1);
    const auto& injections = nativeInjections();
    return index < injections.size() ? &injections[index] : nullptr;
}

} // namespace ZeusBuiltins
