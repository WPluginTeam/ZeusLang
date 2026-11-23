// ZeusLLVM/ZeusEmitter.hpp
//
// Emitter v0：主程序单编 IR → LLVM IR 文本（naive 栈机直译）。
//
// 输入：ZeusCompile::compile() 的主程序结果（无包合并、无解释器参与）。
//   v0 是 freestanding 子集：不支持 import / 类。带原生签名标注的顶层函数。
//   会直接生成 LLVM function；闭包、捕获变量和包初始化仍不在处理范围内。
//
// 约定：
// - 所有 top-level chunk 按序拼进同一个 zeus_entry（与 ZeusCompile::run 的
//   执行顺序一致）；FUNC_BEGIN chunk 由签名标注生成原生 LLVM function。
//   对应的 MAKE_FUNCTION/STORE 只作为动态闭包兼容痕迹被跳过。
// - The native path keeps proven scalar values in LLVM registers.
//   Dynamic values use the C-compatible ZeusValue layout and value-prefixed ABI.
//   Runtime adapters bridge into legacy ZeusBox-based VM operations when required.
// - Dynamic variadic calls use a shared argc + ZeusValue*[] + result-out ABI.
// - COUNT_CHECK_* use the runtime range-check ABI and preserve the VM checks.

#pragma once

#include <cstdint>
#include <cstdio>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../ZeusIR/IRType.hpp"
#include "../ZeusRunner/ZeusBuiltins.hpp"
#include "../ZeusBuiltins/BuiltinNativeABI.hpp"
#include "../ZeusBuiltins/NativeTypeABI.hpp"
#include "../ZeusCompile.hpp"
#include "ZeusMCURuntime.hpp"
#include "ZeusDynamicABI.hpp"
#include "ZeusDynamicBinaryABI.hpp"
#include "ZeusDynamicMemberBridge.hpp"
#include "ZeusDynamicFieldBridge.hpp"
#include "ZeusContainerABI.hpp"
#include "ZeusIntegerABI.hpp"
#include "ZeusFloatingPointABI.hpp"
#include "../ZeusRuntimeLib/ZeusRangeCheckABI.hpp"

namespace ZeusLLVM {

struct EmitResult {
    bool ok = false;
    std::string text;
    std::string error;
    int errorLine = 0;
    std::vector<std::string> warnings;
};

namespace detail {

inline constexpr char NativeBytesCleanupPrefix[] = "__zeus_native_bytes_cleanup__:";
inline constexpr char NativeStringCleanupPrefix[] = "__zeus_native_string_cleanup__:";

inline std::string nativeBytesCleanupToken(const std::string& value) {
    return std::string(NativeBytesCleanupPrefix) + value;
}

inline bool isNativeBytesCleanupToken(const std::string& value) {
    return value.rfind(NativeBytesCleanupPrefix, 0) == 0;
}

inline std::string nativeBytesCleanupValue(const std::string& value) {
    return isNativeBytesCleanupToken(value)
        ? value.substr(sizeof(NativeBytesCleanupPrefix) - 1)
        : value;
}

inline std::string nativeStringCleanupToken(const std::string& value) {
    return std::string(NativeStringCleanupPrefix) + value;
}

inline bool isNativeStringCleanupToken(const std::string& value) {
    return value.rfind(NativeStringCleanupPrefix, 0) == 0;
}

inline std::string nativeStringCleanupValue(const std::string& value) {
    return isNativeStringCleanupToken(value)
        ? value.substr(sizeof(NativeStringCleanupPrefix) - 1)
        : value;
}

inline std::string escapeLLVMCommentText(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        if (ch == '\n') {
            escaped += "\\n";
        } else if (ch == '\r') {
            escaped += "\\r";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

inline std::string callArgumentCleanup(const std::string& value) {
    if (isNativeBytesCleanupToken(value)) {
        return "call void @zeus_bytes_release(ptr " + nativeBytesCleanupValue(value) + ")";
    }
    if (isNativeStringCleanupToken(value)) {
        return "call void @zeus_string_release(ptr " + nativeStringCleanupValue(value) + ")";
    }
    return "call void @zeus_value_release(ptr " + value + ")";
}

inline bool tryArgInt(const IRType::IRInstr& ins, size_t pos, int64_t& out) {
    if (pos >= ins.args.size()) return false;

    const auto& a = ins.args[pos];

    if (!std::holds_alternative<LexerType::Value>(a)) return false;

    const auto& v = std::get<LexerType::Value>(a);

    if (!std::holds_alternative<ZeusType::BigInteger>(v.data)) return false;

    return std::get<ZeusType::BigInteger>(v.data).tryToInt64(out);
}

inline bool isNativeBytesDataAddressReceiver(
    const std::vector<IRType::IRInstr>& chunk,
    size_t pc
) {
    for (size_t next = pc + 1; next < chunk.size(); ++next) {
        const auto& instruction = chunk[next];
        if (instruction.op == VMOperator::IR_ANNOTATION) continue;
        if (instruction.op != VMOperator::OBJ_CALL_METHOD ||
            instruction.args.size() < 2 ||
            !std::holds_alternative<std::string>(instruction.args[0]) ||
            std::get<std::string>(instruction.args[0]) != "dataAddress") {
            return false;
        }
        int64_t argumentCount = -1;
        return tryArgInt(instruction, 1, argumentCount) && argumentCount == 0;
    }
    return false;
}

inline bool isReadOnlyBytesMethodName(const std::string& methodName) {
    return methodName == "dataAddress" || methodName == "get" ||
        methodName == "length" || methodName == "slice" ||
        methodName == "operator[[]]";
}

inline std::string dynamicVariadicABISymbol(const std::string& name) {
    if (name == "print" || name == "delayPrint") {
        return "@zeus_value_print_call";
    }
    if (ZeusBuiltins::hasStandaloneValueCallABI(name)) {
        return "@zeus_value_native_call";
    }
    return {};
}

// LLVM c"..." 转义（不可见字符 " \ 用 \xx）
inline std::string escapeLLString(const std::string& s) {
    std::string o;

    for (unsigned char c : s) {
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            o += (char)c;
        } else {
            char buf[5];
            snprintf(buf, sizeof(buf), "\\%02X", (unsigned)c);
            o += buf;
        }
    }

    return o;
}

struct FunctionSignature {
    std::string name;
    std::string returnType;
    std::vector<std::string> parameterTypes;
    std::vector<std::string> parameterNames;
    std::vector<bool> parameterIsReference;
    bool hasVariadicArgs = false;
    bool hasVariadicKwargs = false;
    int minRequiredParams = 0;
    std::vector<bool> defaultParameterMask;
    std::string ownerType;
    std::string operatorSymbol;
    std::string nativeSymbol;
    std::string nativeReferenceABI = "raw";
    std::string nativeLibraryPath;
    // `@native(addr) decl func`: parameter 0 is the callee address.
    bool nativeIndirect = false;
    std::string genericOrigin;
    int captureCount = 0;
    size_t chunkIndex = 0;
    bool isExternal = false;
    bool isStaticMethod = false;
    bool isInline = false;
    bool isNoSafepoint = false;
    bool isGenericTemplate = false;

    bool hasDefaultParameters() const {
        return std::any_of(
            defaultParameterMask.begin(), defaultParameterMask.end(),
            [](bool hasDefault) { return hasDefault; }
        );
    }
};

inline bool hasCompleteFunctionAnnotations(const FunctionSignature& signature) {
    if (signature.name.empty() || signature.returnType.empty()) return false;
    for (const auto& typeName : signature.parameterTypes) {
        if (typeName.empty()) return false;
    }
    return true;
}

struct ClassLayout {
    std::vector<std::string> fieldNames;
    std::vector<std::string> fieldTypes;
    std::vector<bool> fieldTypeExplicit;
    std::vector<bool> fieldConstness;
    std::vector<IRType::ClassFieldDefault> fieldDefaults;
    std::vector<std::string> fieldInitializerNames;
    // LLVM-only slots for fields first assigned after construction. Each slot
    // has a per-instance initialized bit so reading an absent field still fails.
    std::map<std::string, size_t> inferredFieldFlags;
};

class Emitter {
public:
    struct NativeValue {
        std::string value;
        std::string type;
        bool functionRef = false;
        bool dynamic = false;
        std::string sourceType;
        bool ownsDynamic = false;
        std::string namedArgument;
        bool ownsClass = false;
        bool isReference = false;
        // Set by @addr for the physical layout of the addressed variable.
        // Native slots are read/written as their LLVM type; ZeusValue slots
        // use the value-slot ABI to preserve tags and ownership.
        std::string addressStorageABI;
        // Native Bytes owns an independent [length][data] allocation.
        bool ownsNativeBytes = false;
        // Native Strings share immutable storage through explicit references.
        bool ownsNativeString = false;
    };

    struct NativeSlot {
        std::string address;
        std::string type;
        bool dynamic = false;
        std::string sourceType;
        // Physical layout of this slot (used when its own address is taken).
        std::string addressStorageABI;
        // Provenance of an int value held in this slot, if it came from @addr.
        std::string valueAddressStorageABI;
        // Addressable Bytes parameters normally own a cloned value. A few
        // native receiver views, such as bytes.dataAddress(), borrow instead.
        bool ownsNativeBytes = false;
        // Addressable native String slots own one immutable-string reference.
        bool ownsNativeString = false;
        // Tracks whether this slot was registered with the active lvalue table
        // (e.g. via @addr). Pure scalar locals never get registered and must
        // never generate unnecessary unregister cleanup calls.
        bool needsLValueUnregister = false;
    };

    static std::string rangeCheckCall(
        const NativeValue& value,
        const IRType::IRInstr& instruction
    ) {
        const int32_t checkKind = instruction.op == VMOperator::COUNT_CHECK_NEG
            ? ZEUS_RANGE_CHECK_NEGATIVE_END
            : ZEUS_RANGE_CHECK_ZERO_STEP;
        const int32_t sourceLine = instruction.line > 0 ? instruction.line : 0;

        if (value.dynamic) {
            return "call void @zeus_range_check_value(ptr " + value.value +
                ", i32 " + std::to_string(checkKind) + ", i32 " +
                std::to_string(sourceLine) + ")";
        }
        if (value.type == "i64") {
            return "call void @zeus_range_check_i64(i64 " + value.value +
                ", i32 " + std::to_string(checkKind) + ", i32 " +
                std::to_string(sourceLine) + ")";
        }

        // VM range checks only inspect BigInteger values. Other statically
        // represented Zeus types pass through unchanged, matching that rule.
        return {};
    }

    // Removes every alloca from the part of a function body that starts at
    // bodyStart and returns them separately so the caller can place them at the
    // top of that body. At -O0 an alloca inside a loop grows the frame on each
    // iteration, so a loop that boxes values would exhaust the stack. Hoisting is
    // safe because these temporaries are always re-initialized before they are
    // read.
    static std::string takeAllocas(const std::string& text, size_t bodyStart,
                                   std::string& allocas) {
        std::string kept;
        kept.reserve(text.size() - bodyStart);
        size_t cursor = bodyStart;
        while (cursor < text.size()) {
            const size_t newline = text.find('\n', cursor);
            const size_t end = newline == std::string::npos ? text.size() : newline + 1;
            const std::string line = text.substr(cursor, end - cursor);
            if (line.find(" = alloca ") != std::string::npos) {
                allocas += line;
            } else {
                kept += line;
            }
            if (newline == std::string::npos) break;
            cursor = end;
        }
        return kept;
    }

    EmitResult result;
    std::string body;
    struct EntryExceptionPad {
        std::string switchLabel;
        std::string padLabel;
        std::string resumeLabel;
        std::string valueSlot;
        size_t catchChunk;
        size_t catchPc;
        bool cleanupOnly = false;
        std::vector<std::string> cleanupCalls;
        bool emit = true;
    };
    std::vector<EntryExceptionPad> entryExceptionPads;
    // Owned NativeString temporaries produced in the top-level entry chunk.
    // Entry catch pads release only owners created inside the matching try;
    // owners that existed before TRY_BEGIN remain available to the catch body.
    std::vector<std::string> entryNativeStringOwners;
    std::map<std::string, uint64_t> entryNativeStringOwnerGenerations;
    uint64_t entryNativeStringOwnerGeneration = 0;
    std::map<std::pair<size_t, size_t>, uint64_t>
        entryNativeStringTryBaselines;
    std::set<int> entryDynamicValueGlobals;
    std::map<std::vector<int>, std::string> entryGlobalCleanupHelpers;
    // On-demand emit: program-level usage flags, computed once per program
    // by collectProgramUsageFlags() before reachability and emission. Every
    // gate defaults to the old unconditional behavior; a flag only ever
    // *removes* emission when the program provably has no use for it.
    bool programUsesSharedValues = false;
    bool programCallsPrintOrStr = false;
    std::set<std::string> constructedClassNames;
    bool programHasDynamicClassConstruction = false;
    // See EmitterLiveness.inl: a program that cannot destroy class objects while
    // they are referenced verifies each class parameter once, in the prologue.
    bool elideParameterLivenessChecks = false;
    std::set<std::string> verifiedLiveParameters;
    std::set<std::string> blockVerifiedLiveValues;
    // A class needs registration, copy helpers, a finalizer and field
    // tracing iff it may have live instances at runtime: statically
    // constructed, dynamically constructed, returned from a native factory,
    // held in an embedded native slot, or one of the two error classes when
    // the runtime error factory is emitted (it builds those instances
    // itself). Every site that references class machinery must agree on
    // this set; otherwise the linker sees a dangling reference.
    bool classNeedsRuntimeMachinery(const std::string& className) const {
        if (programHasDynamicClassConstruction) return true;
        if (constructedClassNames.find(className) != constructedClassNames.end()) {
            return true;
        }
        if (embeddedSlotTypes.find(className) != embeddedSlotTypes.end()) {
            return true;
        }
        if (needsRuntimeErrorFactory &&
            (className == "RuntimeError" || className == "TypeError")) {
            return true;
        }
        return false;
    }
    std::map<std::pair<size_t, size_t>, std::string> entryExceptionSlots;
    int64_t activeEntryExceptionTarget = -1;
    size_t activeEntryChunk = 0;
    bool entryHasTry = false;
    // Storage for native slots has to dominate every access, and at -O0 LLVM
    // does not hoist allocas out of loops. Slot allocas therefore go to the
    // entry block instead of the block that first mentions the slot.
    std::string entryPrologue;

    // The prologue is built while the body is emitted, so it is only available
    // once every top-level chunk is in. takeEntryAllocas hands it back and
    // clears it, which keeps a second call from duplicating the allocas.
    std::string takeEntryAllocas() {
        std::string result;
        result.swap(entryPrologue);
        return result;
    }
    std::map<std::string, std::string> strPool; // 内容 -> @.str.N
    int strCount = 0;
    long tmpCount = 0;

    std::vector<std::string> globalNames;
    const std::vector<std::vector<IRType::IRInstr>>* programChunks = nullptr;
    std::map<size_t, FunctionSignature> functionSignaturesByChunk;
    std::map<std::string, FunctionSignature> functionSignatures;
    // Type implementation static methods: "TypeName::method" -> signature name
    std::map<std::string, std::string> typeImplStaticSignatures;
    std::map<std::string, size_t> internalFunctionSymbolCounts;
    std::map<std::string, std::string> dynamicNativeImportThunkSymbols;
    std::map<std::string, std::string> externalDeclarationByFunction;
    std::set<size_t> reachableFunctionChunks;
    std::set<std::string> reachableExternalFunctions;
    std::set<std::string> functionValueReferences;
    bool keepAllExternalDeclarations = false;
    std::set<std::string> dynamicMemberMethodNames;
    std::map<int, std::string> functionGlobals;
    std::map<int, std::string> classGlobals;
    std::map<int, std::string> namespacePackagesByGlobal;
    std::map<std::string, std::set<std::string>> namespaceExportsByPackage;
    std::map<int, NativeValue> nativeGlobals;
    std::map<int, NativeValue> nativeLocals;
    std::map<int, NativeSlot> nativeGlobalSlots;
    std::map<int, NativeSlot> nativeLocalSlots;
    // Class name a global holds: recorded by prescanGlobalTypes from the
    // initializer and refreshed by the top-level path as it writes. Storage for
    // globals lives in the runtime slot table so top-level statements and native
    // functions agree; this only keeps the type information that delete needs.
    std::map<int, std::string> nativeGlobalClassTypes;
    // Scalar LLVM type ("i64" / "i1") last written into a global, so reads can
    // use the unboxed global accessors instead of materializing a ZeusValue.
    std::map<int, std::string> nativeGlobalScalarTypes;
    // Statically proven immutable-view globals (string/bytes), for native LLVM
    // operations that need the source value's ABI representation.
    std::map<int, std::string> nativeGlobalDeclaredIntegerTypes;
    std::map<int, std::string> nativeGlobalDeclaredDoubleTypes;
    std::map<int, std::string> prescanGlobalNativeViewTypes;
    // Globals whose storage address can escape must always be read and written
    // through the runtime slot. This is established by prescan so control-flow
    // paths agree even when only one branch passes the address to native code.
    std::set<int> prescanAddressExposedGlobals;
    // `@native func` 定义的函数名：它们是 C 链接导出函数，调用点要按 C 的
    // void ABI 处理（不传 ZeusValue 出参槽）。改名后的符号名存在签名里，
    // 而调用点解析到的签名副本不一定带符号，所以按 Zeus 函数名记录一份。
    std::set<std::string> nativeExportFunctions;
    std::set<std::string> nativeExportSymbols;
    bool mcuTarget = false;

    bool isMCUMode() const {
        return ZeusMCURuntime::shouldEnable(mcuTarget, nativeExportSymbols);
    }

    // Static instance type of a global, from its initializer. Kept apart from
    // the map above because the top-level path rewrites that one as it stores,
    // while this one answers which values need class-pointer access.
    std::map<int, std::string> prescanGlobalClassTypes;
    // Built-in containers remain boxed values, but knowing their language type
    // lets reachability avoid pulling in unrelated dynamic subscript methods.
    std::map<int, std::string> prescanGlobalReachabilityTypes;

    // Records what each global slot holds, so both the top-level path and native
    // functions can pick the cheapest correct access for it. Must run after
    // collectMetadata, which is what fills classLayouts and classGlobals.
    std::set<std::string> structTypes;
    std::map<std::string, ClassLayout> classLayouts;
    // Class definitions stay in classLayouts so AOT class-handle IDs remain
    // stable, while this set tracks which layout bodies need LLVM emission.
    std::set<std::string> reachableClassLayouts;
    std::set<std::string> embeddedSlotTypes;
    std::string nativeFunctions;
    std::map<std::string, std::string> boundMethodAdapterSymbols;
    std::string boundMethodAdapters;
    bool needsRuntimeErrorFactory = false;
    std::string externalDeclarations;
    DynamicMemberBridge::GeneratedIR dynamicMemberBridge;
    DynamicFieldBridge::GeneratedIR dynamicFieldBridge;
    std::set<std::string> dynamicFieldLoads;
    std::set<std::string> dynamicFieldStores;

    struct ClosureCaptureDescriptor {
        std::string sourceKind;
        int sourceIndex = -1;
        int destinationIndex = -1;
        std::string name;
        bool byReference = false;
    };

    // 动态变长函数待决状态（LOAD 与 CALL 之间的参数求值期保持）。
    std::string pendingVariadicCall;
    std::vector<std::string> pendingVariadicCallStack;
    std::string pendingDirectCall;
    std::string pendingFunctionLoad;

    // 跨 chunk 持续的块状态：上一操作是否终结了块 / 下一条指令是否必须起新块。
    // blkTerminated 初始为 true：entry 块的 br %L0_0 在它之前，逻辑上已终结。
    bool blkTerminated = true;
    bool blkNeedLabel = false;
    bool entryLabelEmitted = false;

    #include "ZeusClassCopy.hpp"
    #include "ZeusClassReachability.hpp"
    #include "ZeusLValueAddress.hpp"


    #include "Emitter/EmitterPrescan.inl"
#include "Emitter/EmitterLiveness.inl"
#include "Emitter/EmitterBytesFastPath.inl"
    #include "Emitter/EmitterMetadata.inl"
    #include "Emitter/EmitterReachability.inl"
    #include "Emitter/EmitterValues.inl"
    #include "Emitter/EmitterClassAndMemory.inl"
    #include "Emitter/EmitterFunction.inl"
}; // class Emitter

#include "Emitter/EmitterPostPass.inl"

} // namespace ZeusLLVM
