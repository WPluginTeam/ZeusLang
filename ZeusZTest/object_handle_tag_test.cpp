// Verifies the ZEUS_VALUE_TAG_OBJECT contract: a class instance handle packs
// into the 16-byte value, round trips {id, generation}, and is non-owning.
#include "../ZeusRuntimeLib/ZeusABI.hpp"
#include "../ZeusRuntimeLib/ZeusABIInternal.hpp"
#include "../ZeusRuntimeLib/ZeusValueSlot.hpp"
#include "../ZeusRuntimeLib/ZeusType.hpp"
#include "../ZeusRuntimeLib/ZeusVMUtils.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

int failures = 0;

// A rejected value fails fast: the panic cannot unwind out of the extern "C"
// boundary, so the process dies with a fast-fail code instead of throwing. Such
// cases therefore run in a child process and only the exit status is checked.
constexpr int32_t kCaseForgedOwnershipFlag = 1;
constexpr int32_t kCaseRawAddressRequest = 2;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    std::fflush(stdout);
    if (!condition) ++failures;
}

// Runs one panic case in this same executable and reports whether it refused
// the input instead of accepting it.
void checkRefusedInChild(int32_t caseId, const char* what) {
    const char* self = std::getenv("ZEUS_OBJECT_TAG_SELF");
    if (!self) {
        check(false, what);
        std::printf("     ZEUS_OBJECT_TAG_SELF is not set\n");
        return;
    }
    char command[1024];
    std::snprintf(command, sizeof(command), "\"%s\" --case %d", self, caseId);
    const int status = std::system(command);
    check(status != 0, what);
}

// Builds the value a VM object handle would produce. The public ABI has no
// minting entry point by design, so the test assembles one locally.
ZeusValue makeHandle(int32_t id, int32_t generation) {
    const ZeusObjectHandle handle{
        static_cast<uint32_t>(id), static_cast<uint32_t>(generation)};
    ZeusValue value{};
    value.tag = ZEUS_VALUE_TAG_OBJECT;
    value.flags = 0;
    std::memcpy(&value.payload, &handle, sizeof(handle));
    return value;
}

// Each refusal case, run as the child process of the parent run.
int runCase(int32_t caseId) {
    ZeusValue value = makeHandle(1, 0);
    if (caseId == kCaseForgedOwnershipFlag) {
        // An ownership flag on a handle must be refused: releasing it as an
        // owned box would free memory the handle never owned.
        value.flags = ZEUS_VALUE_FLAG_OWNS_BOX;
        zeus_value_release(&value);
    } else if (caseId == kCaseRawAddressRequest) {
        // A handle names a slot, so there is no storage address to reveal.
        (void)zeus_value_native_address(&value, 1);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--case") == 0) {
        return runCase(std::atoi(argv[2]));
    }

    check(ZEUS_VALUE_TAG_OBJECT == 11,
        "tag 11 is appended after the existing tags");
    check(sizeof(ZeusObjectHandle) == sizeof(uint64_t),
        "handle payload stays 8 bytes");
    check(sizeof(ZeusValue) == 16,
        "ZeusValue layout is unchanged at 16 bytes");

    // 1. The handle keeps both halves across the ABI boundary.
    ZeusValue value = makeHandle(42, 7);
    check(value.flags == 0, "a handle carries no ownership flag");
    const ZeusObjectHandle read = zeus_value_require_object_handle(&value);
    check(read.id == 42u && read.generation == 7u,
        "{id, generation} round trips exactly");

    // 2. Release validates but never frees: the instance belongs to the
    //    reference count, so releasing the handle must leave it alone.
    zeus_value_release(&value);
    check(value.tag == ZEUS_VALUE_TAG_NIL && value.payload == 0,
        "release clears the handle without touching the instance");

    // 3. Clone copies the name, never an owning copy.
    ZeusValue source = makeHandle(5, 3);
    ZeusValue clone{};
    zeus_value_clone(&clone, &source);
    const ZeusObjectHandle cloned = zeus_value_require_object_handle(&clone);
    check(clone.flags == 0 && cloned.id == 5u && cloned.generation == 3u,
        "clone stays non-owning and preserves the handle");
    zeus_value_release(&source);
    zeus_value_release(&clone);

    // 4. Refusals run out of process: the panic cannot unwind across the
    //    extern "C" boundary, so the parent only sees the exit status.
    checkRefusedInChild(kCaseForgedOwnershipFlag,
        "an ownership flag on a handle is refused");
    checkRefusedInChild(kCaseRawAddressRequest,
        "raw address access on a handle is refused");

    // 6. A cell round trips the handle back to a VM handle, with the decode
    //    side needing no VM state.
    {
        ZeusValueSlot slot;
        slot.write(ZeusType::UniversalVariant(
            ZeusType::ObjectHandle{ 1234, 56 }));
        const ZeusType::UniversalVariant decoded = slot.read();
        const auto* handle = std::get_if<ZeusType::ObjectHandle>(&decoded);
        check(handle != nullptr && handle->id == 1234 &&
                  handle->generation == 56,
            "a cell stores and restores the handle as an ObjectHandle");
    }

    // 7. Slot lookup must compare the generation before touching a reused ID.
    {
        std::vector<ZeusType::ObjectInstance> objectTable(1);
        auto& slot = objectTable[0];
        slot.classId = 0;
        slot.generation = 8;
        const ZeusType::ObjectHandle currentHandle{ 0, 8 };
        const ZeusType::ObjectHandle staleHandle{ 0, 7 };
        check(ZeusVMUtils::getValidObject(objectTable, currentHandle) == &slot,
            "a current handle resolves its object slot");
        check(ZeusVMUtils::getValidObject(objectTable, staleHandle) == nullptr,
            "a reused slot rejects an older generation");
        slot.markedForDeletion = true;
        check(ZeusVMUtils::getValidObject(objectTable, currentHandle) == nullptr,
            "a deleted slot rejects its current handle");
    }

    if (failures == 0) {
        std::printf("OBJECT_HANDLE_TAG_TEST_PASS\n");
        return 0;
    }
    std::printf("OBJECT_HANDLE_TAG_TEST_FAIL %d\n", failures);
    return 1;
}