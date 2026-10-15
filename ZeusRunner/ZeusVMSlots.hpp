#pragma once

#include <map>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>

#include "ZeusVMAddress.hpp"
#include "../ZeusRuntimeLib/ZeusMemoryABI.hpp"
#include "../ZeusRuntimeLib/ZeusGC.hpp"

namespace ZeusVMSlots {

    class Registry {
        public:
            template <typename VMType>
            bool address(VMType& vm, Cell* cell, bool reveal, bool readOnly,
                         void*& out, std::string& error) {
                for (int depth = 0; depth < 64; ++depth) {
                    if (!cell) {
                        error = "VM @addr refers to a missing cell";
                        return false;
                    }
                    const auto value = cell->readValue();
                    const auto* reference = std::get_if<ZeusType::ReferenceHandle>(
                        &value);
                    if (!reference) {
                        Entry entry;
                        entry.cell = cell;
                        entry.cellId = cell->debugId;
                        entry.reveal = reveal;
                        entry.readOnly = readOnly || (cell->flags & 1) != 0;
                        return registerEntry(vm, std::move(entry), out, error);
                    }
                    if (!reference->address) {
                        error = "VM @addr needs a logical cell for this reference";
                        return false;
                    }
                    readOnly = readOnly || (cell->flags & 1) != 0;
                    if (reference->typeName == "cell") {
                        cell = static_cast<Cell*>(reference->address);
                        continue;
                    }
                    if (reference->typeName == "shared") {
                        std::string name;
                        auto shared = ZeusShared::getGlobalHeap().findByAddress(
                            static_cast<ZeusShared::SharedCell*>(reference->address),
                            reference->generation, &name);
                        if (!shared) {
                            error = "VM @addr refers to an expired shared cell";
                            return false;
                        }
                        return address(vm, shared, name, reveal, readOnly, out, error);
                    }
                    error = "VM @addr needs a logical cell for this reference";
                    return false;
                }
                error = "VM @addr reference chain exceeds 64 cells";
                return false;
            }

            template <typename VMType>
            bool address(VMType& vm, const std::shared_ptr<ZeusShared::SharedCell>& cell,
                         const std::string& name, bool reveal, bool readOnly,
                         void*& out, std::string& error) {
                if (!cell) {
                    error = "VM @addr refers to a missing shared cell";
                    return false;
                }
                std::lock_guard<std::recursive_mutex> lock(cell->mutex);
                const auto value = cell->read();
                if (const auto* reference = std::get_if<ZeusType::ReferenceHandle>(
                        &value)) {
                    if (reference->typeName == "cell" && reference->address) {
                        return address(vm, static_cast<Cell*>(reference->address),
                                       reveal, readOnly, out, error);
                    }
                    error = "VM @addr needs a logical cell for this shared reference";
                    return false;
                }
                Entry entry;
                entry.shared = cell;
                entry.sharedName = name;
                entry.sharedGeneration = cell->generation;
                entry.reveal = reveal;
                entry.readOnly = readOnly;
                return registerEntry(vm, std::move(entry), out, error);
            }

            template <typename VMType>
            bool fetch(VMType& vm, void* address, const std::string& targetType,
                       ZeusType::UniversalVariant& out, std::string& error,
                       int64_t accessSize = -1) {
                if (targetType == "&bytes" && accessSize >= 0) {
                    return ZeusVMAddress::fetch(
                        address, targetType, out, error, accessSize);
                }
                const auto found = slots.find(reinterpret_cast<uintptr_t>(address));
                if (found != slots.end() && !found->second.reveal) {
                    auto& entry = found->second;
                    std::shared_ptr<ZeusShared::SharedCell> shared;
                    if (!resolve(entry, shared, error)) return false;
                    out = shared ? shared->read() : entry.cell->readValue();
                    if (!validateType(vm, out, targetType, accessSize, error)) return false;
                    ZeusGC::retainVariantDeep(vm, out);
                    return true;
                }
                if (!validateRaw(vm, address, targetType, accessSize, false, nullptr, error)) return false;
                return ZeusVMAddress::fetch(address, targetType, out, error, accessSize);
            }

            template <typename VMType>
            bool store(VMType& vm, void* address, const std::string& targetType,
                       const ZeusType::UniversalVariant& source, std::string& error,
                       int64_t accessSize = -1) {
                if (targetType == "&bytes" && accessSize >= 0) {
                    return ZeusVMAddress::store(
                        address, targetType, source, error, accessSize);
                }
                const std::string resolvedTarget = targetType.rfind("__zeus_typevar:", 0) == 0
                    ? typeName(vm, source) : targetType;
                const auto found = slots.find(reinterpret_cast<uintptr_t>(address));
                if (found != slots.end() && !found->second.reveal) {
                    auto& entry = found->second;
                    std::shared_ptr<ZeusShared::SharedCell> shared;
                    std::unique_lock<std::recursive_mutex> lock;
                    if (!resolve(entry, shared, error)) return false;
                    if (shared) lock = std::unique_lock<std::recursive_mutex>(shared->mutex);
                    if (entry.readOnly || (entry.cell && (entry.cell->flags & 1) != 0)) {
                        error = "VM @faddr cannot write a const slot";
                        return false;
                    }
                    const auto currentValue = shared ? shared->read() : entry.cell->readValue();
                    if (!validateType(vm, currentValue, resolvedTarget, accessSize, error)) return false;
                    if (typeName(vm, source) != resolvedTarget) {
                        error = "VM @faddr assignment requires a value of slot type '" + resolvedTarget + "'";
                        return false;
                    }
                    if (shared) {
                        ZeusGC::retainVariantDeep(vm, source);
                        auto oldValue = shared->read();
                        shared->write(source);
                        ZeusGC::releaseVariantDeep(vm, oldValue);
                    } else {
                        vm.assignCellValue(entry.cell, source);
                    }
                    return true;
                }
                if (const auto* sourceRange =
                        std::get_if<ZeusType::RawBytesRange>(&source)) {
                    if (accessSize < 0 ||
                        sourceRange->length != static_cast<uint64_t>(accessSize)) {
                        error = "VM @faddr raw bytes range write length must equal the source length";
                        return false;
                    }
                    if (!validateRaw(
                            vm, reinterpret_cast<void*>(sourceRange->address),
                            "&bytes", static_cast<int64_t>(sourceRange->length),
                            false, nullptr, error)) return false;
                }
                if (!validateRaw(vm, address, resolvedTarget, accessSize, true, &source, error)) return false;
                return ZeusVMAddress::store(address, resolvedTarget, source, error, accessSize);
            }

        private:
            struct Entry {
                Cell* cell = nullptr;
                uint32_t cellId = 0;
                std::weak_ptr<ZeusShared::SharedCell> shared;
                std::string sharedName;
                uint64_t sharedGeneration = 0;
                std::string type;
                bool reveal = false;
                uintptr_t revealedBase = 0;
                uint64_t revealedSize = 0;
                bool readOnly = false;
            };
            std::map<uintptr_t, Entry> slots;
            using SlotIdentity = std::tuple<int, std::string, uintptr_t, uint64_t>;
            std::map<SlotIdentity, uintptr_t> identities;
            uint64_t nextLogicalAddress = 0x6000000000000000ULL;

            bool allocateLogicalAddress(void*& out, std::string& error) {
                if (sizeof(uintptr_t) < sizeof(uint64_t)) {
                    error = "VM logical slot addresses require a 64-bit host";
                    return false;
                }
                const uint64_t maximum = static_cast<uint64_t>(
                    std::numeric_limits<int64_t>::max());
                while (nextLogicalAddress <= maximum) {
                    const uintptr_t candidate =
                        static_cast<uintptr_t>(nextLogicalAddress++);
                    if (slots.find(candidate) == slots.end()) {
                        out = reinterpret_cast<void*>(candidate);
                        return true;
                    }
                }
                error = "VM logical slot address space exhausted";
                return false;
            }

            template <typename VMType>
            static std::string typeName(VMType& vm, const ZeusType::UniversalVariant& value) {
                const auto nativeType = ZeusVMAddress::runtimeNativeTypeName(value);
                if (nativeType == "string" || nativeType == "bytes" ||
                    nativeType == "&bytes") return nativeType;
                return ZeusRuntimeType::canonicalTypeName(vm.runtimeValueTypeName(value));
            }

            static bool resolve(
                const Entry& entry, std::shared_ptr<ZeusShared::SharedCell>& shared,
                std::string& error
            ) {
                if (entry.cell) {
                    std::lock_guard<std::mutex> lock(HeapStats::cellRegistryMutex);
                    if (HeapStats::liveCells.count(entry.cell) == 0 ||
                        entry.cell->debugId != entry.cellId) {
                        error = "VM @faddr refers to an expired cell slot";
                        return false;
                    }
                    return true;
                }
                shared = ZeusShared::getGlobalHeap().find(entry.sharedName, entry.sharedGeneration);
                if (!shared || entry.shared.lock() != shared) {
                    error = "VM @faddr refers to an expired shared slot";
                    return false;
                }
                return true;
            }

            template <typename VMType>
            bool registerEntry(VMType& vm, Entry entry, void*& out, std::string& error) {
                std::shared_ptr<ZeusShared::SharedCell> shared;
                if (!resolve(entry, shared, error)) return false;
                const auto value = shared ? shared->read() : entry.cell->readValue();
                entry.type = typeName(vm, value);
                if (entry.type.empty()) {
                    error = "VM @addr cannot determine the slot type";
                    return false;
                }

                if (!entry.reveal) {
                    pruneExpiredSlots();
                    const SlotIdentity identity = shared
                        ? SlotIdentity{2, entry.sharedName, 0, entry.sharedGeneration}
                        : SlotIdentity{1, {}, reinterpret_cast<uintptr_t>(entry.cell), entry.cellId};
                    const auto existing = identities.find(identity);
                    if (existing != identities.end()) {
                        const auto slot = slots.find(existing->second);
                        if (slot != slots.end()) {
                            slot->second.type = entry.type;
                            slot->second.readOnly = slot->second.readOnly || entry.readOnly;
                            out = reinterpret_cast<void*>(existing->second);
                            return true;
                        }
                        identities.erase(existing);
                    }
                    if (!allocateLogicalAddress(out, error)) return false;
                    const auto token = reinterpret_cast<uintptr_t>(out);
                    slots[token] = std::move(entry);
                    identities[identity] = token;
                    return true;
                }

                if (entry.reveal) {
                    if (shared) {
                        if (!ZeusVMAddress::address(shared.get(), true, out, error)) return false;
                    } else if (!ZeusVMAddress::address(entry.cell, true, out, error)) return false;
                    entry.revealedBase = reinterpret_cast<uintptr_t>(out);
                    if (entry.type == "string" || entry.type == "bytes") {
                        ZeusVMAddress::NativeBufferSpan span;
                        if (!ZeusVMAddress::nativeBufferSpan(
                                out, entry.type, span, error)) return false;
                        entry.revealedBase = reinterpret_cast<uintptr_t>(span.header);
                        entry.revealedSize = span.allocationSize;
                    } else if (entry.type == "int") entry.revealedSize = sizeof(int64_t);
                    else if (entry.type == "bool") entry.revealedSize = sizeof(bool);
                    else if (entry.type == "bigint") entry.revealedSize = sizeof(ZeusType::BigInteger);
                    else if (entry.type == "float") entry.revealedSize = sizeof(ZeusType::BigDecimal);
                    else if (entry.type == "Array") entry.revealedSize = sizeof(ZeusType::ArrayData);
                    else if (entry.type == "dict") entry.revealedSize = sizeof(ZeusType::ObjectData);
                    else entry.revealedSize = std::visit([](const auto& payload) {
                        return static_cast<uint64_t>(sizeof(payload));
                    }, value);
                } else if (!allocateLogicalAddress(out, error)) {
                    return false;
                }
                slots[reinterpret_cast<uintptr_t>(out)] = std::move(entry);
                return true;
            }

            void pruneExpiredSlots() {
                for (auto slot = slots.begin(); slot != slots.end();) {
                    if (slot->second.reveal) {
                        ++slot;
                        continue;
                    }
                    std::shared_ptr<ZeusShared::SharedCell> shared;
                    std::string error;
                    if (resolve(slot->second, shared, error)) {
                        ++slot;
                        continue;
                    }
                    const SlotIdentity identity = slot->second.cell
                        ? SlotIdentity{1, {}, reinterpret_cast<uintptr_t>(slot->second.cell), slot->second.cellId}
                        : SlotIdentity{2, slot->second.sharedName, 0,
                                       slot->second.sharedGeneration};
                    const auto cached = identities.find(identity);
                    if (cached != identities.end() && cached->second == slot->first) {
                        identities.erase(cached);
                    }
                    slot = slots.erase(slot);
                }
            }

            template <typename VMType>
            static bool validateType(VMType& vm,
                                     const ZeusType::UniversalVariant& value,
                                     const std::string& targetType, int64_t accessSize,
                                     std::string& error) {
                const auto type = ZeusRuntimeType::canonicalTypeName(targetType);
                const std::string currentType = typeName(vm, value);
                if (type != currentType) {
                    error = "VM @faddr slot type is '" + currentType + "', requested '" + type + "'";
                    return false;
                }
                if (accessSize >= 0 && (type != "int" || accessSize != 8)) {
                    error = "VM @faddr typed slots require a complete value; partial widths and byte lengths use raw memory";
                    return false;
                }
                return true;
            }

            static bool within(void* address, uint64_t size, const void* base, uint64_t length) {
                const auto start = reinterpret_cast<uintptr_t>(base);
                const auto target = reinterpret_cast<uintptr_t>(address);
                return base && target >= start && target - start <= length &&
                    size <= length - (target - start);
            }

            template <typename VMType>
            bool validateRaw(VMType& vm, void* address, const std::string& targetType,
                             int64_t accessSize, bool writing,
                             const ZeusType::UniversalVariant* source, std::string& error) {
                const auto type = ZeusRuntimeType::canonicalTypeName(targetType);
                uint64_t size = 0;
                void* checkedAddress = address;
                const bool unsizedBufferRead = !writing && accessSize < 0 &&
                    (type == "string" || type == "bytes");
                if ((type == "bytes" || type == "&bytes") && accessSize >= 0) {
                    size = static_cast<uint64_t>(accessSize);
                } else if (type == "&bytes") {
                    error = "VM @faddr raw bytes range requires an explicit length";
                    return false;
                } else if (type == "int") size = accessSize >= 0 ? static_cast<uint64_t>(accessSize) : 8;
                else if (type == "bool") size = 1;
                else if (unsizedBufferRead) {
                    ZeusVMAddress::NativeBufferSpan span;
                    if (!ZeusVMAddress::nativeBufferSpan(address, type, span, error)) return false;
                    checkedAddress = const_cast<uint8_t*>(span.header);
                    size = span.allocationSize;
                } else if (!writing && (type == "string" || type == "bytes")) {
                    size = static_cast<uint64_t>(accessSize);
                } else if (writing && type == "string" && source) {
                    auto value = *source;
                    auto* text = ZeusNativeAddress::stringPayload(value);
                    if (!text) { error = "VM @faddr string write requires string data"; return false; }
                    size = text->size();
                } else if (writing && type == "bytes" && source) {
                    auto value = *source;
                    auto* bytes = ZeusNativeAddress::bytesPayload(value);
                    if (!bytes) { error = "VM @faddr bytes write requires bytes data"; return false; }
                    size = bytes->data.size();
                    if (accessSize >= 0 && size != static_cast<uint64_t>(accessSize)) {
                        error = "VM @faddr bytes write length must equal the source length";
                        return false;
                    }
                } else if (writing && type == "&bytes" && source) {
                    const auto* sourceRange =
                        std::get_if<ZeusType::RawBytesRange>(source);
                    auto value = *source;
                    auto* bytes = ZeusNativeAddress::bytesPayload(value);
                    if (accessSize < 0) {
                        error = "VM @faddr raw bytes range assignment requires an explicit length";
                        return false;
                    }
                    if (sourceRange) {
                        size = sourceRange->length;
                    } else if (bytes) {
                        size = bytes->data.size();
                    } else {
                        error = "VM @faddr raw bytes range write requires bytes data";
                        return false;
                    }
                    if (size != static_cast<uint64_t>(accessSize)) {
                        error = "VM @faddr raw bytes range write length must equal the source length";
                        return false;
                    }
                } else {
                    error = "VM @faddr raw memory has no complete mapping for type '" + type + "'";
                    return false;
                }
                const uintptr_t start = reinterpret_cast<uintptr_t>(checkedAddress);
                if (size > static_cast<uint64_t>(
                        std::numeric_limits<uintptr_t>::max() - start)) {
                    error = "VM @faddr byte range overflows the address space";
                    return false;
                }
                if (size == 0 && !checkedAddress) return true;
                bool interiorSlot = false;
                for (auto& item : slots) {
                    auto& entry = item.second;
                    if (!entry.reveal) {
                        if (within(checkedAddress, size, reinterpret_cast<void*>(item.first),
                                   sizeof(ZeusValueSlot))) {
                            interiorSlot = true;
                        }
                        continue;
                    }
                    std::shared_ptr<ZeusShared::SharedCell> shared;
                    std::string resolveError;
                    if (!resolve(entry, shared, resolveError)) {
                        if (within(checkedAddress, size,
                                   reinterpret_cast<void*>(entry.revealedBase),
                                   entry.revealedSize)) {
                            error = resolveError;
                            return false;
                        }
                        continue;
                    }
                    std::unique_lock<std::recursive_mutex> sharedLock;
                    if (shared) {
                        sharedLock = std::unique_lock<std::recursive_mutex>(shared->mutex);
                    }
                    const auto value = shared ? shared->read() : entry.cell->readValue();
                    if (typeName(vm, value) != entry.type) continue;
                    void* currentData = nullptr;
                    if (shared) {
                        if (!ZeusVMAddress::address(
                                shared.get(), true, currentData, resolveError)) continue;
                    } else if (!ZeusVMAddress::address(
                                   entry.cell, true, currentData, resolveError)) {
                        continue;
                    }
                    uintptr_t currentBaseAddress = reinterpret_cast<uintptr_t>(currentData);
                    uint64_t currentSize = entry.revealedSize;
                    uint64_t currentLength = currentSize;
                    if (entry.type == "string" || entry.type == "bytes") {
                        ZeusVMAddress::NativeBufferSpan span;
                        if (!ZeusVMAddress::nativeBufferSpan(
                                currentData, entry.type, span, resolveError)) continue;
                        currentBaseAddress = reinterpret_cast<uintptr_t>(span.header);
                        currentSize = span.allocationSize;
                        currentLength = span.length;
                    }
                    void* currentBase = reinterpret_cast<void*>(currentBaseAddress);
                    if (!within(checkedAddress, size, currentBase, currentSize)) continue;
                    if (writing) {
                        const uintptr_t writeStart = reinterpret_cast<uintptr_t>(address);
                        const uintptr_t writeEnd = writeStart + size;
                        const uintptr_t headerEnd = currentBaseAddress + sizeof(uint64_t);
                        const bool overlapsHeader = size != 0 &&
                            writeStart < headerEnd && currentBaseAddress < writeEnd;
                        if (entry.readOnly || entry.type == "string" || overlapsHeader) {
                            error = "VM @faddr cannot overwrite a const value or string/bytes length";
                            return false;
                        }
                        if (entry.type == "bytes" &&
                            !within(address, size, currentData, currentLength)) {
                            error = "VM @faddr bytes writes must stay inside the data range";
                            return false;
                        }
                        return true;
                    }
                    if (unsizedBufferRead &&
                        (address != currentData || type != entry.type)) {
                        error = "VM @faddr unsized string/bytes reads require the data pointer";
                        return false;
                    }
                    return true;
                }
                if (interiorSlot) {
                    error = "VM @faddr cannot access an interior or mismatched typed slot";
                    return false;
                }
                if (zeus_memory_find_allocation(checkedAddress, size)) {
                    return true;
                }
                error = "VM @faddr address has no live typed slot, revealed storage, or tracked allocation";
                return false;
            }
    };
}
