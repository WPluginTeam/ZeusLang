#include "ZeusLValueABI.hpp"
#include "ZeusMemoryABI.hpp"
#include "ZeusObjectLifetime.hpp"
#include "ZeusABIInternal.hpp"
#include "ZeusDynamicMemberABI.hpp"
#include "ZeusSharedHeap.hpp"
#include "ZeusVMUtils.hpp"
#include <cstring>
#include <atomic>
#include <algorithm>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
struct LValueSlot {
    std::string type;
    int32_t abi = 0;
    uint64_t size = 0;
    bool writable = true;
    std::weak_ptr<ZeusShared::SharedCell> shared;
    void* storage = nullptr;
};
struct OwnedRawRange {
    void* owner = nullptr;
    void* address = nullptr;
    uint64_t size = 0;
    bool writable = false;
};
std::mutex& lvalueMutex() {
    // Native buffers can be released by static Zeus cells during process
    // teardown; keep the address registry available for their unregister path.
    static auto* value = new std::mutex();
    return *value;
}
std::unordered_map<void*, LValueSlot>& lvalueSlots() {
    static auto* value = new std::unordered_map<void*, LValueSlot>();
    return *value;
}
std::unordered_map<void*, void*>& lvalueTokenByStorage() {
    static auto* value = new std::unordered_map<void*, void*>();
    return *value;
}
std::vector<OwnedRawRange>& ownedRawRanges() {
    static auto* value = new std::vector<OwnedRawRange>();
    return *value;
}
// Raw address checks are opt-in: they only run while the current thread is
// dynamically executing inside a `try` region. Outside try, @faddr/@reveal
// raw access is unchecked (a bad address is a wild pointer, i.e. a code bug).
int32_t& tryDepth() {
    static thread_local int32_t value = 0;
    return value;
}
void validateRawRange(void* address, uint64_t size) {
    if (!address || !size) throw std::runtime_error("invalid raw lvalue range");
    const uintptr_t base = reinterpret_cast<uintptr_t>(address);
    if (size > std::numeric_limits<uintptr_t>::max() - base)
        throw std::runtime_error("raw lvalue range overflows the address space");
}
void registerOwnedRawRanges(void* owner, const OwnedRawRange* ranges, size_t count) {
    if (!owner || !ranges || count == 0)
        throw std::runtime_error("invalid owned raw lvalue registration");
    for (size_t index = 0; index < count; ++index)
        validateRawRange(ranges[index].address, ranges[index].size);

    std::lock_guard<std::mutex> lock(lvalueMutex());
    size_t additions = 0;
    for (size_t index = 0; index < count; ++index) {
        const auto found = std::find_if(
            ownedRawRanges().begin(), ownedRawRanges().end(),
            [&](const OwnedRawRange& entry) {
                return entry.owner == owner && entry.address == ranges[index].address;
            });
        if (found == ownedRawRanges().end()) ++additions;
    }
    ownedRawRanges().reserve(ownedRawRanges().size() + additions);
    for (size_t index = 0; index < count; ++index) {
        auto found = std::find_if(
            ownedRawRanges().begin(), ownedRawRanges().end(),
            [&](const OwnedRawRange& entry) {
                return entry.owner == owner && entry.address == ranges[index].address;
            });
        if (found != ownedRawRanges().end()) *found = ranges[index];
        else ownedRawRanges().push_back(ranges[index]);
    }
}
void registerRevealedString(void* address) {
    if (!address) throw std::runtime_error("revealed native string has no data");
    const auto* data = static_cast<const char*>(address);
    zeus_native_string_validate(data);
    uint64_t length = 0;
    std::memcpy(&length, data - sizeof(uint64_t), sizeof(length));
    if (length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        length > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t) - 1) {
        throw std::runtime_error("revealed native string length is invalid");
    }
    auto* characters = const_cast<char*>(data);
    if (characters[length] != '\0')
        throw std::runtime_error("revealed native string is missing its terminator");
    void* stableOwner = const_cast<char*>(data);
    OwnedRawRange ranges[3] = {
        {stableOwner, const_cast<char*>(data - sizeof(uint64_t)), sizeof(uint64_t), false},
        {stableOwner, characters, length, false},
        {stableOwner, characters + length, 1, false}
    };
    const size_t count = length == 0 ? 2 : 3;
    if (length == 0) ranges[1] = ranges[2];
    registerOwnedRawRanges(stableOwner, ranges, count);
}
void registerDynamicRevealedString(void* owner, void* address,
                                   uint64_t expectedLength) {
    if (!owner || !address)
        throw std::runtime_error("revealed dynamic string has no owner or data");
    const uintptr_t dataAddress = reinterpret_cast<uintptr_t>(address);
    if (dataAddress < sizeof(uint64_t))
        throw std::runtime_error("revealed dynamic string header underflows");
    auto* data = static_cast<char*>(address);
    auto* header = reinterpret_cast<char*>(dataAddress - sizeof(uint64_t));
    uint64_t length = 0;
    std::memcpy(&length, header, sizeof(length));
    if (length != expectedLength ||
        length > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t) - 1 ||
        !zeus_memory_find_allocation(header, sizeof(uint64_t) + length + 1) ||
        data[length] != '\0') {
        throw std::runtime_error("revealed dynamic string has an invalid [length][data] layout");
    }
    const OwnedRawRange range{
        owner, header, sizeof(uint64_t) + length + 1, false};
    registerOwnedRawRanges(owner, &range, 1);
}
void registerNativeBytesData(void* address) {
    if (!address) throw std::runtime_error("native Bytes has no data pointer");
    const auto* data = static_cast<const uint8_t*>(address);
    const int64_t signedLength = zeus_bytes_len(data);
    if (signedLength < 0) throw std::runtime_error("native Bytes has a negative length");
    const uint64_t length = static_cast<uint64_t>(signedLength);
    const uintptr_t dataAddress = reinterpret_cast<uintptr_t>(data);
    if (length > std::numeric_limits<uint64_t>::max() - dataAddress) {
        throw std::runtime_error("native Bytes length overflows the address space");
    }
    void* stableOwner = const_cast<uint8_t*>(data);
    OwnedRawRange ranges[2] = {
        {stableOwner, const_cast<uint8_t*>(data - sizeof(uint64_t)), sizeof(uint64_t), false},
        {stableOwner, const_cast<uint8_t*>(data), length, true}
    };
    registerOwnedRawRanges(stableOwner, ranges, length == 0 ? 1 : 2);
}
void registerDynamicRevealedBytes(void* owner, void* address,
                                  uint64_t expectedLength) {
    if (!owner || !address)
        throw std::runtime_error("revealed dynamic Bytes has no owner or data");
    const uintptr_t dataAddress = reinterpret_cast<uintptr_t>(address);
    if (dataAddress < sizeof(uint64_t))
        throw std::runtime_error("revealed dynamic Bytes header underflows");
    auto* data = static_cast<uint8_t*>(address);
    auto* header = reinterpret_cast<uint8_t*>(dataAddress - sizeof(uint64_t));
    uint64_t length = 0;
    std::memcpy(&length, header, sizeof(length));
    if (length != expectedLength ||
        length > std::numeric_limits<uint64_t>::max() - sizeof(uint64_t) ||
        !zeus_memory_find_allocation(header, sizeof(uint64_t) + length)) {
        throw std::runtime_error("revealed dynamic Bytes has an invalid [length][data] layout");
    }
    OwnedRawRange ranges[2] = {
        {owner, header, sizeof(uint64_t), false},
        {owner, data, length, true}
    };
    registerOwnedRawRanges(owner, ranges, length == 0 ? 1 : 2);
}
bool rangeStartsInside(uintptr_t base, uint64_t size, uintptr_t candidate) {
    return candidate >= base && candidate - base < size;
}
bool rangesOverlap(uintptr_t left, uint64_t leftSize,
                   uintptr_t right, uint64_t rightSize) {
    return leftSize != 0 && rightSize != 0 &&
        (left <= right ? right - left < leftSize : left - right < rightSize);
}
void unregisterOwnedRawRangesUnlocked(void* owner) {
    auto& ranges = ownedRawRanges();
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
        [owner](const OwnedRawRange& range) { return range.owner == owner; }),
        ranges.end());
}
void* nextLValueToken() {
    static std::atomic<uint64_t> next{0x6000000000000000ULL};
    const uint64_t token = next.fetch_add(1, std::memory_order_relaxed);
    if (sizeof(uintptr_t) < sizeof(uint64_t) ||
        token > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        throw std::runtime_error("LLVM logical slot address space exhausted");
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(token));
}
std::string lvalueType(const char* type) {
    std::string result = type ? type : "";
    if (result == "&bytes") return result;
    if (!result.empty() && result.front() == '&') result.erase(0, 1);
    if (result == "int64") result = "int";
    return result;
}
uint64_t rawValueSize(const std::string& type) {
    if (type == "&bytes") return sizeof(ZeusType::RawBytesRange);
    if (type == "int" || type == "float") return 8;
    if (type == "int32") return 4;
    if (type == "bool") return 1;
    if (type == "string" || type == "bytes") return sizeof(void*);
    throw std::runtime_error("raw @faddr requires an explicit supported native storage type");
}
bool findRegisteredBuffer(void* address, const std::string& type,
                          uint64_t& length, bool& writable) {
    if (!address || (type != "string" && type != "bytes")) return false;
    const uintptr_t dataAddress = reinterpret_cast<uintptr_t>(address);
    if (dataAddress < sizeof(uint64_t)) return false;
    const uintptr_t headerAddress = dataAddress - sizeof(uint64_t);
    const uint64_t terminator = type == "string" ? 1 : 0;

    std::lock_guard<std::mutex> lock(lvalueMutex());
    for (const auto& header : ownedRawRanges()) {
        if (reinterpret_cast<uintptr_t>(header.address) != headerAddress ||
            header.size < sizeof(uint64_t)) {
            continue;
        }

        uint64_t candidateLength = 0;
        std::memcpy(&candidateLength, header.address, sizeof(candidateLength));
        if (candidateLength > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
            candidateLength > static_cast<uint64_t>(std::numeric_limits<size_t>::max()) ||
            candidateLength > std::numeric_limits<uint64_t>::max() -
                sizeof(uint64_t) - terminator) {
            continue;
        }
        const uintptr_t maxAddress = std::numeric_limits<uintptr_t>::max();
        if (dataAddress > maxAddress - terminator ||
            candidateLength > maxAddress - terminator - dataAddress) {
            continue;
        }
        const uint64_t completeSize = sizeof(uint64_t) + candidateLength + terminator;
        bool payloadRegistered = candidateLength == 0 || header.size >= completeSize;
        bool terminatorRegistered = terminator == 0 || header.size >= completeSize;
        bool payloadWritable = candidateLength == 0;

        for (const auto& range : ownedRawRanges()) {
            if (range.owner != header.owner) continue;
            const uintptr_t rangeAddress = reinterpret_cast<uintptr_t>(range.address);
            if (rangeAddress == dataAddress && range.size >= candidateLength) {
                payloadRegistered = true;
                payloadWritable = payloadWritable || range.writable;
            }
            if (terminator != 0 && rangeAddress == dataAddress + candidateLength &&
                range.size >= terminator) {
                terminatorRegistered = true;
            }
        }

        if (!payloadRegistered || !terminatorRegistered) continue;
        if (terminator != 0 && static_cast<const char*>(address)[candidateLength] != '\0') {
            continue;
        }
        length = candidateLength;
        writable = type == "bytes" && payloadWritable;
        return true;
    }
    return false;
}
LValueSlot resolveLValue(void* address, const std::string& type, bool writing) {
    if (!address) throw std::runtime_error("@faddr received a null address");
    {
        std::lock_guard<std::mutex> lock(lvalueMutex());
        auto found = lvalueSlots().find(address);
        if (found != lvalueSlots().end() && found->second.abi != 2) {
            auto slot = found->second;
            if (!slot.type.empty() && slot.type != type)
                throw std::runtime_error("@faddr type '" + type + "' does not match slot type '" + slot.type + "'");
            if (writing && !slot.writable) throw std::runtime_error("@faddr cannot write a const slot");
            if (slot.abi == 3 && slot.shared.expired()) throw std::runtime_error("@faddr shared slot is no longer alive");
            return slot;
        }
    }
    uint64_t bufferLength = 0;
    bool bufferWritable = false;
    if (findRegisteredBuffer(address, type, bufferLength, bufferWritable)) {
        if (writing && !bufferWritable)
            throw std::runtime_error("@faddr cannot write readonly string storage");
        return LValueSlot{type, 4, bufferLength, bufferWritable, {}, address};
    }
    zeus_lvalue_require_raw(address, rawValueSize(type), writing ? 1 : 0);
    return LValueSlot{type, 2, rawValueSize(type), true, {}};
}
}
extern "C" void* zeus_lvalue_register(void* slot, const char* type, int32_t abi, uint64_t size, int32_t writable) noexcept(false) {
    if (!slot || abi < 0 || abi > 1 || !size) throw std::runtime_error("invalid lvalue slot registration");
    std::lock_guard<std::mutex> lock(lvalueMutex());
    auto& slots = lvalueSlots();
    auto& tokenByStorage = lvalueTokenByStorage();
    const auto existing = tokenByStorage.find(slot);
    if (existing != tokenByStorage.end()) {
        const auto registered = slots.find(existing->second);
        if (registered != slots.end() && registered->second.storage == slot &&
            registered->second.abi != 2) {
            registered->second.type = lvalueType(type);
            registered->second.abi = abi;
            registered->second.size = size;
            registered->second.writable = registered->second.writable && writable != 0;
            return existing->second;
        }
        tokenByStorage.erase(existing);
    }

    void* token = nextLValueToken();
    slots[token] = LValueSlot{lvalueType(type), abi, size, writable != 0, {}, slot};
    try {
        tokenByStorage.emplace(slot, token);
    } catch (...) {
        slots.erase(token);
        throw;
    }
    return token;
}
extern "C" void zeus_lvalue_register_raw(void* address, uint64_t size, int32_t writable) noexcept(false) {
    if (!address || !size) return;
    std::lock_guard<std::mutex> lock(lvalueMutex());
    lvalueSlots()[address] = LValueSlot{"", 2, size, writable != 0, {}};
}
extern "C" void zeus_lvalue_register_raw_owned(void* owner, void* address, uint64_t size, int32_t writable) noexcept(false) {
    const OwnedRawRange range{owner, address, size, writable != 0};
    registerOwnedRawRanges(owner, &range, 1);
}
extern "C" void zeus_lvalue_unregister_revealed(void* owner) noexcept {
    std::lock_guard<std::mutex> lock(lvalueMutex());
    unregisterOwnedRawRangesUnlocked(owner);
}
extern "C" void zeus_lvalue_unregister(void* slot) noexcept {
    std::lock_guard<std::mutex> lock(lvalueMutex());
    for (auto entry = lvalueSlots().begin(); entry != lvalueSlots().end();) {
        if (entry->first == slot || entry->second.storage == slot) {
            if (entry->second.storage) {
                const auto identity = lvalueTokenByStorage().find(entry->second.storage);
                if (identity != lvalueTokenByStorage().end() && identity->second == entry->first)
                    lvalueTokenByStorage().erase(identity);
            }
            entry = lvalueSlots().erase(entry);
        } else {
            ++entry;
        }
    }
    unregisterOwnedRawRangesUnlocked(slot);
}
extern "C" void zeus_try_enter() noexcept {
    ++tryDepth();
}
extern "C" void zeus_try_exit() noexcept {
    if (tryDepth() > 0) --tryDepth();
}
extern "C" void zeus_try_depth_reset() noexcept {
    tryDepth() = 0;
}
extern "C" void zeus_lvalue_require_raw(void* address, uint64_t size, int32_t writable) noexcept(false) {
    // Opt-in checks: outside a dynamically active `try` region the access is
    // intentionally unchecked and costs nothing beyond this early return.
    if (tryDepth() == 0) return;
    const auto target = reinterpret_cast<uintptr_t>(address);
    {
        std::lock_guard<std::mutex> lock(lvalueMutex());
        const auto exact = lvalueSlots().find(address);
        if (exact != lvalueSlots().end() && exact->second.abi != 2)
            throw std::runtime_error("raw @faddr cannot reinterpret a typed variable slot");
        for (const auto& entry : lvalueSlots()) {
            if (entry.second.abi != 2) continue;
            const auto base = reinterpret_cast<uintptr_t>(entry.first);
            if (target < base || target - base > entry.second.size) continue;
            if (size > entry.second.size - (target - base)) continue;
            if (writable && !entry.second.writable) throw std::runtime_error("raw @faddr cannot write readonly storage");
            return;
        }
        bool crossesOwnedRangeBoundary = false;
        for (const auto& range : ownedRawRanges()) {
            const auto base = reinterpret_cast<uintptr_t>(range.address);
            if (target >= base && target - base <= range.size &&
                size <= range.size - (target - base)) {
                if (writable && !range.writable)
                    throw std::runtime_error("raw @faddr cannot write readonly storage");
                return;
            }
            if (rangesOverlap(target, size, base, range.size)) {
                crossesOwnedRangeBoundary = true;
            }
        }
        if (crossesOwnedRangeBoundary)
            throw std::runtime_error("raw @faddr range crosses a registered storage boundary");
    }
    if (!zeus_memory_find_allocation(address, size))
        throw std::runtime_error("@faddr address has no live typed slot or tracked raw allocation");
}
extern "C" void zeus_lvalue_load(ZeusValue* out, void* address, const char* expected_type) noexcept(false) {
    const std::string type = lvalueType(expected_type);
    const auto slot = resolveLValue(address, type, false);
    void* storage = slot.storage ? slot.storage : address;
    if (slot.abi == 1) {
        const auto* value = static_cast<const ZeusValue*>(storage);
        zeus_value_validate_argument_type(value, "@faddr", 0, type.c_str());
        zeus_value_clone(out, value); return;
    }
    if (slot.abi == 3) {
        auto cell = slot.shared.lock();
        if (!cell) throw std::runtime_error("@faddr shared slot is no longer alive");
        ZeusBox snapshot; snapshot.assignVariant(cell->read());
        zeus_value_from_box(out, &snapshot);
        zeus_value_validate_argument_type(out, "@faddr", 0, type.c_str()); return;
    }
    if (slot.abi == 4) {
        if (type == "string") {
            zeus_value_from_native_string(out, static_cast<const char*>(storage));
        } else if (type == "bytes") {
            zeus_value_from_bytes(out, static_cast<const uint8_t*>(storage),
                                  static_cast<int64_t>(slot.size));
        } else {
            throw std::runtime_error("unsupported revealed buffer type '" + type + "'");
        }
        return;
    }
    if (type == "int") { int64_t value; std::memcpy(&value,storage,8); zeus_value_init_i64(out,value); }
    else if (type == "int32") { int32_t value; std::memcpy(&value,storage,4); zeus_value_init_i64(out,value); }
    else if (type == "float") { double value; std::memcpy(&value,storage,8); ZeusBox boxed; boxed.assignVariant(ZeusType::BigDecimal(value)); zeus_value_from_box(out,&boxed); }
    else if (type == "bool") { uint8_t value; std::memcpy(&value,storage,1); zeus_value_init_bool(out,value!=0); }
    else if (type == "string") {
        ZeusNativeString value = nullptr;
        std::memcpy(&value, storage, sizeof(value));
        zeus_value_from_native_string(out, value);
    } else if (type == "bytes") {
        ZeusNativeBytes data = nullptr;
        std::memcpy(&data, storage, sizeof(data));
        zeus_value_from_native_bytes(out, data);
    } else if (type == "&bytes") {
        ZeusType::RawBytesRange range{};
        std::memcpy(&range, storage, sizeof(range));
        ZeusBox boxed;
        boxed.assignVariant(range);
        zeus_value_from_box(out, &boxed);
    } else if (slot.abi == 0) {
        void* object; std::memcpy(&object,storage,sizeof(object));
        zeus_class_ref_require_live(object);
        zeus_value_from_class_ptr(out,object,type.c_str());
    } else throw std::runtime_error("unsupported @faddr native slot type '"+type+"'");
}
extern "C" void zeus_lvalue_store(void* address, const char* expected_type, const ZeusValue* value) noexcept(false) {
    const std::string type = lvalueType(expected_type);
    const auto slot = resolveLValue(address,type,true);
    void* storage = slot.storage ? slot.storage : address;
    zeus_value_validate_argument_type(value,"@faddr",0,type.c_str());
    if (slot.abi == 1) {
        ZeusValue replacement{};
        if(zeus_value_slot_is_gc_edge(static_cast<ZeusValue*>(storage))) zeus_value_clone_internal(&replacement,value);
        else zeus_value_clone(&replacement,value);
        zeus_lvalue_unregister_revealed(storage);
        zeus_value_release(static_cast<ZeusValue*>(storage));
        *static_cast<ZeusValue*>(storage)=replacement; return;
    }
    if (slot.abi == 3) {
        auto cell=slot.shared.lock(); if(!cell) throw std::runtime_error("@faddr shared slot is no longer alive");
        std::unique_ptr<ZeusBox,decltype(&zeus_box_release)> boxed(zeus_value_to_box(value),&zeus_box_release);
        zeus_lvalue_unregister_revealed(cell.get());
        cell->write(boxed->v); return;
    }
    if (slot.abi == 4) {
        if (type != "bytes")
            throw std::runtime_error("@faddr cannot replace a revealed string buffer");
        std::unique_ptr<const uint8_t, decltype(&zeus_bytes_release)> source(
            zeus_value_copy_bytes(value), &zeus_bytes_release);
        const int64_t sourceLength = zeus_bytes_len(source.get());
        if (sourceLength < 0 || static_cast<uint64_t>(sourceLength) != slot.size)
            throw std::runtime_error("@faddr bytes buffer write must match its current length");
        if (sourceLength > 0)
            std::memcpy(storage, source.get(), static_cast<size_t>(sourceLength));
        return;
    }
    if (slot.abi == 0) zeus_lvalue_unregister_revealed(storage);
    if(type=="int") { int64_t v=zeus_value_require_i64(value); std::memcpy(storage,&v,8); }
    else if(type=="int32") { int32_t v=static_cast<int32_t>(zeus_value_require_i64(value)); std::memcpy(storage,&v,4); }
    else if(type=="float") { std::unique_ptr<ZeusBox,decltype(&zeus_box_release)> boxed(zeus_value_to_box(value),&zeus_box_release); ZeusType::BigDecimal decimal; if(!ZeusVMUtils::extractNumber(boxed->v,decimal)) throw std::runtime_error("expected float slot value"); double v=decimal.toDouble(); std::memcpy(storage,&v,8); }
    else if(type=="bool") { uint8_t v=zeus_value_require_bool(value)!=0; std::memcpy(storage,&v,1); }
    else if(type=="string") {
        const ZeusNativeString nativeString = zeus_value_intern_string(value);
        std::memcpy(storage, &nativeString, sizeof(nativeString));
        zeus_global_sync_native_string_slot(storage);
    } else if(type=="bytes") {
        if (slot.abi == 2) {
            throw std::runtime_error("@faddr bytes assignment requires a typed native slot");
        }
        const ZeusNativeBytes replacement = zeus_value_copy_bytes(value);
        ZeusNativeBytes previous = nullptr;
        std::memcpy(&previous, storage, sizeof(previous));
        std::memcpy(storage, &replacement, sizeof(replacement));
        if (previous) zeus_bytes_release(previous);
        zeus_global_sync_native_bytes_slot(storage);
    } else if(type=="&bytes") {
        ZeusRawBytesRange range{};
        zeus_value_require_raw_bytes_range(&range, value);
        std::memcpy(storage, &range, sizeof(range));
    } else if(slot.abi==0) {
        void* object=zeus_value_require_class_ptr(value,type.c_str());
        zeus_class_ref_retain(object);
        void* previous; std::memcpy(&previous,storage,sizeof(previous));
        std::memcpy(storage,&object,sizeof(object));
        zeus_class_ref_release(previous);
    } else throw std::runtime_error("unsupported @faddr store type '"+type+"'");
}
extern "C" void zeus_lvalue_register_native_buffer(
    void* owner, void* address, const char* native_type
) noexcept(false) {
    const std::string type = lvalueType(native_type);
    if (type == "string") registerRevealedString(address);
    else if (type == "bytes") {
        // The native data pointer owns its buffer; key raw access by that stable
        // allocation so a temporary argument slot cannot shorten its lifetime.
        registerNativeBytesData(address);
    }
    else throw std::runtime_error("native raw view registration requires string or bytes");
}
extern "C" int64_t zeus_string_data_address(ZeusNativeString value) noexcept(false) {
    zeus_native_string_validate(value);
    uint64_t length = 0;
    std::memcpy(&length, value - sizeof(uint64_t), sizeof(length));
    const uintptr_t characterAddress = reinterpret_cast<uintptr_t>(value);
    if (length > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        length >= std::numeric_limits<uintptr_t>::max() - characterAddress) {
        throw std::runtime_error("native string data address has an invalid length");
    }
    auto* characters = const_cast<char*>(value);
    if (characters[length] != '\0')
        throw std::runtime_error("native string is missing its terminator");
    OwnedRawRange ranges[3] = {
        {const_cast<char*>(value), const_cast<char*>(value - sizeof(uint64_t)),
         sizeof(uint64_t), false},
        {const_cast<char*>(value), characters, length, false},
        {const_cast<char*>(value), characters + length, 1, false}
    };
    const size_t count = length == 0 ? 2 : 3;
    if (length == 0) ranges[1] = ranges[2];
    registerOwnedRawRanges(const_cast<char*>(value), ranges, count);
    return static_cast<int64_t>(characterAddress);
}
extern "C" int64_t zeus_bytes_data_address(ZeusNativeBytes value) noexcept(false) {
    registerNativeBytesData(const_cast<uint8_t*>(value));
    return static_cast<int64_t>(reinterpret_cast<uintptr_t>(value));
}
extern "C" void zeus_lvalue_register_revealed(void* owner, const ZeusValue* value, void* address) noexcept(false) {
    if (!owner || !value || !address)
        throw std::runtime_error("invalid revealed lvalue registration");
    if (value->tag == ZEUS_VALUE_TAG_STRING &&
        value->flags == ZEUS_VALUE_FLAG_OWNS_BUFFER) {
        uint64_t length = 0;
        const auto* data = static_cast<const uint8_t*>(
            zeus_value_native_address(value, 1));
        std::memcpy(&length, data - sizeof(uint64_t), sizeof(length));
        registerDynamicRevealedString(owner, address, length);
        return;
    }
    if (value->tag == ZEUS_VALUE_TAG_BYTES &&
        value->flags == ZEUS_VALUE_FLAG_OWNS_BUFFER) {
        uint64_t length = 0;
        const auto* data = static_cast<const uint8_t*>(
            zeus_value_native_address(value, 1));
        std::memcpy(&length, data - sizeof(uint64_t), sizeof(length));
        registerDynamicRevealedBytes(owner, address, length);
        return;
    }
    std::unique_ptr<ZeusBox,decltype(&zeus_box_release)> box(zeus_value_to_box(value),&zeus_box_release);
    uint64_t size=8;
    if(std::holds_alternative<ZeusType::Bytes>(box->v)) {
        throw std::runtime_error("Bytes values must use their direct ZeusValue buffer tag");
    }
    else if(std::holds_alternative<bool>(box->v)) size=1;
    else if(std::holds_alternative<ZeusType::LLVMClassHandle>(box->v))
        throw std::runtime_error("raw revealed class access requires an explicit layout span");
    zeus_lvalue_register_raw_owned(owner,address,size,0);
}
extern "C" void* zeus_shared_value_slot_address(const char* name,const char* type,int32_t writable) noexcept(false) {
    auto cell=ZeusShared::getGlobalHeap().find(name);
    if(!cell) throw std::runtime_error("undefined shared slot");
    void* address=nextLValueToken();
    std::lock_guard<std::mutex> lock(lvalueMutex());
    lvalueSlots()[address]=LValueSlot{lvalueType(type),3,sizeof(void*),writable!=0,cell,nullptr};
    return address;
}

extern "C" void zeus_lvalue_unregister_span(void* address,uint64_t size) noexcept {
    const auto base=reinterpret_cast<uintptr_t>(address);
    std::lock_guard<std::mutex> lock(lvalueMutex());
    for(auto it=lvalueSlots().begin();it!=lvalueSlots().end();) {
        const auto target=reinterpret_cast<uintptr_t>(it->first);
        const auto storage = reinterpret_cast<uintptr_t>(it->second.storage);
        const bool tokenInSpan = target >= base && target - base < size;
        const bool storageInSpan = it->second.storage &&
            storage >= base && storage - base < size;
        if(tokenInSpan || storageInSpan) it=lvalueSlots().erase(it); else ++it;
    }
    auto& ranges=ownedRawRanges();
    ranges.erase(std::remove_if(ranges.begin(),ranges.end(),[&](const OwnedRawRange& range) {
        const auto target=reinterpret_cast<uintptr_t>(range.address);
        const auto owner=reinterpret_cast<uintptr_t>(range.owner);
        return rangeStartsInside(base,size,target) || rangeStartsInside(base,size,owner) ||
            rangesOverlap(base,size,target,range.size);
    }),ranges.end());
}
