#pragma once

#include "crypto.hpp"
#include "websocket.hpp"
#include "regex.hpp"
#include "ThreadNativeBuiltins.hpp"
#include "dict.hpp"
#include "process.hpp"
#include "strings.hpp"
#include "../../ZeusBuiltins/NativeTypeIDs.hpp"
#include "../../ZeusBuiltins/NativeTypeABI.hpp"
#include "../../ZeusRuntimeLib/ZeusSignedInteger.hpp"
#include "../../ZeusRuntimeLib/ZeusUnsignedInteger.hpp"
#include "../../ZeusRuntimeLib/ZeusNativeDouble.hpp"
#include "../../ZeusRuntimeLib/ZeusNativeFloat.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace ZeusBuiltins {

struct NativeInjectionDescriptor {
    std::string_view declarationName;
    std::string_view symbol;
    ZeusType::NativeCallback callback;
    int minArity;
    int maxArity;
};

// Dynamic imports use a deliberately small, 64-bit C ABI surface. Integer,
// boolean and pointer arguments all occupy one general-purpose ABI slot on the
// supported x64 targets; strings cross as NUL-terminated UTF-8 pointers.
struct DynamicNativeImportDescriptor {
    std::string declarationName;
    std::string symbol;
    std::string libraryPath;
    std::string returnType;
    std::vector<std::string> parameterTypes;
    std::vector<bool> parameterIsReference;
    std::string referenceABI = "raw";
};

// The Windows x64 convention hands out argument slots by position: the n-th
// argument takes the n-th slot, and its class decides whether that slot is
// one of RCX/RDX/R8/R9 or one of XMM0-XMM3. The first four slots are
// registers, the rest are spilled onto the stack in argument order. The
// caller therefore hands over the finished placement and the trampoline
// only has to copy the payload into the slot the caller already picked.
struct DynamicNativeCallFrame {
    // One 8-byte payload per argument, in argument order. A float payload
    // occupies the low four bytes of its slot.
    const uint64_t* values = nullptr;
    // Index into values for RCX/RDX/R8/R9, or -1 when the register is unused.
    const int32_t* generalSlots = nullptr;
    // Index into values for XMM0-XMM3, or -1 when the register is unused.
    const int32_t* vectorSlots = nullptr;
    // Arguments that did not fit into a register, compacted in argument order.
    const uint64_t* stackValues = nullptr;
    uint32_t stackCount = 0;
    // 0 = integer, 1 = double, 2 = float
    uint32_t returnsFloatingPoint = 0;
    // Number of vector registers the callee may read, mirrored into AL.
    uint32_t vectorRegisterCount = 0;
};

static_assert(offsetof(DynamicNativeCallFrame, values) == 0);
static_assert(offsetof(DynamicNativeCallFrame, generalSlots) == 8);
static_assert(offsetof(DynamicNativeCallFrame, vectorSlots) == 16);
static_assert(offsetof(DynamicNativeCallFrame, stackValues) == 24);
static_assert(offsetof(DynamicNativeCallFrame, stackCount) == 32);
static_assert(offsetof(DynamicNativeCallFrame, returnsFloatingPoint) == 36);
static_assert(offsetof(DynamicNativeCallFrame, vectorRegisterCount) == 40);

inline constexpr bool hasDynamicNativeFloatingPointCallABI() {
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
    return true;
#else
    return false;
#endif
}

#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
extern "C" uint64_t zeus_dynamic_native_invoke(
    void* address,
    const DynamicNativeCallFrame* frame
);
#endif

inline std::unordered_map<int, DynamicNativeImportDescriptor>& dynamicNativeImports() {
    static std::unordered_map<int, DynamicNativeImportDescriptor> imports;
    return imports;
}

inline std::unordered_map<std::string, int>& dynamicNativeImportHandles() {
    static std::unordered_map<std::string, int> handles;
    return handles;
}

inline std::mutex& dynamicNativeImportMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::string dynamicNativeImportKey(
    const std::string& declarationName,
    const std::string& symbol,
    const std::string& libraryPath,
    const std::string& returnType,
    const std::vector<std::string>& parameterTypes,
    const std::vector<bool>& parameterIsReference,
    const std::string& referenceABI
) {
    std::string key;
    auto append = [&](const std::string& item) {
        key += std::to_string(item.size());
        key += ':';
        key += item;
    };
    append(declarationName);
    append(symbol);
    append(libraryPath);
    append(returnType);
    append(referenceABI);
    key += std::to_string(parameterTypes.size());
    key += ':';
    for (size_t i = 0; i < parameterTypes.size(); ++i) {
        append(parameterTypes[i]);
        key += (i < parameterIsReference.size() && parameterIsReference[i]) ? '1' : '0';
    }
    return key;
}

inline int registerDynamicNativeImport(DynamicNativeImportDescriptor descriptor) {
    const std::string key = dynamicNativeImportKey(
        descriptor.declarationName, descriptor.symbol, descriptor.libraryPath,
        descriptor.returnType, descriptor.parameterTypes,
        descriptor.parameterIsReference, descriptor.referenceABI);
    std::lock_guard<std::mutex> lock(dynamicNativeImportMutex());
    auto& handles = dynamicNativeImportHandles();
    const auto known = handles.find(key);
    if (known != handles.end()) return known->second;

    // Stable FNV-1a handles let a .zir file be loaded in another process while
    // leaving the historical -1..-12 built-in injection range untouched.
    uint32_t hash = 2166136261u;
    for (unsigned char byte : key) {
        hash ^= byte;
        hash *= 16777619u;
    }
    const int handle = -1000 - static_cast<int>(hash % 2000000000u);
    auto& imports = dynamicNativeImports();
    const auto collision = imports.find(handle);
    if (collision != imports.end()) {
        const auto& existing = collision->second;
        const std::string existingKey = dynamicNativeImportKey(
            existing.declarationName, existing.symbol, existing.libraryPath,
            existing.returnType, existing.parameterTypes,
            existing.parameterIsReference, existing.referenceABI);
        if (existingKey != key) {
            throw std::runtime_error("dynamic native import handle collision");
        }
    } else {
        imports.emplace(handle, std::move(descriptor));
    }
    handles.emplace(key, handle);
    return handle;
}

inline const DynamicNativeImportDescriptor* dynamicNativeImportFromHandle(int handle) {
    std::lock_guard<std::mutex> lock(dynamicNativeImportMutex());
    const auto found = dynamicNativeImports().find(handle);
    return found == dynamicNativeImports().end() ? nullptr : &found->second;
}

inline bool isDynamicNativeIntType(const std::string& typeName) {
    // Alias-aware: i8..u64 and the reclaimed int32 resolve through the
    // native table; int/int64 keep the 64-bit language-integer slot.
    if (findNativeIntegerABI(typeName)) return true;
    return typeName == "int" || typeName == "int64";
}

// A pointer occupies the same 64-bit register an int does, so it needs no
// separate dispatch case; the name only keeps declarations readable.
inline bool isDynamicNativePointerType(const std::string& typeName) {
    return isNativePointerTypeName(typeName);
}

inline bool isSupportedDynamicNativeImport(
    const DynamicNativeImportDescriptor& descriptor,
    std::string* error = nullptr,
    bool floatingPointCallABI = hasDynamicNativeFloatingPointCallABI()
) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (descriptor.libraryPath.empty() || descriptor.symbol.empty()) {
        return fail("dynamic native import requires a library path and symbol");
    }
    if (descriptor.parameterTypes.size() > kDynamicNativeMaxParameters) {
        return fail(
            "dynamic native imports support at most " +
            std::to_string(kDynamicNativeMaxParameters) + " parameters");
    }
    if (descriptor.referenceABI != "raw") {
        return fail("dynamic native imports use the raw x64 C ABI");
    }
    for (size_t i = 0; i < descriptor.parameterTypes.size(); ++i) {
        if (i < descriptor.parameterIsReference.size() && descriptor.parameterIsReference[i]) {
            return fail("dynamic native imports do not support reference parameters yet");
        }
        const auto& type = descriptor.parameterTypes[i];
        if (isNativeFloatingPointType(type)) {
            if (!floatingPointCallABI) {
                return fail("VM dynamic native imports do not support floating-point parameters; compile with LLVM/AOT");
            }
            continue;
        }
        if (!isDynamicNativeIntType(type) && !isDynamicNativePointerType(type) &&
            type != "bool" && type != "string") {
            return fail("dynamic native parameters support signed and unsigned integers, floating-point values, ptr, bool, and string");
        }
    }
    const bool returnIsFloat = isNativeFloatingPointType(descriptor.returnType);
    if (returnIsFloat && !floatingPointCallABI) {
        return fail("VM dynamic native imports do not support floating-point returns; compile with LLVM/AOT");
    }
    if (!(returnIsFloat && floatingPointCallABI) &&
        !isDynamicNativeIntType(descriptor.returnType) &&
        !isDynamicNativePointerType(descriptor.returnType) &&
        descriptor.returnType != "bool" && descriptor.returnType != "string" &&
      descriptor.returnType != "nil") {
        return fail("dynamic native returns support signed and unsigned integers, floating-point values, ptr, bool, string, and nil");
    }
    if (error) error->clear();
    return true;
}

inline void* loadDynamicNativeSymbol(
    const std::string& libraryPath,
    const std::string& symbol
) {
    static std::mutex loaderMutex;
    static std::unordered_map<std::string, void*> handlesByPath;
    std::lock_guard<std::mutex> lock(loaderMutex);
    void* library = nullptr;
    const auto cached = handlesByPath.find(libraryPath);
    if (cached != handlesByPath.end()) {
        library = cached->second;
    } else {
#ifdef _WIN32
        HMODULE module = LoadLibraryA(libraryPath.c_str());
        if (!module) {
            throw std::runtime_error("cannot load native library '" + libraryPath +
                                     "' (Windows error " +
                                     std::to_string(GetLastError()) + ")");
        }
        library = reinterpret_cast<void*>(module);
#else
        dlerror();
        library = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library && (libraryPath == "c" || libraryPath == "libc")) {
            library = dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL);
            if (!library) library = dlopen(nullptr, RTLD_NOW | RTLD_LOCAL);
        }
        if (!library && (libraryPath == "ssl" || libraryPath == "libssl")) {
            library = dlopen("libssl.so.3", RTLD_NOW | RTLD_LOCAL);
            if (!library) library = dlopen("libssl.so.1.1", RTLD_NOW | RTLD_LOCAL);
            if (!library) library = dlopen("libssl.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!library) {
            const char* message = dlerror();
            throw std::runtime_error("cannot load native library '" + libraryPath +
                                     "': " + (message ? message : "unknown loader error"));
        }
#endif
        handlesByPath.emplace(libraryPath, library);
    }
#ifdef _WIN32
    FARPROC address = GetProcAddress(reinterpret_cast<HMODULE>(library), symbol.c_str());
    if (!address) {
        throw std::runtime_error("native symbol '" + symbol + "' was not found in '" +
                                 libraryPath + "'");
    }
    return reinterpret_cast<void*>(address);
#else
    dlerror();
    void* address = dlsym(library, symbol.c_str());
    const char* message = dlerror();
    if (message || !address) {
        throw std::runtime_error("native symbol '" + symbol + "' was not found in '" +
                                 libraryPath + "': " +
                                 (message ? message : "unknown loader error"));
    }
    return address;
#endif
}

inline int64_t dynamicNativeIntArgument(
    const ZeusType::UniversalVariant& argument,
    size_t index
) {
    int64_t value = 0;
    if (!ZeusVMUtils::tryGetInt64(argument, value)) {
        throw std::runtime_error("native import argument " + std::to_string(index) +
                                 " must be int");
    }
    return value;
}

inline double dynamicNativeDoubleArgument(
    const ZeusType::UniversalVariant& argument,
    size_t index
) {
    try {
        return ZeusNativeDouble::require(argument);
    } catch (const std::exception& error) {
        throw std::runtime_error("native import argument " + std::to_string(index) +
                                 " must be DOUBLE: " + error.what());
    }
}

inline bool dynamicNativeBoolArgument(
    const ZeusType::UniversalVariant& argument,
    size_t index
) {
    if (const auto* direct = std::get_if<bool>(&argument)) return *direct;
    if (const auto* literal = std::get_if<LexerType::Value>(&argument)) {
        if (const auto* value = std::get_if<bool>(&literal->data)) return *value;
    }
    throw std::runtime_error("native import argument " + std::to_string(index) +
                             " must be bool");
}

inline const std::string& dynamicNativeStringArgument(
    const ZeusType::UniversalVariant& argument,
    size_t index
) {
    if (const auto* direct = std::get_if<std::string>(&argument)) return *direct;
    if (const auto* literal = std::get_if<LexerType::Value>(&argument)) {
        if (const auto* value = std::get_if<std::string>(&literal->data)) return *value;
    }
    throw std::runtime_error("native import argument " + std::to_string(index) +
                             " must be string");
}

// Prepares one argument as the single 64-bit word the call below passes.
// x64 classifies an int, a pointer and a bool alike: each occupies one general
// purpose register, and the callee reads only the bits its own declaration
// describes. A string differs solely in needing a buffer that outlives the
// call, which the caller's variant already guarantees.
//
// Keeping this to one arm matters: a branch per declared type would make the
// recursion below instantiate kDynamicNativeMaxParameters^branches functions,
// which at a limit of 16 is tens of millions of bodies.
inline uint64_t dynamicNativeScalarArgument(
    const ZeusType::UniversalVariant& argument,
    const std::string& type,
    size_t index
) {
    if (type == "string") {
        return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(
            dynamicNativeStringArgument(argument, index).c_str()));
    }
    if (type == "bool") {
        return dynamicNativeBoolArgument(argument, index) ? 1 : 0;
    }
    if (const auto* scalar = findNativeIntegerABI(type)) {
        if (scalar->valueABI == NativeValueABI::UnsignedInteger) {
            return ZeusUnsignedInteger::require(argument, scalar->integerBits);
        }
        return static_cast<uint64_t>(ZeusSignedInteger::require(argument, scalar->integerBits));
    }
    return static_cast<uint64_t>(dynamicNativeIntArgument(argument, index));
}

inline bool isDynamicNativeFloatingPointType(const std::string& type) {
    return isNativeFloatingPointType(type);
}

inline uint8_t dynamicNativeFloatingPointKind(const std::string& type) {
    const unsigned bits = nativeFloatingPointBits(type);
    return bits == 32 ? 2 : bits == 64 ? 1 : 0;
}

inline ZeusType::UniversalVariant dynamicNativeIntegerReturn(
    uint64_t raw,
    const NativeTypeABI* scalar
) {
    if (scalar && scalar->valueABI == NativeValueABI::UnsignedInteger) {
        uint64_t value = raw;
        if (scalar->integerBits == 8) value = static_cast<uint8_t>(raw);
        else if (scalar->integerBits == 16) value = static_cast<uint16_t>(raw);
        else if (scalar->integerBits == 32) value = static_cast<uint32_t>(raw);
        else if (scalar->integerBits != 64) {
            throw std::runtime_error("Unsupported native unsigned integer return width");
        }
        return ZeusType::BigInteger(std::to_string(static_cast<unsigned long long>(value)));
    }

    int64_t value = 0;
    std::memcpy(&value, &raw, sizeof(value));
    if (scalar && scalar->valueABI == NativeValueABI::SignedInteger) {
        if (scalar->integerBits == 8) value = static_cast<int8_t>(raw);
        else if (scalar->integerBits == 16) value = static_cast<int16_t>(raw);
        else if (scalar->integerBits == 32) value = static_cast<int32_t>(raw);
        else if (scalar->integerBits != 64) {
            throw std::runtime_error("Unsupported native signed integer return width");
        }
    }
    return LexerType::Value(value);
}

inline ZeusType::UniversalVariant invokeDynamicNativeFloatingPointImport(
    void* address,
    const DynamicNativeImportDescriptor& descriptor,
    const std::vector<ZeusType::UniversalVariant>& arguments
) {
#if defined(_WIN64) && (defined(_M_X64) || defined(__x86_64__))
    std::vector<uint64_t> values;
    std::vector<int32_t> generalSlots(4, -1);
    std::vector<int32_t> vectorSlots(4, -1);
    std::vector<uint64_t> stackValues;
    values.reserve(arguments.size());
    stackValues.reserve(arguments.size());
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::string& type = descriptor.parameterTypes[i];
        const uint8_t floatingKind = dynamicNativeFloatingPointKind(type);
        uint64_t payload = 0;
        if (floatingKind == 1) {
            const double converted = dynamicNativeDoubleArgument(arguments[i], i);
            std::memcpy(&payload, &converted, sizeof(payload));
        } else if (floatingKind == 2) {
            const float converted = ZeusNativeFloat::require(arguments[i]);
            uint32_t bits = 0;
            std::memcpy(&bits, &converted, sizeof(bits));
            payload = bits;
        } else {
            payload = dynamicNativeScalarArgument(arguments[i], type, i);
        }
        values.push_back(payload);

        // The Windows x64 convention hands out register slots by position,
        // not per class: the n-th argument takes the n-th slot and its class
        // decides whether that slot is a general purpose register or a
        // vector register. The first four slots are registers, the rest go
        // onto the stack in argument order.
        if (i < 4) {
            if (floatingKind == 0) generalSlots[i] = static_cast<int32_t>(i);
            else vectorSlots[i] = static_cast<int32_t>(i);
        } else {
            stackValues.push_back(payload);
        }
    }

    uint32_t vectorRegisterCount = 0;
    for (int32_t slot : vectorSlots) {
        if (slot >= 0) {
            vectorRegisterCount = static_cast<uint32_t>(slot + 1);
        }
    }

    const DynamicNativeCallFrame frame{
        values.data(),
        generalSlots.data(),
        vectorSlots.data(),
        stackValues.data(),
        static_cast<uint32_t>(stackValues.size()),
        static_cast<uint32_t>(dynamicNativeFloatingPointKind(descriptor.returnType)),
        vectorRegisterCount
    };
    const uint64_t raw = zeus_dynamic_native_invoke(address, &frame);

    if (frame.returnsFloatingPoint == 1) {
        double result = 0.0;
        std::memcpy(&result, &raw, sizeof(result));
        return ZeusNativeDouble::boxed(result);
    }
    if (frame.returnsFloatingPoint == 2) {
        const uint32_t bits = static_cast<uint32_t>(raw);
        float result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return ZeusNativeFloat::boxed(result);
    }
    if (descriptor.returnType == "nil") return ZeusType::Variant(ZeusType::Null{});
    if (descriptor.returnType == "bool") return (raw & 0xffu) != 0;
    if (descriptor.returnType == "string") {
        const char* result = reinterpret_cast<const char*>(static_cast<uintptr_t>(raw));
        if (!result) throw std::runtime_error("native import returned a null string");
        return std::string(result);
    }

    if (const auto* scalar = findNativeIntegerABI(descriptor.returnType)) {
        return dynamicNativeIntegerReturn(raw, scalar);
    }
    return LexerType::Value(static_cast<int64_t>(raw));
#else
    static_cast<void>(address);
    static_cast<void>(descriptor);
    static_cast<void>(arguments);
    throw std::runtime_error(
        "VM dynamic native floating-point imports require the Windows x64 call ABI");
#endif
}

template <typename Return, typename... Arguments>
inline Return invokeDynamicNativeTyped(
    void* address,
    const DynamicNativeImportDescriptor& descriptor,
    const std::vector<ZeusType::UniversalVariant>& values,
    Arguments... arguments
) {
    const size_t index = sizeof...(Arguments);
    if (index == descriptor.parameterTypes.size()) {
        using Function = Return (*)(Arguments...);
        return reinterpret_cast<Function>(address)(arguments...);
    }

    if constexpr (sizeof...(Arguments) >= kDynamicNativeMaxParameters) {
        throw std::runtime_error(
            "dynamic native imports support at most " +
            std::to_string(kDynamicNativeMaxParameters) + " parameters");
    } else {
        return invokeDynamicNativeTyped<Return, Arguments..., uint64_t>(
            address, descriptor, values, arguments...,
            dynamicNativeScalarArgument(
                values[index], descriptor.parameterTypes[index], index));
    }
}

inline ZeusType::UniversalVariant invokeDynamicNativeImport(
    const DynamicNativeImportDescriptor& descriptor,
    const std::vector<ZeusType::UniversalVariant>& arguments
) {
    std::string validationError;
    if (!isSupportedDynamicNativeImport(descriptor, &validationError)) {
        throw std::runtime_error(validationError);
    }
    if (arguments.size() != descriptor.parameterTypes.size()) {
        throw std::runtime_error("native import argument count does not match its declaration");
    }

    void* address = loadDynamicNativeSymbol(descriptor.libraryPath, descriptor.symbol);

    const bool hasFloatingPointABI =
        isDynamicNativeFloatingPointType(descriptor.returnType) ||
        std::any_of(descriptor.parameterTypes.begin(), descriptor.parameterTypes.end(),
            [](const std::string& type) { return isDynamicNativeFloatingPointType(type); });
    if (hasFloatingPointABI) {
        return invokeDynamicNativeFloatingPointImport(address, descriptor, arguments);
    }

    if (descriptor.returnType == "nil") {
        // A void callee writes nothing worth reading, so it keeps its own
        // instantiation rather than borrowing the scalar one.
        invokeDynamicNativeTyped<void>(address, descriptor, arguments);
        return ZeusType::Variant(ZeusType::Null{});
    }

    if (descriptor.returnType == "bool") {
        // Read only the ABI-defined boolean result. On x64 the unused high bits
        // of RAX are unspecified, so reading a bool through int64_t can turn a
        // native false result into a nonzero value.
        return invokeDynamicNativeTyped<bool>(address, descriptor, arguments);
    }

    if (const auto* returnABI = findNativeIntegerABI(descriptor.returnType)) {
        const uint64_t raw = invokeDynamicNativeTyped<uint64_t>(
            address, descriptor, arguments);
        return dynamicNativeIntegerReturn(raw, returnABI);
    }

    // Integer, pointer and string returns all use the same 64-bit register.
    uint64_t rawBits = invokeDynamicNativeTyped<uint64_t>(
        address, descriptor, arguments);

    if (descriptor.returnType == "string") {
        const char* result = reinterpret_cast<const char*>(
            static_cast<uintptr_t>(rawBits));
        if (!result) throw std::runtime_error("native import returned a null string");
        return std::string(result);
    }

    // An int or a pointer both arrive as a plain integer; a returned address
    // stays one, and the caller turns it back into a pointer when it uses it.
    int64_t raw = 0;
    std::memcpy(&raw, &rawBits, sizeof(raw));
    return LexerType::Value(raw);
}

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
        {"__dict_length_i64", "zeus_value_dict_length_i64",
         builtin_dict_length, 1, 1},
        {"__dict_has_key", "zeus_value_dict_contains_key_i64",
         builtin_dict_has_key, 2, 2},
        // Append regex declarations so existing injected native handles stay stable.
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
         RegexBuiltinDetail::builtinEscape, 1, 1},
        {"__process_capture_start", "zeus_process_capture_start",
         builtin_process_capture_start, 3, 3},
        {"__process_capture_read", "zeus_process_capture_read",
         builtin_process_capture_read, 10, 10},
        {"__process_capture_release", "zeus_process_capture_release",
         builtin_process_capture_release, 1, 1},
        // Append native string operations so existing injected handles stay stable.
        {"__str_index", "zeus_native_string_index", builtin_str_index, 2, 2},
        {"__str_rindex", "zeus_native_string_rindex", builtin_str_rindex, 2, 2},
        {"__str_startswith", "zeus_native_string_startswith",
         builtin_str_startswith, 2, 2}
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
