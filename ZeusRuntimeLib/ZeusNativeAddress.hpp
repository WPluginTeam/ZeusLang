#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <variant>

#include "ZeusCell.hpp"
#include "ZeusSharedHeap.hpp"

namespace ZeusNativeAddress {

inline std::string* stringPayload(ZeusType::UniversalVariant& value) {
    if (auto* text = std::get_if<std::string>(&value)) return text;
    if (auto* literal = std::get_if<LexerType::Value>(&value)) {
        return std::get_if<std::string>(&literal->data);
    }
    return nullptr;
}

inline ZeusType::Bytes* bytesPayload(ZeusType::UniversalVariant& value) {
    if (auto* bytes = std::get_if<ZeusType::Bytes>(&value)) return bytes;
    if (auto* variant = std::get_if<ZeusType::Variant>(&value)) {
        return std::get_if<ZeusType::Bytes>(variant);
    }
    return nullptr;
}

inline ZeusType::BigInteger* integerPayload(ZeusType::UniversalVariant& value) {
    if (auto* integer = std::get_if<ZeusType::BigInteger>(&value)) return integer;
    if (auto* literal = std::get_if<LexerType::Value>(&value)) {
        return std::get_if<ZeusType::BigInteger>(&literal->data);
    }
    return nullptr;
}

inline ZeusType::BigDecimal* decimalPayload(ZeusType::UniversalVariant& value) {
    if (auto* decimal = std::get_if<ZeusType::BigDecimal>(&value)) return decimal;
    if (auto* literal = std::get_if<LexerType::Value>(&value)) {
        return std::get_if<ZeusType::BigDecimal>(&literal->data);
    }
    return nullptr;
}

inline bool boolPayload(const ZeusType::UniversalVariant& value, bool& out) {
    if (const auto* boolean = std::get_if<bool>(&value)) {
        out = *boolean;
        return true;
    }
    if (const auto* literal = std::get_if<LexerType::Value>(&value)) {
        if (const auto* boolean = std::get_if<bool>(&literal->data)) {
            out = *boolean;
            return true;
        }
    }
    return false;
}

inline bool addressCell(Cell* cell, bool reveal, void*& out, std::string& error);

inline bool addressValue(ZeusType::UniversalVariant& value,
                         bool reveal, void*& out, std::string& error) {
    if (auto* reference = std::get_if<ZeusType::ReferenceHandle>(&value)) {
        if (!reference->address) {
            error = "cannot take the address of a null VM reference";
            return false;
        }
        if (reference->typeName == "cell") {
            return addressCell(static_cast<Cell*>(reference->address), reveal, out, error);
        }
        out = reference->address;
        return true;
    }

    if (auto* text = stringPayload(value)) {
        (void)text;
        (void)reveal;
        error = "string values require native [length][data] slot storage";
        return false;
    }

    if (auto* bytes = bytesPayload(value)) {
        (void)bytes;
        (void)reveal;
        error = "bytes values require native [length][data] slot storage";
        return false;
    }

    if (auto* integer = integerPayload(value)) {
        if (int64_t* native = integer->nativeInt64Address()) {
            out = native;
        } else {
            out = integer;
        }
        return true;
    }
    if (auto* decimal = decimalPayload(value)) {
        out = decimal;
        return true;
    }
    if (auto* boolean = std::get_if<bool>(&value)) {
        out = boolean;
        return true;
    }
    if (auto* literal = std::get_if<LexerType::Value>(&value)) {
        if (auto* boolean = std::get_if<bool>(&literal->data)) {
            out = boolean;
            return true;
        }
        if (auto* decimal = std::get_if<ZeusType::BigDecimal>(&literal->data)) {
            out = decimal;
            return true;
        }
    }
    if (auto* decimal = std::get_if<ZeusType::BigDecimal>(&value)) {
        out = decimal;
        return true;
    }
    if (auto* character = std::get_if<char>(&value)) {
        out = character;
        return true;
    }
    if (auto* classHandle = std::get_if<ZeusType::LLVMClassHandle>(&value)) {
        if (classHandle->state) {
            if (void* object = classHandle->state->object.load(std::memory_order_acquire)) {
                out = object;
                return true;
            }
        }
    }
    if (auto* variant = std::get_if<ZeusType::Variant>(&value)) {
        if (auto* object = std::get_if<ZeusType::Object>(variant)) {
            if (object->impl) {
                out = object->impl.get();
                return true;
            }
        }
        if (auto* array = std::get_if<ZeusType::Array>(variant)) {
            if (array->impl) {
                out = array->impl.get();
                return true;
            }
        }
    }
    if (auto* closure = std::get_if<ZeusType::NativeClosureHandle>(&value)) {
        if (closure->state) {
            out = closure->state.get();
            return true;
        }
    }

    out = std::visit([](auto& payload) -> void* { return &payload; }, value);
    return true;
}

inline bool addressCell(Cell* cell, bool reveal, void*& out, std::string& error) {
    std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
    for (int depth = 0; depth < 64; ++depth) {
        if (!cell) {
            error = "address refers to a missing VM cell";
            return false;
        }
        if (void* inlineAddress = cell->value.nativeAddress()) {
            out = inlineAddress;
            return true;
        }
        Cell* nextCell = nullptr;
        bool complete = false;
        bool success = false;
        cell->withValue([&](ZeusType::UniversalVariant& value) {
            auto* reference = std::get_if<ZeusType::ReferenceHandle>(&value);
            if (!reference) {
                success = addressValue(value, reveal, out, error);
                complete = true;
                return;
            }
            if (!reference->address) {
                error = "cannot take the address of a null VM reference";
                complete = true;
                return;
            }
            if (reference->typeName == "cell") {
                nextCell = static_cast<Cell*>(reference->address);
                return;
            }
            out = reference->address;
            success = true;
            complete = true;
        });
        if (complete) return success;
        if (!nextCell) return false;
        cell = nextCell;
    }
    error = "VM address reference chain exceeds 64 cells";
    return false;
}

inline bool addressShared(ZeusShared::SharedCell* cell, bool reveal,
                          void*& out, std::string& error) {
    if (!cell) {
        error = "address refers to a missing VM shared cell";
        return false;
    }
    std::lock_guard<std::recursive_mutex> cellLock(cell->mutex);
    if (void* inlineAddress = cell->value.nativeAddress()) {
        out = inlineAddress;
        return true;
    }
    bool success = false;
    cell->withValue([&](ZeusType::UniversalVariant& value) {
        success = addressValue(value, reveal, out, error);
    });
    return success;
}

} // namespace ZeusNativeAddress
