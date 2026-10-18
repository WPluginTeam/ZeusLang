#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace ZeusBuiltins {

// One ABI policy is used by both compile-time native type aliases and the
// LLVM/runtime bridges. Raw pointers remain handled by the existing pointer
// path instead of NativeTypeABI.
enum class NativeValueABI {
    ZeusValue,
    Int64,
    SignedInteger,
    UnsignedInteger,
    FloatingPoint,
    Bool1,
    NativeString,
    NativeBytes,
    RawBytesRange,
    NativePointer
};

// These source IDs are stable. Existing IDs must never be renumbered.
enum class NativeTypeID : int32_t {
    Double = 1,
    LongLong = 2,
    Int32 = 3,
    Int8 = 4,
    Integer = 5,
    Bool = 6,
    Pointer = 7,
    String = 8,
    Bytes = 9,
    RawBytesRange = 10,
    Float32 = 11,
    Int16 = 12,
    UInt8 = 13,
    UInt16 = 14,
    UInt32 = 15,
    UInt64 = 16,
};

struct NativeTypeIDEntry {
    NativeTypeID id;
    std::string_view enumName;
    std::string_view canonicalName;
    std::string_view cppABI;
    std::string_view llvmType;
    NativeValueABI valueABI;
    unsigned bits = 0;
    std::array<std::string_view, 2> aliases{};
    unsigned aliasCount = 0;
};

inline const std::array<NativeTypeIDEntry, 16>& nativeTypeIDs() {
    static const std::array<NativeTypeIDEntry, 16> types = {{
        {NativeTypeID::Double, "DOUBLE", "@native.double", "double", "double",
            NativeValueABI::FloatingPoint, 64},
        {NativeTypeID::LongLong, "LONGLONG", "@native.longlong", "int64_t", "i64",
            NativeValueABI::SignedInteger, 64},
        {NativeTypeID::Int32, "INT32", "@native.int32", "int32_t", "i32",
            NativeValueABI::SignedInteger, 32},
        {NativeTypeID::Int8, "INT8", "@native.int8", "int8_t", "i8",
            NativeValueABI::SignedInteger, 8},
        {NativeTypeID::Integer, "INTEGER", "int", "int64_t", "i64",
            NativeValueABI::Int64, 64, {"int32", "int64"}, 2},
        {NativeTypeID::Bool, "BOOL", "bool", "bool", "i1",
            NativeValueABI::Bool1, 1},
        {NativeTypeID::Pointer, "PTR", "ptr", "void*", "i64",
            NativeValueABI::NativePointer, 64},
        {NativeTypeID::String, "STRING", "string", "ZeusNativeString", "ptr",
            NativeValueABI::NativeString, 0, {"str", "String"}, 2},
        {NativeTypeID::Bytes, "BYTES", "bytes", "ZeusNativeBytes", "ptr",
            NativeValueABI::NativeBytes},
        {NativeTypeID::RawBytesRange, "RAW_BYTES_RANGE", "&bytes", "ZeusRawBytesRange",
            "%ZeusRawBytesRange", NativeValueABI::RawBytesRange},
        {NativeTypeID::Float32, "FLOAT", "@native.float32", "float", "float",
            NativeValueABI::FloatingPoint, 32},
        {NativeTypeID::Int16, "INT16", "@native.int16", "int16_t", "i16",
            NativeValueABI::SignedInteger, 16},
        {NativeTypeID::UInt8, "UINT8", "@native.uint8", "uint8_t", "i8",
            NativeValueABI::UnsignedInteger, 8},
        {NativeTypeID::UInt16, "UINT16", "@native.uint16", "uint16_t", "i16",
            NativeValueABI::UnsignedInteger, 16},
        {NativeTypeID::UInt32, "UINT32", "@native.uint32", "uint32_t", "i32",
            NativeValueABI::UnsignedInteger, 32},
        {NativeTypeID::UInt64, "UINT64", "@native.uint64", "uint64_t", "i64",
            NativeValueABI::UnsignedInteger, 64},
    }};
    return types;
}

inline const NativeTypeIDEntry* findNativeTypeID(int64_t id) {
    for (const auto& type : nativeTypeIDs()) {
        if (static_cast<int64_t>(type.id) == id) return &type;
    }
    return nullptr;
}

inline const NativeTypeIDEntry* findNativeTypeIDValue(std::string_view value) {
    for (const auto& type : nativeTypeIDs()) {
        if (value == std::to_string(static_cast<int64_t>(type.id))) return &type;
    }
    return nullptr;
}

inline const NativeTypeIDEntry* findNativeTypeIDEnum(std::string_view name) {
    for (const auto& type : nativeTypeIDs()) {
        if (type.enumName == name) return &type;
    }
    return nullptr;
}

inline const NativeTypeIDEntry* findNativeTypeIDCanonical(std::string_view name) {
    for (const auto& type : nativeTypeIDs()) {
        if (type.canonicalName == name) return &type;
    }
    return nullptr;
}

inline const NativeTypeIDEntry* findNativeScalarCanonical(std::string_view name) {
    const auto* type = findNativeTypeIDCanonical(name);
    if (type == nullptr ||
        (type->valueABI != NativeValueABI::SignedInteger &&
         type->valueABI != NativeValueABI::UnsignedInteger &&
         type->valueABI != NativeValueABI::FloatingPoint)) return nullptr;
    return type;
}

} // namespace ZeusBuiltins
