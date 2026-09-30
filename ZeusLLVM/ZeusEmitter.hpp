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
#include "ZeusDynamicABI.hpp"
#include "ZeusDynamicBinaryABI.hpp"
#include "ZeusDynamicMemberBridge.hpp"
#include "ZeusContainerABI.hpp"
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

inline bool tryArgInt(const IRType::IRInstr& ins, size_t pos, int64_t& out) {
    if (pos >= ins.args.size()) return false;

    const auto& a = ins.args[pos];

    if (!std::holds_alternative<LexerType::Value>(a)) return false;

    const auto& v = std::get<LexerType::Value>(a);

    if (!std::holds_alternative<ZeusType::BigInteger>(v.data)) return false;

    return std::get<ZeusType::BigInteger>(v.data).tryToInt64(out);
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
    bool hasVariadicArgs = false;
    bool hasVariadicKwargs = false;
    int minRequiredParams = 0;
    std::vector<bool> defaultParameterMask;
    std::string ownerType;
    std::string operatorSymbol;
    std::string nativeSymbol;
    int captureCount = 0;
    size_t chunkIndex = 0;
    bool isExternal = false;
    bool isStaticMethod = false;

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
    std::vector<bool> fieldConstness;
    std::vector<IRType::ClassFieldDefault> fieldDefaults;
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
    };

    struct NativeSlot {
        std::string address;
        std::string type;
        bool dynamic = false;
        std::string sourceType;
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
    };
    std::vector<EntryExceptionPad> entryExceptionPads;
    std::set<int> entryDynamicValueGlobals;
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
    std::map<std::string, size_t> internalFunctionSymbolCounts;
    std::set<size_t> reachableFunctionChunks;
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

    std::vector<std::string> entryExceptionCleanupCalls() {
        std::vector<std::string> calls;
        for (int index : entryDynamicValueGlobals) {
            calls.push_back("call void @zeus_global_clear(i32 " +
                            std::to_string(index) + ")");
        }
        auto appendSlots = [&](const std::map<int, NativeSlot>& slots) {
            for (const auto& entry : slots) {
                if (entry.second.type != "ptr" ||
                    !isClassType(entry.second.sourceType)) {
                    continue;
                }
                const std::string value = freshTmp();
                calls.push_back(value + " = load ptr, ptr " + entry.second.address);
                calls.push_back("call void @zeus_class_ref_release(ptr " + value + ")");
            }
        };
        appendSlots(nativeGlobalSlots);
        appendSlots(nativeLocalSlots);
        auto appendValues = [&](const std::map<int, NativeValue>& values) {
            for (const auto& entry : values) {
                const NativeValue& value = entry.second;
                if (value.dynamic && value.ownsDynamic) {
                    calls.push_back("call void @zeus_value_release(ptr " +
                                    value.value + ")");
                } else if (value.ownsClass && isManagedClassValue(value)) {
                    calls.push_back("call void @zeus_class_ref_release(ptr " +
                                    value.value + ")");
                }
            }
        };
        appendValues(nativeGlobals);
        appendValues(nativeLocals);
        return calls;
    }
    // Static class type of a global, from its initializer. Kept apart from the
    // map above because the top-level path rewrites that one as it stores, while
    // this one answers "what type does the program declare here" for native
    // functions that need a class pointer instead of a boxed value.
    std::map<int, std::string> prescanGlobalClassTypes;

    // Records what each global slot holds, so both the top-level path and native
    // functions can pick the cheapest correct access for it. Must run after
    // collectMetadata, which is what fills classLayouts and classGlobals.
    void prescanGlobalTypes() {
        nativeGlobalScalarTypes.clear();
        prescanGlobalClassTypes.clear();
        if (!programChunks) return;
        for (const auto& chunk : *programChunks) {
            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                const auto& store = chunk[pc];
                if (store.op != VMOperator::STORE_VALUE_GLOBAL &&
                    store.op != VMOperator::ASSIGN_VALUE_GLOBAL) {
                    continue;
                }
                if (store.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(store.args[0])) {
                    continue;
                }
                const auto& indexLiteral = std::get<LexerType::Value>(store.args[0]);
                if (!std::holds_alternative<ZeusType::BigInteger>(indexLiteral.data)) {
                    continue;
                }
                int64_t index = 0;
                if (!ZeusVMUtils::tryGetInt64FromValue(indexLiteral, index) || index < 0 ||
                    index > std::numeric_limits<int>::max()) {
                    continue;
                }

                if (pc > 0 && chunk[pc - 1].op == VMOperator::PUSH &&
                    !chunk[pc - 1].args.empty() &&
                    std::holds_alternative<LexerType::Value>(chunk[pc - 1].args[0])) {
                    const auto& literal =
                        std::get<LexerType::Value>(chunk[pc - 1].args[0]);
                    const char* type = nullptr;
                    if (std::holds_alternative<ZeusType::BigInteger>(literal.data) ||
                        std::holds_alternative<ZeusType::BigDecimal>(literal.data)) {
                        type = "i64";
                    } else if (std::holds_alternative<bool>(literal.data)) {
                        type = "i1";
                    }
                    if (type) {
                        nativeGlobalScalarTypes[static_cast<int>(index)] = type;
                        continue;
                    }
                }

                if (pc == 0) continue;
                const auto& build = chunk[pc - 1];

                // A class declaration keeps its name in the CLASS_BUILD metadata.
                if (build.op == VMOperator::CLASS_BUILD ||
                    build.op == VMOperator::CLASS_BUILD_SHARED) {
                    std::string className;
                    ClassLayout layout;
                    if (build.metadata.readClassLayout(
                            className, layout.fieldNames, layout.fieldTypes,
                            &layout.fieldConstness, &layout.fieldDefaults)) {
                        prescanGlobalClassTypes[static_cast<int>(index)] =
                            std::move(className);
                    }
                    continue;
                }

                // An instance is built by OBJ_BUILD, preceded by the class
                // reference and then the constructor arguments.
                if (build.op != VMOperator::OBJ_BUILD &&
                    build.op != VMOperator::NEW_OBJ) {
                    continue;
                }
                for (size_t back = 1; back <= 8 && pc >= back; ++back) {
                    const auto& load = chunk[pc - back];
                    if (load.op != VMOperator::LOAD_VALUE || load.args.empty() ||
                        !std::holds_alternative<std::string>(load.args[0])) {
                        continue;
                    }
                    const std::string& name = std::get<std::string>(load.args[0]);
                    if (classLayouts.find(name) != classLayouts.end()) {
                        prescanGlobalClassTypes[static_cast<int>(index)] = name;
                        break;
                    }
                    if (name == "global" && load.args.size() > 1 &&
                        std::holds_alternative<LexerType::Value>(load.args[1])) {
                        int64_t classSlot = 0;
                        if (ZeusVMUtils::tryGetInt64FromValue(
                                std::get<LexerType::Value>(load.args[1]), classSlot)) {
                            const auto classGlobal =
                                classGlobals.find(static_cast<int>(classSlot));
                            if (classGlobal != classGlobals.end()) {
                                prescanGlobalClassTypes[static_cast<int>(index)] =
                                    classGlobal->second;
                            }
                        }
                    }
                    break;
                }
            }
        }

        // Top-level expressions may span multiple IR chunks. Track class
        // references and constructed instances across those chunks so a later
        // DELETE_VAR can retain only the destructor for the variable's class.
        std::vector<std::string> stack;
        auto popAbstract = [&]() {
            if (stack.empty()) return std::string();
            std::string value = std::move(stack.back());
            stack.pop_back();
            return value;
        };
        auto popAbstractCount = [&](int64_t count) {
            if (count < 0 || static_cast<uint64_t>(count) > stack.size()) {
                stack.clear();
                return;
            }
            while (count-- > 0) stack.pop_back();
        };
        for (size_t ci = 0; ci < programChunks->size(); ++ci) {
            const auto& chunk = (*programChunks)[ci];
            if (functionSignaturesByChunk.find(ci) != functionSignaturesByChunk.end() ||
                (!chunk.empty() && chunk.front().metadata.kind ==
                    IRType::IRMetadataKind::ModuleImportInit)) {
                stack.clear();
                continue;
            }

            for (const auto& ins : chunk) {
                switch (ins.op) {
                    case VMOperator::PUSH:
                    case VMOperator::LOAD_NATIVE:
                    case VMOperator::MAKE_FUNCTION:
                    case VMOperator::LOAD_CELL_REFERENCE:
                        stack.emplace_back();
                        break;

                    case VMOperator::LOAD_VALUE: {
                        std::string value;
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0]) &&
                            std::get<std::string>(ins.args[0]) == "global") {
                            int64_t index = -1;
                            if (tryArgInt(ins, 1, index) && index >= 0 &&
                                index <= std::numeric_limits<int>::max()) {
                                const int globalIndex = static_cast<int>(index);
                                const auto nameSpace =
                                    namespacePackagesByGlobal.find(globalIndex);
                                const auto classGlobal = classGlobals.find(globalIndex);
                                const auto instance = prescanGlobalClassTypes.find(globalIndex);
                                if (nameSpace != namespacePackagesByGlobal.end()) {
                                    value = "namespace:" + nameSpace->second;
                                } else if (classGlobal != classGlobals.end()) {
                                    value = "class:" + classGlobal->second;
                                } else if (instance != prescanGlobalClassTypes.end()) {
                                    value = "instance:" + instance->second;
                                }
                            }
                        }
                        stack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::CLASS_BUILD:
                    case VMOperator::CLASS_BUILD_SHARED: {
                        std::string className;
                        std::vector<std::string> fieldNames;
                        std::vector<std::string> fieldTypes;
                        if (ins.metadata.readClassLayout(
                                className, fieldNames, fieldTypes)) {
                            stack.push_back("class:" + className);
                        } else {
                            stack.emplace_back();
                        }
                        break;
                    }

                    case VMOperator::OBJ_LOAD_ATTR: {
                        const std::string receiver = popAbstract();
                        std::string member;
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            member = std::get<std::string>(ins.args[0]);
                        }
                        std::string value;
                        const std::string namespacePrefix = "namespace:";
                        if (receiver.rfind(namespacePrefix, 0) == 0) {
                            const std::string package = receiver.substr(namespacePrefix.size());
                            const auto exports = namespaceExportsByPackage.find(package);
                            if (exports != namespaceExportsByPackage.end() &&
                                exports->second.find(member) != exports->second.end() &&
                                classLayouts.find(member) != classLayouts.end()) {
                                value = "class:" + member;
                            }
                        }
                        stack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::OBJ_BUILD:
                    case VMOperator::OBJ_BUILD_SHARED:
                    case VMOperator::NEW_OBJ: {
                        int64_t argc = 0;
                        if (!tryArgInt(ins, 0, argc) || argc < 0) {
                            stack.clear();
                            break;
                        }
                        popAbstractCount(argc);
                        std::string targetType;
                        if (ins.args.size() > 1 &&
                            std::holds_alternative<std::string>(ins.args[1])) {
                            targetType = std::get<std::string>(ins.args[1]);
                        }
                        const std::string classRef = targetType.empty()
                            ? popAbstract() : "class:" + targetType;
                        const std::string classPrefix = "class:";
                        if (classRef.rfind(classPrefix, 0) == 0) {
                            const std::string type = classRef.substr(classPrefix.size());
                            if (classLayouts.find(type) != classLayouts.end()) {
                                stack.push_back("instance:" + type);
                                break;
                            }
                        }
                        stack.emplace_back();
                        break;
                    }

                    case VMOperator::STORE_VALUE_GLOBAL:
                    case VMOperator::ASSIGN_VALUE_GLOBAL: {
                        int64_t index = -1;
                        const std::string value = popAbstract();
                        if (!tryArgInt(ins, 0, index) || index < 0 ||
                            index > std::numeric_limits<int>::max()) break;
                        const std::string instancePrefix = "instance:";
                        if (value.rfind(instancePrefix, 0) == 0) {
                            prescanGlobalClassTypes[static_cast<int>(index)] =
                                value.substr(instancePrefix.size());
                        }
                        break;
                    }

                    case VMOperator::OBJ_CALL_METHOD:
                    case VMOperator::CALL_NATIVE:
                    case VMOperator::CALL_CLOSURE: {
                        int64_t argc = 0;
                        if (!tryArgInt(ins, ins.op == VMOperator::OBJ_CALL_METHOD ? 1 : 0,
                                       argc) || argc < 0) {
                            stack.clear();
                            break;
                        }
                        const bool directCall = ins.op == VMOperator::CALL_CLOSURE &&
                            ins.metadata.kind == IRType::IRMetadataKind::CallTarget;
                        popAbstractCount(argc +
                            (ins.op == VMOperator::OBJ_CALL_METHOD ||
                             ins.op == VMOperator::CALL_NATIVE || !directCall ? 1 : 0));
                        stack.emplace_back();
                        break;
                    }

                    case VMOperator::LIST_BUILD:
                    case VMOperator::DICT_BUILD: {
                        int64_t count = 0;
                        if (!tryArgInt(ins, 0, count) || count < 0) {
                            stack.clear();
                            break;
                        }
                        popAbstractCount(count * (ins.op == VMOperator::DICT_BUILD ? 2 : 1));
                        stack.emplace_back();
                        break;
                    }

                    case VMOperator::LIST_LENGTH:
                    case VMOperator::DICT_KEYS_BUILD:
                        popAbstract();
                        stack.emplace_back();
                        break;

                    case VMOperator::LIST_LOAD_ELEM:
                    case VMOperator::DICT_LOAD_VALUE:
                        popAbstractCount(2);
                        stack.emplace_back();
                        break;

                    case VMOperator::LIST_STORE_ELEM:
                    case VMOperator::DICT_STORE_VALUE:
                    case VMOperator::OBJ_STORE_ATTR:
                        popAbstractCount(3);
                        break;

                    case VMOperator::ADD:
                    case VMOperator::SUBTRACT:
                    case VMOperator::MULTIPLY:
                    case VMOperator::DIVIDE:
                    case VMOperator::MODULUS:
                    case VMOperator::EQUL:
                    case VMOperator::NOT_EQUAL:
                    case VMOperator::GREATER_THAN:
                    case VMOperator::LESS_THAN:
                    case VMOperator::GREATER_EQUAL:
                    case VMOperator::LESS_EQUAL:
                    case VMOperator::LOGICAL_AND:
                    case VMOperator::LOGICAL_OR:
                        popAbstractCount(2);
                        stack.emplace_back();
                        break;

                    case VMOperator::POP:
                        popAbstract();
                        break;

                    case VMOperator::CLEAR:
                        stack.clear();
                        break;

                    case VMOperator::COPY:
                        stack.push_back(stack.empty() ? std::string() : stack.back());
                        break;

                    case VMOperator::SWAP:
                        if (stack.size() >= 2) {
                            std::iter_swap(stack.end() - 1, stack.end() - 2);
                        } else {
                            stack.clear();
                        }
                        break;

                    case VMOperator::JUMP_IF_TRUE:
                    case VMOperator::JUMP_IF_FALSE:
                    case VMOperator::COUNT_CHECK_NEG:
                    case VMOperator::COUNT_CHECK_STEP_ZERO:
                        popAbstract();
                        break;

                    default:
                        stack.clear();
                        break;
                }
            }
        }
    }
    std::set<std::string> structTypes;
    std::map<std::string, ClassLayout> classLayouts;
    std::string nativeFunctions;
    bool needsRuntimeErrorFactory = false;
    std::string externalDeclarations;
    DynamicMemberBridge::GeneratedIR dynamicMemberBridge;

    struct ClosureCaptureDescriptor {
        std::string sourceKind;
        int sourceIndex = -1;
        int destinationIndex = -1;
        std::string name;
        bool byReference = false;
    };

    static std::string closureAdapterSymbol(size_t chunkIndex) {
        return "@zeus_closure_adapter_" + std::to_string(chunkIndex);
    }

    std::vector<ClosureCaptureDescriptor> captureDescriptors(size_t chunkIndex) const {
        std::vector<ClosureCaptureDescriptor> result;
        if (!programChunks || chunkIndex >= programChunks->size()) return result;
        for (const auto& ins : (*programChunks)[chunkIndex]) {
            if (ins.op != VMOperator::CAPTURE_OUTSIDE_VAR || ins.args.size() < 5 ||
                !std::holds_alternative<std::string>(ins.args[0]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[1]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[2]) ||
                !std::holds_alternative<std::string>(ins.args[3]) ||
                !std::holds_alternative<LexerType::Value>(ins.args[4])) {
                continue;
            }
            ClosureCaptureDescriptor capture;
            capture.sourceKind = std::get<std::string>(ins.args[0]);
            capture.sourceIndex = VMHelpers::getIntFromValue(
                std::get<LexerType::Value>(ins.args[1]));
            capture.destinationIndex = VMHelpers::getIntFromValue(
                std::get<LexerType::Value>(ins.args[2]));
            capture.name = std::get<std::string>(ins.args[3]);
            capture.byReference = VMHelpers::getIntFromValue(
                std::get<LexerType::Value>(ins.args[4])) != 0;
            result.push_back(std::move(capture));
        }
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.destinationIndex < rhs.destinationIndex;
        });
        return result;
    }

    bool needsClosureAdapter(size_t chunkIndex) const {
        if (!programChunks) return false;
        for (const auto& chunk : *programChunks) {
            for (const auto& ins : chunk) {
                if (ins.op != VMOperator::MAKE_FUNCTION || ins.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                    continue;
                }
                if (VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(ins.args[0])) ==
                    static_cast<int>(chunkIndex)) {
                    return true;
                }
            }
        }
        return false;
    }

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

    std::string freshTmp() {
        return "%t" + std::to_string(tmpCount++);
    }

    static std::string llvmFunctionName(const std::string& name) {
        std::string result = "@zeus_";

        for (unsigned char c : name) {
            if (std::isalnum(c) || c == '_') {
                result += static_cast<char>(c);
            } else {
                result += '_';
            }
        }

        return result;
    }

    static std::string llvmExternalFunctionName(const std::string& name) {
        std::string result = "@\"";
        for (unsigned char c : name) {
            if (std::isalnum(c) || c == '_' || c == '.' || c == '$' || c == '-') {
                result += static_cast<char>(c);
            } else {
                char escaped[5];
                snprintf(escaped, sizeof(escaped), "\\%02X", static_cast<unsigned>(c));
                result += escaped;
            }
        }
        result += '\"';
        return result;
    }

    std::string llvmCallTarget(const FunctionSignature& signature) const {
        if (!signature.nativeSymbol.empty()) {
            return llvmExternalFunctionName(signature.nativeSymbol);
        }

        std::string target = llvmFunctionName(signature.name);
        const auto count = internalFunctionSymbolCounts.find(target);
        if (count != internalFunctionSymbolCounts.end() && count->second > 1) {
            target += "_chunk" + std::to_string(signature.chunkIndex);
        }
        return target;
    }

    std::string llvmCallTarget(const std::string& name) const {
        const auto signature = functionSignatures.find(name);
        if (signature != functionSignatures.end()) return llvmCallTarget(signature->second);
        return llvmFunctionName(name);
    }

    const FunctionSignature* findNativeBuiltinDeclaration(
        const std::string& builtinName
    ) const {
        const FunctionSignature* match = nullptr;
        for (const auto& entry : functionSignatures) {
            const auto& signature = entry.second;
            if (!signature.isExternal || signature.nativeSymbol.empty()) continue;

            const std::string suffix = "." + builtinName;
            const bool sameName = signature.name == builtinName;
            const bool qualifiedName = signature.name.size() > suffix.size() &&
                signature.name.compare(
                    signature.name.size() - suffix.size(), suffix.size(), suffix
                ) == 0;
            if (!sameName && !qualifiedName) continue;

            if (match != nullptr && match->nativeSymbol != signature.nativeSymbol) {
                return nullptr;
            }
            match = &signature;
        }
        return match;
    }

    const FunctionSignature* resolveFunctionSignature(
        const std::string& name
    ) const {
        const auto exact = functionSignatures.find(name);
        if (exact != functionSignatures.end()) return &exact->second;
        return findNativeBuiltinDeclaration(name);
    }

    static uint64_t providedPrefixMask(size_t count) {
        return count >= 64 ? UINT64_MAX : ((uint64_t{1} << count) - 1);
    }

    static uint64_t providedArgumentMask(const std::vector<bool>& provided) {
        uint64_t mask = 0;
        for (size_t i = 0; i < provided.size() && i < 64; ++i) {
            if (provided[i]) mask |= uint64_t{1} << i;
        }
        return mask;
    }

    static bool bindCallArguments(
        const FunctionSignature& signature,
        const std::vector<NativeValue>& arguments,
        std::vector<NativeValue>& ordered,
        std::vector<bool>& provided,
        std::vector<NativeValue>& variadic,
        std::vector<std::pair<std::string, NativeValue>>& keywordVariadic,
        std::string& error
    ) {
        const size_t regular = signature.parameterTypes.size();
        ordered.resize(regular);
        provided.assign(regular, false);
        size_t positional = 0;
        std::set<std::string> seenNames;
        for (const auto& argument : arguments) {
            if (argument.namedArgument.empty()) {
                if (positional < regular) {
                    if (provided[positional]) {
                        error = "positional argument duplicates a named argument";
                        return false;
                    }
                    ordered[positional] = argument;
                    provided[positional] = true;
                } else if (signature.hasVariadicArgs) {
                    variadic.push_back(argument);
                } else {
                    error = "too many positional arguments";
                    return false;
                }
                ++positional;
                continue;
            }
            if (!seenNames.insert(argument.namedArgument).second) {
                error = "duplicate named argument '" + argument.namedArgument + "'";
                return false;
            }
            const auto found = std::find(signature.parameterNames.begin(),
                                         signature.parameterNames.end(), argument.namedArgument);
            if (found == signature.parameterNames.end()) {
                if (signature.hasVariadicKwargs) {
                    keywordVariadic.emplace_back(argument.namedArgument, argument);
                    continue;
                }
                error = "unexpected named argument '" + argument.namedArgument + "'";
                return false;
            }
            const size_t index = static_cast<size_t>(found - signature.parameterNames.begin());
            if (provided[index]) {
                error = "multiple values for parameter '" + argument.namedArgument + "'";
                return false;
            }
            ordered[index] = argument;
            provided[index] = true;
        }
        for (size_t i = 0; i < regular; ++i) {
            if (!provided[i] && !signature.defaultParameterMask[i]) {
                error = "missing required argument '" +
                    (i < signature.parameterNames.size() ? signature.parameterNames[i]
                                                          : std::to_string(i)) + "'";
                return false;
            }
        }
        return true;
    }

    bool emitVariadicContainers(
        bool argsNeeded,
        bool kwargsNeeded,
        const std::vector<NativeValue>& variadic,
        const std::vector<std::pair<std::string, NativeValue>>& keywordVariadic,
        const std::function<bool(const NativeValue&, std::string&, bool&)>& toDynamic,
        const ContainerABI::FreshTemporary& fresh,
        const ContainerABI::EmitLine& emit,
        std::string& argsArray,
        std::string& kwargsDict
    ) {
        std::vector<std::string> values;
        std::vector<std::string> owned;
        for (const auto& argument : variadic) {
            std::string converted;
            bool owns = false;
            if (!toDynamic(argument, converted, owns)) return false;
            values.push_back(converted);
            if (owns) owned.push_back(converted);
        }
        if (argsNeeded) {
            argsArray = ContainerABI::emitBuildCall(false,
                static_cast<int64_t>(variadic.size()), values, fresh, emit);
        }
        for (const auto& value : owned) emit("call void @zeus_value_release(ptr " + value + ")");
        values.clear();
        owned.clear();
        for (const auto& [name, argument] : keywordVariadic) {
            const std::string nameValue = fresh();
            const std::string pooled = poolStr(name);
            emit(nameValue + " = alloca %ZeusValue, align 8");
            emit("call void @zeus_value_from_string_data(ptr " + nameValue +
                 ", ptr " + pooled + ", i64 " + std::to_string(name.size()) + ")");
            values.push_back(nameValue);
            owned.push_back(nameValue);
            std::string converted;
            bool owns = false;
            if (!toDynamic(argument, converted, owns)) return false;
            values.push_back(converted);
            if (owns) owned.push_back(converted);
        }
        if (kwargsNeeded) {
            kwargsDict = ContainerABI::emitBuildCall(true,
                static_cast<int64_t>(keywordVariadic.size()), values, fresh, emit);
        }
        for (const auto& value : owned) emit("call void @zeus_value_release(ptr " + value + ")");
        return true;
    }

    static bool usesDynamicZeusValueSignature(const FunctionSignature& signature) {
        if (!signature.returnType.empty()) return false;
        return std::all_of(
            signature.parameterTypes.begin(), signature.parameterTypes.end(),
            [](const std::string& typeName) { return typeName.empty(); }
        );
    }

    static bool annotationKind(
        const IRType::IRInstr& ins,
        const std::string& expected
    ) {
        if (ins.op != VMOperator::IR_ANNOTATION) return false;
        if (!ins.metadata.tag.empty()) return ins.metadata.tag == expected;

        return !ins.args.empty() &&
               std::holds_alternative<std::string>(ins.args[0]) &&
               std::get<std::string>(ins.args[0]) == expected;
    }

    static bool annotationString(
        const IRType::IRInstr& ins,
        size_t index,
        std::string& value
    ) {
        if (index >= ins.args.size() || !std::holds_alternative<std::string>(ins.args[index])) {
            return false;
        }

        value = std::get<std::string>(ins.args[index]);
        return true;
    }

    static bool annotationSymbolName(
        const IRType::IRInstr& ins,
        std::string& value
    ) {
        if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall ||
            ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
            return ins.metadata.readDirectCall(value);
        }

        return annotationString(ins, 1, value);
    }

    static bool annotationInteger(
        const IRType::IRInstr& ins,
        size_t index,
        int64_t& value
    ) {
        return tryArgInt(ins, index, value);
    }

    static bool isNativeType(const std::string& typeName) {
        const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName);
        return abi != nullptr &&
               abi->valueABI != ZeusBuiltins::NativeValueABI::ZeusValue;
    }

    static bool isBuiltinTypeName(const std::string& typeName) {
        if (!typeName.empty() && typeName.front() == '&') {
            return isBuiltinTypeName(typeName.substr(1));
        }
        return ZeusBuiltins::findNativeTypeABI(typeName) != nullptr ||
               ZeusBuiltins::findTypeImplementation(typeName) != nullptr;
    }

    static bool isReferenceTypeName(const std::string& typeName) {
        return !typeName.empty() && typeName.front() == '&';
    }

    static std::string referencedTypeName(const std::string& typeName) {
        return isReferenceTypeName(typeName) ? typeName.substr(1) : std::string();
    }

    bool isClassType(const std::string& typeName) const {
        return classLayouts.find(typeName) != classLayouts.end();
    }

    bool isManagedClassValue(const NativeValue& value) const {
        return !value.dynamic && !value.functionRef && value.type == "ptr" &&
            isClassType(value.sourceType);
    }

    std::string llvmStructType(const std::string& typeName) const {
        std::string name = "%zeus_";
        for (unsigned char c : typeName) {
            name += std::isalnum(c) || c == '_' ? static_cast<char>(c) : '_';
        }
        return name;
    }

    std::string classLifetimeFinalizer(const std::string& typeName) const {
        return llvmFunctionName("__class_finalize_" + typeName);
    }

    std::string llvmType(const std::string& typeName) const {
        if (!typeName.empty() && typeName.front() == '&') return "ptr";
        if (const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName)) {
            return std::string(abi->llvmType);
        }
        if (isClassType(typeName)) return "ptr";
        if (structTypes.find(typeName) != structTypes.end()) {
            return llvmStructType(typeName);
        }
        return "";
    }

    bool usesZeusValueABI(const std::string& typeName) const {
        if (!typeName.empty() && typeName.front() == '&') return false;
        if (typeName.empty() || typeName == "nil") return true;

        if (const auto* abi = ZeusBuiltins::findNativeTypeABI(typeName)) {
            return abi->valueABI == ZeusBuiltins::NativeValueABI::ZeusValue;
        }

        const std::string type = llvmType(typeName);
        if (type == "i1" || type == "i64") return false;
        if (isClassType(typeName) || type.rfind("%zeus_", 0) == 0) return false;
        return true;
    }

    std::string functionParameterABIType(const std::string& typeName) const {
        return usesZeusValueABI(typeName) ? "ptr" : llvmType(typeName);
    }

    template <typename EmitLine, typename FreshTemporary>
    bool emitOptionalArgumentPlaceholder(
        const std::string& typeName,
        NativeValue& value,
        EmitLine&& emitLine,
        FreshTemporary&& freshTemporary
    ) {
        const std::string abiType = functionParameterABIType(typeName);
        if (abiType.empty()) return false;
        if (usesZeusValueABI(typeName)) {
            const std::string nilValue = freshTemporary();
            emitLine(nilValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_init_nil(ptr " + nilValue + ")");
            value = NativeValue{ nilValue, "ptr", false, true, typeName, false };
            return true;
        }
        if (abiType == "i64" || abiType == "i1") {
            value = NativeValue{ "0", abiType, false, false, typeName, false };
            return true;
        }
        if (abiType == "ptr") {
            value = NativeValue{ "null", "ptr", false, false, typeName, false };
            return true;
        }
        return false;
    }

    std::string llvmFieldType(const std::string& typeName) const {
        if (usesZeusValueABI(typeName)) return "%ZeusValue";

        const std::string nativeType = llvmType(typeName);
        if (!nativeType.empty()) return nativeType;

        // 未完成静态 ABI 的字段先保留为指针槽位，避免类布局再次退。
        // { i8 } 占位；真正的字符串 容器字段 lowering 仍需动态 ABI。
        return "ptr";
    }

    const ClassLayout* findClassLayout(const std::string& typeName) const {
        auto layout = classLayouts.find(typeName);
        return layout == classLayouts.end() ? nullptr : &layout->second;
    }

    int findFieldIndex(
        const ClassLayout& layout,
        const std::string& fieldName
    ) const {
        for (size_t i = 0; i < layout.fieldNames.size(); ++i) {
            if (layout.fieldNames[i] == fieldName) return static_cast<int>(i);
        }
        return -1;
    }

    const FunctionSignature* findClassOperatorSignature(
        const std::string& ownerType,
        const std::string& operatorSymbol,
        size_t parameterCount
    ) const {
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (signature.ownerType == ownerType &&
                signature.operatorSymbol == operatorSymbol &&
                signature.parameterTypes.size() == parameterCount &&
                !signature.parameterTypes.empty() &&
                signature.parameterTypes.front() == ownerType) {
                return &signature;
            }
        }
        return nullptr;
    }

    const FunctionSignature* findNewOperatorSignature(
        const std::string& ownerType
    ) const {
        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            if (signature.ownerType == ownerType &&
                signature.operatorSymbol == "new") {
                return &signature;
            }
        }
        return nullptr;
    }

    bool isFunctionGlobal(int index) const {
        return functionGlobals.find(index) != functionGlobals.end();
    }

    bool collectMetadata(const ZeusCompile::Result& program) {
        globalNames = program.globalNames;
        functionSignatures.clear();
        functionSignaturesByChunk.clear();
        internalFunctionSymbolCounts.clear();
        reachableFunctionChunks.clear();
        functionGlobals.clear();
        classGlobals.clear();
        namespacePackagesByGlobal.clear();
        namespaceExportsByPackage.clear();
        dynamicMemberMethodNames.clear();
        dynamicMemberBridge = {};
        structTypes.clear();
        classLayouts.clear();
        externalDeclarations.clear();

        for (size_t ci = 0; ci < program.program.size(); ++ci) {
            const auto& chunk = program.program[ci];

            for (const auto& ins : chunk) {
                if (ins.metadata.kind == IRType::IRMetadataKind::ClassLayout) {
                    std::string className;
                    ClassLayout layout;
                    if (!ins.metadata.readClassLayout(
                            className,
                            layout.fieldNames,
                            layout.fieldTypes,
                            &layout.fieldConstness,
                            &layout.fieldDefaults)) {
                        result.error = "[emit-llvm] 类布局元信息无效";
                        return false;
                    }

                    classLayouts[className] = std::move(layout);
                    structTypes.insert(className);
                    continue;
                }

                const bool isExternalSignature =
                    annotationKind(ins, "external_function_signature");
                if (!isExternalSignature && !annotationKind(ins, "function_signature")) {
                    continue;
                }

                FunctionSignature signature;
                signature.chunkIndex = ci;
                signature.isExternal = isExternalSignature;

                if (ins.metadata.kind == IRType::IRMetadataKind::FunctionSignature ||
                    ins.metadata.kind == IRType::IRMetadataKind::ExternalFunctionSignature) {
                        if (!ins.metadata.readFunctionSignature(
                            signature.name,
                            signature.returnType,
                            signature.captureCount,
                            signature.parameterTypes,
                            &signature.ownerType,
                            &signature.operatorSymbol,
                            &signature.nativeSymbol)) {
                        result.error = "[emit-llvm] 结构化函数签名元信息无效";
                        return false;
                    }
                    signature.isStaticMethod = ins.metadata.isStaticMethod;
                } else {
                    int64_t captureCount = 0;
                    int64_t parameterCount = 0;

                    if (!annotationString(ins, 1, signature.name) ||
                        !annotationString(ins, 2, signature.returnType) ||
                        !annotationInteger(ins, 3, captureCount) ||
                        !annotationInteger(ins, 4, parameterCount) ||
                        parameterCount < 0 ||
                        ins.args.size() < 5 + static_cast<size_t>(parameterCount)) {
                        result.error = "[emit-llvm] 函数签名标注格式错误";
                        return false;
                    }

                    signature.captureCount = static_cast<int>(captureCount);
                    for (int64_t i = 0; i < parameterCount; ++i) {
                        std::string typeName;
                        if (!annotationString(ins, 5 + static_cast<size_t>(i), typeName)) {
                            result.error = "[emit-llvm] 函数参数类型标注格式错误";
                            return false;
                        }
                        signature.parameterTypes.push_back(typeName);
                    }
                    const size_t nativeSymbolIndex =
                        5 + static_cast<size_t>(parameterCount);
                    if (nativeSymbolIndex < ins.args.size()) {
                        annotationString(ins, nativeSymbolIndex, signature.nativeSymbol);
                    }
                }

                signature.minRequiredParams = static_cast<int>(signature.parameterTypes.size());
                signature.defaultParameterMask.assign(signature.parameterTypes.size(), false);
                if (!isExternalSignature && !chunk.empty() &&
                    chunk.front().op == VMOperator::FUNC_BEGIN &&
                    chunk.front().args.size() >= 7 &&
                    std::holds_alternative<LexerType::Value>(chunk.front().args[5]) &&
                    std::holds_alternative<LexerType::Value>(chunk.front().args[6])) {
                    const auto& header = chunk.front();
                    const int minRequired = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[5]));
                    const int defaultCount = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[6]));
                    if (minRequired < 0 ||
                        minRequired > static_cast<int>(signature.parameterTypes.size()) ||
                        defaultCount < 0 || header.args.size() < 7 +
                            static_cast<size_t>(defaultCount) * 2) {
                        result.error = "[emit-llvm] function default-parameter metadata is invalid";
                        return false;
                    }
                    signature.minRequiredParams = minRequired;
                    for (int i = 0; i < defaultCount; ++i) {
                        const size_t index = 7 + static_cast<size_t>(i) * 2;
                        if (!std::holds_alternative<LexerType::Value>(header.args[index])) {
                            result.error = "[emit-llvm] function default parameter index is invalid";
                            return false;
                        }
                        const int parameterIndex = VMHelpers::getIntFromValue(
                            std::get<LexerType::Value>(header.args[index]));
                        if (parameterIndex < 0 ||
                            parameterIndex >= static_cast<int>(signature.parameterTypes.size())) {
                            result.error = "[emit-llvm] function default parameter index is out of range";
                            return false;
                        }
                        signature.defaultParameterMask[static_cast<size_t>(parameterIndex)] = true;
                    }
                    const size_t namesOffset = 7 + static_cast<size_t>(defaultCount) * 2;
                    if (header.args.size() <= namesOffset ||
                        !std::holds_alternative<LexerType::Value>(header.args[namesOffset])) {
                        result.error = "[emit-llvm] function parameter-name metadata is invalid";
                        return false;
                    }
                    const int nameCount = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[namesOffset]));
                    if (nameCount != static_cast<int>(signature.parameterTypes.size()) ||
                        header.args.size() < namesOffset + 1 + static_cast<size_t>(nameCount)) {
                        result.error = "[emit-llvm] function parameter-name count is invalid";
                        return false;
                    }
                    for (int i = 0; i < nameCount; ++i) {
                        const auto& name = header.args[namesOffset + 1 + static_cast<size_t>(i)];
                        if (!std::holds_alternative<std::string>(name)) {
                            result.error = "[emit-llvm] function parameter name is invalid";
                            return false;
                        }
                        signature.parameterNames.push_back(std::get<std::string>(name));
                    }
                    signature.hasVariadicArgs = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[1])) != 0;
                    signature.hasVariadicKwargs = VMHelpers::getIntFromValue(
                        std::get<LexerType::Value>(header.args[2])) != 0;
                    bool sawDefaultParameter = false;
                    for (bool hasDefault : signature.defaultParameterMask) {
                        if (hasDefault) {
                            sawDefaultParameter = true;
                        } else if (sawDefaultParameter) {
                            result.error = "[emit-llvm] native calls require default parameters to be trailing";
                            return false;
                        }
                    }
                    if (signature.hasDefaultParameters() &&
                        signature.parameterTypes.size() > 63) {
                        result.error = "[emit-llvm] default parameters currently support at most 63 parameters";
                        return false;
                    }
                }

                functionSignatures[signature.name] = signature;

                    if (!isBuiltinTypeName(signature.returnType) &&
                        !signature.returnType.empty()) {
                    structTypes.insert(
                        !signature.returnType.empty() && signature.returnType.front() == '&'
                            ? signature.returnType.substr(1)
                            : signature.returnType
                    );
                }
                for (const auto& typeName : signature.parameterTypes) {
                    if (!isBuiltinTypeName(typeName) && !typeName.empty()) {
                        structTypes.insert(
                            typeName.front() == '&' ? typeName.substr(1) : typeName
                        );
                    }
                }

                if (!signature.isExternal) {
                    functionSignaturesByChunk[ci] = signature;
                    ++internalFunctionSymbolCounts[llvmFunctionName(signature.name)];
                }
            }
        }

        for (const auto& entry : functionSignatures) {
            const auto& signature = entry.second;
            if (!signature.isExternal) continue;

            const bool returnsDynamic = usesZeusValueABI(signature.returnType);
            const std::string returnType = returnsDynamic
                ? "void"
                : llvmType(signature.returnType);
            if (returnType.empty()) {
                result.error = "[emit-llvm] decl 函数 '" + signature.name +
                               "' 的返回类型没有 LLVM ABI 映射";
                return false;
            }

            const std::string externalName = signature.nativeSymbol.empty()
                ? llvmFunctionName(signature.name)
                : llvmExternalFunctionName(signature.nativeSymbol);
            externalDeclarations += "declare " + returnType + " " +
                                    externalName + "(";
            if (returnsDynamic) externalDeclarations += "ptr";
            for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
                if (i > 0 || returnsDynamic) externalDeclarations += ", ";
                const std::string parameterType =
                    functionParameterABIType(signature.parameterTypes[i]);
                if (parameterType.empty()) {
                    result.error = "[emit-llvm] decl 函数 '" + signature.name +
                                   "' 的参数类型没有 LLVM ABI 映射";
                    return false;
                }
                externalDeclarations += parameterType;
            }
            externalDeclarations += ")\n";
        }

        for (const auto& entry : classLayouts) {
            for (const auto& fieldType : entry.second.fieldTypes) {
                if (!isBuiltinTypeName(fieldType) && !fieldType.empty()) {
                    structTypes.insert(fieldType);
                }
            }
        }

        for (size_t i = 0; i < globalNames.size(); ++i) {
            if (functionSignatures.find(globalNames[i]) != functionSignatures.end()) {
                functionGlobals[static_cast<int>(i)] = globalNames[i];
            }
            if (classLayouts.find(globalNames[i]) != classLayouts.end()) {
                classGlobals[static_cast<int>(i)] = globalNames[i];
                continue;
            }

            // Package globals are stored as `package.Class`, while the
            // class-layout metadata currently carries the source-level class
            // name. Match a unique suffix so `new Class(...)` inside an
            // imported implementation module remains a native class ref.
            std::string matchedClassName;
            bool ambiguousClassName = false;
            for (const auto& layout : classLayouts) {
                const std::string suffix = "." + layout.first;
                if (globalNames[i].size() <= suffix.size() ||
                    globalNames[i].compare(
                        globalNames[i].size() - suffix.size(), suffix.size(), suffix
                    ) != 0) {
                    continue;
                }
                if (!matchedClassName.empty()) {
                    ambiguousClassName = true;
                    break;
                }
                matchedClassName = layout.first;
            }
            if (!ambiguousClassName && !matchedClassName.empty()) {
                classGlobals[static_cast<int>(i)] = std::move(matchedClassName);
            }
        }

        for (const auto& chunk : program.program) {
            if (chunk.empty()) continue;
            if (chunk.front().metadata.kind !=
                    IRType::IRMetadataKind::ModuleImportInit) {
                continue;
            }

            const std::string& packageName = chunk.front().metadata.moduleName;
            int64_t aliasGlobal = -1;
            std::set<std::string> exports;
            for (const auto& instruction : chunk) {
                if (instruction.op == VMOperator::IMPORT &&
                    instruction.args.size() > 3) {
                    tryArgInt(instruction, 3, aliasGlobal);
                }
                if (instruction.op == VMOperator::STORE_VALUE_GLOBAL) {
                    tryArgInt(instruction, 0, aliasGlobal);
                }
                if (instruction.op != VMOperator::PUSH ||
                    instruction.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(
                        instruction.args[0])) {
                    continue;
                }
                const auto& literal =
                    std::get<LexerType::Value>(instruction.args[0]);
                if (std::holds_alternative<std::string>(literal.data)) {
                    const std::string& name =
                        std::get<std::string>(literal.data);
                    if (name != packageName) exports.insert(name);
                }
            }

            if (aliasGlobal < 0 ||
                static_cast<size_t>(aliasGlobal) >= globalNames.size()) {
                continue;
            }
            namespacePackagesByGlobal[static_cast<int>(aliasGlobal)] = packageName;
            auto& packageExports = namespaceExportsByPackage[packageName];
            packageExports.insert(exports.begin(), exports.end());
        }

        return true;
    }

    bool isBuiltinImplementationFunction(const FunctionSignature& signature) const {
        if (signature.name.rfind("ZeusBuiltins.", 0) == 0) return true;
        if (signature.ownerType.empty()) return false;
        if (ZeusBuiltins::findTypeImplementation(signature.ownerType) != nullptr) {
            return true;
        }
        const size_t lastDot = signature.ownerType.rfind('.');
        return lastDot != std::string::npos &&
            ZeusBuiltins::findTypeImplementation(
                signature.ownerType.substr(lastDot + 1)) != nullptr;
    }

    bool isReachabilityManagedFunction(const FunctionSignature& signature) const {
        return isBuiltinImplementationFunction(signature) ||
            (!signature.ownerType.empty() && isClassType(signature.ownerType));
    }

    static std::string methodNameForSignature(const FunctionSignature& signature) {
        if (signature.ownerType.empty()) return {};
        const std::string prefix = signature.ownerType + "::";
        if (signature.name.rfind(prefix, 0) != 0) return {};
        return signature.name.substr(prefix.size());
    }

    bool hasTypedInstanceMethodSignature(
        const std::string& ownerType,
        const std::string& methodName
    ) const {
        if (ownerType.empty() || methodName.empty()) return false;
        const auto function = functionSignatures.find(ownerType + "::" + methodName);
        return function != functionSignatures.end() &&
            function->second.ownerType == ownerType &&
            !function->second.isStaticMethod &&
            !function->second.parameterTypes.empty() &&
            function->second.parameterTypes.front() == ownerType;
    }

    void collectReachableFunctionChunks(
        const std::vector<std::vector<IRType::IRInstr>>& program
    ) {
        dynamicMemberMethodNames.clear();
        dynamicMemberMethodNames.insert("toString");
        std::map<std::string, std::set<std::string>> inferredFields;
        std::vector<size_t> pending;
        for (const auto& entry : functionSignaturesByChunk) {
            if (!isReachabilityManagedFunction(entry.second)) {
                reachableFunctionChunks.insert(entry.first);
                pending.push_back(entry.first);
            }
        }
        for (size_t ci = 0; ci < program.size(); ++ci) {
            if (functionSignaturesByChunk.find(ci) == functionSignaturesByChunk.end()) {
                pending.push_back(ci);
            }
        }

        std::set<size_t> scanned;
        auto addTarget = [&](const std::string& name) {
            const auto* function = resolveFunctionSignature(name);
            if (function == nullptr || function->isExternal ||
                !isReachabilityManagedFunction(*function)) {
                return;
            }
            const size_t targetChunk = function->chunkIndex;
            if (reachableFunctionChunks.insert(targetChunk).second) {
                pending.push_back(targetChunk);
            }
        };

        // print() uses the runtime bridge for object stringification. Built-in
        // classes opt in through the registry only after their toString method
        // has a native LLVM implementation.
        for (const auto& function : functionSignatures) {
            const auto& candidate = function.second;
            if (!isBuiltinImplementationFunction(candidate) ||
                methodNameForSignature(candidate) != "toString" ||
                candidate.parameterTypes.empty() ||
                candidate.parameterTypes.front() != candidate.ownerType) {
                continue;
            }
            const auto* type = ZeusBuiltins::findTypeImplementation(
                candidate.ownerType);
            if (type != nullptr && type->dynamicStringification) {
                addTarget(candidate.name);
            }
        }

        while (!pending.empty()) {
            const size_t ci = pending.back();
            pending.pop_back();
            if (ci >= program.size() || !scanned.insert(ci).second) continue;

            std::set<std::string> constructedTypes;
            std::set<std::string> deletedTypes;
            bool usesSubscriptLoad = false;
            bool usesSubscriptStore = false;
            std::vector<std::string> methodCalls;

            enum class ReachabilityValueKind { Untracked, Namespace, Class, Instance };
            struct ReachabilityValue {
                ReachabilityValueKind kind;
                std::string type;
                ReachabilityValue()
                    : kind(ReachabilityValueKind::Untracked) {}
                ReachabilityValue(ReachabilityValueKind valueKind, std::string valueType)
                    : kind(valueKind), type(std::move(valueType)) {}
            };
            std::vector<ReachabilityValue> valueStack;
            std::map<int, ReachabilityValue> localValues;
            std::map<int, ReachabilityValue> globalValues;
            const auto function = functionSignaturesByChunk.find(ci);
            if (function != functionSignaturesByChunk.end()) {
                const auto& parameters = function->second.parameterTypes;
                for (size_t i = 0; i < parameters.size(); ++i) {
                    if (classLayouts.find(parameters[i]) != classLayouts.end()) {
                        localValues[static_cast<int>(i)] = {
                            ReachabilityValueKind::Instance, parameters[i]
                        };
                    }
                }
            }
            for (const auto& global : prescanGlobalClassTypes) {
                globalValues[global.first] = {
                    ReachabilityValueKind::Instance, global.second
                };
            }
            auto popValue = [&]() {
                if (valueStack.empty()) return ReachabilityValue{};
                ReachabilityValue value = std::move(valueStack.back());
                valueStack.pop_back();
                return value;
            };
            auto popValues = [&](int64_t count) {
                if (count < 0 || static_cast<uint64_t>(count) > valueStack.size()) {
                    valueStack.clear();
                    return;
                }
                while (count-- > 0) valueStack.pop_back();
            };
            auto pushUnknown = [&]() { valueStack.emplace_back(); };
            auto readSlotIndex = [&](const IRType::IRInstr& ins,
                                     size_t argIndex,
                                     int64_t& index) {
                return tryArgInt(ins, argIndex, index) && index >= 0 &&
                    index <= std::numeric_limits<int>::max();
            };
            for (const auto& ins : program[ci]) {
                std::string targetName;
                if (ins.metadata.readDirectCall(targetName)) {
                    addTarget(targetName);
                } else if (annotationKind(ins, "direct_call") &&
                           annotationString(ins, 1, targetName)) {
                    addTarget(targetName);
                }

                if (ins.op == VMOperator::DICT_LOAD_VALUE) {
                    usesSubscriptLoad = true;
                    dynamicMemberMethodNames.insert("operator[[]]");
                }
                if (ins.op == VMOperator::DICT_STORE_VALUE) {
                    usesSubscriptStore = true;
                    dynamicMemberMethodNames.insert("operator[[]=]");
                }
                if (ins.op == VMOperator::OBJ_CALL_METHOD && !ins.args.empty() &&
                    std::holds_alternative<std::string>(ins.args.front())) {
                    const std::string method = std::get<std::string>(ins.args.front());
                    methodCalls.push_back(method);
                    dynamicMemberMethodNames.insert(method);
                }
                if (ins.op == VMOperator::WITH_BEGIN) {
                    methodCalls.push_back("operator[with]");
                    methodCalls.push_back("operator[~with]");
                }

                switch (ins.op) {
                    case VMOperator::PUSH:
                    case VMOperator::LOAD_NATIVE:
                    case VMOperator::MAKE_FUNCTION:
                    case VMOperator::LOAD_CELL_REFERENCE:
                        pushUnknown();
                        break;

                    case VMOperator::LOAD_VALUE: {
                        ReachabilityValue value;
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            const std::string& kind = std::get<std::string>(ins.args[0]);
                            int64_t index = -1;
                            if (kind == "global" && readSlotIndex(ins, 1, index)) {
                                const auto nameSpace = namespacePackagesByGlobal.find(
                                    static_cast<int>(index));
                                if (nameSpace != namespacePackagesByGlobal.end()) {
                                    value = { ReachabilityValueKind::Namespace,
                                              nameSpace->second };
                                } else {
                                    const auto classGlobal = classGlobals.find(
                                        static_cast<int>(index));
                                    if (classGlobal != classGlobals.end()) {
                                        value = { ReachabilityValueKind::Class,
                                                  classGlobal->second };
                                    } else {
                                        const auto global = globalValues.find(
                                            static_cast<int>(index));
                                        if (global != globalValues.end()) {
                                            value = global->second;
                                        }
                                    }
                                }
                            } else if (kind == "local" && readSlotIndex(ins, 1, index)) {
                                const auto local = localValues.find(static_cast<int>(index));
                                if (local != localValues.end()) value = local->second;
                            } else if (kind != "local" && kind != "global" &&
                                       kind != "upvalue" &&
                                       classLayouts.find(kind) != classLayouts.end()) {
                                value = { ReachabilityValueKind::Class, kind };
                            }
                        }
                        valueStack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::OBJ_LOAD_ATTR: {
                        const ReachabilityValue receiver = popValue();
                        std::string member;
                        if (!ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            member = std::get<std::string>(ins.args[0]);
                        }
                        ReachabilityValue value;
                        if (receiver.kind == ReachabilityValueKind::Namespace) {
                            const auto exports = namespaceExportsByPackage.find(receiver.type);
                            if (exports != namespaceExportsByPackage.end() &&
                                exports->second.find(member) != exports->second.end() &&
                                classLayouts.find(member) != classLayouts.end()) {
                                value = { ReachabilityValueKind::Class, member };
                            }
                        } else if (receiver.kind == ReachabilityValueKind::Instance) {
                            const ClassLayout* layout = findClassLayout(receiver.type);
                            if (layout != nullptr) {
                                const int field = findFieldIndex(*layout, member);
                                if (field >= 0 && static_cast<size_t>(field) <
                                    layout->fieldTypes.size()) {
                                    const std::string& type = layout->fieldTypes[
                                        static_cast<size_t>(field)];
                                    if (classLayouts.find(type) != classLayouts.end()) {
                                        value = { ReachabilityValueKind::Instance, type };
                                    }
                                }
                            }
                        }
                        valueStack.push_back(std::move(value));
                        break;
                    }

                    case VMOperator::OBJ_BUILD:
                    case VMOperator::OBJ_BUILD_SHARED:
                    case VMOperator::NEW_OBJ: {
                        int64_t argc = 0;
                        if (!readSlotIndex(ins, 0, argc)) {
                            valueStack.clear();
                            break;
                        }
                        popValues(argc);
                        ReachabilityValue classValue;
                        std::string targetType;
                        if (ins.args.size() > 1 &&
                            std::holds_alternative<std::string>(ins.args[1])) {
                            targetType = std::get<std::string>(ins.args[1]);
                        }
                        if (targetType.empty()) classValue = popValue();
                        const std::string type = !targetType.empty()
                            ? targetType
                            : classValue.kind == ReachabilityValueKind::Class
                                ? classValue.type : std::string();
                        if (!type.empty() && classLayouts.find(type) != classLayouts.end()) {
                            constructedTypes.insert(type);
                            valueStack.push_back({ ReachabilityValueKind::Instance, type });
                        } else {
                            pushUnknown();
                        }
                        break;
                    }

                    case VMOperator::STORE_VALUE_GLOBAL:
                    case VMOperator::ASSIGN_VALUE_GLOBAL:
                    case VMOperator::STORE_VALUE_LOCAL:
                    case VMOperator::ASSIGN_VALUE_LOCAL: {
                        int64_t index = -1;
                        if (!readSlotIndex(ins, 0, index)) {
                            popValue();
                            break;
                        }
                        const ReachabilityValue value = popValue();
                        auto& slots = (ins.op == VMOperator::STORE_VALUE_GLOBAL ||
                                       ins.op == VMOperator::ASSIGN_VALUE_GLOBAL)
                            ? globalValues : localValues;
                        if (value.kind == ReachabilityValueKind::Instance) {
                            slots[static_cast<int>(index)] = value;
                        } else {
                            slots.erase(static_cast<int>(index));
                        }
                        break;
                    }

                    case VMOperator::DELETE_VAR: {
                        if (ins.args.size() >= 3 &&
                            std::holds_alternative<std::string>(ins.args[1])) {
                            int64_t index = -1;
                            if (readSlotIndex(ins, 2, index)) {
                                const std::string& kind = std::get<std::string>(ins.args[1]);
                                auto& slots = kind == "global" ? globalValues : localValues;
                                const auto variable = slots.find(static_cast<int>(index));
                                if (variable != slots.end() &&
                                    variable->second.kind == ReachabilityValueKind::Instance) {
                                    deletedTypes.insert(variable->second.type);
                                }
                                slots.erase(static_cast<int>(index));
                            }
                        }
                        break;
                    }

                    case VMOperator::OBJ_CALL_METHOD: {
                        int64_t argc = 0;
                        if (tryArgInt(ins, 1, argc) && argc >= 0) {
                            popValues(argc + 1);
                            pushUnknown();
                        } else {
                            valueStack.clear();
                        }
                        break;
                    }

                    case VMOperator::CALL_NATIVE:
                    case VMOperator::CALL_CLOSURE: {
                        int64_t argc = 0;
                        if (readSlotIndex(ins, 0, argc)) {
                            const bool directCall = ins.op == VMOperator::CALL_CLOSURE &&
                                ins.metadata.kind == IRType::IRMetadataKind::CallTarget;
                            popValues(argc + (directCall ? 0 : 1));
                            pushUnknown();
                        } else {
                            valueStack.clear();
                        }
                        break;
                    }

                    case VMOperator::LIST_BUILD:
                    case VMOperator::DICT_BUILD: {
                        int64_t count = 0;
                        if (readSlotIndex(ins, 0, count)) {
                            popValues(count * (ins.op == VMOperator::DICT_BUILD ? 2 : 1));
                            pushUnknown();
                        } else {
                            valueStack.clear();
                        }
                        break;
                    }

                    case VMOperator::LIST_LENGTH:
                    case VMOperator::DICT_KEYS_BUILD:
                        popValue();
                        pushUnknown();
                        break;

                    case VMOperator::LIST_LOAD_ELEM:
                    case VMOperator::DICT_LOAD_VALUE:
                        popValues(2);
                        pushUnknown();
                        break;

                    case VMOperator::LIST_STORE_ELEM:
                    case VMOperator::DICT_STORE_VALUE:
                        popValues(3);
                        break;

                    case VMOperator::OBJ_STORE_ATTR: {
                        popValue();
                        const ReachabilityValue receiver = popValue();
                        if (receiver.kind == ReachabilityValueKind::Instance &&
                            !ins.args.empty() &&
                            std::holds_alternative<std::string>(ins.args[0])) {
                            const std::string& name = std::get<std::string>(ins.args[0]);
                            const ClassLayout* layout = findClassLayout(receiver.type);
                            if (layout != nullptr && findFieldIndex(*layout, name) < 0) {
                                inferredFields[receiver.type].insert(name);
                            }
                        }
                        break;
                    }

                    case VMOperator::ADD:
                    case VMOperator::SUBTRACT:
                    case VMOperator::MULTIPLY:
                    case VMOperator::DIVIDE:
                    case VMOperator::MODULUS:
                    case VMOperator::EQUL:
                    case VMOperator::NOT_EQUAL:
                    case VMOperator::GREATER_THAN:
                    case VMOperator::LESS_THAN:
                    case VMOperator::GREATER_EQUAL:
                    case VMOperator::LESS_EQUAL:
                    case VMOperator::LOGICAL_AND:
                    case VMOperator::LOGICAL_OR:
                        popValues(2);
                        pushUnknown();
                        break;

                    case VMOperator::POP:
                    case VMOperator::WITH_BEGIN:
                        popValue();
                        break;

                    case VMOperator::WITH_END:
                        break;

                    case VMOperator::CLEAR:
                        valueStack.clear();
                        break;

                    case VMOperator::COPY:
                        valueStack.push_back(valueStack.empty()
                            ? ReachabilityValue{} : valueStack.back());
                        break;

                    case VMOperator::SWAP:
                        if (valueStack.size() >= 2) {
                            std::iter_swap(valueStack.end() - 1, valueStack.end() - 2);
                        } else {
                            valueStack.clear();
                        }
                        break;

                    case VMOperator::JUMP_IF_TRUE:
                    case VMOperator::JUMP_IF_FALSE:
                    case VMOperator::COUNT_CHECK_NEG:
                    case VMOperator::COUNT_CHECK_STEP_ZERO:
                        popValue();
                        break;

                    default:
                        valueStack.clear();
                        break;
                }
            }

            for (const auto& method : methodCalls) {
                for (const auto& function : functionSignatures) {
                    const auto& candidate = function.second;
                    if (!isReachabilityManagedFunction(candidate) ||
                        methodNameForSignature(candidate) != method) {
                        continue;
                    }
                    if (method == "toString") {
                        const auto* type = ZeusBuiltins::findTypeImplementation(
                            candidate.ownerType);
                        if (type == nullptr || !type->dynamicStringification) {
                            continue;
                        }
                    }
                    addTarget(candidate.name);
                }
            }

            auto addLifecycleFunctions = [&](const std::set<std::string>& types,
                                             bool constructors,
                                             bool destructors) {
                for (const auto& function : functionSignatures) {
                    const auto& candidate = function.second;
                    if (!isReachabilityManagedFunction(candidate) ||
                        types.find(candidate.ownerType) == types.end()) {
                        continue;
                    }
                    if ((constructors &&
                         (candidate.name == candidate.ownerType + "::init" ||
                          candidate.operatorSymbol == "new")) ||
                        (destructors && candidate.operatorSymbol == "~")) {
                        addTarget(candidate.name);
                    }
                }
            };
            std::set<std::string> destructorTypes;
            for (const auto& layout : classLayouts) {
                destructorTypes.insert(layout.first);
            }
            addLifecycleFunctions(constructedTypes, true, false);
            addLifecycleFunctions(deletedTypes, false, true);
            // Every emitted class finalizer contains a direct destructor call,
            // even when source code never spells `delete` for that class.
            addLifecycleFunctions(destructorTypes, false, true);

            if (usesSubscriptLoad || usesSubscriptStore) {
                for (const auto& function : functionSignatures) {
                    const auto& candidate = function.second;
                    if ((usesSubscriptLoad && candidate.operatorSymbol == "[]") ||
                        (usesSubscriptStore && candidate.operatorSymbol == "[]=")) {
                        addTarget(candidate.name);
                    }
                }
            }
        }

        for (const auto& entry : inferredFields) {
            ClassLayout& layout = classLayouts.at(entry.first);
            for (const std::string& name : entry.second) {
                if (findFieldIndex(layout, name) >= 0) continue;
                const size_t fieldIndex = layout.fieldNames.size();
                layout.fieldNames.push_back(name);
                layout.fieldTypes.emplace_back(); // Unannotated Zeus value.
                layout.fieldConstness.push_back(false);
                IRType::ClassFieldDefault missingValue;
                missingValue.kind = IRType::ClassFieldDefaultKind::Null;
                layout.fieldDefaults.push_back(missingValue);
                layout.fieldNames.push_back("$initialized:" + name);
                layout.fieldTypes.push_back("bool");
                layout.fieldConstness.push_back(false);
                layout.fieldDefaults.emplace_back(); // Zero / false at construction.
                layout.inferredFieldFlags[name] = fieldIndex + 1;
            }
        }
    }

    bool buildDynamicMemberBridge() {
        std::vector<DynamicMemberBridge::MethodBinding> bindings;
        std::set<std::pair<std::string, std::string>> registeredMethods;

        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            const std::string methodName = methodNameForSignature(signature);
            const bool hasNativeOwnerABI =
                ZeusBuiltins::findNativeTypeABI(signature.ownerType) != nullptr;
            if (methodName.empty() ||
                dynamicMemberMethodNames.find(methodName) == dynamicMemberMethodNames.end() ||
                signature.isStaticMethod ||
                signature.parameterTypes.empty() ||
                signature.parameterTypes.front() != signature.ownerType ||
                (!isClassType(signature.ownerType) && !hasNativeOwnerABI)) {
                continue;
            }

            if (!signature.isExternal &&
                reachableFunctionChunks.find(signature.chunkIndex) ==
                    reachableFunctionChunks.end()) {
                continue;
            }

            const auto key = std::make_pair(signature.ownerType, methodName);
            if (!registeredMethods.insert(key).second) {
                result.error = "[emit-llvm] 动态成员调用存在重载歧义 " +
                    signature.ownerType + "::" + methodName;
                return false;
            }

            bindings.push_back(DynamicMemberBridge::MethodBinding{
                signature.ownerType,
                methodName,
                signature.name,
                signature.returnType,
                signature.parameterTypes
            });
        }

        DynamicMemberBridge::TypeRules rules;
        rules.llvmType = [this](const std::string& typeName) {
            return llvmType(typeName);
        };
        rules.parameterABIType = [this](const std::string& typeName) {
            return functionParameterABIType(typeName);
        };
        rules.usesZeusValueABI = [this](const std::string& typeName) {
            return usesZeusValueABI(typeName);
        };
        rules.isClassType = [this](const std::string& typeName) {
            return isClassType(typeName);
        };
        rules.callTarget = [this](const std::string& name) {
            return llvmCallTarget(name);
        };

        dynamicMemberBridge = DynamicMemberBridge::generate(bindings, rules);
        if (!dynamicMemberBridge.error.empty()) {
            result.error = "[emit-llvm] " + dynamicMemberBridge.error;
            return false;
        }
        return true;
    }

    std::string poolStr(const std::string& s) {
        auto it = strPool.find(s);

        if (it != strPool.end()) return it->second;

        std::string name = "@.str." + std::to_string(strCount++);
        strPool[s] = name;
        return name;
    }

    void emitLine(const std::string& s) {
        const size_t callAt = s.rfind("call ", 0) == 0 ? 0 : s.find(" = call ");
        if (callAt == std::string::npos ||
            s.find("@llvm.", callAt) != std::string::npos) {
            body += "  " + s + "\n";
            return;
        }
        if (activeEntryExceptionTarget >= 0) {
            const size_t catchPc = static_cast<size_t>(activeEntryExceptionTarget);
            const auto key = std::make_pair(activeEntryChunk, catchPc);
            auto slot = entryExceptionSlots.find(key);
            if (slot == entryExceptionSlots.end()) {
                const std::string valueSlot = freshTmp();
                body += "  " + valueSlot + " = alloca %ZeusValue, align 8\n";
                slot = entryExceptionSlots.emplace(key, valueSlot).first;
            }
            const std::string stem = std::to_string(entryExceptionPads.size());
            const EntryExceptionPad pad{
                "%entry_eh_switch_" + stem, "%entry_eh_pad_" + stem,
                "%entry_eh_resume_" + stem, slot->second,
                activeEntryChunk, catchPc
            };
            entryExceptionPads.push_back(pad);
            const std::string continuation = "%entry_eh_continue_" + stem;
            std::string invoke = s;
            invoke.replace(callAt == 0 ? 0 : callAt + 3, 4, "invoke");
            body += "  " + invoke + " to label " + continuation +
                " unwind label " + pad.switchLabel + "\n";
            body += continuation.substr(1) + ":\n";
            return;
        }
        std::vector<std::string> cleanupCalls = entryExceptionCleanupCalls();
        if (cleanupCalls.empty()) {
            body += "  " + s + "\n";
            return;
        }
        const std::string stem = std::to_string(entryExceptionPads.size());
        EntryExceptionPad pad;
        pad.switchLabel = "%entry_eh_switch_" + stem;
        pad.padLabel = "%entry_eh_pad_" + stem;
        pad.resumeLabel = "%entry_eh_resume_" + stem;
        pad.cleanupOnly = true;
        pad.cleanupCalls = std::move(cleanupCalls);
        entryExceptionPads.push_back(std::move(pad));
        const std::string continuation = "%entry_eh_continue_" + stem;
        std::string invoke = s;
        invoke.replace(callAt == 0 ? 0 : callAt + 3, 4, "invoke");
        body += "  " + invoke + " to label " + continuation +
            " unwind label %entry_eh_switch_" + stem + "\n";
        body += continuation.substr(1) + ":\n";
    }

    // 新 label 定义（含 % 前缀传入，如 "%L1_10"）。
    // 铁律：label 之前必须有终结指令；若当前块未终结（fallthrough 位置），
    // 先补一条 br 跳过去（语义等价于 fallthrough，clang 实测只认这种形状）。
    void emitLabelDef(const std::string& pct) {
        if (!blkTerminated) {
            emitLine("br label " + pct);
        }

        body += pct.substr(1) + ":\n";
        blkTerminated = false;
        blkNeedLabel = false;
    }

    void fail(const IRType::IRInstr& ins, const std::string& msg) {
        result.ok = false;
        result.error = msg + " (" + ins.toString() + ")";
        result.errorLine = ins.line;
    }

    // PUSH/LOAD-字面量 的值部分（不含 push）：返回 box 的 SSA 名，失败返回 ""。
    std::string emitDynamicValueForLiteral(const LexerType::Value& v, const IRType::IRInstr& ins) {
        if (std::holds_alternative<std::string>(v.data)) {
            const std::string& s = std::get<std::string>(v.data);

            std::string sym = poolStr(s);
            std::string t = freshTmp();
            emitLine(t + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_string_data(ptr " + t +
                     ", ptr " + sym + ", i64 " + std::to_string(s.size()) + ")");
            return t;
        }

        if (std::holds_alternative<ZeusType::BigInteger>(v.data)) {
            const auto& integer = std::get<ZeusType::BigInteger>(v.data);
            int64_t integer64 = 0;
            if (integer.tryToInt64(integer64)) {
                return emitScalarAsValue("i64", std::to_string(integer64));
            }

            const std::string symbol = poolStr(integer.toString());
            std::string dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_bigint_decimal(ptr " +
                     dynamicValue + ", ptr " + symbol + ")");
            return dynamicValue;
        }

        if (std::holds_alternative<ZeusType::BigDecimal>(v.data)) {
            const std::string symbol = poolStr(std::get<ZeusType::BigDecimal>(v.data).toString());
            std::string dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_bigdecimal_decimal(ptr " +
                     dynamicValue + ", ptr " + symbol + ")");
            return dynamicValue;
        }

        if (std::holds_alternative<bool>(v.data)) {
            return emitScalarAsValue("i1", std::get<bool>(v.data) ? "true" : "false");
        }

        fail(ins, "[emit-llvm] v0: 该字面量类型暂不支持（仅 int/string/nil）");
        return "";
    }

    std::string emitDynamicValueForNull(const IRType::IRInstr& ins) {
        std::string value = freshTmp();
        emitLine(value + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_init_nil(ptr " + value + ")");
        (void)ins;
        return value;
    }

    std::string emitDynamicValueForBytes(
        const ZeusType::Bytes& bytes,
        const IRType::IRInstr& ins
    ) {
        const std::string contents(bytes.data.begin(), bytes.data.end());
        const std::string symbol = poolStr(contents);
        const std::string dynamicValue = freshTmp();
        emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_from_bytes(ptr " + dynamicValue +
                 ", ptr " + symbol + ", i64 " +
                 std::to_string(bytes.data.size()) + ")");
        (void)ins;
        return dynamicValue;
    }

    void emitPushValue(const std::string& valueTmp) {
        emitLine("call void @zeus_value_push(ptr " + valueTmp + ")");
    }

    std::string emitPop() {
        std::string t = freshTmp();
        emitLine(t + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_pop(ptr " + t + ")");
        return t;
    }

    void emitRelease(const std::string& valueTmp) {
        emitLine("call void @zeus_value_release(ptr " + valueTmp + ")");
    }

    template <typename EmitLine, typename FreshTemporary>
    std::string emitDynamicVariadicCall(
        const std::string& callSymbol,
        const std::string& nativeName,
        const std::vector<std::string>& values,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary
    ) {
        if (callSymbol.empty()) return {};

        std::string argumentArray = "null";
        if (!values.empty()) {
            argumentArray = freshTemporary();
            emit(argumentArray + " = alloca ptr, i64 " +
                 std::to_string(values.size()) + ", align 8");
            for (size_t i = 0; i < values.size(); ++i) {
                const std::string slot = freshTemporary();
                emit(slot + " = getelementptr inbounds ptr, ptr " + argumentArray +
                     ", i64 " + std::to_string(i));
                emit("store ptr " + values[i] + ", ptr " + slot + ", align 8");
            }
        }

        const std::string result = freshTemporary();
        emit(result + " = alloca %ZeusValue, align 8");
        if (callSymbol == "@zeus_value_native_call") {
            const std::string nameSymbol = poolStr(nativeName);
            const std::string namePointer = freshTemporary();
            emit(namePointer + " = getelementptr inbounds [" +
                 std::to_string(nativeName.size() + 1) + " x i8], ptr " +
                 nameSymbol + ", i64 0, i64 0");
            emit("call void " + callSymbol + "(ptr " + result + ", ptr " +
                 namePointer + ", i32 " + std::to_string(values.size()) +
                 ", ptr " + argumentArray + ")");
        } else {
            emit("call void " + callSymbol + "(ptr " + result + ", i32 " +
                 std::to_string(values.size()) + ", ptr " + argumentArray + ")");
        }
        return result;
    }

    std::string emitScalarAsValue(const std::string& type, const std::string& value) {
        if (type != "i64" && type != "i1") return "";

        std::string dynamicValue = freshTmp();
        emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
        if (type == "i64") {
            emitLine("call void @zeus_value_init_i64(ptr " + dynamicValue +
                     ", i64 " + value + ")");
        } else {
            std::string raw = freshTmp();
            emitLine(raw + " = zext i1 " + value + " to i32");
            emitLine("call void @zeus_value_init_bool(ptr " + dynamicValue +
                     ", i32 " + raw + ")");
        }
        return dynamicValue;
    }

    std::string emitValueAsScalar(const std::string& dynamicValue, const std::string& type) {
        if (type != "i64" && type != "i1") return "";

        std::string nativeValue;
        if (type == "i64") {
            nativeValue = freshTmp();
            emitLine(nativeValue + " = call i64 @zeus_value_require_i64(ptr " +
                     dynamicValue + ")");
        } else {
            std::string raw = freshTmp();
            emitLine(raw + " = call i32 @zeus_value_require_bool(ptr " +
                     dynamicValue + ")");
            nativeValue = freshTmp();
            emitLine(nativeValue + " = icmp ne i32 " + raw + ", 0");
        }
        emitRelease(dynamicValue);
        return nativeValue;
    }

    // Global/local slot access has to be emitted into whichever function body
    // is currently being built. The legacy top-level path uses emitLine, while a
    // native function must write into its own functionText: leaking the load
    // into the entry block would leave the use without a dominating definition.
    template <typename EmitLine>
    std::string emitSlotLoad(int index, bool global, EmitLine&& emit) {
        std::string value = freshTmp();
        emit(value + " = alloca %ZeusValue, align 8");
        emit(std::string("call void @zeus_value_") + (global ? "global" : "local") +
             "_load(ptr " + value + ", i32 " + std::to_string(index) + ")");
        return value;
    }

    // Reads a global that the AST walker did not describe, straight out of the
    // runtime slot table, and returns it in whatever representation the rest of
    // the native path expects: an unboxed scalar, a native class pointer, or a
    // boxed ZeusValue.
    bool boxNativeGlobalValue(const IRType::IRInstr& ins, int index, NativeValue& out) {
        const auto classType = prescanGlobalClassTypes.find(index);
        if (classType != prescanGlobalClassTypes.end() &&
            findClassLayout(classType->second) != nullptr) {
            const std::string boxed = emitGlobalValueLoad(index);
            const std::string unboxed = freshTmp();
            emitLine(unboxed + " = call ptr @zeus_value_require_class_ptr(ptr " +
                     boxed + ", ptr " + poolStr(classType->second) + ")");
            emitLine("call void @zeus_class_ref_retain(ptr " + unboxed + ")");
            emitLine("call void @zeus_value_release(ptr " + boxed + ")");
            out = NativeValue{ unboxed, "ptr", false, false, classType->second };
            out.ownsClass = true;
            return true;
        }

        const auto scalarType = nativeGlobalScalarTypes.find(index);
        if (scalarType != nativeGlobalScalarTypes.end()) {
            const bool isBool = scalarType->second == "i1";
            const std::string loaded = freshTmp();
            emitLine(loaded + " = call " + std::string(isBool ? "i32" : "i64") +
                     " @zeus_global_load_" + (isBool ? "bool" : "i64") +
                     "(i32 " + std::to_string(index) + ")");
            if (isBool) {
                const std::string flag = freshTmp();
                emitLine(flag + " = icmp ne i32 " + loaded + ", 0");
                out = NativeValue{ flag, "i1", false, false };
            } else {
                out = NativeValue{ loaded, "i64", false, false };
            }
            return true;
        }

        out = NativeValue{ emitGlobalValueLoad(index), "ptr", false, true };
        return true;
    }

    std::string emitGlobalValueLoad(int index) {
        return emitSlotLoad(index, true,
            [this](const std::string& line) { emitLine(line); });
    }

    std::string emitLocalValueLoad(int index) {
        return emitSlotLoad(index, false,
            [this](const std::string& line) { emitLine(line); });
    }

    void emitPushNull() {
        std::string t = freshTmp();
        emitLine(t + " = alloca %ZeusValue, align 8");
        emitLine("call void @zeus_value_init_nil(ptr " + t + ")");
        emitPushValue(t);
    }

    // Invoke a dynamic variadic function with values popped from the VM ABI stack.
    void emitDynamicVariadicIntrinsic(int64_t argc, const std::string& name) {
        std::vector<std::string> values(static_cast<size_t>(argc));
        for (int64_t i = argc - 1; i >= 0; --i) {
            values[static_cast<size_t>(i)] = emitPop();
        }
        const std::string result = emitDynamicVariadicCall(
            dynamicVariadicABISymbol(name),
            name,
            values,
            [this](const std::string& line) { emitLine(line); },
            [this]() { return freshTmp(); }
        );
        for (const auto& value : values) emitRelease(value);
        emitPushValue(result);
        pendingVariadicCall.clear();
    }

    // Convert dynamic ABI arguments to native scalars, then wrap the result.
    bool emitDirectCallFromDynamic(const IRType::IRInstr& ins, int64_t argc) {
        if (pendingDirectCall.empty()) {
            fail(ins, "[emit-llvm] 直接调用缺少目标函数标注");
            return false;
        }

        auto function = functionSignatures.find(pendingDirectCall);
        if (function == functionSignatures.end()) {
            fail(ins, "[emit-llvm] 找不到直接调用目标'" + pendingDirectCall + "'");
            return false;
        }

        const auto& called = function->second;
        if (called.captureCount != 0) {
            fail(ins, "[emit-llvm] 直接调用目标包含捕获变量，不能走原生 ABI");
            return false;
        }

        if (argc < called.minRequiredParams ||
            argc > static_cast<int64_t>(called.parameterTypes.size())) {
            fail(ins, "[emit-llvm] 原生函数调用参数数量不匹配");
            return false;
        }

        if (!pendingFunctionLoad.empty() && pendingFunctionLoad != pendingDirectCall) {
            fail(ins, "[emit-llvm] 函数引用与直接调用标注不一致");
            return false;
        }

        std::vector<std::string> dynamicArguments(static_cast<size_t>(argc));
        for (int64_t i = argc - 1; i >= 0; --i) {
            dynamicArguments[static_cast<size_t>(i)] = emitPop();
        }

        std::vector<std::string> nativeArguments;
        nativeArguments.reserve(dynamicArguments.size());
        std::vector<std::string> nativeArgumentTypes;

        for (size_t i = 0; i < called.parameterTypes.size(); ++i) {
            if (i >= dynamicArguments.size()) {
                NativeValue placeholder;
                if (!emitOptionalArgumentPlaceholder(
                        called.parameterTypes[i], placeholder,
                        [this](const std::string& line) { emitLine(line); },
                        [this]() { return freshTmp(); })) {
                    fail(ins, "[emit-llvm] optional argument has no native ABI placeholder");
                    return false;
                }
                nativeArguments.push_back(placeholder.value);
                nativeArgumentTypes.push_back(
                    functionParameterABIType(called.parameterTypes[i]));
                continue;
            }
            const std::string argumentType = functionParameterABIType(called.parameterTypes[i]);
            if (usesZeusValueABI(called.parameterTypes[i])) {
                nativeArguments.push_back(dynamicArguments[i]);
                nativeArgumentTypes.push_back(argumentType);
                continue;
            }
            if (isReferenceTypeName(called.parameterTypes[i])) {
                const std::string targetType = referencedTypeName(called.parameterTypes[i]);
                if (targetType == "int" || targetType == "int32" ||
                    targetType == "int64" || targetType == "bool") {
                    const std::string pointer = freshTmp();
                    emitLine(pointer + " = call ptr @zeus_value_require_" +
                             (targetType == "bool" ? "bool" : "i64") +
                             "_ptr(ptr " + dynamicArguments[i] + ")");
                    nativeArguments.push_back(pointer);
                } else if (isClassType(targetType)) {
                    const std::string pointer = freshTmp();
                    const std::string typeName = poolStr(targetType);
                    emitLine(pointer + " = call ptr @zeus_value_require_class_ptr(ptr " +
                             dynamicArguments[i] + ", ptr " + typeName + ")");
                    nativeArguments.push_back(pointer);
                } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
                           ZeusBuiltins::isNativeViewABI(abi)) {
                    const std::string pointer = freshTmp();
                    emitLine(pointer + " = call ptr @" +
                             std::string(ZeusBuiltins::nativeViewFromDynamicHelper(abi)) + "(ptr " +
                             dynamicArguments[i] + ")");
                    nativeArguments.push_back(pointer);
                } else {
                    fail(ins, "[emit-llvm] 该引用类型无法从 ZeusValue 解箱: &" + targetType);
                    return false;
                }
            } else if (argumentType == "i64" || argumentType == "i1") {
                nativeArguments.push_back(emitValueAsScalar(dynamicArguments[i], argumentType));
            } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(
                           called.parameterTypes[i]);
                       ZeusBuiltins::isNativeViewABI(abi)) {
                const std::string nativeView = freshTmp();
                emitLine(nativeView + " = call ptr @" +
                         std::string(ZeusBuiltins::nativeViewFromDynamicHelper(abi)) + "(ptr " +
                         dynamicArguments[i] + ")");
                nativeArguments.push_back(nativeView);
            } else if (isClassType(called.parameterTypes[i])) {
                const std::string typeName = poolStr(called.parameterTypes[i]);
                const std::string classArgument = freshTmp();
                emitLine(classArgument + " = call ptr @zeus_value_require_class_ptr(ptr " +
                         dynamicArguments[i] + ", ptr " + typeName + ")");
                nativeArguments.push_back(classArgument);
            } else {
                fail(ins, "[emit-llvm] ZeusValue 无法转换为原生 class/struct 参数");
                return false;
            }
            nativeArgumentTypes.push_back(argumentType);
        }

        const bool returnsDynamic = usesZeusValueABI(called.returnType);
        const std::string returnType = returnsDynamic ? "void" : llvmType(called.returnType);
        const bool returnsClassPointer = !returnsDynamic && isClassType(called.returnType);
        const std::string referenceTargetType = referencedTypeName(called.returnType);
        const auto* returnABI = ZeusBuiltins::findNativeTypeABI(called.returnType);
        const auto* referenceTargetABI = ZeusBuiltins::findNativeTypeABI(referenceTargetType);
        const bool returnsNativeView = ZeusBuiltins::isNativeViewABI(returnABI) ||
            ZeusBuiltins::isNativeViewABI(referenceTargetABI);
        const auto* nativeViewReturnABI = ZeusBuiltins::isNativeViewABI(returnABI)
            ? returnABI : referenceTargetABI;
        const bool returnsReferenceClassPointer = isReferenceTypeName(called.returnType) &&
            isClassType(referenceTargetType);
        const bool returnsScalarReference = referenceTargetType == "int" ||
                                            referenceTargetType == "int32" ||
                                            referenceTargetType == "int64" ||
                                            referenceTargetType == "bool";
        if (returnType.empty() || (!returnsDynamic && returnType != "i64" &&
                                   returnType != "i1" && !returnsClassPointer &&
                                   !returnsReferenceClassPointer &&
                                    !(returnType == "ptr" && returnsNativeView) &&
                                   !(returnType == "ptr" && returnsScalarReference))) {
            fail(ins, "[emit-llvm] 动态栈调用暂不支持原生 class/struct 返回值");
            return false;
        }

        std::string callResult = freshTmp();
        std::string callText;
        if (returnsDynamic) {
            emitLine(callResult + " = alloca %ZeusValue, align 8");
            callText = "call void " + llvmCallTarget(called.name) + "(ptr " + callResult;
        } else {
            callText = callResult + " = call " + returnType + " " +
                       llvmCallTarget(called.name) + "(";
        }
        for (size_t i = 0; i < nativeArguments.size(); ++i) {
            if (i > 0 || returnsDynamic) callText += ", ";
            callText += nativeArgumentTypes[i] + " " + nativeArguments[i];
        }
        if (called.hasDefaultParameters()) {
            if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
            callText += "i64 " + std::to_string(providedPrefixMask(static_cast<size_t>(argc)));
        }
        callText += ")";
        emitLine(callText);

        if (returnsDynamic) {
            emitPushValue(callResult);
        } else if (returnsClassPointer || returnsReferenceClassPointer) {
            const std::string dynamicResult = freshTmp();
            const std::string typeName = poolStr(
                returnsReferenceClassPointer ? referenceTargetType : called.returnType);
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_class_ptr(ptr " + dynamicResult +
                     ", ptr " + callResult + ", ptr " + typeName + ")");
            emitPushValue(dynamicResult);
        } else if (returnsNativeView) {
            const std::string dynamicResult = freshTmp();
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @" +
                     std::string(ZeusBuiltins::nativeViewToDynamicHelper(nativeViewReturnABI)) +
                     "(ptr " + dynamicResult +
                     ", ptr " + callResult + ")");
            emitPushValue(dynamicResult);
        } else if (returnsScalarReference) {
            const std::string dynamicResult = freshTmp();
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine(std::string("call void @zeus_value_from_") +
                     (referenceTargetType == "bool" ? "bool" : "i64") +
                     "_ptr(ptr " + dynamicResult + ", ptr " + callResult + ")");
            emitPushValue(dynamicResult);
        } else {
            std::string dynamicResult = emitScalarAsValue(returnType, callResult);
            emitPushValue(dynamicResult);
        }
        for (const auto& argument : dynamicArguments) emitRelease(argument);

        pendingDirectCall.clear();
        pendingFunctionLoad.clear();
        return true;
    }

    bool emitNativeValueAsDynamic(
        const NativeValue& value,
        const IRType::IRInstr& ins,
        std::string& dynamicValue
    ) {
        if (value.functionRef) {
            fail(ins, "[emit-llvm] 函数引用不能直接转换为 dynamic 值");
            return false;
        }

        if (value.dynamic) {
            dynamicValue = value.value;
            return true;
        }

        if (value.type == "ptr" && isReferenceTypeName(value.sourceType)) {
            const std::string targetType = referencedTypeName(value.sourceType);
            dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            if (targetType == "int" || targetType == "int32" ||
                targetType == "int64" || targetType == "bool") {
                emitLine(std::string("call void @zeus_value_from_") +
                         (targetType == "bool" ? "bool" : "i64") +
                         "_ptr(ptr " + dynamicValue + ", ptr " + value.value + ")");
            } else if (isClassType(targetType)) {
                const std::string typeName = poolStr(targetType);
                emitLine("call void @zeus_value_from_class_ptr(ptr " + dynamicValue +
                         ", ptr " + value.value + ", ptr " + typeName + ")");
            } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
                       ZeusBuiltins::isNativeViewABI(abi)) {
                emitLine("call void @" +
                         std::string(ZeusBuiltins::nativeViewToDynamicHelper(abi)) +
                         "(ptr " + dynamicValue +
                         ", ptr " + value.value + ")");
            } else {
                fail(ins, "[emit-llvm] 该引用类型不能装箱为 ZeusValue: &" + targetType);
                return false;
            }
            return true;
        }

        const auto* sourceABI = ZeusBuiltins::findNativeTypeABI(value.sourceType);
        if (value.type == "ptr" && ZeusBuiltins::isNativeViewABI(sourceABI)) {
            dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @" +
                     std::string(ZeusBuiltins::nativeViewToDynamicHelper(sourceABI)) +
                     "(ptr " + dynamicValue +
                     ", ptr " + value.value + ")");
            return true;
        }

        if (value.type == "ptr" && isClassType(value.sourceType)) {
            dynamicValue = freshTmp();
            const std::string typeName = poolStr(value.sourceType);
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_class_ptr(ptr " + dynamicValue +
                     ", ptr " + value.value + ", ptr " + typeName + ")");
            return true;
        }

        dynamicValue = emitScalarAsValue(value.type, value.value);
        if (dynamicValue.empty()) {
            fail(ins, "[emit-llvm] 原生值没有可用的动态 ABI 表示");
            return false;
        }
        return true;
    }

    void releaseNativeValue(const NativeValue& value) {
        if (value.dynamic && !value.value.empty()) {
            emitRelease(value.value);
        } else if (value.ownsClass && !value.value.empty()) {
            emitLine("call void @zeus_class_ref_release(ptr " + value.value + ")");
        }
    }

    bool emitNativeValueAsType(
        NativeValue value,
        const std::string& expectedType,
        const IRType::IRInstr& ins,
        std::string& nativeValue,
        const std::string& declaredType = std::string()
    ) {
        if (value.functionRef) {
            fail(ins, "[emit-llvm] 函数引用不能作为原生值");
            return false;
        }

        if (!value.dynamic) {
            if (value.type != expectedType ||
                (isClassType(declaredType) && value.sourceType != declaredType)) {
                fail(ins, "[emit-llvm] 原生值类型不匹配");
                return false;
            }

            nativeValue = value.value;
            return true;
        }

        if (expectedType == "ptr" && isClassType(declaredType)) {
            nativeValue = freshTmp();
            const std::string typeName = poolStr(declaredType);
            emitLine(nativeValue + " = call ptr @zeus_value_require_class_ptr(ptr " +
                     value.value + ", ptr " + typeName + ")");
            return true;
        }

        if (expectedType == "ptr" && isReferenceTypeName(declaredType)) {
            const std::string targetType = referencedTypeName(declaredType);
            if (targetType == "int" || targetType == "int32" ||
                targetType == "int64" || targetType == "bool") {
                nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @zeus_value_require_" +
                         (targetType == "bool" ? "bool" : "i64") +
                         "_ptr(ptr " + value.value + ")");
                return true;
            }
            if (isClassType(targetType)) {
                nativeValue = freshTmp();
                const std::string typeName = poolStr(targetType);
                emitLine(nativeValue + " = call ptr @zeus_value_require_class_ptr(ptr " +
                         value.value + ", ptr " + typeName + ")");
                return true;
            }
            const auto* referenceABI = ZeusBuiltins::findNativeTypeABI(targetType);
            if (ZeusBuiltins::isNativeViewABI(referenceABI)) {
                nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @" +
                         std::string(ZeusBuiltins::nativeViewFromDynamicHelper(referenceABI)) +
                         "(ptr " +
                         value.value + ")");
                return true;
            }
        }

        const auto* declaredABI = ZeusBuiltins::findNativeTypeABI(declaredType);
        if (expectedType == "ptr" && ZeusBuiltins::isNativeViewABI(declaredABI)) {
            nativeValue = freshTmp();
            emitLine(nativeValue + " = call ptr @" +
                     std::string(ZeusBuiltins::nativeViewFromDynamicHelper(declaredABI)) +
                     "(ptr " +
                     value.value + ")");
            return true;
        }

        nativeValue = emitValueAsScalar(value.value, expectedType);
        if (nativeValue.empty()) {
            fail(ins, "[emit-llvm] 目标函数参数类型不支持原生 int/bool");
            return false;
        }
        return true;
    }

    template <typename EmitLine, typename FreshTemporary,
              typename ConvertArgument, typename Fail>
    bool emitNewOperatorConstruction(
        const IRType::IRInstr& ins,
        const std::string& typeName,
        const std::vector<NativeValue>& arguments,
        NativeValue& result,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        ConvertArgument&& convertArgument,
        Fail&& failAt
    ) {
        const FunctionSignature* factory = findNewOperatorSignature(typeName);
        if (factory == nullptr) {
            return failAt(ins, "type '" + typeName + "' has no operator[new] factory");
        }
        if (factory->captureCount != 0) {
            return failAt(ins, "operator[new] cannot capture variables");
        }
        if (arguments.size() < static_cast<size_t>(factory->minRequiredParams) ||
            arguments.size() > factory->parameterTypes.size()) {
            return failAt(ins, "operator[new] argument count does not match its signature");
        }

        const bool returnsDynamic = usesZeusValueABI(factory->returnType);
        const std::string returnType = returnsDynamic
            ? "void"
            : llvmType(factory->returnType);
        if (returnType.empty() || (!returnsDynamic && returnType == "void")) {
            return failAt(ins,
                "operator[new] must return a value with a valid LLVM ABI mapping");
        }

        const std::string callResult = freshTemporary();
        std::string callText;
        if (returnsDynamic) {
            emit(callResult + " = alloca %ZeusValue, align 8");
            callText = "call void " + llvmCallTarget(factory->name) + "(ptr " + callResult;
        } else {
            callText = callResult + " = call " + returnType + " " +
                llvmCallTarget(factory->name) + "(";
        }

        std::vector<std::string> dynamicArguments;
        std::vector<bool> ownsDynamicArguments;
        for (size_t i = 0; i < factory->parameterTypes.size(); ++i) {
            std::string argument;
            bool ownsDynamic = false;
            const std::string& declaredType = factory->parameterTypes[i];
            if (i < arguments.size()) {
                if (!convertArgument(
                        arguments[i], declaredType, argument, ownsDynamic)) {
                    return false;
                }
            } else {
                NativeValue placeholder;
                if (!emitOptionalArgumentPlaceholder(
                        declaredType, placeholder, emit, freshTemporary)) {
                    return failAt(ins,
                        "operator[new] has no ABI placeholder for an omitted argument");
                }
                argument = placeholder.value;
            }

            const std::string parameterABIType =
                functionParameterABIType(declaredType);
            if (parameterABIType.empty()) {
                return failAt(ins,
                    "operator[new] parameter has no LLVM ABI mapping");
            }
            callText += ", " + parameterABIType + " " + argument;
            if (usesZeusValueABI(declaredType) && i < arguments.size()) {
                dynamicArguments.push_back(argument);
                ownsDynamicArguments.push_back(ownsDynamic);
            }
        }
        if (factory->hasDefaultParameters()) {
            callText += ", i64 " + std::to_string(providedPrefixMask(arguments.size()));
        }
        callText += ")";
        emit(callText);

        if (!returnsDynamic && isClassType(factory->returnType)) {
            emit("call void @zeus_class_ref_register(ptr " + callResult +
                 ", ptr " + classLifetimeFinalizer(factory->returnType) + ")");
        }

        std::set<std::string> releasedDynamicArguments;
        for (size_t i = 0; i < dynamicArguments.size(); ++i) {
            if (ownsDynamicArguments[i] &&
                releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                emit("call void @zeus_value_release(ptr " + dynamicArguments[i] + ")");
            }
        }
        for (const auto& argument : arguments) {
            if (argument.dynamic && argument.ownsDynamic &&
                releasedDynamicArguments.insert(argument.value).second) {
                emit("call void @zeus_value_release(ptr " + argument.value + ")");
            }
            if (isManagedClassValue(argument) && argument.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + argument.value + ")");
            }
        }

        result = NativeValue{
            callResult,
            returnsDynamic ? "ptr" : returnType,
            false,
            returnsDynamic,
            factory->returnType,
            returnsDynamic
        };
        result.ownsClass = !returnsDynamic && isClassType(factory->returnType);
        return true;
    }

    template <typename EmitLine, typename EmitLabel, typename FreshTemporary,
              typename ConvertArgument, typename Fail>
    bool emitClassConstruction(
        const IRType::IRInstr& ins,
        const std::string& className,
        const std::vector<NativeValue>& arguments,
        NativeValue& instance,
        EmitLine&& emit,
        EmitLabel&& emitLabel,
        FreshTemporary&& freshTemporary,
        ConvertArgument&& convertArgument,
        Fail&& failAt
    ) {
        if (findNewOperatorSignature(className) != nullptr) {
            return emitNewOperatorConstruction(
                ins,
                className,
                arguments,
                instance,
                emit,
                freshTemporary,
                convertArgument,
                failAt);
        }

        const ClassLayout* layout = findClassLayout(className);
        if (layout == nullptr) {
            return failAt(ins, "[emit-llvm] new 目标没有 LLVM class 布局");
        }
        if (layout->fieldNames.size() != layout->fieldTypes.size()) {
            return failAt(ins, "[emit-llvm] class 布局的字段名与类型数量不一致");
        }

        const FunctionSignature* constructor = nullptr;
        for (const auto& entry : functionSignatures) {
            const auto& candidate = entry.second;
            if (candidate.ownerType == className &&
                candidate.name == className + "::init") {
                constructor = &candidate;
                break;
            }
        }
        if (constructor == nullptr && !arguments.empty()) {
            return failAt(ins, "[emit-llvm] class 没有 init 构造函数");
        }
        if (constructor != nullptr && constructor->captureCount != 0) {
            return failAt(ins, "[emit-llvm] init 构造函数包含捕获变量，不能走原生 ABI");
        }
        if (constructor != nullptr &&
            (constructor->parameterTypes.empty() ||
             arguments.size() + 1 < static_cast<size_t>(constructor->minRequiredParams) ||
             arguments.size() + 1 > constructor->parameterTypes.size() ||
             constructor->parameterTypes.front() != className)) {
            return failAt(ins, "[emit-llvm] init 构造函数参数数量或类型不匹配");
        }

        std::string allocationSize;
        if (layout->fieldNames.empty()) {
            allocationSize = "1";
        } else {
            const std::string sizePointer = freshTemporary();
            allocationSize = freshTemporary();
            emit(sizePointer + " = getelementptr " + llvmStructType(className) +
                 ", ptr null, i32 1");
            emit(allocationSize + " = ptrtoint ptr " + sizePointer + " to i64");
        }

        const std::string allocationKey = freshTemporary();
        const std::string allocationStem = allocationKey.size() > 1 &&
            allocationKey.front() == '%' ? allocationKey.substr(1) : allocationKey;
        const std::string object = freshTemporary();
        emit(object + " = call ptr @malloc(i64 " + allocationSize + ")");
        const std::string isNull = freshTemporary();
        const std::string allocationFailed = "new_fail_" + allocationStem;
        const std::string allocationReady = "new_ready_" + allocationStem;
        emit(isNull + " = icmp eq ptr " + object + ", null");
        emit("br i1 " + isNull + ", label %" + allocationFailed +
             ", label %" + allocationReady);
        emitLabel(allocationFailed);
        emit("call void @llvm.trap()");
        emit("unreachable");
        emitLabel(allocationReady);

        for (size_t fieldIndex = 0; fieldIndex < layout->fieldNames.size(); ++fieldIndex) {
            const std::string& fieldTypeName = layout->fieldTypes[fieldIndex];
            const std::string fieldType = llvmFieldType(fieldTypeName);
            const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldTypeName);
            const bool isDynamicField = usesZeusValueABI(fieldTypeName);
            const bool isNativeViewField = ZeusBuiltins::isNativeViewABI(fieldABI);
            if (fieldType.empty() || (!isDynamicField &&
                (fieldTypeName.empty() ||
                 (fieldTypeName != "int" && fieldTypeName != "int32" &&
                  fieldTypeName != "int64" && fieldTypeName != "bool" &&
                  !isNativeViewField && fieldTypeName != "nil" &&
                 !isClassType(fieldTypeName))))) {
                return failAt(ins, "[emit-llvm] class 字段类型没有原生存储 ABI");
            }

            IRType::ClassFieldDefault fieldDefault;
            if (fieldIndex < layout->fieldDefaults.size()) {
                fieldDefault = layout->fieldDefaults[fieldIndex];
            }
            if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Unsupported) {
                return failAt(ins, "[emit-llvm] class 字段默认值不能原生初始化");
            }

            const std::string fieldAddress = freshTemporary();
            emit(fieldAddress + " = getelementptr inbounds " + llvmStructType(className) +
                 ", ptr " + object + ", i32 0, i32 " + std::to_string(fieldIndex));

            if (isDynamicField) {
                switch (fieldDefault.kind) {
                    case IRType::ClassFieldDefaultKind::Zero:
                    case IRType::ClassFieldDefaultKind::Null:
                        emit("call void @zeus_value_init_nil(ptr " + fieldAddress + ")");
                        break;
                    case IRType::ClassFieldDefaultKind::Integer:
                        emit("call void @zeus_value_init_i64(ptr " + fieldAddress +
                             ", i64 " + std::to_string(fieldDefault.integerValue) + ")");
                        break;
                    case IRType::ClassFieldDefaultKind::Boolean:
                        emit("call void @zeus_value_init_bool(ptr " + fieldAddress +
                             ", i32 " + (fieldDefault.booleanValue ? "1" : "0") + ")");
                        break;
                    case IRType::ClassFieldDefaultKind::String: {
                        const std::string symbol = poolStr(fieldDefault.stringValue);
                        const std::string stringPointer = freshTemporary();
                        emit(stringPointer + " = getelementptr inbounds [" +
                             std::to_string(fieldDefault.stringValue.size() + 1) +
                             " x i8], ptr " + symbol + ", i64 0, i64 0");
                        emit("call void @zeus_value_from_string_data(ptr " + fieldAddress +
                             ", ptr " + stringPointer + ", i64 " +
                             std::to_string(fieldDefault.stringValue.size()) + ")");
                        break;
                    }
                    case IRType::ClassFieldDefaultKind::Bytes: {
                        const std::string symbol = poolStr(fieldDefault.bytesValue);
                        const std::string bytesPointer = freshTemporary();
                        emit(bytesPointer + " = getelementptr inbounds [" +
                             std::to_string(fieldDefault.bytesValue.size() + 1) +
                             " x i8], ptr " + symbol + ", i64 0, i64 0");
                        emit("call void @zeus_value_from_bytes(ptr " + fieldAddress +
                             ", ptr " + bytesPointer + ", i64 " +
                             std::to_string(fieldDefault.bytesValue.size()) + ")");
                        break;
                    }
                    case IRType::ClassFieldDefaultKind::EmptyArray:
                        emit("call void @zeus_value_array_build(ptr " + fieldAddress +
                             ", i64 0, ptr null)");
                        break;
                    case IRType::ClassFieldDefaultKind::EmptyDict:
                        emit("call void @zeus_value_dict_build(ptr " + fieldAddress +
                             ", i64 0, ptr null)");
                        break;
                    case IRType::ClassFieldDefaultKind::Unsupported:
                        return failAt(ins, "[emit-llvm] class 动态字段默认值不能原生初始化");
                }
                continue;
            }

            std::string initializer;
            if (fieldType == "i64") {
                if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Integer) {
                    initializer = std::to_string(fieldDefault.integerValue);
                } else if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Zero) {
                    initializer = "0";
                } else {
                    return failAt(ins, "[emit-llvm] int 字段默认值类型不匹配");
                }
            } else if (fieldType == "i1") {
                if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Boolean) {
                    initializer = fieldDefault.booleanValue ? "1" : "0";
                } else if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Zero) {
                    initializer = "0";
                } else {
                    return failAt(ins, "[emit-llvm] bool 字段默认值类型不匹配");
                }
            } else if (fieldType == "ptr") {
                if (fieldDefault.kind == IRType::ClassFieldDefaultKind::Zero ||
                    fieldDefault.kind == IRType::ClassFieldDefaultKind::Null) {
                    initializer = "null";
                } else if ((fieldDefault.kind == IRType::ClassFieldDefaultKind::String &&
                            fieldABI != nullptr &&
                            fieldABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) ||
                           (fieldDefault.kind == IRType::ClassFieldDefaultKind::Bytes &&
                            fieldABI != nullptr &&
                            fieldABI->valueABI == ZeusBuiltins::NativeValueABI::BytesView)) {
                    const std::string& contents =
                        fieldDefault.kind == IRType::ClassFieldDefaultKind::Bytes
                            ? fieldDefault.bytesValue : fieldDefault.stringValue;
                    const std::string symbol = poolStr(contents);
                    const std::string stringPointer = freshTemporary();
                    emit(stringPointer + " = getelementptr inbounds [" +
                         std::to_string(contents.size() + 1) +
                         " x i8], ptr " + symbol + ", i64 0, i64 0");
                    initializer = freshTemporary();
                    emit(initializer + " = call ptr @" +
                         std::string(ZeusBuiltins::nativeViewFromDataHelper(fieldABI)) +
                         "(ptr " +
                         stringPointer + ", i64 " +
                         std::to_string(contents.size()) + ")");
                } else {
                    return failAt(ins, "[emit-llvm] pointer 字段默认值类型不匹配");
                }
            } else {
                return failAt(ins, "[emit-llvm] class 字段 LLVM 类型暂不支持");
            }

            emit("store " + fieldType + " " + initializer + ", ptr " + fieldAddress);
        }

        emit("call void @zeus_class_ref_register(ptr " + object + ", ptr " +
             classLifetimeFinalizer(className) + ")");

        std::set<std::string> releasedDynamicArguments;
        if (constructor != nullptr) {
            const bool constructorReturnsDynamic = usesZeusValueABI(constructor->returnType);
            const std::string constructorReturnType = constructorReturnsDynamic
                ? "void"
                : llvmType(constructor->returnType);
            if (constructorReturnType.empty()) {
                return failAt(ins, "[emit-llvm] init 构造函数返回类型没有 LLVM ABI 表示");
            }

            std::string constructorResult;
            std::string callText;
            if (constructorReturnsDynamic) {
                constructorResult = freshTemporary();
                emit(constructorResult + " = alloca %ZeusValue, align 8");
                callText = "call void " + llvmCallTarget(constructor->name) +
                           "(ptr " + constructorResult + ", ptr " + object;
            } else {
                callText = "call " + constructorReturnType + " " +
                           llvmCallTarget(constructor->name) + "(ptr " + object;
            }

            std::vector<std::string> dynamicArguments;
            std::vector<bool> ownsDynamicArguments;
            for (size_t i = 0; i + 1 < constructor->parameterTypes.size(); ++i) {
                std::string argument;
                bool ownsDynamic = false;
                const std::string& declaredType = constructor->parameterTypes[i + 1];
                if (i < arguments.size()) {
                    if (!convertArgument(arguments[i], declaredType, argument, ownsDynamic)) {
                        return false;
                    }
                } else {
                    NativeValue placeholder;
                    if (!emitOptionalArgumentPlaceholder(
                            declaredType, placeholder, emit, freshTemporary)) {
                        return failAt(ins,
                            "[emit-llvm] init constructor has no ABI placeholder for an omitted argument");
                    }
                    argument = placeholder.value;
                }
                callText += ", " + functionParameterABIType(declaredType) + " " + argument;
                if (usesZeusValueABI(declaredType) && i < arguments.size()) {
                    dynamicArguments.push_back(argument);
                    ownsDynamicArguments.push_back(ownsDynamic);
                }
            }
            if (constructor->hasDefaultParameters()) {
                callText += ", i64 " + std::to_string(providedPrefixMask(arguments.size() + 1));
            }
            callText += ")";
            emit(callText);
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i] &&
                    releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                    emit("call void @zeus_value_release(ptr " + dynamicArguments[i] + ")");
                }
            }
            if (constructorReturnsDynamic) {
                emit("call void @zeus_value_release(ptr " + constructorResult + ")");
            }
        }

        for (const auto& argument : arguments) {
            if (argument.dynamic && argument.ownsDynamic &&
                releasedDynamicArguments.insert(argument.value).second) {
                emit("call void @zeus_value_release(ptr " + argument.value + ")");
            }
            if (isManagedClassValue(argument) && argument.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + argument.value + ")");
            }
        }

        instance = NativeValue{ object, "ptr", false, false, className };
        instance.ownsClass = true;
        return true;
    }

    std::string emitClassLifetimeFinalizers() const {
        std::string generated;
        for (const auto& entry : classLayouts) {
            const std::string& className = entry.first;
            const ClassLayout& layout = entry.second;
            const FunctionSignature* destructor = nullptr;
            for (const auto& candidate : functionSignatures) {
                if (candidate.second.ownerType == className &&
                    candidate.second.operatorSymbol == "~") {
                    destructor = &candidate.second;
                    break;
                }
            }

            generated += "define void " + classLifetimeFinalizer(className) +
                "(ptr %object) personality ptr @__CxxFrameHandler3 {\nentry:\n";
            int temporaryIndex = 0;
            auto fresh = [&]() {
                return "%life" + std::to_string(temporaryIndex++);
            };

            if (destructor != nullptr) {
                const bool returnsDynamic = usesZeusValueABI(destructor->returnType);
                if (returnsDynamic) {
                    const std::string result = fresh();
                    generated += "  " + result + " = alloca %ZeusValue, align 8\n";
                    generated += "  call void " + llvmCallTarget(destructor->name) +
                        "(ptr " + result + ", ptr %object)\n";
                    generated += "  call void @zeus_value_release(ptr " + result + ")\n";
                } else {
                    const std::string returnType = llvmType(destructor->returnType);
                    if (returnType.empty() || returnType == "void") {
                        generated += "  call void " + llvmCallTarget(destructor->name) +
                            "(ptr %object)\n";
                    } else {
                        generated += "  call " + returnType + " " +
                            llvmCallTarget(destructor->name) + "(ptr %object)\n";
                    }
                }
            }

            for (size_t fieldIndex = 0; fieldIndex < layout.fieldTypes.size(); ++fieldIndex) {
                const std::string& fieldType = layout.fieldTypes[fieldIndex];
                const bool classField = isClassType(fieldType);
                if (!usesZeusValueABI(fieldType) && !classField) continue;

                const std::string address = fresh();
                generated += "  " + address + " = getelementptr inbounds " +
                    llvmStructType(className) + ", ptr %object, i32 0, i32 " +
                    std::to_string(fieldIndex) + "\n";
                if (classField) {
                    const std::string fieldValue = fresh();
                    generated += "  " + fieldValue + " = load ptr, ptr " + address + "\n";
                    generated += "  call void @zeus_class_ref_release(ptr " +
                        fieldValue + ")\n";
                } else {
                    generated += "  call void @zeus_value_release(ptr " + address + ")\n";
                }
            }

            generated += "  ret void\n}\n\n";
        }
        return generated;
    }

    template <typename EmitLine, typename EmitLabel, typename FreshTemporary, typename Fail>
    bool emitClassDelete(
        const IRType::IRInstr& ins,
        const NativeValue& object,
        EmitLine&& emit,
        EmitLabel&& emitLabel,
        FreshTemporary&& freshTemporary,
        Fail&& failAt
    ) {
        if (object.type != "ptr" || !isClassType(object.sourceType)) {
            return failAt(ins, "[emit-llvm] delete 目标不是 class 实例");
        }

        const FunctionSignature* destructor = nullptr;
        for (const auto& entry : functionSignatures) {
            const auto& candidate = entry.second;
            if (candidate.ownerType == object.sourceType &&
                candidate.operatorSymbol == "~") {
                destructor = &candidate;
                break;
            }
        }

        bool destructorReturnsDynamic = false;
        std::string destructorReturnType;
        if (destructor != nullptr) {
            if (destructor->parameterTypes.size() != 1 ||
                destructor->parameterTypes.front() != object.sourceType) {
                return failAt(ins, "[emit-llvm] operator[~] 必须只接受 class 实例");
            }
            destructorReturnsDynamic = usesZeusValueABI(destructor->returnType);
            destructorReturnType = destructorReturnsDynamic
                ? "void"
                : llvmType(destructor->returnType);
            if (destructorReturnType.empty()) {
                return failAt(ins, "[emit-llvm] operator[~] 必须声明可表示的返回类型");
            }
        }

        const std::string isNull = freshTemporary();
        const std::string deleteDone = freshTemporary();
        const std::string deleteLive = freshTemporary();
        emit(isNull + " = icmp eq ptr " + object.value + ", null");
        emit("br i1 " + isNull + ", label " + deleteDone +
             ", label " + deleteLive);
        emitLabel(deleteLive);

        const std::string managedResult = freshTemporary();
        const std::string isManaged = freshTemporary();
        const std::string managedLabel = freshTemporary();
        const std::string legacyLabel = freshTemporary();
        emit(managedResult + " = call i32 @zeus_class_ref_destroy_now(ptr " +
             object.value + ")");
        emit(isManaged + " = icmp ne i32 " + managedResult + ", 0");
        emit("br i1 " + isManaged + ", label " + managedLabel +
             ", label " + legacyLabel);
        emitLabel(managedLabel);
        emit("call void @zeus_class_ref_release(ptr " + object.value + ")");
        emit("br label " + deleteDone);
        emitLabel(legacyLabel);

        if (destructor != nullptr) {
            if (destructorReturnsDynamic) {
                const std::string destructorResult = freshTemporary();
                emit(destructorResult + " = alloca %ZeusValue, align 8");
                emit(
                    "call void " + llvmCallTarget(destructor->name) +
                    "(ptr " + destructorResult + ", ptr " + object.value + ")"
                );
                emit("call void @zeus_value_release(ptr " + destructorResult + ")");
            } else {
                emit(
                    "call " + destructorReturnType + " " +
                    llvmCallTarget(destructor->name) + "(ptr " + object.value + ")"
                );
            }
        }

        const ClassLayout* layout = findClassLayout(object.sourceType);
        if (layout != nullptr) {
            for (size_t fieldIndex = 0; fieldIndex < layout->fieldTypes.size(); ++fieldIndex) {
                const std::string& fieldType = layout->fieldTypes[fieldIndex];
                const bool classField = isClassType(fieldType);
                if (!usesZeusValueABI(fieldType) && !classField) continue;
                const std::string fieldAddress = freshTemporary();
                emit(fieldAddress + " = getelementptr inbounds " +
                     llvmStructType(object.sourceType) + ", ptr " + object.value +
                     ", i32 0, i32 " + std::to_string(fieldIndex));
                if (classField) {
                    const std::string fieldValue = freshTemporary();
                    emit(fieldValue + " = load ptr, ptr " + fieldAddress);
                    emit("call void @zeus_class_ref_release(ptr " + fieldValue + ")");
                } else {
                    emit("call void @zeus_value_release(ptr " + fieldAddress + ")");
                }
            }
        }

        emit("call void @zeus_class_pointer_invalidate(ptr " + object.value + ")");
        emit("call void @free(ptr " + object.value + ")");
        emit("br label " + deleteDone);
        emitLabel(deleteDone);
        return true;
    }

    template <typename EmitLine, typename FreshTemporary, typename ConvertArgument,
              typename ReleaseDynamic, typename Fail>
    bool emitKnownClassMethodCall(
        const IRType::IRInstr& ins,
        const std::string& methodName,
        const std::vector<NativeValue>& arguments,
        NativeValue& resultValue,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        ConvertArgument&& convertArgument,
        ReleaseDynamic&& releaseDynamic,
        Fail&& failAt,
        std::function<void(bool, const std::vector<NativeValue>&,
                           const std::vector<std::string>&)> exceptionCallScope = {}
    ) {
        if (arguments.empty()) {
            return failAt(ins, "[emit-llvm] class method call is missing its receiver");
        }

        const NativeValue& receiver = arguments.front();
        const bool classReceiver = !receiver.sourceType.empty() &&
            isClassType(receiver.sourceType);
        const bool staticClassReceiver = classReceiver &&
            receiver.type == "classref" && !receiver.dynamic;
        const auto* nativeReceiverABI = receiver.sourceType.empty()
            ? nullptr
            : ZeusBuiltins::findNativeTypeABI(receiver.sourceType);
        const bool nativeReceiver = nativeReceiverABI != nullptr;
        const bool receiverTypeMatches = classReceiver
            ? (receiver.type == "ptr" || staticClassReceiver)
            : nativeReceiver && (receiver.dynamic
                ? receiver.type == "ptr"
                : receiver.type == llvmType(receiver.sourceType));
        if (receiver.functionRef || receiver.sourceType.empty() ||
            !receiverTypeMatches) {
            return failAt(
                ins,
                "[emit-llvm] typed method call requires a matching class or native receiver ABI"
            );
        }

        const std::string qualifiedName = receiver.sourceType + "::" + methodName;
        const auto function = functionSignatures.find(qualifiedName);
        const size_t suppliedCount = arguments.size() - (staticClassReceiver ? 1 : 0);
        if (function == functionSignatures.end() ||
            function->second.ownerType != receiver.sourceType ||
            suppliedCount < static_cast<size_t>(function->second.minRequiredParams) ||
            suppliedCount > function->second.parameterTypes.size() ||
            (staticClassReceiver && !function->second.isStaticMethod) ||
            (!staticClassReceiver && (function->second.isStaticMethod ||
                function->second.parameterTypes.empty() ||
                function->second.parameterTypes.front() != receiver.sourceType))) {
            return failAt(
                ins,
                "[emit-llvm] no matching typed method LLVM signature: " + qualifiedName
            );
        }

        const FunctionSignature& called = function->second;
        const bool returnsDynamic = usesZeusValueABI(called.returnType);
        const std::string returnType = returnsDynamic
            ? "void"
            : llvmType(called.returnType);
        if (returnType.empty()) {
            return failAt(ins, "[emit-llvm] class method return type has no LLVM ABI mapping");
        }

        if (exceptionCallScope) exceptionCallScope(true, arguments, {});
        if (!staticClassReceiver && isManagedClassValue(receiver)) {
            emit("call void @zeus_class_ref_require_live(ptr " + receiver.value + ")");
        }

        std::vector<std::string> nativeArguments;
        std::vector<std::string> cleanupValues;
        nativeArguments.reserve(called.parameterTypes.size());
        for (size_t i = 0; i < called.parameterTypes.size(); ++i) {
            const std::string& parameterType = called.parameterTypes[i];
            if (functionParameterABIType(parameterType).empty()) {
                return failAt(ins, "[emit-llvm] class method parameter has no LLVM ABI mapping");
            }

            std::string nativeArgument;
            if (i < suppliedCount) {
                std::string cleanupValue;
                if (!convertArgument(
                        arguments[i + (staticClassReceiver ? 1 : 0)],
                        parameterType, nativeArgument, cleanupValue)) {
                    return false;
                }
                if (!cleanupValue.empty()) {
                    cleanupValues.push_back(std::move(cleanupValue));
                    if (exceptionCallScope) {
                        exceptionCallScope(true, arguments, cleanupValues);
                    }
                }
            } else {
                NativeValue placeholder;
                if (!emitOptionalArgumentPlaceholder(
                        parameterType, placeholder, emit, freshTemporary)) {
                    return failAt(ins,
                        "[emit-llvm] class method has no ABI placeholder for an omitted argument");
                }
                nativeArgument = placeholder.value;
            }
            nativeArguments.push_back(std::move(nativeArgument));
        }

        const std::string callResult = freshTemporary();
        std::string callText;
        if (returnsDynamic) {
            emit(callResult + " = alloca %ZeusValue, align 8");
            callText = "call void " + llvmCallTarget(called.name) +
                       "(ptr " + callResult;
        } else {
            callText = callResult + " = call " + returnType + " " +
                       llvmCallTarget(called.name) + "(";
        }
        for (size_t i = 0; i < nativeArguments.size(); ++i) {
            if (i > 0 || returnsDynamic) callText += ", ";
            callText += functionParameterABIType(called.parameterTypes[i]) +
                        " " + nativeArguments[i];
        }
        if (called.hasDefaultParameters()) {
            if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
            callText += "i64 " + std::to_string(providedPrefixMask(suppliedCount));
        }
        callText += ")";
        emit(callText);
        if (exceptionCallScope) exceptionCallScope(false, {}, {});

        std::set<std::string> releasedDynamicArguments;
        for (const auto& cleanupValue : cleanupValues) {
            if (releasedDynamicArguments.insert(cleanupValue).second) {
                releaseDynamic(cleanupValue);
            }
        }
        for (const auto& argument : arguments) {
            if (argument.dynamic && argument.ownsDynamic &&
                releasedDynamicArguments.insert(argument.value).second) {
                releaseDynamic(argument.value);
            }
        }
        for (const auto& argument : arguments) {
            if (isManagedClassValue(argument) && argument.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + argument.value + ")");
            }
        }

        if (returnsDynamic) {
            resultValue = NativeValue{
                callResult, "ptr", false, true, called.returnType, true
            };
            return true;
        }

        const auto* returnABI = ZeusBuiltins::findNativeTypeABI(called.returnType);
        if (ZeusBuiltins::isNativeViewABI(returnABI)) {
            const std::string stableView = freshTemporary();
            emit(stableView + " = call ptr @" +
                 std::string(ZeusBuiltins::nativeViewInternHelper(returnABI)) + "(ptr " +
                 callResult + ")");
            resultValue = NativeValue{
                stableView, returnType, false, false, called.returnType, false
            };
        } else {
            resultValue = NativeValue{
                callResult, returnType, false, false, called.returnType, false
            };
        }
        resultValue.ownsClass = !returnsDynamic && isClassType(called.returnType);
        return true;
    }

    template <typename EmitLine, typename FreshTemporary, typename ToDynamic,
              typename ReleaseDynamic, typename Fail>
    bool emitDynamicMemberCall(
        const IRType::IRInstr& ins,
        const std::string& methodName,
        const std::vector<NativeValue>& arguments,
        NativeValue& resultValue,
        EmitLine&& emit,
        FreshTemporary&& freshTemporary,
        ToDynamic&& toDynamic,
        ReleaseDynamic&& releaseDynamic,
        Fail&& failAt,
        std::function<void(bool, const std::vector<NativeValue>&,
                           const std::vector<std::string>&)> exceptionCallScope = {}
    ) {
        if (arguments.empty()) {
            return failAt(ins, "dynamic member call is missing its receiver");
        }

        std::vector<std::string> ownedDynamicArguments;
        if (exceptionCallScope) {
            exceptionCallScope(true, arguments, ownedDynamicArguments);
        }
        std::vector<std::string> dynamicValues(arguments.size());
        std::vector<bool> ownsDynamicValues(arguments.size(), false);
        for (size_t i = 0; i < arguments.size(); ++i) {
            bool ownsDynamic = false;
            if (arguments[i].functionRef ||
                !toDynamic(arguments[i], dynamicValues[i], ownsDynamic)) {
                return failAt(ins, "dynamic member argument cannot be boxed as ZeusValue");
            }
            ownsDynamicValues[i] = ownsDynamic;
            if (ownsDynamic) {
                ownedDynamicArguments.push_back(dynamicValues[i]);
                if (exceptionCallScope) {
                    exceptionCallScope(true, arguments, ownedDynamicArguments);
                }
            }
        }

        std::string argumentArray = "null";
        if (arguments.size() > 1) {
            const size_t argumentCount = arguments.size() - 1;
            argumentArray = freshTemporary();
            emit(argumentArray + " = alloca ptr, i64 " +
                 std::to_string(argumentCount) + ", align 8");
            for (size_t i = 0; i < argumentCount; ++i) {
                const std::string slot = freshTemporary();
                emit(slot + " = getelementptr inbounds ptr, ptr " + argumentArray +
                     ", i64 " + std::to_string(i));
                emit("store ptr " + dynamicValues[i + 1] + ", ptr " + slot + ", align 8");
            }
        }

        const std::string methodSymbol = poolStr(methodName);
        const std::string methodPointer = freshTemporary();
        emit(methodPointer + " = getelementptr inbounds [" +
             std::to_string(methodName.size() + 1) + " x i8], ptr " + methodSymbol +
             ", i64 0, i64 0");

        const std::string dynamicResult = freshTemporary();
        emit(dynamicResult + " = alloca %ZeusValue, align 8");
        emit("call void @zeus_value_dynamic_member_call(ptr " + dynamicResult +
             ", ptr " + dynamicValues[0] + ", ptr " + methodPointer + ", i32 " +
             std::to_string(arguments.size() - 1) + ", ptr " + argumentArray + ")");
        if (exceptionCallScope) exceptionCallScope(false, {}, {});

        for (size_t i = 0; i < dynamicValues.size(); ++i) {
            if (ownsDynamicValues[i]) releaseDynamic(dynamicValues[i]);
        }
        for (const auto& argument : arguments) {
            if (isManagedClassValue(argument) && argument.ownsClass) {
                emit("call void @zeus_class_ref_release(ptr " + argument.value + ")");
            }
        }

        resultValue = NativeValue{
            dynamicResult, "ptr", false, true, std::string(), true
        };
        return true;
    }

    bool emitNativeFunction(
        const std::vector<IRType::IRInstr>& chunk,
        size_t ci
    ) {
        auto signatureIt = functionSignaturesByChunk.find(ci);
        if (signatureIt == functionSignaturesByChunk.end()) {
            result.error = "[emit-llvm] 函数 chunk 缺少签名标注";
            return false;
        }

        const FunctionSignature& signature = signatureIt->second;

        std::vector<int64_t> exceptionTarget(chunk.size(), -1);
        std::vector<size_t> exceptionStack;
        std::vector<size_t> catchBodyStack;
        std::map<size_t, size_t> catchBeginForEnd;
        std::map<size_t, size_t> catchEndForBegin;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& instruction = chunk[pc];
            if (instruction.op == VMOperator::TRY_BEGIN) {
                int64_t offset = 0;
                if (!tryArgInt(instruction, 0, offset) || offset <= 0 ||
                    static_cast<uint64_t>(offset) >= chunk.size() - pc ||
                    chunk[pc + static_cast<size_t>(offset)].op != VMOperator::CATCH_BEGIN) {
                    result.error = "[emit-llvm] invalid TRY_BEGIN catch target";
                    result.errorLine = instruction.line;
                    return false;
                }
                exceptionStack.push_back(pc + static_cast<size_t>(offset));
            } else if (instruction.op == VMOperator::TRY_END) {
                if (exceptionStack.empty()) {
                    result.error = "[emit-llvm] TRY_END without TRY_BEGIN";
                    result.errorLine = instruction.line;
                    return false;
                }
                exceptionStack.pop_back();
            } else if (!exceptionStack.empty()) {
                exceptionTarget[pc] = static_cast<int64_t>(exceptionStack.back());
            }
            if (instruction.op == VMOperator::CATCH_BEGIN) {
                catchBodyStack.push_back(pc);
            } else if (instruction.op == VMOperator::CATCH_END) {
                if (catchBodyStack.empty()) {
                    result.error = "[emit-llvm] CATCH_END without CATCH_BEGIN";
                    return false;
                }
                catchBeginForEnd[pc] = catchBodyStack.back();
                catchEndForBegin[catchBodyStack.back()] = pc;
                catchBodyStack.pop_back();
            }
        }
        if (!exceptionStack.empty() || !catchBodyStack.empty()) {
            result.error = "[emit-llvm] unterminated TRY_BEGIN";
            return false;
        }
        const bool hasNativeTry = std::find_if(chunk.begin(), chunk.end(),
            [](const IRType::IRInstr& instruction) {
                return instruction.op == VMOperator::TRY_BEGIN;
            }) != chunk.end();
        needsRuntimeErrorFactory = needsRuntimeErrorFactory || hasNativeTry;

        std::set<int> referenceCapturedLocals;
        if (programChunks) {
            for (const auto& instruction : chunk) {
                if (instruction.op != VMOperator::MAKE_FUNCTION ||
                    instruction.args.empty() ||
                    !std::holds_alternative<LexerType::Value>(instruction.args[0])) {
                    continue;
                }
                const int childChunk = VMHelpers::getIntFromValue(
                    std::get<LexerType::Value>(instruction.args[0]));
                if (childChunk < 0) continue;
                for (const auto& capture : captureDescriptors(static_cast<size_t>(childChunk))) {
                    if (capture.sourceKind == "local" && capture.byReference &&
                        capture.sourceIndex >= 0) {
                        referenceCapturedLocals.insert(capture.sourceIndex);
                    }
                }
            }
        }

        const bool returnsDynamic = usesZeusValueABI(signature.returnType);
        const std::string returnType = returnsDynamic ? "void" : llvmType(signature.returnType);
        if (returnType.empty()) {
            result.error = "[emit-llvm] 函数 '" + signature.name +
                           "' 的返回类型没有 LLVM ABI 映射";
            return false;
        }

        std::vector<std::string> parameterLLVMTypes;
        for (const auto& typeName : signature.parameterTypes) {
            const std::string type = functionParameterABIType(typeName);
            if (type.empty()) {
                result.error = "[emit-llvm] 函数 '" + signature.name +
                               "' 的参数类型没有 LLVM ABI 映射";
                return false;
            }
            parameterLLVMTypes.push_back(type);
        }

        std::string functionText;
        functionText += "define " + returnType + " " + llvmCallTarget(signature) + "(";
        bool hasFunctionParameter = false;
        if (signature.captureCount != 0) {
            functionText += "ptr %closure.env";
            hasFunctionParameter = true;
        }
        if (returnsDynamic) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "ptr %return.out";
            hasFunctionParameter = true;
        }
        for (size_t i = 0; i < parameterLLVMTypes.size(); ++i) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += parameterLLVMTypes[i] + " %arg" + std::to_string(i);
            hasFunctionParameter = true;
        }
        if (signature.hasVariadicArgs) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "ptr %varargs";
            hasFunctionParameter = true;
        }
        if (signature.hasVariadicKwargs) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "ptr %kwargs";
            hasFunctionParameter = true;
        }
        if (signature.hasDefaultParameters()) {
            if (hasFunctionParameter) functionText += ", ";
            functionText += "i64 %provided.mask";
        }
        functionText += ") personality ptr @__CxxFrameHandler3 {\nentry:\n";
        const size_t nativeEntryBody = functionText.size();

        int nativeTmpCount = 0;
        auto freshNativeTmp = [&]() {
            return std::string("%n") + std::to_string(nativeTmpCount++);
        };
        struct NativeExceptionPad {
            std::string switchLabel;
            std::string padLabel;
            std::string resumeLabel;
            std::string valueSlot;
            size_t catchPc = 0;
            bool cleanupOnly = false;
            std::vector<std::string> cleanupCalls;
        };
        std::vector<NativeExceptionPad> exceptionPads;
        std::map<size_t, std::string> exceptionValueSlots;
        int64_t activeExceptionTarget = -1;
        size_t activeExceptionPc = 0;
        bool suppressExceptionWrapping = false;
        std::function<void(size_t, const std::string&)> recordExceptionEdge;
        std::function<void(const std::string&)> enterExceptionContinuation;
        std::function<std::vector<std::string>(bool)> exceptionCleanupSnapshot;
        auto emitNativeLine = [&](const std::string& line) {
            const size_t callAt = line.rfind("call ", 0) == 0 ? 0 : line.find(" = call ");
            const bool isReferenceCountMutation =
                line.find("@zeus_class_ref_retain") != std::string::npos ||
                line.find("@zeus_class_ref_release") != std::string::npos ||
                line.find("@zeus_value_release") != std::string::npos ||
                line.find("@zeus_capture_cell_release") != std::string::npos;
            if (suppressExceptionWrapping || callAt == std::string::npos ||
                line.find("@llvm.", callAt) != std::string::npos ||
                isReferenceCountMutation) {
                functionText += "  " + line + "\n";
                return;
            }
            const bool hasLocalCatch = activeExceptionTarget >= 0 &&
                                       recordExceptionEdge;
            std::vector<std::string> cleanupCalls;
            if (exceptionCleanupSnapshot) {
                cleanupCalls = exceptionCleanupSnapshot(hasLocalCatch);
            }
            if (!hasLocalCatch && cleanupCalls.empty()) {
                functionText += "  " + line + "\n";
                return;
            }

            const std::string stem = std::to_string(exceptionPads.size());
            NativeExceptionPad pad;
            pad.switchLabel = "%eh_switch_" + stem;
            pad.padLabel = "%eh_pad_" + stem;
            pad.resumeLabel = "%eh_resume_" + stem;
            pad.cleanupOnly = !hasLocalCatch;
            pad.cleanupCalls = std::move(cleanupCalls);
            if (hasLocalCatch) {
                const int savedTarget = static_cast<int>(activeExceptionTarget);
                activeExceptionTarget = -1;
                pad.catchPc = static_cast<size_t>(savedTarget);
                auto slot = exceptionValueSlots.find(pad.catchPc);
                if (slot == exceptionValueSlots.end()) {
                    pad.valueSlot = freshNativeTmp();
                    functionText += "  " + pad.valueSlot + " = alloca %ZeusValue, align 8\n";
                    slot = exceptionValueSlots.emplace(pad.catchPc, pad.valueSlot).first;
                } else {
                    pad.valueSlot = slot->second;
                }
                const bool previousSuppression = suppressExceptionWrapping;
                suppressExceptionWrapping = true;
                recordExceptionEdge(pad.catchPc, pad.resumeLabel);
                suppressExceptionWrapping = previousSuppression;
                activeExceptionTarget = savedTarget;
            }
            exceptionPads.push_back(std::move(pad));
            const std::string continuation = "%eh_continue_" + stem;
            std::string invoke = line;
            invoke.replace(callAt == 0 ? 0 : callAt + 3, 4, "invoke");
            functionText += "  " + invoke + " to label " + continuation +
                " unwind label %eh_switch_" + stem + "\n";
            functionText += continuation.substr(1) + ":\n";
            if (enterExceptionContinuation) enterExceptionContinuation(continuation);
        };

        std::vector<NativeValue> valueStack;
        std::vector<NativeValue> withValues;
        std::vector<NativeValue> exceptionCallArguments;
        std::vector<std::string> exceptionCallDynamicValues;
        std::map<int, NativeValue> locals;
        std::map<int, std::string> closureLocalCells;
        auto exceptionCallScope = [&](bool begin,
                                      const std::vector<NativeValue>& arguments,
                                      const std::vector<std::string>& dynamicValues) {
            if (begin) {
                exceptionCallArguments = arguments;
                exceptionCallDynamicValues = dynamicValues;
            } else {
                exceptionCallArguments.clear();
                exceptionCallDynamicValues.clear();
            }
        };
        exceptionCleanupSnapshot = [&](bool hasLocalCatch) {
            std::vector<std::string> cleanupCalls;
            auto appendValueCleanup = [&](const NativeValue& value) {
                if (value.dynamic && value.ownsDynamic) {
                    cleanupCalls.push_back("call void @zeus_value_release(ptr " +
                                           value.value + ")");
                } else if (value.ownsClass && isManagedClassValue(value)) {
                    cleanupCalls.push_back("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                }
            };
            if (!hasLocalCatch) {
                for (const auto& entry : locals) appendValueCleanup(entry.second);
                for (const auto& entry : closureLocalCells) {
                    cleanupCalls.push_back("call void @zeus_capture_cell_release(ptr " +
                                           entry.second + ")");
                }
                for (const auto& value : withValues) appendValueCleanup(value);
            }
            for (const auto& value : exceptionCallArguments) {
                appendValueCleanup(value);
            }
            for (const auto& value : exceptionCallDynamicValues) {
                cleanupCalls.push_back("call void @zeus_value_release(ptr " +
                                       value + ")");
            }
            for (const auto& value : valueStack) appendValueCleanup(value);
            return cleanupCalls;
        };
        for (size_t i = 0; i < parameterLLVMTypes.size(); ++i) {
            const bool dynamicParameter = usesZeusValueABI(signature.parameterTypes[i]);
            locals[static_cast<int>(i)] = NativeValue{
                "%arg" + std::to_string(i),
                parameterLLVMTypes[i],
                false,
                dynamicParameter,
                signature.parameterTypes[i],
                false
            };
            if (isClassType(signature.parameterTypes[i])) {
                const bool destructorReceiver = signature.operatorSymbol == "~" && i == 0;
                locals[static_cast<int>(i)].ownsClass = !destructorReceiver;
                if (!destructorReceiver) {
                    emitNativeLine("call void @zeus_class_ref_retain(ptr %arg" +
                                   std::to_string(i) + ")");
                }
            }
        }
        const int variadicBase = static_cast<int>(parameterLLVMTypes.size());
        if (signature.hasVariadicArgs) {
            locals[variadicBase] = NativeValue{
                "%varargs", "ptr", false, true, "Array", false
            };
        }
        if (signature.hasVariadicKwargs) {
            locals[variadicBase + (signature.hasVariadicArgs ? 1 : 0)] = NativeValue{
                "%kwargs", "ptr", false, true, "Object", false
            };
        }

        auto failNative = [&](const IRType::IRInstr& ins, const std::string& message) {
            result.error = "[emit-llvm] 函数 '" + signature.name + "': " +
                           message + " (" + ins.toString() + ")";
            return false;
        };

        auto popNative = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (valueStack.empty()) {
                result.error = "[emit-llvm] 原生函数栈下溢(" + ins.toString() + ")";
                return false;
            }

            value = valueStack.back();
            valueStack.pop_back();
            return true;
        };

        auto parseNativeValue = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (!ins.args.empty() &&
                std::holds_alternative<IRType::NullLiteral>(ins.args[0])) {
                const std::string nilValue = freshNativeTmp();
                emitNativeLine(nilValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_init_nil(ptr " + nilValue + ")");
                value = NativeValue{ nilValue, "ptr", false, true, "nil", true };
                return true;
            }
            if (!ins.args.empty() &&
                std::holds_alternative<ZeusType::Bytes>(ins.args[0])) {
                const auto& bytes = std::get<ZeusType::Bytes>(ins.args[0]);
                const std::string contents(bytes.data.begin(), bytes.data.end());
                const std::string symbol = poolStr(contents);
                const std::string dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_bytes(ptr " + dynamicValue +
                               ", ptr " + symbol + ", i64 " +
                               std::to_string(bytes.data.size()) + ")");
                value = NativeValue{
                    dynamicValue, "ptr", false, true, "bytes", true
                };
                return true;
            }
            if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                return failNative(ins, "[emit-llvm] 原生函数 PUSH 参数不是 Value");
            }

            const auto& literal = std::get<LexerType::Value>(ins.args[0]);
            if (std::holds_alternative<std::string>(literal.data)) {
                const std::string& text = std::get<std::string>(literal.data);
                const std::string symbol = poolStr(text);
                const std::string dynamicString = freshNativeTmp();
                emitNativeLine(dynamicString + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_string_data(ptr " +
                               dynamicString + ", ptr " + symbol + ", i64 " +
                               std::to_string(text.size()) + ")");
                value = NativeValue{ dynamicString, "ptr", false, true, "string", true };
                return true;
            }

            if (std::holds_alternative<ZeusType::BigInteger>(literal.data)) {
                int64_t integerValue = 0;
                if (!std::get<ZeusType::BigInteger>(literal.data).tryToInt64(integerValue)) {
                    const std::string symbol = poolStr(
                        std::get<ZeusType::BigInteger>(literal.data).toString());
                    const std::string dynamicValue = freshNativeTmp();
                    emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                    emitNativeLine("call void @zeus_value_from_bigint_decimal(ptr " +
                                   dynamicValue + ", ptr " + symbol + ")");
                    value = NativeValue{
                        dynamicValue, "ptr", false, true, "bigint", true
                    };
                    return true;
                }

                value = NativeValue{ std::to_string(integerValue), "i64", false };
                return true;
            }

            if (std::holds_alternative<ZeusType::BigDecimal>(literal.data)) {
                const std::string symbol = poolStr(
                    std::get<ZeusType::BigDecimal>(literal.data).toString());
                const std::string dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_bigdecimal_decimal(ptr " +
                               dynamicValue + ", ptr " + symbol + ")");
                value = NativeValue{
                    dynamicValue, "ptr", false, true, "float", true
                };
                return true;
            }

            if (std::holds_alternative<bool>(literal.data)) {
                value = NativeValue{
                    std::get<bool>(literal.data) ? "1" : "0",
                    "i1",
                    false
                };
                return true;
            }

            return failNative(ins, "[emit-llvm] 原生函数只支持 int/bool 字面量");
        };

        auto emitNativeDynamicValue = [&](
            const IRType::IRInstr& ins,
            const NativeValue& value,
            std::string& dynamicValue,
            bool& ownsDynamic
        ) {
            if (value.dynamic && value.type == "ptr") {
                dynamicValue = value.value;
                ownsDynamic = value.ownsDynamic;
                return true;
            }

            if (value.type == "i64" || value.type == "i1") {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                if (value.type == "i64") {
                    emitNativeLine("call void @zeus_value_init_i64(ptr " + dynamicValue +
                                   ", i64 " + value.value + ")");
                } else {
                    const std::string booleanValue = freshNativeTmp();
                    emitNativeLine(booleanValue + " = zext i1 " + value.value + " to i32");
                    emitNativeLine("call void @zeus_value_init_bool(ptr " + dynamicValue +
                                   ", i32 " + booleanValue + ")");
                }
                ownsDynamic = true;
                return true;
            }

            if (value.type == "ptr" && isReferenceTypeName(value.sourceType)) {
                const std::string targetType = referencedTypeName(value.sourceType);
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                if (targetType == "int" || targetType == "int32" ||
                    targetType == "int64" || targetType == "bool") {
                    emitNativeLine(std::string("call void @zeus_value_from_") +
                                   (targetType == "bool" ? "bool" : "i64") +
                                   "_ptr(ptr " + dynamicValue + ", ptr " + value.value + ")");
                } else if (isClassType(targetType)) {
                    const std::string typeName = poolStr(targetType);
                    emitNativeLine("call void @zeus_value_from_class_ptr(ptr " +
                                   dynamicValue + ", ptr " + value.value + ", ptr " +
                                   typeName + ")");
                } else if (const auto* abi = ZeusBuiltins::findNativeTypeABI(targetType);
                           ZeusBuiltins::isNativeViewABI(abi)) {
                    emitNativeLine("call void @" +
                                   std::string(ZeusBuiltins::nativeViewToDynamicHelper(abi)) +
                                   "(ptr " +
                                   dynamicValue + ", ptr " + value.value + ")");
                } else {
                    return failNative(ins,
                        "[emit-llvm] 该引用类型不能装箱为 ZeusValue: &" + targetType);
                }
                ownsDynamic = true;
                return true;
            }

            if (value.type == "ptr" && isClassType(value.sourceType)) {
                dynamicValue = freshNativeTmp();
                const std::string typeName = poolStr(value.sourceType);
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_class_ptr(ptr " + dynamicValue +
                               ", ptr " + value.value + ", ptr " + typeName + ")");
                ownsDynamic = true;
                return true;
            }

            const auto* sourceABI = ZeusBuiltins::findNativeTypeABI(value.sourceType);
            if (value.type == "ptr" && ZeusBuiltins::isNativeViewABI(sourceABI)) {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @" +
                               std::string(ZeusBuiltins::nativeViewToDynamicHelper(sourceABI)) +
                               "(ptr " + dynamicValue +
                               ", ptr " + value.value + ")");
                ownsDynamic = true;
                return true;
            }

            return failNative(ins, "[emit-llvm] 原生函数参数无法映射为 ZeusValue");
        };

        auto cloneNativeDynamicValue = [&](
            const IRType::IRInstr& ins,
            const NativeValue& value,
            NativeValue& clone
        ) {
            std::string source;
            bool ownsSource = false;
            if (!emitNativeDynamicValue(ins, value, source, ownsSource)) return false;

            const std::string destination = freshNativeTmp();
            emitNativeLine(destination + " = alloca %ZeusValue, align 8");
            emitNativeLine("call void @zeus_value_clone(ptr " + destination +
                           ", ptr " + source + ")");
            // Cloning a dynamic local must not consume the local itself. If
            // conversion created a temporary ZeusValue for a native source,
            // that temporary is the only object we own and can release here.
            if (ownsSource && !value.dynamic) {
                emitNativeLine("call void @zeus_value_release(ptr " + source + ")");
            }
            clone = NativeValue{
                destination, "ptr", false, true, value.sourceType, true
            };
            return true;
        };

        auto emitFunctionABIArgument = [&](
            const IRType::IRInstr& ins,
            const NativeValue& value,
            const std::string& declaredType,
            std::string& argument,
            bool& ownsDynamicArgument
        ) {
            ownsDynamicArgument = false;
            if (value.functionRef) {
                return failNative(ins, "[emit-llvm] 函数引用不能作为参数值");
            }

            if (usesZeusValueABI(declaredType)) {
                return emitNativeDynamicValue(
                    ins, value, argument, ownsDynamicArgument);
            }

            const std::string expectedType = llvmType(declaredType);
            if (value.dynamic) {
                if (expectedType == "i64") {
                    argument = freshNativeTmp();
                    emitNativeLine(argument + " = call i64 @zeus_value_require_i64(ptr " +
                                   value.value + ")");
                } else if (expectedType == "i1") {
                    const std::string raw = freshNativeTmp();
                    emitNativeLine(raw + " = call i32 @zeus_value_require_bool(ptr " +
                                   value.value + ")");
                    argument = freshNativeTmp();
                    emitNativeLine(argument + " = icmp ne i32 " + raw + ", 0");
                } else if (expectedType == "ptr" && isClassType(declaredType)) {
                    const std::string typeName = poolStr(declaredType);
                    argument = freshNativeTmp();
                    emitNativeLine(argument +
                                   " = call ptr @zeus_value_require_class_ptr(ptr " +
                                   value.value + ", ptr " + typeName + ")");
                } else if (expectedType == "ptr") {
                    const auto* abi = ZeusBuiltins::findNativeTypeABI(declaredType);
                    if (!ZeusBuiltins::isNativeViewABI(abi)) {
                        return failNative(ins,
                            "[emit-llvm] ZeusValue 不能直接转换为原生 class/struct 指针");
                    }
                    argument = freshNativeTmp();
                    emitNativeLine(argument + " = call ptr @" +
                                   std::string(ZeusBuiltins::nativeViewFromDynamicHelper(abi)) +
                                   "(ptr " +
                                   value.value + ")");
                } else {
                    return failNative(ins,
                        "[emit-llvm] ZeusValue 不能直接转换为原生 class/struct 指针");
                }
                // A class pointer extracted from an owned ZeusValue must stay
                // alive until the call site has invoked the callee. The callee
                // retains its typed class parameters on entry; releasing the
                // source box here could otherwise destroy the object first.
                const bool deferredClassSource =
                    expectedType == "ptr" && isClassType(declaredType);
                if (value.ownsDynamic && !deferredClassSource) {
                    emitNativeLine("call void @zeus_value_release(ptr " + value.value + ")");
                }
                return true;
            }

            const auto* declaredABI = ZeusBuiltins::findNativeTypeABI(declaredType);
            if (value.type == "ptr" && isClassType(value.sourceType) &&
                declaredABI != nullptr &&
                declaredABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                if (value.sourceType != "String") {
                    return failNative(ins,
                        "[emit-llvm] 只有 String 实例可以转换为原生 string ABI");
                }
                const std::string boxedString = freshNativeTmp();
                const std::string typeName = poolStr("String");
                emitNativeLine(boxedString + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_class_ptr(ptr " + boxedString +
                               ", ptr " + value.value + ", ptr " + typeName + ")");
                argument = freshNativeTmp();
                emitNativeLine(argument +
                               " = call ptr @zeus_value_intern_string_view(ptr " +
                               boxedString + ")");
                emitNativeLine("call void @zeus_value_release(ptr " + boxedString + ")");
                return true;
            }

            if (value.type != expectedType ||
                (isClassType(declaredType) && value.sourceType != declaredType)) {
                return failNative(ins, "[emit-llvm] 原生函数调用参数类型不匹配");
            }
            argument = value.value;
            return true;
        };

        auto emitClosureInvoke = [&] (
            const IRType::IRInstr& ins,
            NativeValue& callee,
            const std::vector<NativeValue>& arguments,
            NativeValue& callResult
        ) {
            if (!callee.dynamic || callee.type != "ptr") {
                return failNative(ins, "[emit-llvm] indirect closure call requires a ZeusValue callee");
            }
            exceptionCallArguments = arguments;
            if (callee.ownsDynamic) exceptionCallArguments.push_back(callee);
            exceptionCallDynamicValues.clear();
            std::vector<std::string> dynamicArguments(arguments.size());
            std::vector<bool> ownsDynamicArguments(arguments.size(), false);
            for (size_t i = 0; i < arguments.size(); ++i) {
                bool ownsDynamic = false;
                if (!emitNativeDynamicValue(
                        ins,
                        arguments[i],
                        dynamicArguments[i],
                        ownsDynamic)) {
                    return false;
                }
                ownsDynamicArguments[i] = ownsDynamic;
                if (ownsDynamic) {
                    exceptionCallDynamicValues.push_back(dynamicArguments[i]);
                    exceptionCallScope(
                        true, exceptionCallArguments,
                        exceptionCallDynamicValues);
                }
            }

            std::string argumentArray = "null";
            std::string argumentNameArray = "null";
            if (!dynamicArguments.empty()) {
                argumentArray = freshNativeTmp();
                argumentNameArray = freshNativeTmp();
                emitNativeLine(argumentArray + " = alloca ptr, i64 " +
                               std::to_string(dynamicArguments.size()) + ", align 8");
                emitNativeLine(argumentNameArray + " = alloca ptr, i64 " +
                               std::to_string(dynamicArguments.size()) + ", align 8");
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    const std::string slot = freshNativeTmp();
                    const std::string nameSlot = freshNativeTmp();
                    emitNativeLine(slot + " = getelementptr inbounds ptr, ptr " +
                                   argumentArray + ", i64 " + std::to_string(i));
                    emitNativeLine("store ptr " + dynamicArguments[i] + ", ptr " + slot +
                                   ", align 8");
                    emitNativeLine(nameSlot + " = getelementptr inbounds ptr, ptr " +
                                   argumentNameArray + ", i64 " + std::to_string(i));
                    if (arguments[i].namedArgument.empty()) {
                        emitNativeLine("store ptr null, ptr " + nameSlot + ", align 8");
                    } else {
                        const std::string nameSymbol = poolStr(arguments[i].namedArgument);
                        const std::string namePointer = freshNativeTmp();
                        emitNativeLine(namePointer + " = getelementptr inbounds [" +
                            std::to_string(arguments[i].namedArgument.size() + 1) +
                            " x i8], ptr " + nameSymbol + ", i64 0, i64 0");
                        emitNativeLine("store ptr " + namePointer + ", ptr " + nameSlot +
                                       ", align 8");
                    }
                }
            }

            const std::string output = freshNativeTmp();
            emitNativeLine(output + " = alloca %ZeusValue, align 8");
            emitNativeLine("call void @zeus_closure_call_named(ptr " + output + ", ptr " +
                           callee.value + ", i32 " +
                           std::to_string(dynamicArguments.size()) + ", ptr " +
                           argumentArray + ", ptr " + argumentNameArray + ")");
            exceptionCallScope(false, {}, {});
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   dynamicArguments[i] + ")");
                }
            }
            for (const auto& argument : arguments) {
                if (isManagedClassValue(argument) && argument.ownsClass) {
                    emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                   argument.value + ")");
                }
            }
            if (callee.ownsDynamic) {
                emitNativeLine("call void @zeus_value_release(ptr " + callee.value + ")");
            }
            callResult = NativeValue{
                output, "ptr", false, true, std::string(), true
            };
            return true;
        };

        auto releaseOwnedFunctionLocals = [&]() {
            for (const auto& entry : locals) {
                if (entry.second.dynamic && entry.second.ownsDynamic) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   entry.second.value + ")");
                } else if (entry.second.ownsClass &&
                           isManagedClassValue(entry.second)) {
                    emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                   entry.second.value + ")");
                }
            }
            for (const auto& entry : closureLocalCells) {
                emitNativeLine("call void @zeus_capture_cell_release(ptr " +
                               entry.second + ")");
            }
        };

        std::string pendingDirectCall;

        struct NativeIncomingState {
            // The block the edge really leaves from. It is not always the block
            // the IR pc belongs to: a null check for new/delete splits a block in
            // two, and the branch leaves from the tail label.
            std::string predecessor;
            // Pc of the instruction that produced the edge, so a loop backedge
            // can be matched up with its phi placeholder later on.
            size_t sourcePc = 0;
            std::map<int, NativeValue> locals;
        };

        size_t functionExitPc = chunk.size();
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            if (chunk[pc].op == VMOperator::FUNC_END) {
                functionExitPc = pc;
                break;
            }
        }

        std::set<size_t> reachableNativePcs;
        std::set<size_t> nativeBlockLeaders{ 0 };
        std::map<size_t, size_t> nativeBranchTargets;
        std::map<size_t, std::set<size_t>> nativePredecessors;
        std::vector<size_t> pendingNativePcs{ 0 };
        bool nativeExitReachable = false;

        auto normalizedNativePc = [&](long long target) -> size_t {
            return target == static_cast<long long>(chunk.size())
                ? functionExitPc
                : static_cast<size_t>(target);
        };

        while (!pendingNativePcs.empty()) {
            const size_t pc = pendingNativePcs.back();
            pendingNativePcs.pop_back();
            if (pc > functionExitPc || !reachableNativePcs.insert(pc).second) {
                continue;
            }
            if (pc == functionExitPc) {
                nativeExitReachable = true;
                continue;
            }

            const auto& ins = chunk[pc];
            if (ins.op == VMOperator::FUNC_END) {
                nativeExitReachable = true;
                continue;
            }
            if (ins.op == VMOperator::RETURN) continue;

            if (exceptionTarget[pc] >= 0 &&
                ins.op != VMOperator::TRY_BEGIN &&
                ins.op != VMOperator::TRY_END &&
                ins.op != VMOperator::IR_ANNOTATION &&
                ins.op != VMOperator::JUMP &&
                ins.op != VMOperator::LOOP_BREAK) {
                const size_t catchPc = static_cast<size_t>(exceptionTarget[pc]);
                nativeBlockLeaders.insert(catchPc);
                nativePredecessors[catchPc].insert(pc);
                pendingNativePcs.push_back(catchPc);
            }
            if (ins.op == VMOperator::THROW) continue;

            const bool unconditionalJump =
                ins.op == VMOperator::JUMP || ins.op == VMOperator::LOOP_BREAK;
            const bool conditionalJump =
                ins.op == VMOperator::JUMP_IF_FALSE ||
                ins.op == VMOperator::JUMP_IF_TRUE ||
                ins.op == VMOperator::JUMP_IF_PARAM_PROVIDED;
            if (unconditionalJump || conditionalJump) {
                int64_t offset = 0;
                const size_t offsetIndex =
                    ins.op == VMOperator::JUMP_IF_PARAM_PROVIDED ? 1 : 0;
                if (!tryArgInt(ins, offsetIndex, offset)) {
                    return failNative(ins, "LLVM 函数分支缺少有效的跳转偏移");
                }
                const long long rawTarget =
                    static_cast<long long>(pc) + 1 + offset;
                if (rawTarget < 0 ||
                    rawTarget > static_cast<long long>(chunk.size())) {
                    return failNative(ins, "LLVM 函数分支目标超出函数范围");
                }
                const size_t target = normalizedNativePc(rawTarget);
                if (target > functionExitPc) {
                    return failNative(ins, "LLVM 函数分支目标落在函数结束标记之后");
                }
                nativeBranchTargets[pc] = target;
                pendingNativePcs.push_back(target);
                if (conditionalJump) {
                    const size_t fallthrough = pc + 1 > functionExitPc
                        ? functionExitPc
                        : pc + 1;
                    pendingNativePcs.push_back(fallthrough);
                }
                continue;
            }

            const size_t fallthrough = pc + 1 > functionExitPc
                ? functionExitPc
                : pc + 1;
            pendingNativePcs.push_back(fallthrough);
        }

        for (const auto& branch : nativeBranchTargets) {
            if (reachableNativePcs.find(branch.first) == reachableNativePcs.end()) {
                continue;
            }
            nativeBlockLeaders.insert(branch.second);
            nativePredecessors[branch.second].insert(branch.first);
            if (chunk[branch.first].op == VMOperator::JUMP_IF_FALSE ||
                chunk[branch.first].op == VMOperator::JUMP_IF_TRUE ||
                chunk[branch.first].op == VMOperator::JUMP_IF_PARAM_PROVIDED) {
                const size_t fallthrough = branch.first + 1 > functionExitPc
                    ? functionExitPc
                    : branch.first + 1;
                nativeBlockLeaders.insert(fallthrough);
                nativePredecessors[fallthrough].insert(branch.first);
            }
        }
        if (nativeExitReachable) nativeBlockLeaders.insert(functionExitPc);

        for (size_t pc : reachableNativePcs) {
            if (pc >= functionExitPc ||
                nativeBranchTargets.find(pc) != nativeBranchTargets.end() ||
                chunk[pc].op == VMOperator::RETURN ||
                chunk[pc].op == VMOperator::FUNC_END) {
                continue;
            }
            const size_t fallthrough = pc + 1 > functionExitPc
                ? functionExitPc
                : pc + 1;
            if (nativeBlockLeaders.find(fallthrough) != nativeBlockLeaders.end()) {
                nativePredecessors[fallthrough].insert(pc);
            }
        }

        auto nativeLabelForPc = [&](size_t pc) {
            return pc == functionExitPc ? endLabelFor(ci) : labelFor(ci, pc);
        };

        std::set<size_t> nativeLoopHeaders;
        for (const auto& predecessors : nativePredecessors) {
            for (size_t predecessorPc : predecessors.second) {
                if (predecessorPc >= predecessors.first) {
                    nativeLoopHeaders.insert(predecessors.first);
                    break;
                }
            }
        }

        std::map<size_t, std::string> nativeBlockForPc;
        std::string activeNativeBlock = "%entry";
        for (size_t pc = 0; pc <= functionExitPc; ++pc) {
            if ((pc == 0 && nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) ||
                (pc != 0 && nativeBlockLeaders.find(pc) != nativeBlockLeaders.end())) {
                activeNativeBlock = nativeLabelForPc(pc);
            }
            nativeBlockForPc[pc] = activeNativeBlock;
        }

        std::map<size_t, std::set<std::string>> nativePredecessorBlocks;
        std::map<size_t, std::set<std::string>> nativeLoopBackedgeBlocks;
        // Pc of every loop backedge, kept next to the block labels above. The
        // labels come from the static analysis and go stale as soon as a block
        // gets split, so the phi patch uses these pcs to find the real
        // predecessor instead.
        std::map<size_t, std::set<size_t>> nativeLoopBackedgePcs;
        for (const auto& predecessors : nativePredecessors) {
            for (size_t predecessorPc : predecessors.second) {
                const auto block = nativeBlockForPc.find(predecessorPc);
                if (block == nativeBlockForPc.end()) continue;
                nativePredecessorBlocks[predecessors.first].insert(block->second);
                if (predecessorPc >= predecessors.first) {
                    nativeLoopBackedgeBlocks[predecessors.first].insert(block->second);
                    nativeLoopBackedgePcs[predecessors.first].insert(predecessorPc);
                }
            }
        }
        IRType::IRInstr captureInitializationInstruction;
        if (!chunk.empty()) captureInitializationInstruction = chunk.front();
        for (const int localIndex : referenceCapturedLocals) {
            const std::string cell = freshNativeTmp();
            emitNativeLine(cell + " = call ptr @zeus_capture_cell_create(ptr null)");
            closureLocalCells.emplace(localIndex, cell);

            const auto parameter = locals.find(localIndex);
            if (parameter != locals.end()) {
                std::string initialValue;
                bool ownsInitialValue = false;
                if (!emitNativeDynamicValue(
                        captureInitializationInstruction,
                        parameter->second,
                        initialValue,
                        ownsInitialValue)) {
                    return false;
                }
                emitNativeLine("call void @zeus_capture_cell_store(ptr " + cell +
                               ", ptr " + initialValue + ")");
                if (ownsInitialValue) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   initialValue + ")");
                }
            }
        }

        if (nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) {
            nativePredecessorBlocks[0].insert("%entry");
        }

        std::map<size_t, std::vector<NativeIncomingState>> nativeIncomingStates;
        std::map<size_t, std::set<int>> nativeLoopHeaderLocals;
        struct NativeLoopPhiPatch {
            size_t headerPc = 0;
            int localIndex = -1;
            size_t sourcePc = 0;
            std::string placeholder;
            // The backedge leaves from a block that is still being emitted, so
            // its label is only known once the whole function body is done.
            std::string labelPlaceholder;
            std::string type;
            bool dynamic = false;
            std::string sourceType;
        };
        std::vector<NativeLoopPhiPatch> nativeLoopPhiPatches;
        size_t nativeLoopPhiPlaceholderCount = 0;
        std::string currentNativeBlock = "%entry";
        bool nativeBlockTerminated = false;

        auto recordNativeEdge = [&](size_t target, const IRType::IRInstr& ins,
                                    size_t sourcePc) {
            if (target == functionExitPc && returnsDynamic && !valueStack.empty()) {
                // Falling off a function returns nil. Discard any unused
                // expression result left by a statement such as a subscript
                // assignment before merging into that implicit-return block.
                for (const auto& value : valueStack) {
                    if (value.dynamic && value.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       value.value + ")");
                    }
                }
                valueStack.clear();
            }
            if (!valueStack.empty()) {
                std::string pending = "LLVM 函数分支边要求操作数栈为空; pc=" +
                    std::to_string(sourcePc) + ", depth=" +
                    std::to_string(valueStack.size()) + ", values=";
                for (const auto& value : valueStack) {
                    pending += " [" + value.type + "," + value.sourceType +
                        (value.dynamic ? ",dynamic" : ",native") + "]";
                }
                const size_t historyStart = sourcePc > 24 ? sourcePc - 24 : 0;
                pending += "; preceding instructions=";
                for (size_t previous = historyStart; previous <= sourcePc; ++previous) {
                    pending += " {" + std::to_string(previous) + ":" +
                        chunk[previous].toString() + "}";
                }
                return failNative(ins, pending);
            }

            std::map<int, NativeValue> edgeLocals = locals;

            const auto loopHeaderLocals = nativeLoopHeaderLocals.find(target);
            if (loopHeaderLocals != nativeLoopHeaderLocals.end()) {
                for (auto local = edgeLocals.begin(); local != edgeLocals.end();) {
                    if (loopHeaderLocals->second.find(local->first) !=
                        loopHeaderLocals->second.end()) {
                        ++local;
                        continue;
                    }
                    if (local->second.dynamic && local->second.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       local->second.value + ")");
                    }
                    local = edgeLocals.erase(local);
                }
                for (int index : loopHeaderLocals->second) {
                    if (edgeLocals.find(index) == edgeLocals.end()) {
                        return failNative(ins,
                            "循环回边缺少循环头已有的局部变量");
                    }
                }
            }

            const auto predecessors = nativePredecessorBlocks.find(target);
            const bool needsMerge = predecessors != nativePredecessorBlocks.end() &&
                                    predecessors->second.size() > 1;
            if (needsMerge) {
                for (auto& local : edgeLocals) {
                    if (!local.second.dynamic || local.second.ownsDynamic) continue;
                    NativeValue owned;
                    if (!cloneNativeDynamicValue(ins, local.second, owned)) return false;
                    local.second = std::move(owned);
                }
            }

            auto& incoming = nativeIncomingStates[target];
            auto existing = std::find_if(
                incoming.begin(), incoming.end(),
                [&](const NativeIncomingState& edge) {
                    return edge.predecessor == currentNativeBlock;
                }
            );
            if (existing == incoming.end()) {
                incoming.push_back(NativeIncomingState{
                    currentNativeBlock, sourcePc, std::move(edgeLocals)
                });
            }
            return true;
        };

        bool exceptionEdgeFailed = false;
        recordExceptionEdge = [&](size_t target, const std::string& resumeLabel) {
            const std::string savedBlock = currentNativeBlock;
            std::vector<NativeValue> savedStack = std::move(valueStack);
            valueStack.clear();
            currentNativeBlock = resumeLabel;
            nativePredecessorBlocks[target].insert(resumeLabel);
            if (!recordNativeEdge(target, chunk[activeExceptionPc], activeExceptionPc)) {
                exceptionEdgeFailed = true;
            }
            currentNativeBlock = savedBlock;
            valueStack = std::move(savedStack);
        };
        enterExceptionContinuation = [&](const std::string& continuation) {
            currentNativeBlock = continuation;
        };

        auto enterNativeBlock = [&](size_t pc, const IRType::IRInstr& ins) {
            const std::string label = nativeLabelForPc(pc);
            functionText += label.substr(1) + ":\n";
            currentNativeBlock = label;
            nativeBlockTerminated = false;
            if (!valueStack.empty()) {
                return failNative(ins,
                    "LLVM 函数基本块合流时操作数栈不为空");
            }

            auto incoming = nativeIncomingStates.find(pc);
            if (incoming == nativeIncomingStates.end() || incoming->second.empty()) {
                return failNative(ins,
                    "LLVM 函数基本块没有可用的前驱状态");
            }

            std::set<int> localIndices;
            for (const auto& edge : incoming->second) {
                for (const auto& local : edge.locals) {
                    localIndices.insert(local.first);
                }
            }

            std::vector<std::string> phiLines;
            std::vector<std::string> cleanupLines;
            std::map<int, NativeValue> mergedLocals;
            const auto backedges = nativeLoopBackedgePcs.find(pc);
            const bool hasBackedges = backedges != nativeLoopBackedgePcs.end() &&
                                      !backedges->second.empty();
            for (int index : localIndices) {
                std::vector<const NativeValue*> values;
                values.reserve(incoming->second.size());
                bool presentOnEveryEdge = true;
                for (const auto& edge : incoming->second) {
                    const auto local = edge.locals.find(index);
                    if (local == edge.locals.end()) {
                        presentOnEveryEdge = false;
                        values.push_back(nullptr);
                    } else {
                        values.push_back(&local->second);
                    }
                }

                const NativeValue* prototype = nullptr;
                for (const NativeValue* value : values) {
                    if (value != nullptr) {
                        prototype = value;
                        break;
                    }
                }
                if (prototype == nullptr) continue;

                if (hasBackedges && !presentOnEveryEdge) {
                    return failNative(ins,
                        "loop-carried local is not initialized on every loop entry");
                }

                auto compatible = [&](const NativeValue& value) {
                    const bool dynamicSourceTypesCompatible = value.dynamic &&
                        prototype->dynamic && value.type == "ptr" &&
                        prototype->type == "ptr";
                    return value.type == prototype->type &&
                           value.dynamic == prototype->dynamic &&
                           (value.sourceType == prototype->sourceType ||
                            dynamicSourceTypesCompatible) &&
                           value.functionRef == prototype->functionRef;
                };
                for (const NativeValue* value : values) {
                    if (value != nullptr && !compatible(*value)) {
                        result.error = "[emit-llvm] 函数 '" + signature.name +
                            "' 的局部变量在 if/else 合流处类型不一致";
                        return false;
                    }
                }

                if (!presentOnEveryEdge) {
                    if (!prototype->dynamic) continue;
                    const std::string phi = freshNativeTmp();
                    std::string expression = phi + " = phi ptr ";
                    for (size_t i = 0; i < values.size(); ++i) {
                        if (i != 0) expression += ", ";
                        expression += "[ " +
                            (values[i] == nullptr ? std::string("null") : values[i]->value) +
                            ", " + incoming->second[i].predecessor + " ]";
                    }
                    phiLines.push_back(std::move(expression));
                    cleanupLines.push_back(
                        "call void @zeus_value_release(ptr " + phi + ")");
                    continue;
                }

                bool sameValue = true;
                bool allOwned = true;
                for (const NativeValue* value : values) {
                    sameValue = sameValue && value->value == prototype->value;
                    allOwned = allOwned && value->ownsDynamic;
                }
                if (sameValue && !hasBackedges) {
                    NativeValue merged = *prototype;
                    if (merged.dynamic) merged.ownsDynamic = allOwned;
                    mergedLocals[index] = std::move(merged);
                    continue;
                }

                if (prototype->functionRef || prototype->type.empty() ||
                    (prototype->dynamic && !allOwned)) {
                    result.error = "[emit-llvm] 函数 '" + signature.name +
                        "' 的局部变量无法在 if/else 合流处合并";
                    return false;
                }

                const std::string phi = freshNativeTmp();
                std::string expression = phi + " = phi " + prototype->type + " ";
                for (size_t i = 0; i < values.size(); ++i) {
                    if (i != 0) expression += ", ";
                    expression += "[ " + values[i]->value + ", " +
                        incoming->second[i].predecessor + " ]";
                }
                if (hasBackedges) {
                    // A backedge is emitted after the header, so both the value
                    // it carries and the block it leaves from are patched in
                    // once the whole body exists. Keying the patch on the pc
                    // that produced the edge is what makes this survive block
                    // splitting: the label recorded while emitting the branch
                    // is the real predecessor, not the IR block it started in.
                    for (size_t sourcePc : backedges->second) {
                        const std::string placeholder = "%loop_phi_" +
                            std::to_string(nativeLoopPhiPlaceholderCount++);
                        const std::string labelPlaceholder = "%loop_phi_label_" +
                            std::to_string(nativeLoopPhiPlaceholderCount++);
                        expression += ", [ " + placeholder + ", " +
                            labelPlaceholder + " ]";
                        nativeLoopPhiPatches.push_back(NativeLoopPhiPatch{
                            pc,
                            index,
                            sourcePc,
                            placeholder,
                            labelPlaceholder,
                            prototype->type,
                            prototype->dynamic,
                            prototype->sourceType
                        });
                    }
                }
                phiLines.push_back(std::move(expression));

                NativeValue merged = *prototype;
                merged.value = phi;
                if (merged.dynamic) merged.ownsDynamic = true;
                for (const NativeValue* value : values) {
                    if (value->sourceType != merged.sourceType) {
                        merged.sourceType.clear();
                        break;
                    }
                }
                mergedLocals[index] = std::move(merged);
            }

            if (hasBackedges) {
                auto& loopLocals = nativeLoopHeaderLocals[pc];
                for (const auto& local : mergedLocals) {
                    loopLocals.insert(local.first);
                }
            }

            for (const auto& line : phiLines) emitNativeLine(line);
            for (const auto& line : cleanupLines) emitNativeLine(line);
            locals = std::move(mergedLocals);
            return true;
        };

        auto recordNativeFallthrough = [&](size_t pc, const IRType::IRInstr& ins) {
            if (pc >= functionExitPc) return true;
            const size_t target = pc + 1 > functionExitPc
                ? functionExitPc
                : pc + 1;
            if (nativeBlockLeaders.find(target) == nativeBlockLeaders.end() ||
                reachableNativePcs.find(target) == reachableNativePcs.end()) {
                return true;
            }
            if (!recordNativeEdge(target, ins, pc)) return false;
            emitNativeLine("br label " + nativeLabelForPc(target));
            nativeBlockTerminated = true;
            return true;
        };

        auto emitImplicitNilReturn = [&]() {
            if (!returnsDynamic) {
                result.error = "[emit-llvm] 函数 '" + signature.name +
                    "' 的原生返回类型存在可达的隐式 nil 路径";
                return false;
            }
            if (!valueStack.empty()) {
                result.error = "[emit-llvm] 函数 '" + signature.name +
                    "' 在隐式返回 nil 时操作数栈不为空";
                return false;
            }
            emitNativeLine("call void @zeus_value_init_nil(ptr %return.out)");
            releaseOwnedFunctionLocals();
            emitNativeLine("ret void");
            nativeBlockTerminated = true;
            return true;
        };

        if (nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) {
            if (chunk.empty() || !recordNativeEdge(0, chunk.front(), 0)) return false;
            emitNativeLine("br label " + nativeLabelForPc(0));
            nativeBlockTerminated = true;
        }

        auto emitNativeDirectCall = [&](const IRType::IRInstr& ins) {
            if (pendingDirectCall.empty()) {
                return failNative(ins, "[emit-llvm] 静态运算缺少 direct_call 标注");
            }

            auto target = functionSignatures.find(pendingDirectCall);
            if (target == functionSignatures.end()) {
                return failNative(ins, "[emit-llvm] 找不到静态运算目标'" +
                                  pendingDirectCall + "'");
            }

            const auto& signature = target->second;
            if (signature.parameterTypes.size() != 2) {
                return failNative(ins, "[emit-llvm] 静态二元运算必须有两个参数");
            }

            NativeValue right;
            NativeValue left;
            if (!popNative(ins, right) || !popNative(ins, left) ||
                left.functionRef || right.functionRef) {
                return failNative(ins, "[emit-llvm] 静态运算操作数无效");
            }

            const bool returnsDynamic = usesZeusValueABI(signature.returnType);
            const std::string resultType = returnsDynamic
                ? "void"
                : llvmType(signature.returnType);
            if (resultType.empty()) {
                return failNative(ins, "[emit-llvm] 静态运算返回类型没有 LLVM ABI 映射");
            }

            const std::string temporary = freshNativeTmp();
            if (returnsDynamic) {
                emitNativeLine(temporary + " = alloca %ZeusValue, align 8");
            }
            std::string callText = returnsDynamic
                ? "call void " + llvmCallTarget(signature.name) + "(ptr " + temporary
                : temporary + " = call " + resultType + " " + llvmCallTarget(signature.name) + "(";
            std::vector<std::string> dynamicArguments;
            std::vector<bool> ownsDynamicArguments;
            const NativeValue arguments[] = { left, right };
            exceptionCallArguments = { left, right };
            exceptionCallDynamicValues.clear();
            for (size_t i = 0; i < 2; ++i) {
                if (i > 0 || returnsDynamic) callText += ", ";
                std::string argument;
                bool ownsDynamic = false;
                if (!emitFunctionABIArgument(
                        ins, arguments[i], signature.parameterTypes[i],
                        argument, ownsDynamic)) {
                    return false;
                }
                callText += functionParameterABIType(signature.parameterTypes[i]) +
                            " " + argument;
                if (usesZeusValueABI(signature.parameterTypes[i])) {
                    dynamicArguments.push_back(argument);
                    ownsDynamicArguments.push_back(ownsDynamic);
                    if (ownsDynamic) {
                        exceptionCallDynamicValues.push_back(argument);
                    }
                    exceptionCallScope(
                        true, exceptionCallArguments, exceptionCallDynamicValues);
                }
            }
            if (signature.hasDefaultParameters()) {
                callText += ", i64 3";
            }
            callText += ")";
            emitNativeLine(callText);
            exceptionCallScope(false, {}, {});
            std::set<std::string> releasedDynamicArguments;
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i] &&
                    releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   dynamicArguments[i] + ")");
                }
            }
            for (const auto& argument : arguments) {
                if (argument.dynamic && argument.ownsDynamic &&
                    releasedDynamicArguments.insert(argument.value).second) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   argument.value + ")");
                }
                if (isManagedClassValue(argument) && argument.ownsClass) {
                    emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                   argument.value + ")");
                }
            }
            valueStack.push_back(NativeValue{
                temporary,
                returnsDynamic ? "ptr" : resultType,
                false,
                returnsDynamic,
                signature.returnType,
                returnsDynamic
            });
            valueStack.back().ownsClass =
                !returnsDynamic && isClassType(signature.returnType);
            pendingDirectCall.clear();
            return true;
        };

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            if (reachableNativePcs.find(pc) == reachableNativePcs.end()) continue;
            const auto& ins = chunk[pc];
            activeExceptionPc = pc;
            activeExceptionTarget = exceptionTarget[pc];

            if (ins.op == VMOperator::CATCH_BEGIN &&
                nativeIncomingStates.find(pc) == nativeIncomingStates.end()) {
                const auto end = catchEndForBegin.find(pc);
                if (end == catchEndForBegin.end()) {
                    return failNative(ins, "catch block has no matching CATCH_END");
                }
                functionText += nativeLabelForPc(pc).substr(1) + ":\n";
                emitNativeLine("unreachable");
                nativeBlockTerminated = true;
                pc = end->second;
                continue;
            }

            if ((pc != 0 || nativeLoopHeaders.find(0) != nativeLoopHeaders.end()) &&
                nativeBlockLeaders.find(pc) != nativeBlockLeaders.end()) {
                if (!enterNativeBlock(pc, ins)) return false;
            }

            if (pc == functionExitPc || ins.op == VMOperator::FUNC_END) {
                if (!emitImplicitNilReturn()) return false;
                break;
            }

            if (ins.op == VMOperator::FUNC_BEGIN) {
                if (!recordNativeFallthrough(pc, ins)) return false;
                continue;
            }

            if (ins.op == VMOperator::IR_ANNOTATION) {
                if (annotationKind(ins, "direct_call")) {
                    std::string directCallName;
                    if (!annotationSymbolName(ins, directCallName)) {
                        return failNative(ins, "[emit-llvm] 直接调用标注缺少函数名");
                    }
                    const auto* target = resolveFunctionSignature(directCallName);
                    pendingDirectCall = target != nullptr &&
                        !hasCompleteFunctionAnnotations(*target)
                            ? std::string()
                            : (target != nullptr ? target->name : directCallName);
                }
                if (!recordNativeFallthrough(pc, ins)) return false;
                continue;
            }

            switch (ins.op) {
                case VMOperator::TRY_BEGIN:
                case VMOperator::TRY_END:
                    break;

                case VMOperator::WITH_BEGIN:
                case VMOperator::WITH_END: {
                    NativeValue receiver;
                    if (ins.op == VMOperator::WITH_BEGIN) {
                        if (!popNative(ins, receiver) || receiver.dynamic ||
                            receiver.type != "ptr" ||
                            !isClassType(receiver.sourceType)) {
                            return failNative(ins,
                                "[emit-llvm] WITH_BEGIN requires a typed class instance");
                        }
                        int64_t captureSlot = -1;
                        if (!tryArgInt(ins, 0, captureSlot)) {
                            return failNative(ins, "[emit-llvm] invalid with capture slot");
                        }
                        if (captureSlot >= 0) {
                            auto existing = locals.find(static_cast<int>(captureSlot));
                            if (isManagedClassValue(receiver) && !receiver.ownsClass) {
                                emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                               receiver.value + ")");
                            }
                            if (existing != locals.end() && existing->second.ownsClass &&
                                isManagedClassValue(existing->second)) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               existing->second.value + ")");
                            }
                            NativeValue capturedReceiver = receiver;
                            capturedReceiver.ownsClass = isManagedClassValue(capturedReceiver);
                            locals[static_cast<int>(captureSlot)] = capturedReceiver;
                            receiver.ownsClass = false;
                            withValues.push_back(receiver);
                        } else {
                            withValues.push_back(receiver);
                            receiver.ownsClass = false;
                        }
                    } else {
                        if (withValues.empty()) {
                            return failNative(ins, "[emit-llvm] WITH_END has no active with block");
                        }
                        receiver = withValues.back();
                        withValues.pop_back();
                    }

                    const std::string methodName = ins.op == VMOperator::WITH_BEGIN
                        ? "operator[with]" : "operator[~with]";
                    const auto method = functionSignatures.find(
                        receiver.sourceType + "::" + methodName);
                    if (method == functionSignatures.end() &&
                        ins.op == VMOperator::WITH_END) {
                        if (receiver.ownsClass && isManagedClassValue(receiver)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           receiver.value + ")");
                        }
                        break;
                    }
                    if (method == functionSignatures.end()) {
                        return failNative(ins,
                            "[emit-llvm] with receiver has no operator[with]: " +
                            receiver.sourceType);
                    }
                    auto convertWithArgument = [&](const NativeValue& value,
                                                   const std::string& declaredType,
                                                   std::string& nativeArgument,
                                                   std::string& cleanupValue) {
                        cleanupValue.clear();
                        bool ownsDynamic = false;
                        if (!emitFunctionABIArgument(ins, value, declaredType,
                                nativeArgument, ownsDynamic)) {
                            return false;
                        }
                        if (ownsDynamic) cleanupValue = nativeArgument;
                        return true;
                    };
                    NativeValue ignoredResult;
                    if (!emitKnownClassMethodCall(ins, methodName, {receiver},
                            ignoredResult, emitNativeLine, freshNativeTmp,
                            convertWithArgument,
                            [&](const std::string& value) {
                                emitNativeLine("call void @zeus_value_release(ptr " + value + ")");
                            },
                            [&](const IRType::IRInstr& at, const std::string& message) {
                                return failNative(at, message);
                            },
                            exceptionCallScope)) {
                        return false;
                    }
                    if (ignoredResult.dynamic && ignoredResult.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                            ignoredResult.value + ")");
                    } else if (ignoredResult.ownsClass &&
                               isManagedClassValue(ignoredResult)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       ignoredResult.value + ")");
                    }
                    break;
                }

                case VMOperator::CATCH_BEGIN: {
                    int64_t slot = -1;
                    if (!tryArgInt(ins, 1, slot)) {
                        return failNative(ins, "invalid CATCH_BEGIN local slot");
                    }
                    const auto captured = exceptionValueSlots.find(pc);
                    if (captured == exceptionValueSlots.end()) {
                        return failNative(ins, "catch has no exception landing pad");
                    }
                    if (slot >= 0) {
                        locals[static_cast<int>(slot)] = NativeValue{
                            captured->second, "ptr", false, true, {}, true
                        };
                    } else {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                            captured->second + ")");
                    }
                    break;
                }

                case VMOperator::CATCH_END: {
                    const auto begin = catchBeginForEnd.find(pc);
                    if (begin != catchBeginForEnd.end()) {
                        const auto captured = exceptionValueSlots.find(begin->second);
                        if (captured != exceptionValueSlots.end()) {
                            int64_t slot = -1;
                            if (tryArgInt(chunk[begin->second], 1, slot) && slot >= 0) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                    captured->second + ")");
                                locals.erase(static_cast<int>(slot));
                            }
                        }
                    }
                    break;
                }

                case VMOperator::THROW: {
                    NativeValue thrown;
                    if (!popNative(ins, thrown) || thrown.functionRef || !valueStack.empty()) {
                        return failNative(ins, "THROW requires one value on the stack");
                    }
                    std::string boxed;
                    bool ownsBoxed = false;
                    if (!emitNativeDynamicValue(ins, thrown, boxed, ownsBoxed)) return false;
                    if (isManagedClassValue(thrown) && thrown.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       thrown.value + ")");
                    }
                    emitNativeLine(std::string("call void @zeus_exception_raise") +
                        (ownsBoxed ? "_owned(ptr " : "(ptr ") + boxed + ")");
                    emitNativeLine("unreachable");
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::PUSH: {
                    NativeValue value;
                    if (!parseNativeValue(ins, value)) return false;
                    valueStack.push_back(std::move(value));
                    break;
                }

                case VMOperator::PUSH_NAMED: {
                    if (valueStack.empty() || ins.args.size() != 1 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "invalid named argument");
                    }
                    valueStack.back().namedArgument = std::get<std::string>(ins.args[0]);
                    break;
                }

                case VMOperator::LOAD_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] LOAD_VALUE_SHARED 缺少名称");
                    }
                    const std::string value = freshNativeTmp();
                    emitNativeLine(value + " = alloca %ZeusValue, align 8");
                    emitNativeLine("call void @zeus_shared_value_load(ptr " + value +
                                   ", ptr " + poolStr(std::get<std::string>(ins.args[0])) + ")");
                    valueStack.push_back(NativeValue{
                        value, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LOAD_VALUE: {
                    if (ins.args.size() == 1 && std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        if (!dynamicVariadicABISymbol(name).empty()) {
                            valueStack.push_back(NativeValue{ name, "builtin", true });
                            break;
                        }
                        if (classLayouts.find(name) != classLayouts.end()) {
                            valueStack.push_back(NativeValue{
                                name, "classref", false, false, name
                            });
                            break;
                        }
                        const auto* function = resolveFunctionSignature(name);
                        if (function == nullptr) {
                            return failNative(ins, "[emit-llvm] 原生函数无法解析名字 '" + name + "'");
                        }
                        valueStack.push_back(NativeValue{ function->name, "function", true });
                        break;
                    }

                    if (ins.args.size() < 2 || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] 原生函数 LOAD_VALUE 形态不支持");
                    }

                    const std::string& kind = std::get<std::string>(ins.args[0]);
                    int64_t index = 0;
                    if (!tryArgInt(ins, 1, index)) {
                        return failNative(ins, "[emit-llvm] 原生函数 LOAD_VALUE 索引不是整数");
                    }

                    if (kind == "local") {
                        const auto capturedCell = closureLocalCells.find(static_cast<int>(index));
                        if (capturedCell != closureLocalCells.end()) {
                            const std::string loaded = freshNativeTmp();
                            emitNativeLine(loaded + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_capture_cell_load(ptr " + loaded +
                                           ", ptr " + capturedCell->second + ")");
                            valueStack.push_back(NativeValue{
                                loaded, "ptr", false, true, std::string(), true
                            });
                            break;
                        }
                        auto local = locals.find(static_cast<int>(index));
                        if (local == locals.end()) {
                            return failNative(ins, "[emit-llvm] 原生函数读取了未初始化的局部变量");
                        }
                        if (local->second.dynamic) {
                            NativeValue clone;
                            if (!cloneNativeDynamicValue(ins, local->second, clone)) return false;
                            valueStack.push_back(std::move(clone));
                        } else {
                            NativeValue borrowed = local->second;
                            borrowed.ownsClass = false;
                            valueStack.push_back(std::move(borrowed));
                        }
                        break;
                    }

                    if (kind == "upvalue") {
                        const std::string loaded = freshNativeTmp();
                        emitNativeLine(loaded + " = alloca %ZeusValue, align 8");
                        emitNativeLine("call void @zeus_closure_env_load(ptr " + loaded +
                                       ", ptr %closure.env, i32 " +
                                       std::to_string(index) + ")");
                        valueStack.push_back(NativeValue{
                            loaded, "ptr", false, true, std::string(), true
                        });
                        break;
                    }


                    if (kind == "global") {
                        const auto importedNamespace =
                            namespacePackagesByGlobal.find(static_cast<int>(index));
                        if (importedNamespace != namespacePackagesByGlobal.end()) {
                            valueStack.push_back(NativeValue{
                                importedNamespace->second,
                                "namespace",
                                false,
                                false,
                                importedNamespace->second
                            });
                            break;
                        }
                    }

                    if (kind == "global" && isFunctionGlobal(static_cast<int>(index))) {
                        valueStack.push_back(NativeValue{
                            functionGlobals[static_cast<int>(index)],
                            "function",
                            true
                        });
                        break;
                    }

                    if (kind == "global") {
                        auto classGlobal = classGlobals.find(static_cast<int>(index));
                        if (classGlobal != classGlobals.end()) {
                            valueStack.push_back(NativeValue{
                                classGlobal->second,
                                "classref",
                                false,
                                false,
                                classGlobal->second
                            });
                            break;
                        }

                        // A global known to hold a class instance is unboxed back
                        // to a native pointer, so field access resolves against
                        // the class layout instead of failing on a boxed value.
                        const auto globalClass =
                            prescanGlobalClassTypes.find(static_cast<int>(index));
                        if (globalClass != prescanGlobalClassTypes.end() &&
                            findClassLayout(globalClass->second) != nullptr) {
                            const std::string boxed = freshNativeTmp();
                            emitNativeLine(boxed + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_value_global_load(ptr " +
                                boxed + ", i32 " + std::to_string(index) + ")");
                            const std::string unboxed = freshNativeTmp();
                            emitNativeLine(unboxed +
                                " = call ptr @zeus_value_require_class_ptr(ptr " +
                                boxed + ", ptr " + poolStr(globalClass->second) + ")");
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           unboxed + ")");
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                boxed + ")");
                            NativeValue loadedClass{
                                unboxed, "ptr", false, false, globalClass->second
                            };
                            loadedClass.ownsClass = true;
                            valueStack.push_back(std::move(loadedClass));
                            break;
                        }

                        const auto globalScalar =
                            nativeGlobalScalarTypes.find(static_cast<int>(index));
                        if (globalScalar != nativeGlobalScalarTypes.end()) {
                            // Unboxed access: a loop that bumps a scalar global
                            // must not build a ZeusValue per iteration.
                            const bool globalIsBool = globalScalar->second == "i1";
                            const std::string loaded = freshNativeTmp();
                            emitNativeLine(loaded + " = call " +
                                std::string(globalIsBool ? "i32" : "i64") +
                                " @zeus_global_load_" +
                                (globalIsBool ? "bool" : "i64") + "(i32 " +
                                std::to_string(index) + ")");
                            if (globalIsBool) {
                                const std::string flag = freshNativeTmp();
                                emitNativeLine(flag + " = icmp ne i32 " + loaded + ", 0");
                                valueStack.push_back(NativeValue{ flag, "i1", false, false });
                            } else {
                                valueStack.push_back(NativeValue{ loaded, "i64", false, false });
                            }
                            break;
                        }

                        const std::string value = emitSlotLoad(
                            static_cast<int>(index), true,
                            [&](const std::string& line) { emitNativeLine(line); });
                        valueStack.push_back(NativeValue{
                            value, "ptr", false, true, std::string(), true
                        });
                        break;
                    }

                    return failNative(ins, "[emit-llvm] 原生函数只支持局部值和直接函数引用");
                }

                case VMOperator::LIST_BUILD:
                case VMOperator::DICT_BUILD: {
                    int64_t count = 0;
                    if (!tryArgInt(ins, 0, count) || count < 0 ||
                        count > std::numeric_limits<int32_t>::max()) {
                        return failNative(ins, "[emit-llvm] 容器构造元素数量无效");
                    }
                    const bool dictionary = ins.op == VMOperator::DICT_BUILD;
                    const int64_t valueCount = dictionary ? count * 2 : count;
                    std::vector<NativeValue> values(
                        static_cast<std::size_t>(valueCount));
                    for (int64_t i = valueCount - 1; i >= 0; --i) {
                        if (!popNative(ins, values[static_cast<std::size_t>(i)])) {
                            return false;
                        }
                    }

                    std::vector<std::string> dynamicValues;
                    std::vector<bool> ownsDynamicValues;
                    dynamicValues.reserve(values.size());
                    ownsDynamicValues.reserve(values.size());
                    for (const auto& value : values) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (value.functionRef ||
                            !emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return failNative(ins,
                                "[emit-llvm] 容器元素不能转换为 ZeusValue");
                        }
                        dynamicValues.push_back(dynamicValue);
                        ownsDynamicValues.push_back(ownsDynamic);
                    }

                    const std::string resultValue = ContainerABI::emitBuildCall(
                        dictionary, count, dynamicValues,
                        [&]() { return freshNativeTmp(); },
                        [&](const std::string& line) { emitNativeLine(line); });
                    for (std::size_t i = 0; i < dynamicValues.size(); ++i) {
                        if (ownsDynamicValues[i]) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValues[i] + ")");
                        }
                    }
                    for (const auto& value : values) {
                        if (isManagedClassValue(value) && value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LENGTH:
                case VMOperator::DICT_KEYS_BUILD: {
                    NativeValue container;
                    if (!popNative(ins, container) || container.functionRef) {
                        return failNative(ins, "[emit-llvm] 容器操作需要动态值");
                    }
                    std::string dynamicContainer;
                    bool ownsDynamic = false;
                    if (!emitNativeDynamicValue(
                            ins, container, dynamicContainer, ownsDynamic)) {
                        return false;
                    }
                    const std::string symbol = ins.op == VMOperator::LIST_LENGTH
                        ? "@zeus_value_array_length"
                        : "@zeus_value_dict_keys";
                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, { dynamicContainer },
                        [&]() { return freshNativeTmp(); },
                        [&](const std::string& line) { emitNativeLine(line); });
                    if (ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       dynamicContainer + ")");
                    }
                    if (isManagedClassValue(container) && container.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       container.value + ")");
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE: {
                    const bool isStore = ins.op == VMOperator::LIST_STORE_ELEM ||
                        ins.op == VMOperator::DICT_STORE_VALUE;
                    const bool isList = ins.op == VMOperator::LIST_LOAD_ELEM ||
                        ins.op == VMOperator::LIST_STORE_ELEM;
                    NativeValue storedValue;
                    NativeValue keyOrIndex;
                    NativeValue container;
                    if ((isStore && !popNative(ins, storedValue)) ||
                        !popNative(ins, keyOrIndex) ||
                        !popNative(ins, container) || storedValue.functionRef ||
                        keyOrIndex.functionRef || container.functionRef) {
                        return failNative(ins, "[emit-llvm] 容器索引操作的栈值无效");
                    }

                    const size_t parameterCount = isStore ? 3 : 2;
                    const std::string operatorSymbol = isStore ? "[]=" : "[]";
                    const FunctionSignature* operatorSignature = nullptr;
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                        std::string targetName;
                        if (!ins.metadata.readDirectCall(targetName)) {
                            return failNative(ins,
                                "[emit-llvm] 下标运算标注缺少函数名");
                        }
                        const auto target = functionSignatures.find(targetName);
                        if (target == functionSignatures.end() ||
                            target->second.operatorSymbol != operatorSymbol ||
                            target->second.parameterTypes.size() != parameterCount) {
                            return failNative(ins,
                                "[emit-llvm] 下标运算标注与目标签名不匹配");
                        }
                        operatorSignature = &target->second;
                        container.sourceType = operatorSignature->ownerType;
                    } else if (!container.sourceType.empty() &&
                               (isClassType(container.sourceType) ||
                                ZeusBuiltins::findNativeTypeABI(
                                    container.sourceType) != nullptr)) {
                        operatorSignature = findClassOperatorSignature(
                            container.sourceType, operatorSymbol, parameterCount);
                        if (operatorSignature == nullptr) {
                            return failNative(ins,
                                "[emit-llvm] type 没有匹配的下标operator");
                        }
                    }

                    if (!isList && operatorSignature != nullptr) {
                        const std::string qualifiedPrefix =
                            operatorSignature->ownerType + "::";
                        const std::string methodName = operatorSignature->name.substr(
                            qualifiedPrefix.size());
                        std::vector<NativeValue> operatorArguments{
                            container, keyOrIndex
                        };
                        if (isStore) operatorArguments.push_back(storedValue);
                        auto convertArgument = [&] (
                            const NativeValue& value,
                            const std::string& declaredType,
                            std::string& nativeArgument,
                            std::string& cleanupValue
                        ) {
                            bool ownsDynamic = false;
                            if (!emitFunctionABIArgument(
                                    ins, value, declaredType,
                                    nativeArgument, ownsDynamic)) {
                                return false;
                            }
                            cleanupValue = ownsDynamic ? nativeArgument : std::string();
                            return true;
                        };
                        NativeValue operatorResult;
                        if (!emitKnownClassMethodCall(
                                ins, methodName, operatorArguments, operatorResult,
                                [&](const std::string& line) { emitNativeLine(line); },
                                [&]() { return freshNativeTmp(); },
                                convertArgument,
                                [&](const std::string& value) {
                                    emitNativeLine("call void @zeus_value_release(ptr " +
                                                   value + ")");
                                },
                                [&](const IRType::IRInstr& at,
                                    const std::string& message) {
                                    return failNative(at, message);
                                },
                                exceptionCallScope)) {
                            return false;
                        }
                        valueStack.push_back(std::move(operatorResult));
                        break;
                    }

                    NativeValue operands[] = { container, keyOrIndex, storedValue };
                    std::string dynamicOperands[3];
                    bool ownsDynamicOperands[3] = { false, false, false };
                    const std::size_t operandCount = isStore ? 3 : 2;
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (!emitNativeDynamicValue(
                                ins, operands[i], dynamicOperands[i],
                                ownsDynamicOperands[i])) {
                            return false;
                        }
                    }

                    std::string symbol;
                    std::vector<std::string> callArguments;
                    if (isList && !isStore) {
                        symbol = "@zeus_value_array_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else if (isList) {
                        symbol = "@zeus_value_array_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    } else if (!isStore) {
                        symbol = "@zeus_value_subscript_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else {
                        symbol = "@zeus_value_subscript_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    }

                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, callArguments,
                        [&]() { return freshNativeTmp(); },
                        [&](const std::string& line) { emitNativeLine(line); });
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (ownsDynamicOperands[i]) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicOperands[i] + ")");
                        }
                        if (isManagedClassValue(operands[i]) &&
                            operands[i].ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           operands[i].value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::OBJ_BUILD: {
                    int64_t argc = 0;
                    const std::string nativeTargetType = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1])
                            ? std::get<std::string>(ins.args[1])
                            : std::string();
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc) +
                            (nativeTargetType.empty() ? 1u : 0u)) {
                        return failNative(ins, "[emit-llvm] class 构造参数数量无效");
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                    }

                    NativeValue classReference;
                    if (nativeTargetType.empty() &&
                        (!popNative(ins, classReference) || classReference.type != "classref")) {
                        return failNative(ins, "[emit-llvm] new 目标不是静态 class 名称");
                    }

                    NativeValue instance;
                    auto emitConstructionLine = [&](const std::string& line) {
                        emitNativeLine(line);
                    };
                    auto emitConstructionLabel = [&](const std::string& label) {
                        functionText += label + ":\n";
                        // The null-check split makes this label the block that
                        // falls through. Phi nodes must name the real LLVM
                        // predecessor, not the logical IR block we started in.
                        currentNativeBlock = "%" + label;
                    };
                    auto freshConstructionTemporary = [&]() {
                        return freshNativeTmp();
                    };
                    auto convertConstructorArgument = [&] (
                        const NativeValue& argument,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        bool& ownsDynamic
                    ) {
                        return emitFunctionABIArgument(
                            ins, argument, declaredType, nativeArgument, ownsDynamic);
                    };
                    auto failConstruction = [&] (
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        return failNative(at, message);
                    };
                    const bool constructionSucceeded = nativeTargetType.empty()
                        ? emitClassConstruction(
                            ins,
                            classReference.sourceType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            emitConstructionLabel,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction)
                        : emitNewOperatorConstruction(
                            ins,
                            nativeTargetType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction);
                    if (!constructionSucceeded) {
                        return false;
                    }

                    valueStack.push_back(std::move(instance));
                    break;
                }

                case VMOperator::MAKE_FUNCTION: {
                    int64_t childChunk = 0;
                    if (!tryArgInt(ins, 0, childChunk) || childChunk < 0) {
                        return failNative(ins, "[emit-llvm] MAKE_FUNCTION has an invalid chunk index");
                    }
                    const auto childSignature = functionSignaturesByChunk.find(
                        static_cast<size_t>(childChunk));
                    if (childSignature == functionSignaturesByChunk.end()) {
                        return failNative(ins, "[emit-llvm] MAKE_FUNCTION has no function signature");
                    }
                    const auto captures = captureDescriptors(static_cast<size_t>(childChunk));
                    if (childSignature->second.captureCount != static_cast<int>(captures.size())) {
                        return failNative(ins, "[emit-llvm] closure capture metadata count mismatch");
                    }

                    std::vector<std::string> cells;
                    std::vector<std::string> temporaryCells;
                    cells.reserve(captures.size());
                    for (const auto& capture : captures) {
                        if (capture.sourceIndex < 0) {
                            return failNative(ins, "[emit-llvm] closure capture has an invalid source slot");
                        }
                        if (capture.sourceKind == "upvalue") {
                            const std::string cell = freshNativeTmp();
                            emitNativeLine(cell + " = call ptr @zeus_closure_capture_" +
                                (capture.byReference ? "ref" : "copy") +
                                "(ptr %closure.env, i32 " + std::to_string(capture.sourceIndex) + ")");
                            cells.push_back(cell);
                            temporaryCells.push_back(cell);
                            continue;
                        }
                        if (capture.sourceKind != "local") {
                            return failNative(ins, "[emit-llvm] closure capture source kind is unsupported");
                        }
                        const auto parentCell = closureLocalCells.find(capture.sourceIndex);
                        if (capture.byReference) {
                            if (parentCell == closureLocalCells.end()) {
                                return failNative(ins, "[emit-llvm] reference capture source was not cellified");
                            }
                            cells.push_back(parentCell->second);
                            continue;
                        }

                        std::string cell;
                        if (parentCell != closureLocalCells.end()) {
                            cell = freshNativeTmp();
                            emitNativeLine(cell + " = call ptr @zeus_capture_cell_clone(ptr " +
                                           parentCell->second + ")");
                        } else {
                            const auto source = locals.find(capture.sourceIndex);
                            if (source == locals.end()) {
                                return failNative(ins, "[emit-llvm] copy capture source local is not initialized");
                            }
                            std::string dynamicSource;
                            bool ownsDynamicSource = false;
                            if (!emitNativeDynamicValue(ins, source->second,
                                    dynamicSource, ownsDynamicSource)) return false;
                            cell = freshNativeTmp();
                            emitNativeLine(cell + " = call ptr @zeus_capture_cell_create(ptr " +
                                           dynamicSource + ")");
                            if (ownsDynamicSource) {
                                emitNativeLine("call void @zeus_value_release(ptr " + dynamicSource + ")");
                            }
                        }
                        cells.push_back(cell);
                        temporaryCells.push_back(cell);
                    }

                    std::string cellsArray = "null";
                    if (!cells.empty()) {
                        cellsArray = freshNativeTmp();
                        emitNativeLine(cellsArray + " = alloca ptr, i64 " +
                                       std::to_string(cells.size()) + ", align 8");
                        for (size_t i = 0; i < cells.size(); ++i) {
                            const std::string slot = freshNativeTmp();
                            emitNativeLine(slot + " = getelementptr inbounds ptr, ptr " +
                                           cellsArray + ", i64 " + std::to_string(i));
                            emitNativeLine("store ptr " + cells[i] + ", ptr " + slot + ", align 8");
                        }
                    }

                    const std::string closure = freshNativeTmp();
                    emitNativeLine(closure + " = alloca %ZeusValue, align 8");
                    if (childSignature->second.hasDefaultParameters() ||
                        childSignature->second.hasVariadicArgs ||
                        childSignature->second.hasVariadicKwargs) {
                        emitNativeLine("call void @zeus_closure_create_range(ptr " + closure +
                            ", ptr " + closureAdapterSymbol(static_cast<size_t>(childChunk)) +
                            ", i32 " + std::to_string(childSignature->second.minRequiredParams) +
                            ", i32 " + std::to_string(
                                childSignature->second.hasVariadicArgs ||
                                childSignature->second.hasVariadicKwargs
                                    ? std::numeric_limits<int32_t>::max()
                                    : static_cast<int32_t>(childSignature->second.parameterTypes.size())) +
                            ", i32 " + std::to_string(cells.size()) + ", ptr " + cellsArray + ")");
                    } else {
                        emitNativeLine("call void @zeus_closure_create(ptr " + closure +
                            ", ptr " + closureAdapterSymbol(static_cast<size_t>(childChunk)) +
                            ", i32 " + std::to_string(childSignature->second.parameterTypes.size()) +
                            ", i32 " + std::to_string(cells.size()) + ", ptr " + cellsArray + ")");
                    }
                    for (const auto& cell : temporaryCells) {
                        emitNativeLine("call void @zeus_capture_cell_release(ptr " + cell + ")");
                    }
                    valueStack.push_back(NativeValue{
                        closure, "ptr", false, true, std::string(), true
                    });
                    break;
                }
                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index) || !popNative(ins, value)) {
                        return failNative(ins, "[emit-llvm] 原生函数局部变量存储失败");
                    }
                    const auto capturedCell = closureLocalCells.find(static_cast<int>(index));
                    if (capturedCell != closureLocalCells.end()) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) return false;
                        emitNativeLine("call void @zeus_capture_cell_store(ptr " +
                                       capturedCell->second + ", ptr " + dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        break;
                    }
                    if (value.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数不能把函数值存入原生局部变量");
                    }
                    if (index >= 0 &&
                        index < static_cast<int64_t>(signature.parameterTypes.size())) {
                        const std::string& declaredType =
                            signature.parameterTypes[static_cast<size_t>(index)];
                        std::string nativeValue;
                        bool ownsDynamic = false;
                        if (!emitFunctionABIArgument(
                                ins, value, declaredType, nativeValue, ownsDynamic)) {
                            return false;
                        }
                        const bool acquiredClassOwnership =
                            isClassType(declaredType) && value.dynamic;
                        if (acquiredClassOwnership) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           nativeValue + ")");
                            if (value.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               value.value + ")");
                            }
                        }
                        value = NativeValue{
                            nativeValue,
                            functionParameterABIType(declaredType),
                            false,
                            usesZeusValueABI(declaredType),
                            declaredType,
                            ownsDynamic
                        };
                        value.ownsClass = acquiredClassOwnership;
                    }
                    auto existing = locals.find(static_cast<int>(index));
                    if (existing != locals.end() && existing->second.dynamic &&
                        existing->second.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       existing->second.value + ")");
                    }
                    const bool releaseOldClass = existing != locals.end() &&
                        existing->second.ownsClass &&
                        isManagedClassValue(existing->second);
                    if (isManagedClassValue(value)) {
                        if (!value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           value.value + ")");
                        }
                        if (releaseOldClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           existing->second.value + ")");
                        }
                        value.ownsClass = true;
                        locals[static_cast<int>(index)] = std::move(value);
                    } else {
                        if (releaseOldClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           existing->second.value + ")");
                        }
                        if (value.dynamic && !value.ownsDynamic) {
                        NativeValue owned;
                        if (!cloneNativeDynamicValue(ins, value, owned)) return false;
                        locals[static_cast<int>(index)] = std::move(owned);
                        } else {
                            locals[static_cast<int>(index)] = value;
                        }
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] STORE_VALUE_SHARED 缺少名称");
                    }
                    NativeValue value;
                    if (!popNative(ins, value) || value.functionRef) return false;
                    std::string dynamicValue;
                    bool ownsDynamic = false;
                    if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) return false;
                    const char* operation = ins.op == VMOperator::STORE_VALUE_SHARED
                        ? "store" : "assign";
                    emitNativeLine(std::string("call void @zeus_shared_value_") + operation +
                        "(ptr " + poolStr(std::get<std::string>(ins.args[0])) +
                        ", ptr " + dynamicValue + ")");
                    if (ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                    }
                    if (value.ownsClass && isManagedClassValue(value)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " + value.value + ")");
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_GLOBAL:
                case VMOperator::ASSIGN_VALUE_GLOBAL: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index) || index < 0 ||
                        !popNative(ins, value) || value.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数全局变量存储失败");
                    }

                    if (value.dynamic && !value.ownsDynamic) {
                        NativeValue owned;
                        if (!cloneNativeDynamicValue(ins, value, owned)) return false;
                        value = std::move(owned);
                    }

                    const auto globalScalar =
                        nativeGlobalScalarTypes.find(static_cast<int>(index));
                    if (globalScalar != nativeGlobalScalarTypes.end() &&
                        !value.dynamic && value.type == globalScalar->second) {
                        if (value.type == "i64") {
                            emitNativeLine("call void @zeus_global_store_i64(i32 " +
                                std::to_string(index) + ", i64 " + value.value + ")");
                        } else {
                            const std::string raw = freshNativeTmp();
                            emitNativeLine(raw + " = zext i1 " + value.value + " to i32");
                            emitNativeLine("call void @zeus_global_store_bool(i32 " +
                                std::to_string(index) + ", i32 " + raw + ")");
                        }
                        break;
                    }

                    std::string dynamicValue;
                    bool ownsDynamic = false;
                    if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) {
                        return false;
                    }
                    entryDynamicValueGlobals.insert(static_cast<int>(index));
                    emitNativeLine("call void @zeus_value_global_store(i32 " +
                                   std::to_string(index) + ", ptr " + dynamicValue + ")");
                    if (isManagedClassValue(value) && value.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       value.value + ")");
                    }
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 || !std::holds_alternative<std::string>(ins.args[0]) ||
                        !std::holds_alternative<std::string>(ins.args[1])) {
                        return failNative(ins, "[emit-llvm] DELETE_VAR 参数无效");
                    }
                    const std::string& kind = std::get<std::string>(ins.args[1]);
                    if (kind == "shared") {
                        emitNativeLine("call i32 @zeus_shared_value_delete(ptr " +
                            poolStr(std::get<std::string>(ins.args[0])) + ")");
                        break;
                    }
                    int64_t index = 0;
                    if (!tryArgInt(ins, 2, index) || index < 0) {
                        return failNative(ins, "[emit-llvm] delete 变量索引无效");
                    }
                    if (kind == "global") {
                        emitNativeLine("call void @zeus_value_global_delete(i32 " +
                                       std::to_string(index) + ")");
                        break;
                    }
                    if (kind == "upvalue") {
                        emitNativeLine("call void @zeus_closure_env_delete(ptr %closure.env, i32 " +
                                       std::to_string(index) + ")");
                        break;
                    }
                    if (kind != "local") {
                        return failNative(ins, "[emit-llvm] DELETE_VAR 变量类型不支持");
                    }
                    const auto capturedCell = closureLocalCells.find(static_cast<int>(index));
                    if (capturedCell != closureLocalCells.end()) {
                        emitNativeLine("call void @zeus_capture_cell_delete(ptr " +
                                       capturedCell->second + ")");
                        break;
                    }
                    auto objectIt = locals.find(static_cast<int>(index));
                    if (objectIt == locals.end()) break;

                    if (objectIt->second.dynamic) {
                        emitNativeLine("call void @zeus_value_delete_object(ptr " +
                                       objectIt->second.value + ")");
                        if (objectIt->second.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           objectIt->second.value + ")");
                        }
                        locals.erase(objectIt);
                        break;
                    }
                    if (objectIt->second.type != "ptr" ||
                        !isClassType(objectIt->second.sourceType)) {
                        locals.erase(objectIt);
                        break;
                    }

                    const NativeValue object = objectIt->second;
                    auto emitDeleteLabel = [&](const std::string& label) {
                        functionText += label.substr(1) + ":\n";
                        // Same as the construction split: the tail label is the
                        // real predecessor of whatever comes next.
                        currentNativeBlock = label;
                    };
                    auto failDelete = [&](
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        return failNative(at, message);
                    };
                    if (!emitClassDelete(
                            ins, object, emitNativeLine, emitDeleteLabel,
                            freshNativeTmp, failDelete)) {
                        return false;
                    }

                    auto deletedLocal = locals.find(static_cast<int>(index));
                    if (deletedLocal != locals.end()) {
                        deletedLocal->second.ownsClass = false;
                    }
                    break;
                }

                case VMOperator::OBJ_LOAD_ATTR: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] 原生函数字段读取缺少字段名");
                    }

                    NativeValue object;
                    if (!popNative(ins, object) || object.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数字段读取的对象无效");
                    }
                    if (isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_require_live(ptr " +
                                       object.value + ")");
                    }

                    const std::string& fieldName = std::get<std::string>(ins.args[0]);
                    if (object.type == "namespace") {
                        const auto exports =
                            namespaceExportsByPackage.find(object.sourceType);
                        if (exports == namespaceExportsByPackage.end() ||
                            exports->second.find(fieldName) == exports->second.end() ||
                            !isClassType(fieldName)) {
                            return failNative(ins,
                                "[emit-llvm] unsupported static module member " +
                                object.sourceType + "." + fieldName);
                        }
                        valueStack.push_back(NativeValue{
                            fieldName, "classref", false, false, fieldName
                        });
                        break;
                    }

                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    if (layout == nullptr) {
                        return failNative(ins, "[emit-llvm] 原生函数字段读取的对象不是已知类布局");
                    }

                    const int fieldIndex = findFieldIndex(*layout, fieldName);
                    if (fieldIndex < 0 || static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        return failNative(ins, "[emit-llvm] 原生函数访问了不存在的字段'" + fieldName + "'");
                    }

                    const std::string& fieldTypeName =
                        layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldType = llvmFieldType(fieldTypeName);
                    if (fieldType.empty()) {
                        return failNative(ins, "[emit-llvm] 类字段类型无效");
                    }
                    const std::string fieldAddress = freshNativeTmp();
                    emitNativeLine(
                        fieldAddress + " = getelementptr inbounds " +
                        llvmStructType(object.sourceType) + ", ptr " + object.value +
                        ", i32 0, i32 " + std::to_string(fieldIndex)
                    );
                    const auto inferred = layout->inferredFieldFlags.find(fieldName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshNativeTmp();
                        const std::string initialized = freshNativeTmp();
                        const std::string classSymbol = poolStr(object.sourceType);
                        const std::string fieldSymbol = poolStr(fieldName);
                        emitNativeLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitNativeLine(initialized + " = load i1, ptr " + flagAddress);
                        const std::string initialized32 = freshNativeTmp();
                        emitNativeLine(initialized32 + " = zext i1 " + initialized + " to i32");
                        emitNativeLine("call void @zeus_class_field_require_initialized(i32 " +
                            initialized32 + ", ptr " + classSymbol + ", ptr " + fieldSymbol +
                            ", i32 " + std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                    }
                    if (usesZeusValueABI(fieldTypeName)) {
                        NativeValue loaded{
                            fieldAddress, "ptr", false, true, fieldTypeName, false
                        };
                        if (object.ownsClass && isManagedClassValue(object)) {
                            loaded.value = freshNativeTmp();
                            emitNativeLine(loaded.value + " = alloca %ZeusValue, align 8");
                            emitNativeLine("call void @zeus_value_clone(ptr " + loaded.value +
                                           ", ptr " + fieldAddress + ")");
                            loaded.ownsDynamic = true;
                        }
                        valueStack.push_back(std::move(loaded));
                        if (object.ownsClass && isManagedClassValue(object)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           object.value + ")");
                        }
                        break;
                    }
                    const std::string temporary = freshNativeTmp();
                    emitNativeLine(
                        temporary + " = load " + fieldType + ", ptr " + fieldAddress
                    );
                    NativeValue loaded{
                        temporary,
                        fieldType,
                        false,
                        false,
                        fieldTypeName
                    };
                    if (isClassType(fieldTypeName) &&
                        object.ownsClass && isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                       temporary + ")");
                        loaded.ownsClass = true;
                    }
                    valueStack.push_back(std::move(loaded));
                    if (object.ownsClass && isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       object.value + ")");
                    }
                    break;
                }

                case VMOperator::OBJ_STORE_ATTR: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] OBJ_STORE_ATTR 缺少字段名");
                    }

                    NativeValue value;
                    NativeValue object;
                    if (!popNative(ins, value) || !popNative(ins, object) ||
                        value.functionRef || object.functionRef) {
                        return failNative(ins, "[emit-llvm] OBJ_STORE_ATTR 操作数无效");
                    }
                    if (isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_require_live(ptr " +
                                       object.value + ")");
                    }

                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    if (layout == nullptr) {
                        return failNative(ins, "[emit-llvm] 对象没有已知的类布局");
                    }

                    const std::string& fieldName = std::get<std::string>(ins.args[0]);
                    const int fieldIndex = findFieldIndex(*layout, fieldName);
                    if (fieldIndex < 0 || static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        return failNative(ins, "[emit-llvm] 类不存在字段 '" + fieldName + "'");
                    }
                    if (static_cast<size_t>(fieldIndex) < layout->fieldConstness.size() &&
                        layout->fieldConstness[static_cast<size_t>(fieldIndex)]) {
                        return failNative(ins, "[emit-llvm] 不能修改 const 字段 '" + fieldName + "'");
                    }

                    const std::string& fieldTypeName =
                        layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldType = llvmFieldType(fieldTypeName);
                    const std::string fieldAddress = freshNativeTmp();
                    emitNativeLine(
                        fieldAddress + " = getelementptr inbounds " +
                        llvmStructType(object.sourceType) + ", ptr " + object.value +
                        ", i32 0, i32 " + std::to_string(fieldIndex)
                    );
                    const auto inferred = layout->inferredFieldFlags.find(fieldName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshNativeTmp();
                        emitNativeLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitNativeLine("store i1 1, ptr " + flagAddress);
                    }

                    if (usesZeusValueABI(fieldTypeName)) {
                        std::string sourceValue;
                        bool ownsSourceValue = false;
                        if (!emitNativeDynamicValue(
                                ins, value, sourceValue, ownsSourceValue)) {
                            return false;
                        }
                        const std::string ownedCopy = freshNativeTmp();
                        emitNativeLine(ownedCopy + " = alloca %ZeusValue, align 8");
                        emitNativeLine("call void @zeus_value_clone(ptr " + ownedCopy +
                                       ", ptr " + sourceValue + ")");
                        if (ownsSourceValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           sourceValue + ")");
                        }
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       fieldAddress + ")");
                        emitNativeLine("call void @zeus_value_clone(ptr " +
                                       fieldAddress + ", ptr " + ownedCopy + ")");
                        emitNativeLine("call void @zeus_value_release(ptr " + ownedCopy + ")");
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        if (object.ownsClass && isManagedClassValue(object)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           object.value + ")");
                        }
                        break;
                    }

                    std::string storedValue = value.value;
                    const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                    if (ZeusBuiltins::isNativeViewABI(fieldABI)) {
                        if (value.dynamic) {
                            bool ownsConvertedValue = false;
                            if (!emitFunctionABIArgument(
                                    ins, value, fieldTypeName, storedValue,
                                    ownsConvertedValue)) {
                                return false;
                            }
                            if (ownsConvertedValue) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               storedValue + ")");
                            }
                        } else {
                            if (value.type != fieldType) {
                                return failNative(ins,
                                    "[emit-llvm] 类字段赋值类型不匹配");
                            }
                            storedValue = freshNativeTmp();
                            emitNativeLine(storedValue + " = call ptr @" +
                                           std::string(ZeusBuiltins::nativeViewInternHelper(fieldABI)) +
                                           "(ptr " + value.value + ")");
                        }
                    } else if (value.dynamic) {
                        bool ownsConvertedValue = false;
                        if (!emitFunctionABIArgument(
                                ins, value, fieldTypeName, storedValue,
                                ownsConvertedValue)) {
                            return false;
                        }
                        if (ownsConvertedValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           storedValue + ")");
                        }
                    } else if (fieldType.empty() || value.type != fieldType ||
                               (isClassType(fieldTypeName) &&
                                value.sourceType != fieldTypeName)) {
                        return failNative(ins, "[emit-llvm] 类字段赋值类型不匹配");
                    }

                    if (isClassType(fieldTypeName)) {
                        if (!value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           storedValue + ")");
                        }
                        const std::string previous = freshNativeTmp();
                        emitNativeLine(previous + " = load ptr, ptr " + fieldAddress);
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       previous + ")");
                        emitNativeLine("store ptr " + storedValue + ", ptr " + fieldAddress);
                        if (value.dynamic && value.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           value.value + ")");
                        }
                        if (value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           storedValue + ")");
                        }
                    } else {
                        emitNativeLine(
                            "store " + fieldType + " " + storedValue + ", ptr " + fieldAddress
                        );
                    }
                    if (object.ownsClass && isManagedClassValue(object)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       object.value + ")");
                    }
                    break;
                }

                case VMOperator::LOGICAL_NOT: {
                    NativeValue operand;
                    if (!popNative(ins, operand) || operand.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数一元逻辑非操作数无效");
                    }

                    const std::string resultValue = freshNativeTmp();
                    if (!operand.dynamic && operand.type == "i1") {
                        emitNativeLine(resultValue + " = xor i1 " + operand.value + ", true");
                    } else {
                        std::string dynamicValue;
                        bool ownsDynamicValue = false;
                        if (!emitNativeDynamicValue(
                                ins, operand, dynamicValue, ownsDynamicValue)) {
                            return false;
                        }
                        const std::string truthy = freshNativeTmp();
                        emitNativeLine(truthy + " = call i32 @zeus_value_truthy(ptr " +
                                       dynamicValue + ")");
                        if (ownsDynamicValue) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (isManagedClassValue(operand) && operand.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           operand.value + ")");
                        }
                        emitNativeLine(resultValue + " = icmp eq i32 " + truthy + ", 0");
                    }
                    valueStack.push_back(NativeValue{ resultValue, "i1", false });
                    break;
                }

                case VMOperator::ADD:
                case VMOperator::SUBTRACT:
                case VMOperator::MULTIPLY:
                case VMOperator::DIVIDE:
                case VMOperator::MODULUS:
                case VMOperator::EQUL:
                case VMOperator::NOT_EQUAL:
                case VMOperator::GREATER_THAN:
                case VMOperator::LESS_THAN:
                case VMOperator::GREATER_EQUAL:
                case VMOperator::LESS_EQUAL:
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR: {
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            return failNative(ins, "[emit-llvm] 静态运算标注缺少函数名");
                        }
                    }

                    if (!pendingDirectCall.empty()) {
                        if (!emitNativeDirectCall(ins)) return false;
                        break;
                    }

                    NativeValue right;
                    NativeValue left;
                    if (!popNative(ins, right) || !popNative(ins, left) ||
                        left.functionRef || right.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数二元运算操作数无效");
                    }

                    // Class instances have no static operator, but string
                    // concatenation and equality are defined on the boxed form,
                    // so they go through the dynamic ABI as well.
                    const bool classOperands = !left.dynamic && !right.dynamic &&
                        (isClassType(left.sourceType) || isClassType(right.sourceType));
                    const bool classOperatorSupported =
                        ins.op == VMOperator::ADD || ins.op == VMOperator::EQUL ||
                        ins.op == VMOperator::NOT_EQUAL;

                    if (left.dynamic || right.dynamic ||
                        (classOperands && classOperatorSupported)) {
                        const int dynamicOpcode = DynamicBinaryABI::opcodeFor(ins.op);
                        if (dynamicOpcode < 0) {
                            return failNative(ins,
                                "[emit-llvm] ZeusValue 参数暂不支持该动态二元运算");
                        }

                        std::string leftDynamic;
                        std::string rightDynamic;
                        bool ownsLeftDynamic = false;
                        bool ownsRightDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, left, leftDynamic, ownsLeftDynamic) ||
                            !emitNativeDynamicValue(
                                ins, right, rightDynamic, ownsRightDynamic)) {
                            return false;
                        }

                        const std::string temporary = freshNativeTmp();
                        emitNativeLine(temporary + " = alloca %ZeusValue, align 8");
                        emitNativeLine("call void @zeus_value_dynamic_binary(ptr " +
                                       temporary + ", ptr null, i32 " +
                                       std::to_string(dynamicOpcode) + ", ptr " +
                                       leftDynamic + ", ptr " + rightDynamic + ")");
                        if (ownsLeftDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           leftDynamic + ")");
                        }
                        if (ownsRightDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           rightDynamic + ")");
                        }
                        for (const NativeValue* operand : { &left, &right }) {
                            if (isManagedClassValue(*operand) && operand->ownsClass) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               operand->value + ")");
                            }
                        }
                        valueStack.push_back(NativeValue{
                            temporary, "ptr", false, true, "", true
                        });
                        break;
                    }

                    std::string valueType;
                    std::string expression;
                    bool isComparison = false;

                    if (ins.op == VMOperator::LOGICAL_AND || ins.op == VMOperator::LOGICAL_OR) {
                        if (left.type != "i1" || right.type != "i1") {
                            return failNative(ins, "[emit-llvm] 逻辑运算要求 bool 操作数");
                        }
                        valueType = "i1";
                        expression = ins.op == VMOperator::LOGICAL_AND ? "and" : "or";
                    } else if (ins.op == VMOperator::EQUL || ins.op == VMOperator::NOT_EQUAL ||
                               ins.op == VMOperator::GREATER_THAN || ins.op == VMOperator::LESS_THAN ||
                               ins.op == VMOperator::GREATER_EQUAL || ins.op == VMOperator::LESS_EQUAL) {
                        if (left.type != right.type || (left.type != "i64" && left.type != "i1")) {
                            return failNative(ins, "[emit-llvm] 原生函数比较操作数类型不一致");
                        }
                        valueType = "i1";
                        isComparison = true;
                        if (ins.op == VMOperator::EQUL) expression = "eq";
                        if (ins.op == VMOperator::NOT_EQUAL) expression = "ne";
                        if (ins.op == VMOperator::GREATER_THAN) expression = "sgt";
                        if (ins.op == VMOperator::LESS_THAN) expression = "slt";
                        if (ins.op == VMOperator::GREATER_EQUAL) expression = "sge";
                        if (ins.op == VMOperator::LESS_EQUAL) expression = "sle";
                    } else {
                        if (left.type != "i64" || right.type != "i64") {
                            return failNative(ins, "[emit-llvm] 算术运算要求 int 操作数");
                        }
                        valueType = "i64";
                        if (ins.op == VMOperator::ADD) expression = "add";
                        if (ins.op == VMOperator::SUBTRACT) expression = "sub";
                        if (ins.op == VMOperator::MULTIPLY) expression = "mul";
                        if (ins.op == VMOperator::DIVIDE) expression = "sdiv";
                        if (ins.op == VMOperator::MODULUS) expression = "srem";
                    }

                    std::string temporary = freshNativeTmp();
                    const std::string llvmOpcode = isComparison
                        ? "icmp " + expression
                        : expression;
                    if (ins.op == VMOperator::DIVIDE || ins.op == VMOperator::MODULUS) {
                        emitNativeLine("call void @zeus_i64_check_division(i64 " +
                            left.value + ", i64 " + right.value + ", i32 " +
                            std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                    }
                    emitNativeLine(
                        temporary + " = " + llvmOpcode + " " + left.type + " " +
                        left.value + ", " + right.value
                    );
                    valueStack.push_back(NativeValue{ temporary, valueType, false });
                    break;
                }

                case VMOperator::LOAD_NATIVE: {
                    int64_t nativeId = 0;
                    if (!tryArgInt(ins, 0, nativeId)) {
                        return failNative(ins, "[emit-llvm] 原生函数 LOAD_NATIVE id 不是整数");
                    }
                    const auto* builtin = ZeusBuiltins::get(static_cast<int>(nativeId));
                    if (!builtin ||
                        (dynamicVariadicABISymbol(builtin->name).empty() &&
                         findNativeBuiltinDeclaration(builtin->name) == nullptr)) {
                        return failNative(ins, "[emit-llvm] 原生函数没有已注册的动态变长 ABI");
                    }
                    valueStack.push_back(NativeValue{
                        builtin->name, "builtin", true, false
                    });
                    break;
                }

                case VMOperator::CALL_NATIVE: {
                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        static_cast<size_t>(argc + 1) > valueStack.size()) {
                        return failNative(ins, "[emit-llvm] 原生动态变长调用参数数量或栈状态无效");
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                    }

                    NativeValue callee;
                    if (!popNative(ins, callee) || !callee.functionRef ||
                        callee.type != "builtin") {
                        return failNative(ins, "[emit-llvm] CALL_NATIVE 缺少原生函数目标");
                    }

                    std::string resultValue;
                    if (const auto* nativeDeclaration =
                            findNativeBuiltinDeclaration(callee.value)) {
                        if (arguments.size() < static_cast<size_t>(
                                nativeDeclaration->minRequiredParams) ||
                            arguments.size() > nativeDeclaration->parameterTypes.size()) {
                            return failNative(ins,
                                "[emit-llvm] @native builtin call argument count does not match '" +
                                callee.value + "'");
                        }

                        exceptionCallArguments = arguments;
                        exceptionCallDynamicValues.clear();
                        std::vector<std::string> nativeArguments;
                        std::vector<bool> ownsDynamicArguments;
                        for (size_t i = 0; i < nativeDeclaration->parameterTypes.size(); ++i) {
                            const std::string& parameterType =
                                nativeDeclaration->parameterTypes[i];
                            if (i >= arguments.size()) {
                                NativeValue placeholder;
                                if (!emitOptionalArgumentPlaceholder(
                                        parameterType, placeholder, emitNativeLine,
                                        freshNativeTmp)) {
                                    return failNative(ins,
                                        "[emit-llvm] @native builtin parameter has no default ABI placeholder");
                                }
                                nativeArguments.push_back(placeholder.value);
                                ownsDynamicArguments.push_back(placeholder.dynamic &&
                                                               placeholder.ownsDynamic);
                                if (placeholder.dynamic && placeholder.ownsDynamic) {
                                    exceptionCallDynamicValues.push_back(placeholder.value);
                                }
                                exceptionCallScope(
                                    true, exceptionCallArguments,
                                    exceptionCallDynamicValues);
                                continue;
                            }

                            std::string nativeArgument;
                            bool ownsDynamicArgument = false;
                            if (!emitFunctionABIArgument(
                                    ins, arguments[i], parameterType, nativeArgument,
                                    ownsDynamicArgument)) {
                                return false;
                            }
                            nativeArguments.push_back(std::move(nativeArgument));
                            ownsDynamicArguments.push_back(ownsDynamicArgument);
                            if (ownsDynamicArgument) {
                                exceptionCallDynamicValues.push_back(nativeArguments.back());
                            }
                            exceptionCallScope(
                                true, exceptionCallArguments,
                                exceptionCallDynamicValues);
                        }

                        const bool returnsDynamic =
                            usesZeusValueABI(nativeDeclaration->returnType);
                        const std::string returnType = returnsDynamic
                            ? "void" : llvmType(nativeDeclaration->returnType);
                        if (returnType.empty()) {
                            return failNative(ins,
                                "[emit-llvm] @native builtin return type has no LLVM ABI");
                        }

                        resultValue = freshNativeTmp();
                        std::string callText;
                        if (returnsDynamic) {
                            emitNativeLine(resultValue + " = alloca %ZeusValue, align 8");
                            callText = "call void " + llvmCallTarget(*nativeDeclaration) +
                                "(ptr " + resultValue;
                        } else {
                            callText = resultValue + " = call " + returnType + " " +
                                llvmCallTarget(*nativeDeclaration) + "(";
                        }
                        for (size_t i = 0; i < nativeArguments.size(); ++i) {
                            if (i > 0 || returnsDynamic) callText += ", ";
                            callText += functionParameterABIType(
                                nativeDeclaration->parameterTypes[i]) + " " +
                                nativeArguments[i];
                        }
                        if (nativeDeclaration->hasDefaultParameters()) {
                            if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
                            callText += "i64 " + std::to_string(providedPrefixMask(arguments.size()));
                        }
                        callText += ")";
                        emitNativeLine(callText);
                        exceptionCallScope(false, {}, {});

                        std::set<std::string> releasedDynamicArguments;
                        for (size_t i = 0; i < nativeArguments.size(); ++i) {
                            if (ownsDynamicArguments[i] &&
                                releasedDynamicArguments.insert(nativeArguments[i]).second) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               nativeArguments[i] + ")");
                            }
                        }
                        for (const auto& argument : arguments) {
                            if (argument.dynamic && argument.ownsDynamic &&
                                releasedDynamicArguments.insert(argument.value).second) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               argument.value + ")");
                            }
                            if (isManagedClassValue(argument) && argument.ownsClass) {
                                emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                               argument.value + ")");
                            }
                        }

                        if (returnsDynamic) {
                            valueStack.push_back(NativeValue{
                                resultValue, "ptr", false, true,
                                nativeDeclaration->returnType, true
                            });
                        } else {
                            const auto* returnABI = ZeusBuiltins::findNativeTypeABI(
                                nativeDeclaration->returnType);
                            if (ZeusBuiltins::isNativeViewABI(returnABI)) {
                                const std::string stableView = freshNativeTmp();
                                emitNativeLine(stableView + " = call ptr @" +
                                    std::string(ZeusBuiltins::nativeViewInternHelper(returnABI)) +
                                    "(ptr " + resultValue + ")");
                                resultValue = stableView;
                            }
                            valueStack.push_back(NativeValue{
                                resultValue, returnType, false, false,
                                nativeDeclaration->returnType, false
                            });
                            valueStack.back().ownsClass =
                                isClassType(nativeDeclaration->returnType);
                        }
                        break;
                    }

                    std::vector<std::string> dynamicArguments(arguments.size());
                    std::vector<bool> ownsDynamicArguments(arguments.size(), false);
                    exceptionCallArguments = arguments;
                    exceptionCallDynamicValues.clear();
                    for (size_t i = 0; i < arguments.size(); ++i) {
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, arguments[i], dynamicArguments[i], ownsDynamic)) {
                            return false;
                        }
                        ownsDynamicArguments[i] = ownsDynamic;
                        if (ownsDynamic) {
                            exceptionCallDynamicValues.push_back(dynamicArguments[i]);
                        }
                        exceptionCallScope(
                            true, exceptionCallArguments,
                            exceptionCallDynamicValues);
                    }
                    const std::string callSymbol = dynamicVariadicABISymbol(callee.value);
                    if (callSymbol.empty()) {
                        return failNative(ins,
                            "[emit-llvm] native call has no implementation ABI for '" +
                            callee.value + "'");
                    }
                    resultValue = emitDynamicVariadicCall(
                        callSymbol, callee.value, dynamicArguments,
                        emitNativeLine, freshNativeTmp);
                    exceptionCallScope(false, {}, {});
                    for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                        if (ownsDynamicArguments[i]) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicArguments[i] + ")");
                        }
                        if (isManagedClassValue(arguments[i]) &&
                            arguments[i].ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           arguments[i].value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, "nil", true
                    });
                    break;
                }

                case VMOperator::CALL_CLOSURE: {
                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            return failNative(ins, "[emit-llvm] CallTarget 元信息缺少函数名");
                        }
                        if (const auto* function = resolveFunctionSignature(pendingDirectCall)) {
                            pendingDirectCall = function->name;
                        }
                    }

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0) {
                        return failNative(ins, "[emit-llvm] 原生函数只支持带直接调用标注的 CALL_CLOSURE");
                    }

                    if (pendingDirectCall.empty()) {
                        if (argc < 0 || static_cast<size_t>(argc) + 1 > valueStack.size()) {
                            return failNative(ins, "[emit-llvm] 原生动态变长调用参数数量或栈状态无效");
                        }

                        std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                        for (int64_t i = argc - 1; i >= 0; --i) {
                            if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                        }

                        NativeValue callee;
                        if (!popNative(ins, callee)) return false;
                        if (callee.dynamic) {
                            NativeValue closureResult;
                            if (!emitClosureInvoke(ins, callee, arguments, closureResult)) return false;
                            valueStack.push_back(std::move(closureResult));
                            pendingDirectCall.clear();
                            break;
                        }
                        if (!callee.functionRef) {
                            return failNative(ins, "[emit-llvm] indirect call target is neither a closure nor a native function");
                        }
                        const std::string callSymbol = dynamicVariadicABISymbol(callee.value);
                        if (callSymbol.empty()) {
                            return failNative(ins, "[emit-llvm] 原生函数没有动态变长 ABI");
                        }

                        exceptionCallArguments = arguments;
                        exceptionCallDynamicValues.clear();
                        std::vector<std::string> dynamicArguments(arguments.size());
                        std::vector<bool> ownsDynamicArguments(arguments.size(), false);
                        for (size_t i = 0; i < arguments.size(); ++i) {
                            bool ownsDynamic = false;
                            if (!emitNativeDynamicValue(
                                    ins,
                                    arguments[i],
                                    dynamicArguments[i],
                                    ownsDynamic)) {
                                return false;
                            }
                            ownsDynamicArguments[i] = ownsDynamic;
                            if (ownsDynamic) {
                                exceptionCallDynamicValues.push_back(dynamicArguments[i]);
                                exceptionCallScope(
                                    true, exceptionCallArguments,
                                    exceptionCallDynamicValues);
                            }
                        }

                        const std::string resultValue = emitDynamicVariadicCall(
                            callSymbol,
                            callee.value,
                            dynamicArguments,
                            emitNativeLine,
                            freshNativeTmp
                        );
                        exceptionCallScope(false, {}, {});
                        for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                            if (ownsDynamicArguments[i]) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               dynamicArguments[i] + ")");
                            }
                        }

                        valueStack.push_back(NativeValue{
                            resultValue, "ptr", false, true, "nil", true
                        });
                        break;
                    }

                    auto function = functionSignatures.find(pendingDirectCall);
                    if (function == functionSignatures.end()) {
                        return failNative(ins, "[emit-llvm] 找不到直接调用目标'" + pendingDirectCall + "'");
                    }

                    const auto& called = function->second;
                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                    }

                    NativeValue callee;
                    if (!popNative(ins, callee)) return false;
                    if (callee.dynamic) {
                        NativeValue closureResult;
                        if (!emitClosureInvoke(ins, callee, arguments, closureResult)) return false;
                        valueStack.push_back(std::move(closureResult));
                        pendingDirectCall.clear();
                        break;
                    }
                    if (!callee.functionRef || callee.value != pendingDirectCall) {
                        return failNative(ins, "[emit-llvm] 原生函数调用缺少匹配的函数引用");
                    }

                    std::vector<NativeValue> orderedArguments;
                    std::vector<bool> providedArguments;
                    std::vector<NativeValue> variadicArguments;
                    std::vector<std::pair<std::string, NativeValue>> keywordArguments;
                    std::string bindingError;
                    if (!bindCallArguments(called, arguments, orderedArguments,
                            providedArguments, variadicArguments, keywordArguments,
                            bindingError)) {
                        return failNative(ins, "[emit-llvm] " + bindingError);
                    }
                    exceptionCallArguments = arguments;
                    exceptionCallDynamicValues.clear();

                    const bool calledReturnsDynamic = usesZeusValueABI(called.returnType);
                    std::string calledReturnType = calledReturnsDynamic
                        ? "void"
                        : llvmType(called.returnType);
                    if (calledReturnType.empty()) {
                        return failNative(ins, "[emit-llvm] 直接调用目标返回类型没有 LLVM ABI 映射");
                    }

                    std::string resultValue = freshNativeTmp();
                    if (calledReturnsDynamic) {
                        emitNativeLine(resultValue + " = alloca %ZeusValue, align 8");
                    }
                    std::string callText = calledReturnsDynamic
                        ? "call void " + llvmCallTarget(called.name) + "(ptr " + resultValue
                        : resultValue + " = call " + calledReturnType + " " + llvmCallTarget(called.name) + "(";
                    std::vector<std::string> dynamicCallArguments;
                    std::vector<bool> ownsDynamicCallArguments;
                    for (size_t i = 0; i < called.parameterTypes.size(); ++i) {
                        if (!providedArguments[i]) {
                            NativeValue placeholder;
                            if (!emitOptionalArgumentPlaceholder(
                                    called.parameterTypes[i], placeholder,
                                    emitNativeLine, freshNativeTmp)) {
                                return failNative(ins,
                                    "[emit-llvm] optional argument has no native ABI placeholder");
                            }
                            if (i > 0 || calledReturnsDynamic) callText += ", ";
                            callText += functionParameterABIType(called.parameterTypes[i]) +
                                        " " + placeholder.value;
                            continue;
                        }
                        if (i > 0 || calledReturnsDynamic) callText += ", ";
                        std::string nativeArgument;
                        bool ownsDynamicArgument = false;
                        if (!emitFunctionABIArgument(
                                ins,
                                orderedArguments[i],
                                called.parameterTypes[i],
                                nativeArgument,
                                ownsDynamicArgument)) {
                            return false;
                        }
                        callText += functionParameterABIType(called.parameterTypes[i]) +
                                    " " + nativeArgument;
                        if (usesZeusValueABI(called.parameterTypes[i])) {
                            dynamicCallArguments.push_back(nativeArgument);
                            ownsDynamicCallArguments.push_back(ownsDynamicArgument);
                            if (ownsDynamicArgument) {
                                exceptionCallDynamicValues.push_back(nativeArgument);
                                exceptionCallScope(
                                    true, exceptionCallArguments,
                                    exceptionCallDynamicValues);
                            }
                        }
                    }
                    std::string packedArgs;
                    std::string packedKwargs;
                    if (called.hasVariadicArgs || called.hasVariadicKwargs) {
                        if (!emitVariadicContainers(
                                called.hasVariadicArgs, called.hasVariadicKwargs,
                                variadicArguments, keywordArguments,
                                [&](const NativeValue& value, std::string& out, bool& owns) {
                                    return emitNativeDynamicValue(ins, value, out, owns);
                                }, freshNativeTmp, emitNativeLine, packedArgs, packedKwargs)) {
                            return false;
                        }
                        if (called.hasVariadicArgs) {
                            if (!called.parameterTypes.empty() || calledReturnsDynamic) callText += ", ";
                            callText += "ptr " + packedArgs;
                            exceptionCallDynamicValues.push_back(packedArgs);
                        }
                        if (called.hasVariadicKwargs) {
                            if (!called.parameterTypes.empty() || calledReturnsDynamic ||
                                called.hasVariadicArgs) callText += ", ";
                            callText += "ptr " + packedKwargs;
                            exceptionCallDynamicValues.push_back(packedKwargs);
                        }
                        exceptionCallScope(
                            true, exceptionCallArguments,
                            exceptionCallDynamicValues);
                    }
                    if (called.hasDefaultParameters()) {
                        if (!called.parameterTypes.empty() || calledReturnsDynamic ||
                            called.hasVariadicArgs || called.hasVariadicKwargs) callText += ", ";
                        callText += "i64 " + std::to_string(providedArgumentMask(providedArguments));
                    }
                    callText += ")";

                    emitNativeLine(callText);
                    exceptionCallScope(false, {}, {});
                    if (!packedArgs.empty()) emitNativeLine("call void @zeus_value_release(ptr " + packedArgs + ")");
                    if (!packedKwargs.empty()) emitNativeLine("call void @zeus_value_release(ptr " + packedKwargs + ")");
                    const auto* calledReturnABI =
                        ZeusBuiltins::findNativeTypeABI(called.returnType);
                    if (!calledReturnsDynamic &&
                        ZeusBuiltins::isNativeViewABI(calledReturnABI)) {
                        const std::string stableView = freshNativeTmp();
                        emitNativeLine(stableView + " = call ptr @" +
                                       std::string(ZeusBuiltins::nativeViewInternHelper(
                                           calledReturnABI)) + "(ptr " +
                                       resultValue + ")");
                        resultValue = stableView;
                    }
                    std::set<std::string> releasedDynamicArguments;
                    for (size_t i = 0; i < dynamicCallArguments.size(); ++i) {
                        if (ownsDynamicCallArguments[i] &&
                            releasedDynamicArguments.insert(dynamicCallArguments[i]).second) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicCallArguments[i] + ")");
                        }
                    }
                    for (const auto& argument : arguments) {
                        if (argument.dynamic && argument.ownsDynamic &&
                            releasedDynamicArguments.insert(argument.value).second) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           argument.value + ")");
                        }
                        if (isManagedClassValue(argument) && argument.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           argument.value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue,
                        calledReturnsDynamic ? "ptr" : calledReturnType,
                        false,
                        calledReturnsDynamic,
                        isReferenceTypeName(called.returnType) &&
                                isClassType(referencedTypeName(called.returnType))
                            ? referencedTypeName(called.returnType)
                            : called.returnType,
                        calledReturnsDynamic
                    });
                    valueStack.back().ownsClass =
                        !calledReturnsDynamic && isClassType(called.returnType);
                    pendingDirectCall.clear();
                    break;
                }

                case VMOperator::OBJ_CALL_METHOD: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        return failNative(ins, "[emit-llvm] class method call is missing its method name");
                    }

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 1, argc) || argc < 0 ||
                        static_cast<size_t>(argc) + 1 > valueStack.size()) {
                        return failNative(ins, "[emit-llvm] class method call has invalid arguments");
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc) + 1);
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i) + 1])) {
                            return false;
                        }
                    }
                    if (!popNative(ins, arguments[0])) return false;

                    auto convertArgument = [&] (
                        const NativeValue& value,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        std::string& cleanupValue
                    ) {
                        cleanupValue.clear();
                        if (usesZeusValueABI(declaredType)) {
                            bool ownsDynamic = false;
                            if (!emitNativeDynamicValue(
                                    ins, value, nativeArgument, ownsDynamic)) {
                                return false;
                            }
                            if (ownsDynamic) cleanupValue = nativeArgument;
                            return true;
                        }

                        bool ignoredOwnership = false;
                        return emitFunctionABIArgument(
                            ins, value, declaredType, nativeArgument, ignoredOwnership);
                    };
                    auto releaseDynamic = [&](const std::string& value) {
                        emitNativeLine("call void @zeus_value_release(ptr " + value + ")");
                    };
                    auto failMethod = [&](const IRType::IRInstr& at,
                                          const std::string& message) {
                        return failNative(at, message);
                    };

                    NativeValue callResult;
                    const std::string methodName = std::get<std::string>(ins.args[0]);
                    const bool hasTypedImplMethod = hasTypedInstanceMethodSignature(
                        arguments[0].sourceType, methodName);
                    const bool staticClassReceiver = arguments[0].type == "classref" &&
                        isClassType(arguments[0].sourceType);
                    if (arguments[0].dynamic ||
                        (arguments[0].type != "ptr" && !staticClassReceiver) ||
                        (!isClassType(arguments[0].sourceType) && !hasTypedImplMethod)) {
                        auto toDynamic = [&](const NativeValue& value,
                                             std::string& dynamicValue,
                                             bool& ownsDynamic) {
                            return emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic);
                        };
                        if (!emitDynamicMemberCall(
                                ins,
                                methodName,
                                arguments,
                                callResult,
                                emitNativeLine,
                                freshNativeTmp,
                                toDynamic,
                                releaseDynamic,
                                failMethod,
                                exceptionCallScope)) {
                            return false;
                        }
                    } else if (!emitKnownClassMethodCall(
                                   ins,
                                   methodName,
                                   arguments,
                                   callResult,
                                   emitNativeLine,
                                   freshNativeTmp,
                                   convertArgument,
                                   releaseDynamic,
                                   failMethod,
                                   exceptionCallScope)) {
                        return false;
                    }
                    valueStack.push_back(std::move(callResult));
                    break;
                }

                case VMOperator::JUMP:
                case VMOperator::LOOP_BREAK: {
                    if (!valueStack.empty()) {
                        return failNative(ins,
                            "LLVM 函数无条件分支要求操作数栈为空");
                    }
                    const auto target = nativeBranchTargets.find(pc);
                    if (target == nativeBranchTargets.end() ||
                        !recordNativeEdge(target->second, ins, pc)) {
                        return failNative(ins,
                            "LLVM 函数无条件分支缺少有效目标块");
                    }
                    emitNativeLine("br label " + nativeLabelForPc(target->second));
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::JUMP_IF_PARAM_PROVIDED: {
                    int64_t parameterIndex = -1;
                    if (!tryArgInt(ins, 0, parameterIndex) || parameterIndex < 0 ||
                        parameterIndex >= static_cast<int64_t>(
                            signature.defaultParameterMask.size()) ||
                        !signature.defaultParameterMask[static_cast<size_t>(parameterIndex)] ||
                        !signature.hasDefaultParameters()) {
                        return failNative(ins,
                            "LLVM default-parameter branch refers to a non-default parameter");
                    }
                    if (!valueStack.empty()) {
                        return failNative(ins,
                            "LLVM default-parameter branch requires an empty VM value stack");
                    }

                    const auto target = nativeBranchTargets.find(pc);
                    if (target == nativeBranchTargets.end()) {
                        return failNative(ins,
                            "LLVM default-parameter branch has no verified target");
                    }
                    const size_t fallthrough = pc + 1 > functionExitPc
                        ? functionExitPc
                        : pc + 1;
                    if (!recordNativeEdge(target->second, ins, pc) ||
                        !recordNativeEdge(fallthrough, ins, pc)) {
                        return false;
                    }

                    const std::string isProvided = freshNativeTmp();
                    const std::string providedBit = freshNativeTmp();
                    emitNativeLine(providedBit + " = and i64 %provided.mask, " +
                                   std::to_string(uint64_t{1} << parameterIndex));
                    emitNativeLine(isProvided + " = icmp ne i64 " + providedBit + ", 0");
                    emitNativeLine("br i1 " + isProvided + ", label " +
                                   nativeLabelForPc(target->second) + ", label " +
                                   nativeLabelForPc(fallthrough));
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::JUMP_IF_TRUE: {
                    NativeValue condition;
                    if (!popNative(ins, condition) || condition.functionRef) {
                        return failNative(ins,
                            "LLVM 函数条件分支的条件值无效");
                    }
                    if (!valueStack.empty()) {
                        return failNative(ins,
                            "LLVM 函数条件分支要求条件值之外的操作数栈为空");
                    }

                    std::string booleanValue;
                    if (condition.dynamic) {
                        const std::string truthy = freshNativeTmp();
                        emitNativeLine(truthy +
                            " = call i32 @zeus_value_truthy(ptr " + condition.value + ")");
                        if (condition.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           condition.value + ")");
                        }
                        booleanValue = freshNativeTmp();
                        emitNativeLine(booleanValue +
                            " = icmp ne i32 " + truthy + ", 0");
                    } else if (condition.type == "i1") {
                        booleanValue = condition.value;
                    } else if (condition.type == "i64") {
                        booleanValue = freshNativeTmp();
                        emitNativeLine(booleanValue + " = icmp ne i64 " +
                                       condition.value + ", 0");
                    } else {
                        return failNative(ins,
                            "LLVM 函数条件分支暂不支持该条件类型");
                    }

                    const auto target = nativeBranchTargets.find(pc);
                    if (target == nativeBranchTargets.end()) {
                        return failNative(ins,
                            "LLVM 函数条件分支缺少已验证的目标块");
                    }
                    const size_t fallthrough = pc + 1 > functionExitPc
                        ? functionExitPc
                        : pc + 1;
            if (!recordNativeEdge(target->second, ins, pc) ||
                !recordNativeEdge(fallthrough, ins, pc)) {
                        return false;
                    }

                    const std::string targetLabel =
                        nativeLabelForPc(target->second);
                    const std::string fallthroughLabel =
                        nativeLabelForPc(fallthrough);
                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        emitNativeLine("br i1 " + booleanValue + ", label " +
                                       fallthroughLabel + ", label " + targetLabel);
                    } else {
                        emitNativeLine("br i1 " + booleanValue + ", label " +
                                       targetLabel + ", label " + fallthroughLabel);
                    }
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO: {
                    NativeValue checked;
                    if (!popNative(ins, checked)) return false;
                    const std::string checkCall = rangeCheckCall(checked, ins);
                    if (!checkCall.empty()) emitNativeLine(checkCall);
                    if (checked.dynamic && checked.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       checked.value + ")");
                    }
                    break;
                }

                case VMOperator::RETURN: {
                    if (returnsDynamic) {
                        NativeValue value;
                        if (!popNative(ins, value) || value.functionRef) {
                            return failNative(ins, "[emit-llvm] ZeusValue 返回值无效");
                        }
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (!emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic)) {
                            return false;
                        }
                        emitNativeLine("call void @zeus_value_clone(ptr %return.out, ptr " +
                                       dynamicValue + ")");
                        if (ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicValue + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                        releaseOwnedFunctionLocals();
                        emitNativeLine("ret void");
                    } else {
                        NativeValue value;
                        if (!popNative(ins, value) || value.functionRef) {
                            return failNative(ins, "[emit-llvm] 原生函数返回值类型不匹配");
                        }
                        std::string nativeValue;
                        bool ignoredOwnership = false;
                        if (!emitFunctionABIArgument(
                                ins,
                                value,
                                signature.returnType,
                                nativeValue,
                                ignoredOwnership)) {
                            return false;
                        }
                        if (isClassType(signature.returnType) && value.dynamic) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           nativeValue + ")");
                            if (value.ownsDynamic) {
                                emitNativeLine("call void @zeus_value_release(ptr " +
                                               value.value + ")");
                            }
                        } else if (isClassType(signature.returnType) &&
                                   isManagedClassValue(value) && !value.ownsClass) {
                            emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                           nativeValue + ")");
                        }
                        releaseOwnedFunctionLocals();
                        emitNativeLine("ret " + returnType + " " + nativeValue);
                    }
                    nativeBlockTerminated = true;
                    break;
                }

                case VMOperator::CLEAR:
                    for (const auto& value : valueStack) {
                        if (value.dynamic && value.ownsDynamic) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           value.value + ")");
                        } else if (value.ownsClass && isManagedClassValue(value)) {
                            emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                           value.value + ")");
                        }
                    }
                    valueStack.clear();
                    break;

                case VMOperator::COPY: {
                    if (valueStack.empty()) {
                        return failNative(ins, "[emit-llvm] 原生函数值栈无法 COPY");
                    }
                    NativeValue copy = valueStack.back();
                    if (isManagedClassValue(copy) && copy.ownsClass) {
                        emitNativeLine("call void @zeus_class_ref_retain(ptr " +
                                       copy.value + ")");
                    } else if (copy.dynamic && copy.ownsDynamic) {
                        NativeValue cloned;
                        if (!cloneNativeDynamicValue(ins, copy, cloned)) return false;
                        copy = std::move(cloned);
                    }
                    valueStack.push_back(std::move(copy));
                    break;
                }

                case VMOperator::SWAP:
                    if (valueStack.size() < 2) {
                        return failNative(ins, "[emit-llvm] 原生函数值栈无法 SWAP");
                    }
                    std::swap(valueStack[valueStack.size() - 1],
                              valueStack[valueStack.size() - 2]);
                    break;

                case VMOperator::POP: {
                    NativeValue value;
                    if (!popNative(ins, value)) return false;
                    if (value.dynamic && value.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       value.value + ")");
                    } else if (value.ownsClass && isManagedClassValue(value)) {
                        emitNativeLine("call void @zeus_class_ref_release(ptr " +
                                       value.value + ")");
                    }
                    break;
                }

                case VMOperator::ASSIGN_VALUE_UPVALUE: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index) || !popNative(ins, value) ||
                        index < 0 || index >= signature.captureCount) {
                        return failNative(ins, "[emit-llvm] upvalue assignment slot is invalid");
                    }
                    std::string dynamicValue;
                    bool ownsDynamic = false;
                    if (!emitNativeDynamicValue(ins, value, dynamicValue, ownsDynamic)) return false;
                    emitNativeLine("call void @zeus_closure_env_store(ptr %closure.env, i32 " +
                                   std::to_string(index) + ", ptr " + dynamicValue + ")");
                    if (ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                    }
                    break;
                }

                case VMOperator::CAPTURE_OUTSIDE_VAR:
                    break;

                default:
                    return failNative(ins, "[emit-llvm] 原生函数暂不支持操作 " + VMO_toString(ins.op));
            }

            if (!nativeBlockTerminated && !recordNativeFallthrough(pc, ins)) {
                return false;
            }
        }

        if (nativeExitReachable && functionExitPc == chunk.size()) {
            if (chunk.empty() || !enterNativeBlock(functionExitPc, chunk.back()) ||
                !emitImplicitNilReturn()) {
                return false;
            }
        }

        for (const NativeLoopPhiPatch& patch : nativeLoopPhiPatches) {
            const auto incoming = nativeIncomingStates.find(patch.headerPc);
            if (incoming == nativeIncomingStates.end()) {
                result.error = "[emit-llvm] loop phi is missing incoming states";
                return false;
            }
            const auto edge = std::find_if(
                incoming->second.begin(), incoming->second.end(),
                [&](const NativeIncomingState& state) {
                    return state.sourcePc == patch.sourcePc;
                }
            );
            if (edge == incoming->second.end()) {
                result.error = "[emit-llvm] loop phi is missing a backedge";
                return false;
            }
            const auto local = edge->locals.find(patch.localIndex);
            if (local == edge->locals.end() ||
                local->second.type != patch.type ||
                local->second.dynamic != patch.dynamic ||
                local->second.sourceType != patch.sourceType ||
                local->second.functionRef ||
                (patch.dynamic && !local->second.ownsDynamic)) {
                result.error = "[emit-llvm] loop-carried local changed ABI type or ownership in '" +
                    signature.name + "' (local " +
                    std::to_string(patch.localIndex) + ")";
                return false;
            }

            const size_t value = functionText.find(patch.placeholder);
            if (value == std::string::npos) {
                result.error = "[emit-llvm] loop phi placeholder was lost during lowering";
                return false;
            }
            functionText.replace(value, patch.placeholder.size(), local->second.value);

            const size_t label = functionText.find(patch.labelPlaceholder);
            if (label == std::string::npos) {
                result.error = "[emit-llvm] loop phi predecessor placeholder was lost";
                return false;
            }
            functionText.replace(label, patch.labelPlaceholder.size(), edge->predecessor);
        }

        if (exceptionEdgeFailed) {
            if (result.error.empty()) result.error = "[emit-llvm] exception edge could not merge local state";
            return false;
        }
        for (const NativeExceptionPad& pad : exceptionPads) {
            if (pad.cleanupOnly) {
                const std::string cleanupToken = freshNativeTmp();
                functionText += pad.switchLabel.substr(1) + ":\n";
                functionText += "  " + cleanupToken + " = cleanuppad within none []\n";
                for (const auto& cleanupCall : pad.cleanupCalls) {
                    functionText += "  " + cleanupCall +
                        " [ \"funclet\"(token " + cleanupToken + ") ]\n";
                }
                functionText += "  cleanupret from " + cleanupToken +
                    " unwind to caller\n";
                continue;
            }
            const std::string switchToken = freshNativeTmp();
            const std::string catchToken = freshNativeTmp();
            functionText += pad.switchLabel.substr(1) + ":\n";
            functionText += "  " + switchToken +
                " = catchswitch within none [label " + pad.padLabel +
                "] unwind to caller\n";
            functionText += pad.padLabel.substr(1) + ":\n";
            functionText += "  " + catchToken + " = catchpad within " + switchToken +
                " [ptr null, i32 64, ptr null]\n";
            functionText += "  call void @zeus_exception_capture(ptr " + pad.valueSlot +
                ", ptr @__zeus_make_runtime_error) [ \"funclet\"(token " + catchToken + ") ]\n";
            for (const auto& cleanupCall : pad.cleanupCalls) {
                functionText += "  " + cleanupCall +
                    " [ \"funclet\"(token " + catchToken + ") ]\n";
            }
            functionText += "  catchret from " + catchToken + " to label " +
                pad.resumeLabel + "\n";
            functionText += pad.resumeLabel.substr(1) + ":\n";
            functionText += "  br label " + nativeLabelForPc(pad.catchPc) + "\n";
        }

        // Hoist the body allocas to the function entry: an alloca inside a loop
        // grows the frame on every iteration at -O0.
        std::string hoistedAllocas;
        const std::string hoistedBody = takeAllocas(functionText, nativeEntryBody, hoistedAllocas);
        functionText = functionText.substr(0, nativeEntryBody) + hoistedAllocas + hoistedBody;
        functionText += "}\n\n";
        nativeFunctions += functionText;

        if (needsClosureAdapter(ci)) {
        std::string adapterText;
        adapterText += "define void " + closureAdapterSymbol(ci) +
            "(ptr %out, ptr %env, i32 %argc, ptr %args, ptr %argument.names) {\nentry:\n";
        std::vector<std::string> adapterArguments;
        adapterArguments.reserve(signature.parameterTypes.size());
        int adapterTmp = 0;
        auto freshAdapterTmp = [&]() {
            return std::string("%a") + std::to_string(adapterTmp++);
        };
        auto emitAdapterLine = [&](const std::string& line) {
            adapterText += "  " + line + "\n";
        };

        const size_t regularParameterCount = signature.parameterTypes.size();
        if (signature.parameterNames.size() != regularParameterCount) {
            result.error = "[emit-llvm] closure signature has incomplete parameter-name metadata";
            return false;
        }

        std::string orderedArgumentArray = "null";
        if (regularParameterCount != 0) {
            orderedArgumentArray = freshAdapterTmp();
            emitAdapterLine(orderedArgumentArray + " = alloca ptr, i64 " +
                            std::to_string(regularParameterCount) + ", align 8");
        }
        std::string parameterNameArray = "null";
        if (regularParameterCount != 0) {
            parameterNameArray = freshAdapterTmp();
            emitAdapterLine(parameterNameArray + " = alloca ptr, i64 " +
                            std::to_string(regularParameterCount) + ", align 8");
            for (size_t i = 0; i < regularParameterCount; ++i) {
                const std::string slot = freshAdapterTmp();
                const std::string nameSymbol = poolStr(signature.parameterNames[i]);
                const std::string namePointer = freshAdapterTmp();
                emitAdapterLine(slot + " = getelementptr inbounds ptr, ptr " +
                    parameterNameArray + ", i64 " + std::to_string(i));
                emitAdapterLine(namePointer + " = getelementptr inbounds [" +
                    std::to_string(signature.parameterNames[i].size() + 1) +
                    " x i8], ptr " + nameSymbol + ", i64 0, i64 0");
                emitAdapterLine("store ptr " + namePointer + ", ptr " + slot +
                    ", align 8");
            }
        }

        const std::string providedMaskStorage = freshAdapterTmp();
        emitAdapterLine(providedMaskStorage + " = alloca i64, align 8");
        std::string variadicArgsValue = "null";
        if (signature.hasVariadicArgs) {
            variadicArgsValue = freshAdapterTmp();
            emitAdapterLine(variadicArgsValue + " = alloca %ZeusValue, align 8");
        }
        std::string variadicKwargsValue = "null";
        if (signature.hasVariadicKwargs) {
            variadicKwargsValue = freshAdapterTmp();
            emitAdapterLine(variadicKwargsValue + " = alloca %ZeusValue, align 8");
        }
        emitAdapterLine(
            "call void @zeus_closure_bind_arguments(i32 " +
            std::to_string(regularParameterCount) + ", ptr " + parameterNameArray +
            ", i64 " + std::to_string(providedArgumentMask(
                signature.defaultParameterMask)) +
            ", i32 " + std::to_string(signature.hasVariadicArgs ? 1 : 0) +
            ", i32 " + std::to_string(signature.hasVariadicKwargs ? 1 : 0) +
            ", i32 %argc, ptr %args, ptr %argument.names, ptr " +
            orderedArgumentArray + ", ptr " + providedMaskStorage +
            ", ptr " + variadicArgsValue + ", ptr " + variadicKwargsValue + ")");
        const std::string providedMaskValue = freshAdapterTmp();
        emitAdapterLine(providedMaskValue + " = load i64, ptr " +
                        providedMaskStorage + ", align 8");

        for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
            const std::string& declaredType = signature.parameterTypes[i];
            const std::string abiType = functionParameterABIType(declaredType);
            auto emitProvidedArgument = [&](std::string& nativeArgument) {
                const std::string slot = freshAdapterTmp();
                const std::string dynamicArgument = freshAdapterTmp();
                emitAdapterLine(slot + " = getelementptr inbounds ptr, ptr " +
                                orderedArgumentArray + ", i64 " + std::to_string(i));
                emitAdapterLine(dynamicArgument + " = load ptr, ptr " + slot + ", align 8");
                nativeArgument = dynamicArgument;
                if (usesZeusValueABI(declaredType)) return true;

                const std::string expectedType = llvmType(declaredType);
                if (expectedType == "i64") {
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(nativeArgument + " = call i64 @zeus_value_require_i64(ptr " +
                                    dynamicArgument + ")");
                } else if (expectedType == "i1") {
                    const std::string raw = freshAdapterTmp();
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(raw + " = call i32 @zeus_value_require_bool(ptr " +
                                    dynamicArgument + ")");
                    emitAdapterLine(nativeArgument + " = icmp ne i32 " + raw + ", 0");
                } else if (expectedType == "ptr" && isReferenceTypeName(declaredType)) {
                    const std::string referenced = referencedTypeName(declaredType);
                    nativeArgument = freshAdapterTmp();
                    if (referenced == "int" || referenced == "int32" || referenced == "int64") {
                        emitAdapterLine(nativeArgument +
                            " = call ptr @zeus_value_require_i64_ptr(ptr " + dynamicArgument + ")");
                    } else if (referenced == "bool") {
                        emitAdapterLine(nativeArgument +
                            " = call ptr @zeus_value_require_bool_ptr(ptr " + dynamicArgument + ")");
                    } else if (isClassType(referenced)) {
                        const std::string typeName = poolStr(referenced);
                        emitAdapterLine(nativeArgument +
                            " = call ptr @zeus_value_require_class_ptr(ptr " + dynamicArgument +
                            ", ptr " + typeName + ")");
                    } else {
                        result.error = "[emit-llvm] closure ABI cannot unbox reference parameter '" +
                            declaredType + "'";
                        return false;
                    }
                } else if (expectedType == "ptr" && isClassType(declaredType)) {
                    const std::string typeName = poolStr(declaredType);
                    nativeArgument = freshAdapterTmp();
                    emitAdapterLine(nativeArgument +
                        " = call ptr @zeus_value_require_class_ptr(ptr " + dynamicArgument +
                        ", ptr " + typeName + ")");
                } else if (expectedType == "ptr") {
                    const auto* abi = ZeusBuiltins::findNativeTypeABI(declaredType);
                    if (ZeusBuiltins::isNativeViewABI(abi)) {
                        nativeArgument = freshAdapterTmp();
                        emitAdapterLine(nativeArgument + " = call ptr @" +
                            std::string(ZeusBuiltins::nativeViewFromDynamicHelper(abi)) +
                            "(ptr " +
                            dynamicArgument + ")");
                    } else {
                        result.error = "[emit-llvm] closure ABI cannot unbox parameter '" +
                            declaredType + "'";
                        return false;
                    }
                } else {
                    result.error = "[emit-llvm] closure ABI has no native parameter conversion for '" +
                        declaredType + "'";
                    return false;
                }
                return true;
            };

            std::string nativeArgument;
            const bool hasDefault = i < signature.defaultParameterMask.size() &&
                signature.defaultParameterMask[i];
            if (hasDefault) {
                const std::string suffix = std::to_string(i);
                const std::string providedLabel = "param_" + suffix + "_provided";
                const std::string missingLabel = "param_" + suffix + "_missing";
                const std::string joinLabel = "param_" + suffix + "_join";
                const std::string providedBit = freshAdapterTmp();
                const std::string isProvided = freshAdapterTmp();
                emitAdapterLine(providedBit + " = and i64 " + providedMaskValue + ", " +
                                std::to_string(uint64_t{1} << i));
                emitAdapterLine(isProvided + " = icmp ne i64 " + providedBit + ", 0");
                emitAdapterLine("br i1 " + isProvided + ", label %" + providedLabel +
                                ", label %" + missingLabel);

                adapterText += providedLabel + ":\n";
                std::string providedValue;
                if (!emitProvidedArgument(providedValue)) return false;
                emitAdapterLine("br label %" + joinLabel);

                adapterText += missingLabel + ":\n";
                NativeValue placeholder;
                if (!emitOptionalArgumentPlaceholder(
                        declaredType,
                        placeholder,
                        emitAdapterLine,
                        freshAdapterTmp)) {
                    result.error = "[emit-llvm] closure ABI has no default placeholder for '" +
                        declaredType + "'";
                    return false;
                }
                emitAdapterLine("br label %" + joinLabel);

                adapterText += joinLabel + ":\n";
                nativeArgument = freshAdapterTmp();
                emitAdapterLine(nativeArgument + " = phi " + abiType + " [ " +
                    providedValue + ", %" + providedLabel + " ], [ " +
                    placeholder.value + ", %" + missingLabel + " ]");
            } else if (!emitProvidedArgument(nativeArgument)) {
                return false;
            }
            adapterArguments.push_back(abiType + " " + nativeArgument);
        }

        const bool adapterReturnsDynamic = usesZeusValueABI(signature.returnType);
        const std::string adapterReturnType = adapterReturnsDynamic
            ? "void" : llvmType(signature.returnType);
        if (adapterReturnType.empty()) {
            result.error = "[emit-llvm] closure ABI has no return conversion for '" +
                signature.returnType + "'";
            return false;
        }
        std::vector<std::string> callArguments;
        if (signature.captureCount != 0) callArguments.push_back("ptr %env");
        if (adapterReturnsDynamic) callArguments.push_back("ptr %out");
        callArguments.insert(callArguments.end(), adapterArguments.begin(), adapterArguments.end());
        std::vector<std::string> variadicAdapterValues;
        if (signature.hasVariadicArgs) {
            callArguments.push_back("ptr " + variadicArgsValue);
            variadicAdapterValues.push_back(variadicArgsValue);
        }
        if (signature.hasVariadicKwargs) {
            callArguments.push_back("ptr " + variadicKwargsValue);
            variadicAdapterValues.push_back(variadicKwargsValue);
        }
        if (signature.hasDefaultParameters()) {
            callArguments.push_back("i64 " + providedMaskValue);
        }
        std::string callText;
        std::string nativeReturn;
        if (adapterReturnsDynamic) {
            callText = "call void " + llvmCallTarget(signature) + "(";
        } else {
            nativeReturn = freshAdapterTmp();
            callText = nativeReturn + " = call " + adapterReturnType + " " +
                llvmCallTarget(signature) + "(";
        }
        for (size_t i = 0; i < callArguments.size(); ++i) {
            if (i != 0) callText += ", ";
            callText += callArguments[i];
        }
        callText += ")";
        emitAdapterLine(callText);
        for (const auto& value : variadicAdapterValues) {
            emitAdapterLine("call void @zeus_value_release(ptr " + value + ")");
        }
        if (!adapterReturnsDynamic) {
            if (adapterReturnType == "i64") {
                emitAdapterLine("call void @zeus_value_init_i64(ptr %out, i64 " +
                                nativeReturn + ")");
            } else if (adapterReturnType == "i1") {
                const std::string raw = freshAdapterTmp();
                emitAdapterLine(raw + " = zext i1 " + nativeReturn + " to i32");
                emitAdapterLine("call void @zeus_value_init_bool(ptr %out, i32 " + raw + ")");
            } else if (adapterReturnType == "ptr" && isClassType(signature.returnType)) {
                const std::string typeName = poolStr(signature.returnType);
                emitAdapterLine("call void @zeus_value_from_class_ptr(ptr %out, ptr " +
                                nativeReturn + ", ptr " + typeName + ")");
            } else if (adapterReturnType == "ptr" && isReferenceTypeName(signature.returnType)) {
                const std::string referenced = referencedTypeName(signature.returnType);
                if (referenced == "int" || referenced == "int32" || referenced == "int64" ||
                    referenced == "bool") {
                    emitAdapterLine(std::string("call void @zeus_value_from_") +
                        (referenced == "bool" ? "bool" : "i64") +
                        "_ptr(ptr %out, ptr " + nativeReturn + ")");
                } else if (isClassType(referenced)) {
                    const std::string typeName = poolStr(referenced);
                    emitAdapterLine("call void @zeus_value_from_class_ptr(ptr %out, ptr " +
                                    nativeReturn + ", ptr " + typeName + ")");
                } else {
                    result.error = "[emit-llvm] closure ABI cannot box reference return '" +
                        signature.returnType + "'";
                    return false;
                }
            } else {
                const auto* abi = ZeusBuiltins::findNativeTypeABI(signature.returnType);
                if (adapterReturnType == "ptr" && ZeusBuiltins::isNativeViewABI(abi)) {
                    emitAdapterLine("call void @" +
                                    std::string(ZeusBuiltins::nativeViewToDynamicHelper(abi)) +
                                    "(ptr %out, ptr " +
                                    nativeReturn + ")");
                } else {
                    result.error = "[emit-llvm] closure ABI cannot box return '" +
                        signature.returnType + "'";
                    return false;
                }
            }
        }
        emitAdapterLine("ret void");
        adapterText += "}\n\n";
        nativeFunctions += adapterText;
        }
        return true;
    }

    static std::string labelFor(size_t ci, size_t pc) {
        return "%L" + std::to_string(ci) + "_" + std::to_string(pc);
    }

    // chunk 尾出口（跳转目标 == size 即程序块尾，语义是“执行流离开该 chunk”）。
    static std::string endLabelFor(size_t ci) {
        return "%L" + std::to_string(ci) + "_end";
    }

    // The top-level fast path keeps proven scalar values in LLVM registers.
    // Dynamic values use ZeusValue and return to the adapter only for VM services.
    bool emitNativeTopLevelChunk(
        const std::vector<IRType::IRInstr>& chunk,
        size_t ci,
        bool isLast
    ) {
        bool hasControlFlow = false;

        for (const auto& ins : chunk) {
            switch (ins.op) {
                case VMOperator::IR_ANNOTATION:
                case VMOperator::PUSH:
                case VMOperator::PUSH_NAMED:
                case VMOperator::POP:
                case VMOperator::CLEAR:
                case VMOperator::SWAP:
                case VMOperator::LOAD_VALUE:
                case VMOperator::LOAD_VALUE_SHARED:
                case VMOperator::OBJ_BUILD:
                case VMOperator::OBJ_LOAD_ATTR:
                case VMOperator::OBJ_STORE_ATTR:
                case VMOperator::OBJ_CALL_METHOD:
                case VMOperator::LIST_BUILD:
                case VMOperator::LIST_LENGTH:
                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_BUILD:
                case VMOperator::DICT_KEYS_BUILD:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE:
                case VMOperator::LOAD_NATIVE:
                case VMOperator::CALL_NATIVE:
                case VMOperator::CALL_CLOSURE:
                case VMOperator::MAKE_FUNCTION:
                case VMOperator::STORE_VALUE_GLOBAL:
                case VMOperator::ASSIGN_VALUE_GLOBAL:
                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED:
                case VMOperator::DELETE_VAR:
                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL:
                case VMOperator::ADD:
                case VMOperator::SUBTRACT:
                case VMOperator::MULTIPLY:
                case VMOperator::DIVIDE:
                case VMOperator::MODULUS:
                case VMOperator::EQUL:
                case VMOperator::NOT_EQUAL:
                case VMOperator::GREATER_THAN:
                case VMOperator::LESS_THAN:
                case VMOperator::GREATER_EQUAL:
                case VMOperator::LESS_EQUAL:
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR:
                case VMOperator::LOGICAL_NOT:
                case VMOperator::TRY_BEGIN:
                case VMOperator::TRY_END:
                case VMOperator::CATCH_BEGIN:
                case VMOperator::CATCH_END:
                case VMOperator::THROW:
                case VMOperator::JUMP:
                case VMOperator::JUMP_IF_TRUE:
                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::LOOP_BREAK:
                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO:
                    hasControlFlow = hasControlFlow ||
                        ins.op == VMOperator::TRY_BEGIN ||
                        ins.op == VMOperator::JUMP ||
                        ins.op == VMOperator::JUMP_IF_TRUE ||
                        ins.op == VMOperator::JUMP_IF_FALSE ||
                        ins.op == VMOperator::LOOP_BREAK;
                    break;
                default:
                    ZEUS_DEBUG_PRINT(
                        "[emit-llvm] native top-level chunk %zu rejected by opcode: %s",
                        ci, ins.toString().c_str());
                    return false;
            }
        }

        const std::string oldBody = body;
        const auto oldStringPool = strPool;
        const int oldStrCount = strCount;
        const long oldTmpCount = tmpCount;
        const auto oldNativeGlobals = nativeGlobals;
        const auto oldNativeLocals = nativeLocals;
        const auto oldNativeGlobalSlots = nativeGlobalSlots;
        const auto oldNativeLocalSlots = nativeLocalSlots;
        const auto oldNativeGlobalClassTypes = nativeGlobalClassTypes;
        const auto oldNativeGlobalScalarTypes = nativeGlobalScalarTypes;
        const std::string oldEntryPrologue = entryPrologue;
        const std::string oldPendingVariadicCall = pendingVariadicCall;
        const std::string oldPendingDirectCall = pendingDirectCall;
        const std::string oldPendingFunctionLoad = pendingFunctionLoad;
        const bool oldBlkTerminated = blkTerminated;
        const bool oldBlkNeedLabel = blkNeedLabel;
        const bool oldEntryLabelEmitted = entryLabelEmitted;
        const std::string oldError = result.error;
        const auto oldEntryExceptionPads = entryExceptionPads;
        const auto oldEntryExceptionSlots = entryExceptionSlots;
        const bool oldEntryHasTry = entryHasTry;
        const bool oldNeedsRuntimeErrorFactory = needsRuntimeErrorFactory;
        const int64_t oldActiveEntryExceptionTarget = activeEntryExceptionTarget;

        auto rollback = [&]() {
            if (!result.error.empty() && result.error != oldError) {
                ZEUS_DEBUG_PRINT(
                    "[emit-llvm] native top-level chunk %zu rolled back: %s",
                    ci, result.error.c_str());
            }
            body = oldBody;
            strPool = oldStringPool;
            strCount = oldStrCount;
            tmpCount = oldTmpCount;
            // The prologue is rolled back with everything else: it holds temp
            // names taken from the same counter, so leaving it behind would let
            // the retried body hand out the same name twice.
            entryPrologue = oldEntryPrologue;
            nativeGlobals = oldNativeGlobals;
            nativeLocals = oldNativeLocals;
            nativeGlobalSlots = oldNativeGlobalSlots;
            nativeLocalSlots = oldNativeLocalSlots;
            pendingVariadicCall = oldPendingVariadicCall;
            pendingDirectCall = oldPendingDirectCall;
            pendingFunctionLoad = oldPendingFunctionLoad;
            blkTerminated = oldBlkTerminated;
            blkNeedLabel = oldBlkNeedLabel;
            entryLabelEmitted = oldEntryLabelEmitted;
            result.error = oldError;
            entryExceptionPads = oldEntryExceptionPads;
            entryExceptionSlots = oldEntryExceptionSlots;
            entryHasTry = oldEntryHasTry;
            needsRuntimeErrorFactory = oldNeedsRuntimeErrorFactory;
            activeEntryExceptionTarget = oldActiveEntryExceptionTarget;
        };

        auto failNativeTopLevel = [&](const IRType::IRInstr& ins, const std::string& message) {
            result.error = message + " (" + ins.toString() + ")";
            result.errorLine = ins.line;
        };

        std::vector<NativeValue> valueStack;
        bool controlFlowStarted = false;
        bool slotsMaterialized = false;

        auto isNativeScalar = [](const NativeValue& value) {
            return !value.dynamic && !value.functionRef &&
                   (value.type == "i64" || value.type == "i1");
        };
        auto isNativeSlotValue = [this, &isNativeScalar](const NativeValue& value) {
            return isNativeScalar(value) ||
                   (!value.dynamic && !value.functionRef && value.type == "ptr" &&
                    isClassType(value.sourceType));
        };

        auto ensureSlot = [&](std::map<int, NativeSlot>& slots,
                              int index,
                              const std::string& type,
                              const std::string& sourceType,
                              const IRType::IRInstr& ins) {
            auto existing = slots.find(index);
            if (existing != slots.end()) {
                if (existing->second.type != type ||
                    existing->second.sourceType != sourceType) {
                    failNativeTopLevel(ins, "[emit-llvm] 控制流变量类型发生变化");
                    return false;
                }
                return true;
            }

            if (controlFlowStarted) {
                failNativeTopLevel(ins,
                                   "[emit-llvm] 控制流开始后才首次出现的原生槽位暂不支持");
                return false;
            }

            NativeSlot slot;
            slot.address = freshTmp();
            slot.type = type;
            slot.sourceType = sourceType;
            // The storage has to dominate every later access, so it belongs to
            // the entry block even when the first store sits inside a branch.
            entryPrologue += slot.address + " = alloca " + type + "\n";
            if (type == "ptr" && isClassType(sourceType)) {
                entryPrologue += "store ptr null, ptr " + slot.address + "\n";
            }
            slots.emplace(index, std::move(slot));
            return true;
        };

        auto emitNativeSlotStore = [&](const NativeValue& value,
                                       const NativeSlot& slot) {
            if (isManagedClassValue(value)) {
                emitLine("call void @zeus_class_ref_retain(ptr " + value.value + ")");
                const std::string previous = freshTmp();
                emitLine(previous + " = load ptr, ptr " + slot.address);
                emitLine("call void @zeus_class_ref_release(ptr " + previous + ")");
                emitLine("store ptr " + value.value + ", ptr " + slot.address);
                if (value.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " + value.value + ")");
                }
            } else {
                emitLine("store " + value.type + " " + value.value +
                         ", ptr " + slot.address);
            }
        };

        auto materializeNativeValues = [&](std::map<int, NativeValue>& values,
                                           std::map<int, NativeSlot>& slots) {
            for (const auto& entry : values) {
                const int index = entry.first;
                const NativeValue& value = entry.second;
                if (!isNativeSlotValue(value)) {
                    failNativeTopLevel(chunk.front(),
                                       "[emit-llvm] 控制流槽位只支持 int/bool 或 class 指针");
                    return false;
                }

                if (!ensureSlot(slots, index, value.type,
                                value.sourceType, chunk.front())) return false;
                emitNativeSlotStore(value, slots[index]);
            }
            values.clear();
            return true;
        };

        std::vector<int64_t> nativeEntryExceptionTarget(chunk.size(), -1);
        std::vector<size_t> nativeEntryExceptionStack;
        std::vector<size_t> nativeEntryCatchStack;
        std::map<size_t, size_t> nativeEntryCatchBeginForEnd;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            if (ins.op == VMOperator::TRY_BEGIN) {
                int64_t offset = 0;
                if (!tryArgInt(ins, 0, offset) || offset <= 0 ||
                    static_cast<uint64_t>(offset) >= chunk.size() - pc ||
                    chunk[pc + static_cast<size_t>(offset)].op != VMOperator::CATCH_BEGIN) {
                    failNativeTopLevel(ins, "[emit-llvm] invalid top-level TRY_BEGIN catch target");
                    rollback();
                    return false;
                }
                entryHasTry = true;
                needsRuntimeErrorFactory = true;
                nativeEntryExceptionStack.push_back(pc + static_cast<size_t>(offset));
            } else if (ins.op == VMOperator::TRY_END) {
                if (nativeEntryExceptionStack.empty()) {
                    failNativeTopLevel(ins, "[emit-llvm] top-level TRY_END without TRY_BEGIN");
                    rollback();
                    return false;
                }
                nativeEntryExceptionStack.pop_back();
            } else if (!nativeEntryExceptionStack.empty()) {
                nativeEntryExceptionTarget[pc] =
                    static_cast<int64_t>(nativeEntryExceptionStack.back());
            }
            if (ins.op == VMOperator::CATCH_BEGIN) nativeEntryCatchStack.push_back(pc);
            if (ins.op == VMOperator::CATCH_END) {
                if (nativeEntryCatchStack.empty()) {
                    failNativeTopLevel(ins, "[emit-llvm] top-level CATCH_END without CATCH_BEGIN");
                    rollback();
                    return false;
                }
                nativeEntryCatchBeginForEnd[pc] = nativeEntryCatchStack.back();
                nativeEntryCatchStack.pop_back();
            }
        }
        if (!nativeEntryExceptionStack.empty() || !nativeEntryCatchStack.empty()) {
            failNativeTopLevel(chunk.back(), "[emit-llvm] unterminated top-level try/catch");
            rollback();
            return false;
        }

        std::set<size_t> labels;
        bool exitJump = false;
        if (hasControlFlow) {
            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                const auto& ins = chunk[pc];
                if (ins.op == VMOperator::CATCH_BEGIN) labels.insert(pc);
                if (ins.op != VMOperator::JUMP &&
                    ins.op != VMOperator::JUMP_IF_FALSE &&
                    ins.op != VMOperator::JUMP_IF_TRUE &&
                    ins.op != VMOperator::LOOP_BREAK) {
                    continue;
                }

                int64_t off = 0;
                if (!tryArgInt(ins, 0, off)) return false;
                const long long target = static_cast<long long>(pc) + 1 + off;
                if (target < 0 || target > static_cast<long long>(chunk.size())) return false;

                if (target == static_cast<long long>(chunk.size())) {
                    exitJump = true;
                } else {
                    labels.insert(static_cast<size_t>(target));
                }

                if (ins.op == VMOperator::JUMP_IF_FALSE ||
                    ins.op == VMOperator::JUMP_IF_TRUE) {
                    if (pc + 1 < chunk.size()) {
                        labels.insert(pc + 1);
                    } else {
                        exitJump = true;
                    }
                }
            }
        }

        auto popValue = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (valueStack.empty()) {
                failNativeTopLevel(ins, "[emit-llvm] 原生值栈下溢");
                return false;
            }

            value = valueStack.back();
            valueStack.pop_back();
            return true;
        };

        auto parseLiteral = [&](const IRType::IRInstr& ins, NativeValue& value) {
            if (!ins.args.empty() &&
                std::holds_alternative<IRType::NullLiteral>(ins.args[0])) {
                const std::string nilValue = emitDynamicValueForNull(ins);
                if (nilValue.empty()) return false;
                value = NativeValue{ nilValue, "ptr", false, true, "nil" };
                return true;
            }
            if (!ins.args.empty() &&
                std::holds_alternative<ZeusType::Bytes>(ins.args[0])) {
                const auto& bytes = std::get<ZeusType::Bytes>(ins.args[0]);
                const std::string dynamicValue = emitDynamicValueForBytes(bytes, ins);
                if (dynamicValue.empty()) return false;
                value = NativeValue{ dynamicValue, "ptr", false, true, "bytes" };
                return true;
            }
            if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                failNativeTopLevel(ins, "[emit-llvm] 原生 PUSH 参数不是 Value");
                return false;
            }

            const auto& literal = std::get<LexerType::Value>(ins.args[0]);
            if (std::holds_alternative<ZeusType::BigInteger>(literal.data)) {
                int64_t integerValue = 0;
                if (std::get<ZeusType::BigInteger>(literal.data).tryToInt64(integerValue)) {
                    value = NativeValue{ std::to_string(integerValue), "i64", false, false };
                    return true;
                }

                const std::string dynamicValue = emitDynamicValueForLiteral(literal, ins);
                if (dynamicValue.empty()) return false;
                value = NativeValue{ dynamicValue, "ptr", false, true, "bigint" };
                return true;
            }

            if (std::holds_alternative<ZeusType::BigDecimal>(literal.data)) {
                const std::string dynamicValue = emitDynamicValueForLiteral(literal, ins);
                if (dynamicValue.empty()) return false;
                value = NativeValue{ dynamicValue, "ptr", false, true, "float" };
                return true;
            }

            if (std::holds_alternative<bool>(literal.data)) {
                value = NativeValue{
                    std::get<bool>(literal.data) ? "1" : "0",
                    "i1",
                    false,
                    false
                };
                return true;
            }

            if (std::holds_alternative<std::string>(literal.data)) {
                const std::string dynamicValue = emitDynamicValueForLiteral(literal, ins);
                if (dynamicValue.empty()) return false;
                value = NativeValue{ dynamicValue, "ptr", false, true };
                return true;
            }

            failNativeTopLevel(ins, "[emit-llvm] 原生值栈暂不支持该字面量类型");
            return false;
        };

        auto pushNull = [&]() {
            const std::string value = freshTmp();
            emitLine(value + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_init_nil(ptr " + value + ")");
            valueStack.push_back(NativeValue{ value, "ptr", false, true });
        };

        auto toDynamicValue = [&] (
            const NativeValue& value,
            const IRType::IRInstr& ins,
            std::string& dynamicValue,
            bool& ownsDynamic
        ) {
            if (value.dynamic) {
                dynamicValue = value.value;
                ownsDynamic = value.ownsDynamic;
                return true;
            }
            if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) return false;
            ownsDynamic = true;
            return true;
        };

        auto emitVariadicValueCall = [&](const IRType::IRInstr& ins, int64_t argc) {
            if (argc < 0 || static_cast<size_t>(argc + 1) > valueStack.size()) return false;

            std::vector<NativeValue> arguments(static_cast<size_t>(argc));
            for (int64_t i = argc - 1; i >= 0; --i) {
                if (!popValue(ins, arguments[static_cast<size_t>(i)])) return false;
            }

            NativeValue callee;
            if (!popValue(ins, callee) || !callee.functionRef ||
                callee.type != "builtin") {
                failNativeTopLevel(ins, "[emit-llvm] invalid native variadic call target");
                return false;
            }

            if (const auto* nativeDeclaration =
                    findNativeBuiltinDeclaration(callee.value)) {
                if (arguments.size() < static_cast<size_t>(
                        nativeDeclaration->minRequiredParams) ||
                    arguments.size() > nativeDeclaration->parameterTypes.size()) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] @native builtin call argument count does not match '" +
                        callee.value + "'");
                    return false;
                }

                std::vector<std::string> nativeArguments;
                std::vector<std::string> dynamicCleanup;
                for (size_t i = 0; i < nativeDeclaration->parameterTypes.size(); ++i) {
                    const std::string& parameterType =
                        nativeDeclaration->parameterTypes[i];
                    if (i >= arguments.size()) {
                        NativeValue placeholder;
                        if (!emitOptionalArgumentPlaceholder(
                                parameterType, placeholder,
                                [this](const std::string& line) { emitLine(line); },
                                [this]() { return freshTmp(); })) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] @native builtin parameter has no default ABI placeholder");
                            return false;
                        }
                        nativeArguments.push_back(placeholder.value);
                        if (placeholder.dynamic && placeholder.ownsDynamic) {
                            dynamicCleanup.push_back(placeholder.value);
                        }
                        continue;
                    }

                    std::string nativeArgument;
                    if (usesZeusValueABI(parameterType)) {
                        if (!emitNativeValueAsDynamic(
                                arguments[i], ins, nativeArgument)) return false;
                        if (arguments[i].dynamic) {
                            if (arguments[i].ownsDynamic) {
                                dynamicCleanup.push_back(nativeArgument);
                            }
                        } else {
                            dynamicCleanup.push_back(nativeArgument);
                        }
                    } else if (!emitNativeValueAsType(
                                   arguments[i], llvmType(parameterType), ins,
                                   nativeArgument, parameterType)) {
                        return false;
                    }
                    nativeArguments.push_back(std::move(nativeArgument));
                }

                const bool returnsDynamic =
                    usesZeusValueABI(nativeDeclaration->returnType);
                const std::string returnType = returnsDynamic
                    ? "void" : llvmType(nativeDeclaration->returnType);
                if (returnType.empty()) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] @native builtin return type has no LLVM ABI");
                    return false;
                }

                const std::string callResult = freshTmp();
                std::string callText;
                if (returnsDynamic) {
                    emitLine(callResult + " = alloca %ZeusValue, align 8");
                    callText = "call void " + llvmCallTarget(*nativeDeclaration) +
                        "(ptr " + callResult;
                } else {
                    callText = callResult + " = call " + returnType + " " +
                        llvmCallTarget(*nativeDeclaration) + "(";
                }
                for (size_t i = 0; i < nativeArguments.size(); ++i) {
                    if (i > 0 || returnsDynamic) callText += ", ";
                    callText += functionParameterABIType(
                        nativeDeclaration->parameterTypes[i]) + " " + nativeArguments[i];
                }
                if (nativeDeclaration->hasDefaultParameters()) {
                    if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
                    callText += "i64 " + std::to_string(providedPrefixMask(arguments.size()));
                }
                callText += ")";
                emitLine(callText);
                std::set<std::string> releasedDynamicArguments;
                for (const auto& cleanupValue : dynamicCleanup) {
                    if (releasedDynamicArguments.insert(cleanupValue).second) {
                        emitRelease(cleanupValue);
                    }
                }
                for (const auto& argument : arguments) {
                    if (argument.dynamic && argument.ownsDynamic &&
                        releasedDynamicArguments.insert(argument.value).second) {
                        emitRelease(argument.value);
                    }
                    if (isManagedClassValue(argument) && argument.ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 argument.value + ")");
                    }
                }

                if (returnsDynamic) {
                    valueStack.push_back(NativeValue{
                        callResult, "ptr", false, true,
                        nativeDeclaration->returnType, true
                    });
                    return true;
                }

                const auto* returnABI = ZeusBuiltins::findNativeTypeABI(
                    nativeDeclaration->returnType);
                std::string nativeResult = callResult;
                if (ZeusBuiltins::isNativeViewABI(returnABI)) {
                    nativeResult = freshTmp();
                    emitLine(nativeResult + " = call ptr @" +
                        std::string(ZeusBuiltins::nativeViewInternHelper(returnABI)) +
                        "(ptr " + callResult + ")");
                }
                NativeValue returned{
                    nativeResult, returnType, false, false,
                    nativeDeclaration->returnType, false
                };
                returned.ownsClass = isClassType(nativeDeclaration->returnType);
                std::string dynamicResult;
                if (!emitNativeValueAsDynamic(returned, ins, dynamicResult)) return false;
                if (returned.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             returned.value + ")");
                }
                valueStack.push_back(NativeValue{
                    dynamicResult, "ptr", false, true,
                    nativeDeclaration->returnType, true
                });
                return true;
            }

            std::vector<std::string> dynamicArguments(arguments.size());
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (!emitNativeValueAsDynamic(arguments[i], ins, dynamicArguments[i])) return false;
            }
            const std::string callSymbol = dynamicVariadicABISymbol(callee.value);
            if (callSymbol.empty()) {
                failNativeTopLevel(ins, "[emit-llvm] native builtin has no dynamic ABI");
                return false;
            }
            const std::string resultValue = emitDynamicVariadicCall(
                callSymbol,
                callee.value,
                dynamicArguments,
                [this](const std::string& line) { emitLine(line); },
                [this]() { return freshTmp(); }
            );
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (arguments[i].dynamic) releaseNativeValue(arguments[i]);
                else {
                    emitRelease(dynamicArguments[i]);
                    if (isManagedClassValue(arguments[i]) &&
                        arguments[i].ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 arguments[i].value + ")");
                    }
                }
            }
            valueStack.push_back(NativeValue{ resultValue, "ptr", false, true, "nil", true });
            return true;
        };

        auto emitClosureCallTopLevel = [&] (
            const IRType::IRInstr& ins,
            NativeValue& callee,
            const std::vector<NativeValue>& arguments,
            NativeValue& callResult
        ) {
            if (!callee.dynamic || callee.type != "ptr") {
                failNativeTopLevel(ins, "[emit-llvm] closure call requires a dynamic closure value");
                return false;
            }
            std::vector<std::string> dynamicArguments(arguments.size());
            std::vector<bool> ownsDynamicArguments(arguments.size(), false);
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (!emitNativeValueAsDynamic(arguments[i], ins, dynamicArguments[i])) {
                    return false;
                }
                ownsDynamicArguments[i] = arguments[i].dynamic
                    ? arguments[i].ownsDynamic : true;
            }
            std::string argumentArray = "null";
            std::string argumentNameArray = "null";
            if (!dynamicArguments.empty()) {
                argumentArray = freshTmp();
                argumentNameArray = freshTmp();
                emitLine(argumentArray + " = alloca ptr, i64 " +
                         std::to_string(dynamicArguments.size()) + ", align 8");
                emitLine(argumentNameArray + " = alloca ptr, i64 " +
                         std::to_string(dynamicArguments.size()) + ", align 8");
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    const std::string slot = freshTmp();
                    const std::string nameSlot = freshTmp();
                    emitLine(slot + " = getelementptr inbounds ptr, ptr " +
                             argumentArray + ", i64 " + std::to_string(i));
                    emitLine("store ptr " + dynamicArguments[i] + ", ptr " + slot +
                             ", align 8");
                    emitLine(nameSlot + " = getelementptr inbounds ptr, ptr " +
                             argumentNameArray + ", i64 " + std::to_string(i));
                    if (arguments[i].namedArgument.empty()) {
                        emitLine("store ptr null, ptr " + nameSlot + ", align 8");
                    } else {
                        const std::string nameSymbol = poolStr(arguments[i].namedArgument);
                        const std::string namePointer = freshTmp();
                        emitLine(namePointer + " = getelementptr inbounds [" +
                            std::to_string(arguments[i].namedArgument.size() + 1) +
                            " x i8], ptr " + nameSymbol + ", i64 0, i64 0");
                        emitLine("store ptr " + namePointer + ", ptr " + nameSlot +
                                 ", align 8");
                    }
                }
            }
            const std::string output = freshTmp();
            emitLine(output + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_closure_call_named(ptr " + output + ", ptr " +
                     callee.value + ", i32 " +
                     std::to_string(dynamicArguments.size()) + ", ptr " +
                     argumentArray + ", ptr " + argumentNameArray + ")");
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) emitRelease(dynamicArguments[i]);
            }
            for (const auto& argument : arguments) {
                if (isManagedClassValue(argument) && argument.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             argument.value + ")");
                }
            }
            if (callee.ownsDynamic) releaseNativeValue(callee);
            callResult = NativeValue{ output, "ptr", false, true, std::string(), true };
            return true;
        };

        auto emitLogicalNot = [&](const IRType::IRInstr& ins) {
            NativeValue operand;
            if (!popValue(ins, operand) || operand.functionRef) {
                failNativeTopLevel(ins, "[emit-llvm] 一元逻辑非操作数无效");
                return false;
            }

            const std::string resultValue = freshTmp();
            if (!operand.dynamic && operand.type == "i1") {
                emitLine(resultValue + " = xor i1 " + operand.value + ", true");
            } else if (!operand.dynamic && operand.type == "i64") {
                emitLine(resultValue + " = icmp eq i64 " + operand.value + ", 0");
            } else {
                std::string dynamicValue;
                if (!emitNativeValueAsDynamic(operand, ins, dynamicValue)) return false;
                const std::string truthy = freshTmp();
                emitLine(truthy + " = call i32 @zeus_value_truthy(ptr " + dynamicValue + ")");
                if (!operand.dynamic || operand.ownsDynamic) emitRelease(dynamicValue);
                if (isManagedClassValue(operand) && operand.ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             operand.value + ")");
                }
                emitLine(resultValue + " = icmp eq i32 " + truthy + ", 0");
            }

            valueStack.push_back(NativeValue{ resultValue, "i1", false, false });
            return true;
        };

        auto emitBinary = [&](const IRType::IRInstr& ins) {
            if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                std::string targetName;
                if (!ins.metadata.readDirectCall(targetName)) {
                    failNativeTopLevel(ins, "[emit-llvm] 静态运算标注缺少函数名");
                    return false;
                }

                const auto target = functionSignatures.find(targetName);
                if (target == functionSignatures.end()) {
                    failNativeTopLevel(ins, "[emit-llvm] 找不到静态运算目标'" +
                        targetName + "'");
                    return false;
                }
                const FunctionSignature& signature = target->second;
                if (signature.captureCount != 0 ||
                    signature.parameterTypes.size() != 2) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] 静态二元运算必须是无捕获的双参数函数");
                    return false;
                }

                NativeValue arguments[2];
                if (!popValue(ins, arguments[1]) || !popValue(ins, arguments[0]) ||
                    arguments[0].functionRef || arguments[1].functionRef) {
                    failNativeTopLevel(ins, "[emit-llvm] 静态运算操作数无效");
                    return false;
                }

                std::vector<std::string> nativeArguments(2);
                std::vector<std::string> dynamicArguments;
                std::vector<bool> ownsDynamicArguments;
                for (size_t i = 0; i < 2; ++i) {
                    const std::string& parameterType = signature.parameterTypes[i];
                    if (usesZeusValueABI(parameterType)) {
                        if (!emitNativeValueAsDynamic(
                                arguments[i], ins, nativeArguments[i])) {
                            return false;
                        }
                        dynamicArguments.push_back(nativeArguments[i]);
                        ownsDynamicArguments.push_back(
                            arguments[i].dynamic ? arguments[i].ownsDynamic : true);
                    } else if (!emitNativeValueAsType(
                                   arguments[i], llvmType(parameterType), ins,
                                   nativeArguments[i], parameterType)) {
                        return false;
                    }
                }

                const bool returnsDynamic = usesZeusValueABI(signature.returnType);
                const std::string returnType = returnsDynamic
                    ? "void"
                    : llvmType(signature.returnType);
                if (returnType.empty()) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] 静态运算返回类型没有 LLVM ABI 映射");
                    return false;
                }

                const std::string callResult = freshTmp();
                std::string callText;
                if (returnsDynamic) {
                    emitLine(callResult + " = alloca %ZeusValue, align 8");
                    callText = "call void " + llvmCallTarget(signature.name) +
                               "(ptr " + callResult;
                } else {
                    callText = callResult + " = call " + returnType + " " +
                               llvmCallTarget(signature.name) + "(";
                }
                for (size_t i = 0; i < 2; ++i) {
                    if (i > 0 || returnsDynamic) callText += ", ";
                    callText += functionParameterABIType(signature.parameterTypes[i]) +
                                " " + nativeArguments[i];
                }
                callText += ")";
                emitLine(callText);

                std::set<std::string> releasedDynamicArguments;
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    if (ownsDynamicArguments[i] &&
                        releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                        emitRelease(dynamicArguments[i]);
                    }
                }
                for (size_t i = 0; i < 2; ++i) {
                    if (arguments[i].dynamic && arguments[i].ownsDynamic &&
                        releasedDynamicArguments.insert(arguments[i].value).second) {
                        emitRelease(arguments[i].value);
                    }
                    if (isManagedClassValue(arguments[i]) &&
                        arguments[i].ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 arguments[i].value + ")");
                    }
                }

                valueStack.push_back(returnsDynamic
                    ? NativeValue{
                        callResult, "ptr", false, true,
                        signature.returnType, true
                      }
                    : NativeValue{
                        callResult, returnType, false, false,
                        signature.returnType, false
                      });
                valueStack.back().ownsClass =
                    !returnsDynamic && isClassType(signature.returnType);
                pendingDirectCall.clear();
                return true;
            }

            NativeValue right;
            NativeValue left;
            if (!popValue(ins, right) || !popValue(ins, left) ||
                left.functionRef || right.functionRef) {
                return false;
            }

            const bool isLogical =
                ins.op == VMOperator::LOGICAL_AND || ins.op == VMOperator::LOGICAL_OR;
            const bool isComparison =
                ins.op == VMOperator::EQUL || ins.op == VMOperator::NOT_EQUAL ||
                ins.op == VMOperator::GREATER_THAN || ins.op == VMOperator::LESS_THAN ||
                ins.op == VMOperator::GREATER_EQUAL || ins.op == VMOperator::LESS_EQUAL;

            if (!left.dynamic && !right.dynamic) {
                if (isLogical) {
                    if (left.type != "i1" || right.type != "i1") {
                        failNativeTopLevel(ins, "[emit-llvm] 逻辑运算要求 bool 操作数");
                        return false;
                    }
                } else if (isComparison) {
                    if (left.type != right.type ||
                        (left.type != "i64" && left.type != "i1")) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生比较操作数类型不一致");
                        return false;
                    }
                } else if (left.type != "i64" || right.type != "i64") {
                    failNativeTopLevel(ins, "[emit-llvm] 原生算术运算要求 int 操作数");
                    return false;
                }

                std::string opcode;
                const std::string operandType = isComparison ? left.type :
                    (isLogical ? "i1" : "i64");
                if (ins.op == VMOperator::ADD) opcode = "add";
                if (ins.op == VMOperator::SUBTRACT) opcode = "sub";
                if (ins.op == VMOperator::MULTIPLY) opcode = "mul";
                if (ins.op == VMOperator::DIVIDE) opcode = "sdiv";
                if (ins.op == VMOperator::MODULUS) opcode = "srem";
                if (ins.op == VMOperator::LOGICAL_AND) opcode = "and";
                if (ins.op == VMOperator::LOGICAL_OR) opcode = "or";
                if (ins.op == VMOperator::EQUL) opcode = "icmp eq";
                if (ins.op == VMOperator::NOT_EQUAL) opcode = "icmp ne";
                if (ins.op == VMOperator::GREATER_THAN) opcode = "icmp sgt";
                if (ins.op == VMOperator::LESS_THAN) opcode = "icmp slt";
                if (ins.op == VMOperator::GREATER_EQUAL) opcode = "icmp sge";
                if (ins.op == VMOperator::LESS_EQUAL) opcode = "icmp sle";

                if (opcode.empty()) {
                    failNativeTopLevel(ins, "[emit-llvm] 未知的原生二元运算");
                    return false;
                }

                const std::string temporary = freshTmp();
                if (ins.op == VMOperator::DIVIDE || ins.op == VMOperator::MODULUS) {
                    emitLine("call void @zeus_i64_check_division(i64 " + left.value +
                        ", i64 " + right.value + ", i32 " +
                        std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                }
                emitLine(temporary + " = " + opcode + " " + operandType + " " +
                         left.value + ", " + right.value);
                valueStack.push_back(NativeValue{
                    temporary,
                    isLogical || isComparison ? "i1" : "i64",
                    false,
                    false
                });
                return true;
            }

            const int dynamicOpcode = DynamicBinaryABI::opcodeFor(ins.op);
            if (dynamicOpcode < 0) {
                failNativeTopLevel(ins, "[emit-llvm] dynamic 值暂不支持该二元运算");
                return false;
            }

            std::string leftDynamic;
            std::string rightDynamic;
            if (!emitNativeValueAsDynamic(left, ins, leftDynamic) ||
                !emitNativeValueAsDynamic(right, ins, rightDynamic)) {
                return false;
            }

            const std::string temporary = freshTmp();
            emitLine(temporary + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_dynamic_binary(ptr " + temporary +
                     ", ptr null, i32 " + std::to_string(dynamicOpcode) +
                     ", ptr " + leftDynamic + ", ptr " + rightDynamic + ")");

            emitRelease(leftDynamic);
            emitRelease(rightDynamic);
            for (const NativeValue* operand : { &left, &right }) {
                if (isManagedClassValue(*operand) && operand->ownsClass) {
                    emitLine("call void @zeus_class_ref_release(ptr " +
                             operand->value + ")");
                }
            }
            valueStack.push_back(NativeValue{ temporary, "ptr", false, true });
            return true;
        };

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            activeEntryExceptionTarget = nativeEntryExceptionTarget[pc];
            activeEntryChunk = ci;

            if ((hasControlFlow && labels.count(pc)) ||
                (pc == 0 && (blkTerminated || blkNeedLabel || ci == 0))) {
                std::string label = labelFor(ci, 0);
                label = labelFor(ci, pc);
                if (!entryLabelEmitted && blkTerminated && pc == 0) {
                    label = "%L0_0";
                    entryLabelEmitted = true;
                }
                emitLabelDef(label);
            }

            if (hasControlFlow && !slotsMaterialized) {
                if (!materializeNativeValues(nativeGlobals, nativeGlobalSlots) ||
                    !materializeNativeValues(nativeLocals, nativeLocalSlots)) {
                    rollback();
                    return false;
                }
                slotsMaterialized = true;
            }

            if (hasControlFlow && blkTerminated) continue;

            std::string comment = "; chunk #" + std::to_string(ci) +
                                  " pc=" + std::to_string(pc);
            if (ins.line >= 0) comment += " [L" + std::to_string(ins.line) + "]";
            comment += " " + ins.toString();
            body += "  " + comment + "\n";

            switch (ins.op) {
                case VMOperator::TRY_BEGIN:
                case VMOperator::TRY_END:
                    break;

                case VMOperator::CATCH_BEGIN: {
                    const auto captured = entryExceptionSlots.find(std::make_pair(ci, pc));
                    if (captured == entryExceptionSlots.end()) {
                        emitLine("unreachable");
                        blkTerminated = true;
                        blkNeedLabel = true;
                        break;
                    }
                    int64_t slot = -1;
                    if (tryArgInt(ins, 1, slot) && slot >= 0) {
                        nativeLocals[static_cast<int>(slot)] = NativeValue{
                            captured->second, "ptr", false, true, {}, true
                        };
                    } else {
                        emitLine("call void @zeus_value_release(ptr " +
                            captured->second + ")");
                    }
                    break;
                }

                case VMOperator::CATCH_END: {
                    const auto begin = nativeEntryCatchBeginForEnd.find(pc);
                    if (begin != nativeEntryCatchBeginForEnd.end()) {
                        const auto captured = entryExceptionSlots.find(
                            std::make_pair(ci, begin->second));
                        int64_t slot = -1;
                        if (captured != entryExceptionSlots.end() &&
                            tryArgInt(chunk[begin->second], 1, slot) && slot >= 0) {
                            emitLine("call void @zeus_value_release(ptr " +
                                captured->second + ")");
                            nativeLocals.erase(static_cast<int>(slot));
                        }
                    }
                    break;
                }

                case VMOperator::THROW: {
                    NativeValue thrown;
                    if (!popValue(ins, thrown) || thrown.functionRef ||
                        !valueStack.empty()) {
                        failNativeTopLevel(ins, "THROW requires one value on the stack");
                        rollback();
                        return false;
                    }
                    std::string boxed;
                    if (!emitNativeValueAsDynamic(thrown, ins, boxed)) {
                        rollback();
                        return false;
                    }
                    if (isManagedClassValue(thrown) && thrown.ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 thrown.value + ")");
                    }
                    emitLine(std::string("call void @zeus_exception_raise") +
                        (thrown.dynamic && !thrown.ownsDynamic ? "(ptr " : "_owned(ptr ") +
                        boxed + ")");
                    emitLine("unreachable");
                    blkTerminated = true;
                    blkNeedLabel = true;
                    break;
                }

                case VMOperator::IR_ANNOTATION:
                    if (annotationKind(ins, "direct_call")) {
                        std::string directCallName;
                        if (!annotationSymbolName(ins, directCallName)) {
                            failNativeTopLevel(ins, "[emit-llvm] 直接调用标注缺少函数名");
                            rollback();
                            return false;
                        }
                        const auto* target = resolveFunctionSignature(directCallName);
                        pendingDirectCall = target != nullptr &&
                            !hasCompleteFunctionAnnotations(*target)
                                ? std::string()
                                : (target != nullptr ? target->name : directCallName);
                    }
                    break;

                case VMOperator::IMPORT:
                case VMOperator::TYPE_IMPL_BUILD:
                    break;

                case VMOperator::PUSH: {
                    NativeValue value;
                    if (!parseLiteral(ins, value)) {
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(value));
                    break;
                }

                case VMOperator::PUSH_NAMED: {
                    if (valueStack.empty() || ins.args.size() != 1 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "invalid named argument");
                        rollback();
                        return false;
                    }
                    valueStack.back().namedArgument = std::get<std::string>(ins.args[0]);
                    break;
                }

                case VMOperator::POP: {
                    NativeValue value;
                    if (!popValue(ins, value)) {
                        rollback();
                        return false;
                    }
                    releaseNativeValue(value);
                    break;
                }

                case VMOperator::CLEAR:
                    for (const auto& value : valueStack) releaseNativeValue(value);
                    valueStack.clear();
                    break;

                case VMOperator::COPY: {
                    if (valueStack.empty()) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生值栈无法 COPY");
                        rollback();
                        return false;
                    }
                    NativeValue copy = valueStack.back();
                    if (isManagedClassValue(copy) && copy.ownsClass) {
                        emitLine("call void @zeus_class_ref_retain(ptr " + copy.value + ")");
                    } else if (copy.dynamic && copy.ownsDynamic) {
                        const std::string cloned = freshTmp();
                        emitLine(cloned + " = alloca %ZeusValue, align 8");
                        emitLine("call void @zeus_value_clone(ptr " + cloned +
                                 ", ptr " + copy.value + ")");
                        copy.value = cloned;
                    }
                    valueStack.push_back(std::move(copy));
                    break;
                }

                case VMOperator::SWAP:
                    if (valueStack.size() < 2) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生值栈无法 SWAP");
                        rollback();
                        return false;
                    }
                    std::swap(valueStack[valueStack.size() - 1], valueStack[valueStack.size() - 2]);
                    break;

                case VMOperator::LOAD_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] LOAD_VALUE_SHARED 缺少名称");
                        rollback();
                        return false;
                    }
                    const std::string value = freshTmp();
                    emitLine(value + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_shared_value_load(ptr " + value +
                             ", ptr " + poolStr(std::get<std::string>(ins.args[0])) + ")");
                    valueStack.push_back(NativeValue{
                        value, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LOAD_VALUE: {
                    if (ins.args.size() == 1 &&
                        std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        NativeValue value;
                        if (!parseLiteral(ins, value)) {
                            rollback();
                            return false;
                        }
                        valueStack.push_back(std::move(value));
                        break;
                    }

                    if (ins.args.size() == 1 &&
                        std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        if (!dynamicVariadicABISymbol(name).empty()) {
                            valueStack.push_back(NativeValue{
                                name, "builtin", true, false
                            });
                        } else if (classLayouts.find(name) != classLayouts.end()) {
                            valueStack.push_back(NativeValue{
                                name, "classref", false, false, name
                            });
                        } else if (const auto* function = resolveFunctionSignature(name)) {
                            pendingFunctionLoad = function->name;
                            valueStack.push_back(NativeValue{
                                function->name, "function", true, false
                            });
                        } else {
                            failNativeTopLevel(ins, "[emit-llvm] 原生 lowering 无法解析名字 '" + name + "'");
                            rollback();
                            return false;
                        }
                        break;
                    }

                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_VALUE 形态不支持");
                        rollback();
                        return false;
                    }

                    const std::string& kind = std::get<std::string>(ins.args[0]);
                    int64_t index = 0;
                    if (!tryArgInt(ins, 1, index)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_VALUE 索引不是整数");
                        rollback();
                        return false;
                    }

                    if (kind == "global") {
                        const auto importedNamespace =
                            namespacePackagesByGlobal.find(static_cast<int>(index));
                        if (importedNamespace != namespacePackagesByGlobal.end()) {
                            valueStack.push_back(NativeValue{
                                importedNamespace->second,
                                "namespace",
                                false,
                                false,
                                importedNamespace->second
                            });
                            break;
                        }
                    }

                    if (kind == "global" && isFunctionGlobal(static_cast<int>(index))) {
                        valueStack.push_back(NativeValue{
                            functionGlobals[static_cast<int>(index)],
                            "function",
                            true,
                            false
                        });
                    } else if (kind == "global" &&
                               classGlobals.find(static_cast<int>(index)) != classGlobals.end()) {
                        const std::string& className =
                            classGlobals[static_cast<int>(index)];
                        valueStack.push_back(NativeValue{
                            className, "classref", false, false, className
                        });
                    } else if (kind == "global") {
                        // A global the slot table owns has to be read back from
                        // it, even when this scope still holds an older copy:
                        // a native function called in between wrote the newer
                        // value there.
                        const auto slotBacked =
                            nativeGlobalScalarTypes.find(static_cast<int>(index));
                        auto slot = nativeGlobalSlots.find(static_cast<int>(index));
                        if (slot != nativeGlobalSlots.end()) {
                            const std::string temporary = freshTmp();
                            emitLine(temporary + " = load " + slot->second.type +
                                     ", ptr " + slot->second.address);
                            valueStack.push_back(NativeValue{
                                temporary, slot->second.type, false, false,
                                slot->second.sourceType
                            });
                            break;
                        }

                        if (slotBacked != nativeGlobalScalarTypes.end()) {
                            NativeValue loaded;
                            if (!boxNativeGlobalValue(
                                    ins, static_cast<int>(index), loaded)) {
                                rollback();
                                return false;
                            }
                            valueStack.push_back(loaded);
                            break;
                        }

                        auto value = nativeGlobals.find(static_cast<int>(index));
                        if (value != nativeGlobals.end()) {
                            NativeValue borrowed = value->second;
                            borrowed.ownsClass = false;
                            valueStack.push_back(std::move(borrowed));
                        } else {
                            NativeValue loaded;
                            if (!boxNativeGlobalValue(
                                    ins, static_cast<int>(index), loaded)) {
                                rollback();
                                return false;
                            }
                            valueStack.push_back(loaded);
                        }
                    } else if (kind == "local") {
                        auto slot = nativeLocalSlots.find(static_cast<int>(index));
                        if (slot != nativeLocalSlots.end()) {
                            const std::string temporary = freshTmp();
                            emitLine(temporary + " = load " + slot->second.type +
                                     ", ptr " + slot->second.address);
                            valueStack.push_back(NativeValue{
                                temporary, slot->second.type, false, false,
                                slot->second.sourceType
                            });
                            break;
                        }

                        auto value = nativeLocals.find(static_cast<int>(index));
                        if (value != nativeLocals.end()) {
                            if (value->second.dynamic && value->second.ownsDynamic) {
                                const std::string cloned = freshTmp();
                                emitLine(cloned + " = alloca %ZeusValue, align 8");
                                emitLine("call void @zeus_value_clone(ptr " + cloned +
                                    ", ptr " + value->second.value + ")");
                                NativeValue copy = value->second;
                                copy.value = cloned;
                                valueStack.push_back(std::move(copy));
                            } else {
                                NativeValue borrowed = value->second;
                                borrowed.ownsClass = false;
                                valueStack.push_back(std::move(borrowed));
                            }
                        } else {
                            const std::string temporary = emitLocalValueLoad(static_cast<int>(index));
                            valueStack.push_back(NativeValue{ temporary, "ptr", false, true });
                        }
                    } else {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_VALUE 暂不支持 " + kind);
                        rollback();
                        return false;
                    }
                    break;
                }

                case VMOperator::OBJ_BUILD: {
                    int64_t argc = 0;
                    const std::string nativeTargetType = ins.args.size() > 1 &&
                        std::holds_alternative<std::string>(ins.args[1])
                            ? std::get<std::string>(ins.args[1])
                            : std::string();
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc) +
                            (nativeTargetType.empty() ? 1u : 0u)) {
                        failNativeTopLevel(ins, "[emit-llvm] class 构造参数数量无效");
                        rollback();
                        return false;
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popValue(ins, arguments[static_cast<size_t>(i)])) {
                            rollback();
                            return false;
                        }
                    }

                    NativeValue classReference;
                    if (nativeTargetType.empty() &&
                        (!popValue(ins, classReference) || classReference.type != "classref")) {
                        failNativeTopLevel(ins, "[emit-llvm] new 目标不是静态 class 名称");
                        rollback();
                        return false;
                    }

                    auto emitConstructionLine = [&](const std::string& line) {
                        emitLine(line);
                        if (line.rfind("br ", 0) == 0 || line == "unreachable") {
                            blkTerminated = true;
                            blkNeedLabel = true;
                        }
                    };
                    auto emitConstructionLabel = [&](const std::string& label) {
                        body += label + ":\n";
                        blkTerminated = false;
                        blkNeedLabel = false;
                    };
                    auto freshConstructionTemporary = [&]() {
                        return freshTmp();
                    };
                    auto convertConstructorArgument = [&] (
                        const NativeValue& argument,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        bool& ownsDynamic
                    ) {
                        ownsDynamic = false;
                        if (usesZeusValueABI(declaredType)) {
                            if (!emitNativeValueAsDynamic(argument, ins, nativeArgument)) {
                                return false;
                            }
                            ownsDynamic = argument.dynamic
                                ? argument.ownsDynamic
                                : true;
                            return true;
                        }
                        return emitNativeValueAsType(
                            argument, llvmType(declaredType), ins,
                            nativeArgument, declaredType);
                    };
                    auto failConstruction = [&] (
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        failNativeTopLevel(at, message);
                        return false;
                    };

                    NativeValue instance;
                    const bool constructionSucceeded = nativeTargetType.empty()
                        ? emitClassConstruction(
                            ins,
                            classReference.sourceType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            emitConstructionLabel,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction)
                        : emitNewOperatorConstruction(
                            ins,
                            nativeTargetType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction);
                    if (!constructionSucceeded) {
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(instance));
                    break;
                }

                case VMOperator::OBJ_LOAD_ATTR: {
                    if (ins.args.empty() ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] attribute load is missing its name");
                        rollback();
                        return false;
                    }

                    NativeValue object;
                    if (!popValue(ins, object) || object.functionRef) {
                        rollback();
                        return false;
                    }
                    if (isManagedClassValue(object)) {
                        emitLine("call void @zeus_class_ref_require_live(ptr " +
                                 object.value + ")");
                    }
                    const std::string& memberName =
                        std::get<std::string>(ins.args[0]);
                    if (object.type == "namespace") {
                        const auto exports =
                            namespaceExportsByPackage.find(object.sourceType);
                        if (exports == namespaceExportsByPackage.end() ||
                            exports->second.find(memberName) == exports->second.end() ||
                            !isClassType(memberName)) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] unsupported static module member " +
                                object.sourceType + "." + memberName);
                            rollback();
                            return false;
                        }
                        valueStack.push_back(NativeValue{
                            memberName, "classref", false, false, memberName
                        });
                        break;
                    }

                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    if (layout == nullptr) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] attribute load requires a known class receiver");
                        rollback();
                        return false;
                    }
                    const int fieldIndex = findFieldIndex(*layout, memberName);
                    if (fieldIndex < 0 ||
                        static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] unknown class field '" + memberName + "'");
                        rollback();
                        return false;
                    }
                    const std::string& fieldTypeName =
                        layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldType = llvmFieldType(fieldTypeName);
                    if (fieldType.empty()) {
                        failNativeTopLevel(ins,
                            "[emit-llvm] class field has no LLVM storage type");
                        rollback();
                        return false;
                    }
                    const std::string fieldAddress = freshTmp();
                    emitLine(fieldAddress + " = getelementptr inbounds " +
                             llvmStructType(object.sourceType) + ", ptr " +
                             object.value + ", i32 0, i32 " +
                             std::to_string(fieldIndex));
                    const auto inferred = layout->inferredFieldFlags.find(memberName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshTmp();
                        const std::string initialized = freshTmp();
                        const std::string initialized32 = freshTmp();
                        emitLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitLine(initialized + " = load i1, ptr " + flagAddress);
                        emitLine(initialized32 + " = zext i1 " + initialized + " to i32");
                        emitLine("call void @zeus_class_field_require_initialized(i32 " +
                            initialized32 + ", ptr " + poolStr(object.sourceType) +
                            ", ptr " + poolStr(memberName) + ", i32 " +
                            std::to_string(ins.line > 0 ? ins.line : 0) + ")");
                    }
                    if (usesZeusValueABI(fieldTypeName)) {
                        NativeValue loaded{
                            fieldAddress, "ptr", false, true, fieldTypeName, false
                        };
                        if (object.ownsClass && isManagedClassValue(object)) {
                            loaded.value = freshTmp();
                            emitLine(loaded.value + " = alloca %ZeusValue, align 8");
                            emitLine("call void @zeus_value_clone(ptr " + loaded.value +
                                     ", ptr " + fieldAddress + ")");
                            loaded.ownsDynamic = true;
                        }
                        valueStack.push_back(std::move(loaded));
                    } else {
                        const std::string loaded = freshTmp();
                        emitLine(loaded + " = load " + fieldType + ", ptr " +
                                 fieldAddress);
                        NativeValue loadedValue{
                            loaded, fieldType, false, false, fieldTypeName, false
                        };
                        if (isClassType(fieldTypeName) &&
                            object.ownsClass && isManagedClassValue(object)) {
                            emitLine("call void @zeus_class_ref_retain(ptr " + loaded + ")");
                            loadedValue.ownsClass = true;
                        }
                        valueStack.push_back(std::move(loadedValue));
                    }
                    if (object.ownsClass && isManagedClassValue(object)) {
                        releaseNativeValue(object);
                    }
                    break;
                }

                case VMOperator::OBJ_STORE_ATTR: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] attribute store is missing its name");
                        rollback();
                        return false;
                    }
                    NativeValue value;
                    NativeValue object;
                    if (!popValue(ins, value) || !popValue(ins, object) ||
                        object.functionRef || value.functionRef) {
                        rollback();
                        return false;
                    }
                    if (isManagedClassValue(object)) {
                        emitLine("call void @zeus_class_ref_require_live(ptr " +
                                 object.value + ")");
                    }
                    const std::string& memberName = std::get<std::string>(ins.args[0]);
                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    const int fieldIndex = layout ? findFieldIndex(*layout, memberName) : -1;
                    if (fieldIndex < 0 || static_cast<size_t>(fieldIndex) >= layout->fieldTypes.size()) {
                        failNativeTopLevel(ins, "[emit-llvm] attribute store requires a known class field '" + memberName + "'");
                        rollback();
                        return false;
                    }
                    if (static_cast<size_t>(fieldIndex) < layout->fieldConstness.size() &&
                        layout->fieldConstness[static_cast<size_t>(fieldIndex)]) {
                        failNativeTopLevel(ins, "[emit-llvm] cannot assign const class field '" + memberName + "'");
                        rollback();
                        return false;
                    }
                    const std::string& fieldTypeName = layout->fieldTypes[static_cast<size_t>(fieldIndex)];
                    const std::string fieldAddress = freshTmp();
                    emitLine(fieldAddress + " = getelementptr inbounds " +
                        llvmStructType(object.sourceType) + ", ptr " + object.value +
                        ", i32 0, i32 " + std::to_string(fieldIndex));
                    if (usesZeusValueABI(fieldTypeName)) {
                        std::string source;
                        bool ownsSource = false;
                        if (!toDynamicValue(value, ins, source, ownsSource)) {
                            rollback();
                            return false;
                        }
                        emitLine("call void @zeus_value_release(ptr " + fieldAddress + ")");
                        emitLine("call void @zeus_value_clone(ptr " + fieldAddress +
                                 ", ptr " + source + ")");
                        if (ownsSource) {
                            emitLine("call void @zeus_value_release(ptr " + source + ")");
                        }
                        if (value.ownsClass && isManagedClassValue(value)) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                    } else if (isClassType(fieldTypeName)) {
                        std::string storedValue = value.value;
                        if (value.dynamic) {
                            if (!emitNativeValueAsType(
                                    value, "ptr", ins, storedValue, fieldTypeName)) {
                                rollback();
                                return false;
                            }
                        } else if (!isManagedClassValue(value) ||
                                   value.sourceType != fieldTypeName) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] class field store requires a matching native class value");
                            rollback();
                            return false;
                        }
                        if (!value.ownsClass) {
                            emitLine("call void @zeus_class_ref_retain(ptr " + storedValue + ")");
                        }
                        const std::string previous = freshTmp();
                        emitLine(previous + " = load ptr, ptr " + fieldAddress);
                        emitLine("call void @zeus_class_ref_release(ptr " + previous + ")");
                        emitLine("store ptr " + storedValue + ", ptr " + fieldAddress);
                        if (value.dynamic && value.ownsDynamic) {
                            emitRelease(value.value);
                        }
                        if (value.ownsClass) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     storedValue + ")");
                        }
                    } else {
                        failNativeTopLevel(ins,
                            "[emit-llvm] native typed field store is not implemented at top level");
                        rollback();
                        return false;
                    }
                    const auto inferred = layout->inferredFieldFlags.find(memberName);
                    if (inferred != layout->inferredFieldFlags.end()) {
                        const std::string flagAddress = freshTmp();
                        emitLine(flagAddress + " = getelementptr inbounds " +
                            llvmStructType(object.sourceType) + ", ptr " + object.value +
                            ", i32 0, i32 " + std::to_string(inferred->second));
                        emitLine("store i1 1, ptr " + flagAddress);
                    }
                    if (object.ownsClass && isManagedClassValue(object)) {
                        releaseNativeValue(object);
                    }
                    break;
                }

                case VMOperator::OBJ_CALL_METHOD: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] class method call is missing its method name");
                        rollback();
                        return false;
                    }

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 1, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc) + 1) {
                        failNativeTopLevel(ins, "[emit-llvm] class method call has invalid arguments");
                        rollback();
                        return false;
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc) + 1);
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popValue(ins, arguments[static_cast<size_t>(i) + 1])) {
                            rollback();
                            return false;
                        }
                    }
                    if (!popValue(ins, arguments[0])) {
                        rollback();
                        return false;
                    }

                    auto convertArgument = [&] (
                        const NativeValue& value,
                        const std::string& declaredType,
                        std::string& nativeArgument,
                        std::string& cleanupValue
                    ) {
                        if (usesZeusValueABI(declaredType)) {
                            if (!emitNativeValueAsDynamic(value, ins, nativeArgument)) {
                                return false;
                            }
                            cleanupValue = value.dynamic
                                ? (value.ownsDynamic ? nativeArgument : std::string())
                                : nativeArgument;
                            return true;
                        }

                        if (!emitNativeValueAsType(
                                value, llvmType(declaredType), ins,
                                nativeArgument, declaredType)) {
                            return false;
                        }
                        cleanupValue = value.dynamic && value.ownsDynamic &&
                                !isClassType(declaredType)
                            ? value.value
                            : std::string();
                        return true;
                    };
                    auto releaseDynamic = [&](const std::string& value) {
                        emitRelease(value);
                    };
                    auto failMethod = [&](const IRType::IRInstr& at,
                                          const std::string& message) {
                        failNativeTopLevel(at, message);
                        return false;
                    };

                    NativeValue callResult;
                    const std::string methodName = std::get<std::string>(ins.args[0]);
                    const bool hasTypedImplMethod = hasTypedInstanceMethodSignature(
                        arguments[0].sourceType, methodName);
                    const bool staticClassReceiver = arguments[0].type == "classref" &&
                        isClassType(arguments[0].sourceType);
                    if (arguments[0].dynamic ||
                        (arguments[0].type != "ptr" && !staticClassReceiver) ||
                        (!isClassType(arguments[0].sourceType) && !hasTypedImplMethod)) {
                        auto toDynamic = [&](const NativeValue& value,
                                             std::string& dynamicValue,
                                             bool& ownsDynamic) {
                            if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                                return false;
                            }
                            ownsDynamic = value.dynamic ? value.ownsDynamic : true;
                            return true;
                        };
                        auto releaseOwnedDynamic = [&](const std::string& value) {
                            emitRelease(value);
                        };
                        if (!emitDynamicMemberCall(
                                ins,
                                methodName,
                                arguments,
                                callResult,
                                [&](const std::string& line) { emitLine(line); },
                                [&]() { return freshTmp(); },
                                toDynamic,
                                releaseOwnedDynamic,
                                failMethod)) {
                            rollback();
                            return false;
                        }
                    } else if (!emitKnownClassMethodCall(
                                   ins,
                                   methodName,
                                   arguments,
                                   callResult,
                                   [&](const std::string& line) { emitLine(line); },
                                   [&]() { return freshTmp(); },
                                   convertArgument,
                                   releaseDynamic,
                                   failMethod)) {
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(callResult));
                    break;
                }

                case VMOperator::MAKE_FUNCTION: {
                    int64_t protoId = 0;
                    if (!tryArgInt(ins, 0, protoId) ||
                        functionSignaturesByChunk.find(static_cast<size_t>(protoId)) ==
                            functionSignaturesByChunk.end()) {
                        failNativeTopLevel(ins, "[emit-llvm] MAKE_FUNCTION 缺少原生函数签名");
                        rollback();
                        return false;
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        failNativeTopLevel(ins, "[emit-llvm] shared store 缺少名称");
                        rollback();
                        return false;
                    }
                    NativeValue value;
                    if (!popValue(ins, value) || value.functionRef) {
                        rollback();
                        return false;
                    }
                    std::string dynamicValue;
                    if (!emitNativeValueAsDynamic(value, ins, dynamicValue)) {
                        rollback();
                        return false;
                    }
                    const char* operation = ins.op == VMOperator::STORE_VALUE_SHARED
                        ? "store" : "assign";
                    emitLine(std::string("call void @zeus_shared_value_") + operation +
                        "(ptr " + poolStr(std::get<std::string>(ins.args[0])) +
                        ", ptr " + dynamicValue + ")");
                    if (value.dynamic) {
                        if (value.ownsDynamic) releaseNativeValue(value);
                    } else {
                        emitLine("call void @zeus_value_release(ptr " + dynamicValue + ")");
                        if (value.ownsClass && isManagedClassValue(value)) {
                            releaseNativeValue(value);
                        }
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_GLOBAL:
                case VMOperator::ASSIGN_VALUE_GLOBAL: {
                    int64_t index = 0;
                    if (!tryArgInt(ins, 0, index)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 STORE_GLOBAL 索引不是整数");
                        rollback();
                        return false;
                    }

                    if (isFunctionGlobal(static_cast<int>(index))) {
                        break;
                    }

                    NativeValue value;
                    if (!popValue(ins, value) || value.functionRef) {
                        rollback();
                        return false;
                    }

                    auto slot = nativeGlobalSlots.find(static_cast<int>(index));
                    auto previousNative = nativeGlobals.find(static_cast<int>(index));
                    const bool releasePreviousClass = previousNative != nativeGlobals.end() &&
                        previousNative->second.ownsClass &&
                        isManagedClassValue(previousNative->second);
                    if (slot != nativeGlobalSlots.end()) {
                        if (!isNativeSlotValue(value) ||
                            slot->second.type != value.type ||
                            slot->second.sourceType != value.sourceType) {
                            failNativeTopLevel(ins,
                                               "[emit-llvm] 控制流全局槽位类型不匹配");
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(value, slot->second);
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeGlobals.erase(previousNative);
                        }
                    } else if (hasControlFlow && isNativeSlotValue(value)) {
                        if (!ensureSlot(nativeGlobalSlots, static_cast<int>(index),
                                        value.type, value.sourceType, ins)) {
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(value,
                            nativeGlobalSlots[static_cast<int>(index)]);
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeGlobals.erase(previousNative);
                        }
                    } else if (value.dynamic) {
                        entryDynamicValueGlobals.insert(static_cast<int>(index));
                        emitLine("call void @zeus_value_global_store(i32 " +
                                 std::to_string(index) + ", ptr " + value.value + ")");
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        nativeGlobals.erase(static_cast<int>(index));
                    } else {
                        if (isManagedClassValue(value)) {
                            if (!value.ownsClass) {
                                emitLine("call void @zeus_class_ref_retain(ptr " +
                                         value.value + ")");
                            }
                            if (releasePreviousClass) {
                                releaseNativeValue(previousNative->second);
                            }
                            value.ownsClass = true;
                            nativeGlobals[static_cast<int>(index)] = value;
                            break;
                        }
                        // A scalar written through a direct global access still
                        // updates the runtime slot, so a later read from a native
                        // function sees the new value.
                        const auto scalar =
                            nativeGlobalScalarTypes.find(static_cast<int>(index));
                        if (scalar != nativeGlobalScalarTypes.end() &&
                            !hasControlFlow && value.type == scalar->second) {
                            if (value.type == "i64") {
                                emitLine("call void @zeus_global_store_i64(i32 " +
                                         std::to_string(index) + ", i64 " + value.value + ")");
                            } else {
                                const std::string raw = freshTmp();
                                emitLine(raw + " = zext i1 " + value.value + " to i32");
                                emitLine("call void @zeus_global_store_bool(i32 " +
                                         std::to_string(index) + ", i32 " + raw + ")");
                            }
                            if (releasePreviousClass) {
                                releaseNativeValue(previousNative->second);
                            }
                            // The slot table is the only owner now: keeping an
                            // SSA copy here would shadow writes made by native
                            // functions called later.
                            nativeGlobals.erase(static_cast<int>(index));
                        } else {
                            if (releasePreviousClass) {
                                releaseNativeValue(previousNative->second);
                            }
                            nativeGlobals[static_cast<int>(index)] = value;
                        }
                    }
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 ||
                        !std::holds_alternative<std::string>(ins.args[0]) ||
                        !std::holds_alternative<std::string>(ins.args[1])) {
                        failNativeTopLevel(ins, "[emit-llvm] DELETE_VAR 参数无效");
                        rollback();
                        return false;
                    }
                    const std::string& kind = std::get<std::string>(ins.args[1]);
                    if (kind == "shared") {
                        emitLine("call i32 @zeus_shared_value_delete(ptr " +
                            poolStr(std::get<std::string>(ins.args[0])) + ")");
                        break;
                    }
                    if (kind != "global") {
                        failNativeTopLevel(ins, "[emit-llvm] 顶层 delete 仅支持 global/shared");
                        rollback();
                        return false;
                    }

                    int64_t index = 0;
                    if (!tryArgInt(ins, 2, index) || index < 0 ||
                        index > std::numeric_limits<int>::max()) {
                        failNativeTopLevel(ins, "[emit-llvm] delete 全局索引无效");
                        rollback();
                        return false;
                    }

                    const int globalIndex = static_cast<int>(index);
                    const auto slot = nativeGlobalSlots.find(globalIndex);
                    const auto value = nativeGlobals.find(globalIndex);
                    const bool slotIsClass = slot != nativeGlobalSlots.end() &&
                        slot->second.type == "ptr" && isClassType(slot->second.sourceType);
                    const bool valueIsClass = value != nativeGlobals.end() &&
                        value->second.type == "ptr" && isClassType(value->second.sourceType);
                    if (!slotIsClass && !valueIsClass) {
                        emitLine("call void @zeus_value_global_delete(i32 " +
                                 std::to_string(globalIndex) + ")");
                        if (value != nativeGlobals.end()) {
                            releaseNativeValue(value->second);
                            nativeGlobals.erase(value);
                        }
                        nativeGlobalSlots.erase(globalIndex);
                        break;
                    }

                    NativeValue object;
                    if (slotIsClass) {
                        object = NativeValue{
                            freshTmp(), "ptr", false, false, slot->second.sourceType
                        };
                        emitLine(object.value + " = load ptr, ptr " + slot->second.address);
                    } else {
                        object = value->second;
                    }

                    auto emitDeleteLine = [&](const std::string& line) {
                        emitLine(line);
                        if (line.rfind("br ", 0) == 0 || line == "unreachable") {
                            blkTerminated = true;
                            blkNeedLabel = true;
                        }
                    };
                    auto emitDeleteLabel = [&](const std::string& label) {
                        emitLabelDef(label);
                    };
                    auto failDelete = [&](
                        const IRType::IRInstr& at,
                        const std::string& message
                    ) {
                        failNativeTopLevel(at, message);
                        return false;
                    };
                    if (!emitClassDelete(
                            ins, object, emitDeleteLine, emitDeleteLabel,
                            [&]() { return freshTmp(); }, failDelete)) {
                        rollback();
                        return false;
                    }

                    if (slot != nativeGlobalSlots.end()) {
                        emitLine("store ptr null, ptr " + slot->second.address);
                    } else {
                        nativeGlobals.erase(globalIndex);
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL: {
                    int64_t index = 0;
                    NativeValue value;
                    if (!tryArgInt(ins, 0, index) || !popValue(ins, value) || value.functionRef) {
                        rollback();
                        return false;
                    }

                    auto slot = nativeLocalSlots.find(static_cast<int>(index));
                    auto previousNative = nativeLocals.find(static_cast<int>(index));
                    const bool releasePreviousClass = previousNative != nativeLocals.end() &&
                        previousNative->second.ownsClass &&
                        isManagedClassValue(previousNative->second);
                    if (slot != nativeLocalSlots.end()) {
                        if (!isNativeSlotValue(value) ||
                            slot->second.type != value.type ||
                            slot->second.sourceType != value.sourceType) {
                            failNativeTopLevel(ins,
                                               "[emit-llvm] 控制流局部槽位类型不匹配");
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(value, slot->second);
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeLocals.erase(previousNative);
                        }
                    } else if (hasControlFlow && isNativeSlotValue(value)) {
                        if (!ensureSlot(nativeLocalSlots, static_cast<int>(index),
                                        value.type, value.sourceType, ins)) {
                            rollback();
                            return false;
                        }
                        emitNativeSlotStore(value,
                            nativeLocalSlots[static_cast<int>(index)]);
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                            nativeLocals.erase(previousNative);
                        }
                    } else if (value.dynamic) {
                        emitLine("call void @zeus_value_local_store(i32 " +
                                 std::to_string(index) + ", ptr " + value.value + ")");
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        nativeLocals.erase(static_cast<int>(index));
                    } else if (isManagedClassValue(value)) {
                        if (!value.ownsClass) {
                            emitLine("call void @zeus_class_ref_retain(ptr " +
                                     value.value + ")");
                        }
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        value.ownsClass = true;
                        nativeLocals[static_cast<int>(index)] = value;
                    } else {
                        if (releasePreviousClass) {
                            releaseNativeValue(previousNative->second);
                        }
                        nativeLocals[static_cast<int>(index)] = value;
                    }
                    break;
                }

                case VMOperator::LIST_BUILD:
                case VMOperator::DICT_BUILD: {
                    int64_t count = 0;
                    if (!tryArgInt(ins, 0, count) || count < 0 ||
                        count > std::numeric_limits<int32_t>::max()) {
                        failNativeTopLevel(ins, "[emit-llvm] 容器构造元素数量无效");
                        rollback();
                        return false;
                    }
                    const bool dictionary = ins.op == VMOperator::DICT_BUILD;
                    const int64_t valueCount = dictionary ? count * 2 : count;
                    std::vector<NativeValue> values(
                        static_cast<std::size_t>(valueCount));
                    for (int64_t i = valueCount - 1; i >= 0; --i) {
                        if (!popValue(ins, values[static_cast<std::size_t>(i)])) {
                            rollback();
                            return false;
                        }
                    }

                    std::vector<std::string> dynamicValues;
                    std::vector<bool> ownsDynamicValues;
                    dynamicValues.reserve(values.size());
                    ownsDynamicValues.reserve(values.size());
                    for (const auto& value : values) {
                        std::string dynamicValue;
                        bool ownsDynamic = false;
                        if (value.functionRef || !toDynamicValue(
                                value, ins, dynamicValue, ownsDynamic)) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 容器元素不能转换为 ZeusValue");
                            rollback();
                            return false;
                        }
                        dynamicValues.push_back(dynamicValue);
                        ownsDynamicValues.push_back(ownsDynamic);
                    }

                    const std::string resultValue = ContainerABI::emitBuildCall(
                        dictionary, count, dynamicValues,
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    for (std::size_t i = 0; i < dynamicValues.size(); ++i) {
                        if (ownsDynamicValues[i]) emitRelease(dynamicValues[i]);
                    }
                    for (const auto& value : values) {
                        if (isManagedClassValue(value) && value.ownsClass) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     value.value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LENGTH:
                case VMOperator::DICT_KEYS_BUILD: {
                    NativeValue container;
                    if (!popValue(ins, container) || container.functionRef) {
                        failNativeTopLevel(ins, "[emit-llvm] 容器操作需要动态值");
                        rollback();
                        return false;
                    }
                    std::string dynamicContainer;
                    bool ownsDynamic = false;
                    if (!toDynamicValue(container, ins, dynamicContainer, ownsDynamic)) {
                        rollback();
                        return false;
                    }
                    const std::string symbol = ins.op == VMOperator::LIST_LENGTH
                        ? "@zeus_value_array_length"
                        : "@zeus_value_dict_keys";
                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, { dynamicContainer },
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    if (ownsDynamic) emitRelease(dynamicContainer);
                    if (isManagedClassValue(container) && container.ownsClass) {
                        emitLine("call void @zeus_class_ref_release(ptr " +
                                 container.value + ")");
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE: {
                    const bool isStore = ins.op == VMOperator::LIST_STORE_ELEM ||
                        ins.op == VMOperator::DICT_STORE_VALUE;
                    const bool isList = ins.op == VMOperator::LIST_LOAD_ELEM ||
                        ins.op == VMOperator::LIST_STORE_ELEM;
                    NativeValue storedValue;
                    NativeValue keyOrIndex;
                    NativeValue container;
                    if ((isStore && !popValue(ins, storedValue)) ||
                        !popValue(ins, keyOrIndex) || !popValue(ins, container) ||
                        (isStore && storedValue.functionRef) ||
                        keyOrIndex.functionRef || container.functionRef) {
                        failNativeTopLevel(ins, "[emit-llvm] 容器索引操作的栈值无效");
                        rollback();
                        return false;
                    }

                    const size_t parameterCount = isStore ? 3 : 2;
                    const std::string operatorSymbol = isStore ? "[]=" : "[]";
                    const FunctionSignature* operatorSignature = nullptr;
                    if (ins.metadata.kind == IRType::IRMetadataKind::DirectCall) {
                        std::string targetName;
                        if (!ins.metadata.readDirectCall(targetName)) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 下标运算标注缺少函数名");
                            rollback();
                            return false;
                        }
                        const auto target = functionSignatures.find(targetName);
                        if (target == functionSignatures.end() ||
                            target->second.operatorSymbol != operatorSymbol ||
                            target->second.parameterTypes.size() != parameterCount) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] 下标运算标注与目标签名不匹配");
                            rollback();
                            return false;
                        }
                        operatorSignature = &target->second;
                        container.sourceType = operatorSignature->ownerType;
                    } else if (!container.sourceType.empty() &&
                               (isClassType(container.sourceType) ||
                                ZeusBuiltins::findNativeTypeABI(
                                    container.sourceType) != nullptr)) {
                        operatorSignature = findClassOperatorSignature(
                            container.sourceType, operatorSymbol, parameterCount);
                        if (operatorSignature == nullptr) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] type 没有匹配的下标operator");
                            rollback();
                            return false;
                        }
                    }

                    if (!isList && operatorSignature != nullptr) {
                        const std::string qualifiedPrefix =
                            operatorSignature->ownerType + "::";
                        const std::string methodName = operatorSignature->name.substr(
                            qualifiedPrefix.size());
                        std::vector<NativeValue> operatorArguments{
                            container, keyOrIndex
                        };
                        if (isStore) operatorArguments.push_back(storedValue);

                        auto convertArgument = [&] (
                            const NativeValue& value,
                            const std::string& declaredType,
                            std::string& nativeArgument,
                            std::string& cleanupValue
                        ) {
                            if (usesZeusValueABI(declaredType)) {
                                bool ownsDynamic = false;
                                if (!toDynamicValue(
                                        value, ins, nativeArgument, ownsDynamic)) {
                                    return false;
                                }
                                cleanupValue = ownsDynamic ? nativeArgument : std::string();
                                return true;
                            }
                            if (!emitNativeValueAsType(
                                    value, llvmType(declaredType), ins,
                                    nativeArgument, declaredType)) {
                                return false;
                            }
                            cleanupValue = value.dynamic && value.ownsDynamic &&
                                    !isClassType(declaredType)
                                ? value.value : std::string();
                            return true;
                        };
                        auto failOperator = [&] (
                            const IRType::IRInstr& at,
                            const std::string& message
                        ) {
                            failNativeTopLevel(at, message);
                            return false;
                        };
                        NativeValue operatorResult;
                        if (!emitKnownClassMethodCall(
                                ins, methodName, operatorArguments, operatorResult,
                                [&](const std::string& line) { emitLine(line); },
                                [&]() { return freshTmp(); },
                                convertArgument,
                                [&](const std::string& value) { emitRelease(value); },
                                failOperator)) {
                            rollback();
                            return false;
                        }
                        valueStack.push_back(std::move(operatorResult));
                        break;
                    }

                    NativeValue operands[] = { container, keyOrIndex, storedValue };
                    std::string dynamicOperands[3];
                    bool ownsDynamicOperands[3] = { false, false, false };
                    const std::size_t operandCount = isStore ? 3 : 2;
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (!toDynamicValue(operands[i], ins,
                                            dynamicOperands[i],
                                            ownsDynamicOperands[i])) {
                            rollback();
                            return false;
                        }
                    }

                    std::string symbol;
                    std::vector<std::string> callArguments;
                    if (isList && !isStore) {
                        symbol = "@zeus_value_array_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else if (isList) {
                        symbol = "@zeus_value_array_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    } else if (!isStore) {
                        symbol = "@zeus_value_subscript_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else {
                        symbol = "@zeus_value_subscript_store";
                        callArguments = {
                            dynamicOperands[0], dynamicOperands[1], dynamicOperands[2]
                        };
                    }
                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, callArguments,
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    for (std::size_t i = 0; i < operandCount; ++i) {
                        if (ownsDynamicOperands[i]) emitRelease(dynamicOperands[i]);
                        if (isManagedClassValue(operands[i]) &&
                            operands[i].ownsClass) {
                            emitLine("call void @zeus_class_ref_release(ptr " +
                                     operands[i].value + ")");
                        }
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::ADD:
                case VMOperator::SUBTRACT:
                case VMOperator::MULTIPLY:
                case VMOperator::DIVIDE:
                case VMOperator::MODULUS:
                case VMOperator::EQUL:
                case VMOperator::NOT_EQUAL:
                case VMOperator::GREATER_THAN:
                case VMOperator::LESS_THAN:
                case VMOperator::GREATER_EQUAL:
                case VMOperator::LESS_EQUAL:
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR:
                    if (!emitBinary(ins)) {
                        rollback();
                        return false;
                    }
                    break;

                case VMOperator::LOGICAL_NOT:
                    if (!emitLogicalNot(ins)) {
                        rollback();
                        return false;
                    }
                    break;

                case VMOperator::LOAD_NATIVE: {
                    int64_t nativeId = 0;
                    if (!tryArgInt(ins, 0, nativeId)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 LOAD_NATIVE id 不是整数");
                        rollback();
                        return false;
                    }

                    const auto* builtin = ZeusBuiltins::get(static_cast<int>(nativeId));
                    if (!builtin ||
                        (dynamicVariadicABISymbol(builtin->name).empty() &&
                         findNativeBuiltinDeclaration(builtin->name) == nullptr)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 lowering 暂不支持该 builtin");
                        rollback();
                        return false;
                    }
                    valueStack.push_back(NativeValue{
                        builtin->name, "builtin", true, false
                    });
                    break;
                }

                case VMOperator::CALL_NATIVE: {
                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || !emitVariadicValueCall(ins, argc)) {
                        if (result.error.empty()) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生变长调用参数数量或类型无效");
                        }
                        rollback();
                        return false;
                    }
                    break;
                }

                case VMOperator::CALL_CLOSURE: {
                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            failNativeTopLevel(ins, "[emit-llvm] CallTarget 元信息缺少函数名");
                            rollback();
                            return false;
                        }
                        if (const auto* function = resolveFunctionSignature(pendingDirectCall)) {
                            pendingDirectCall = function->name;
                        }
                    }

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 CALL_CLOSURE 参数不是整数");
                        rollback();
                        return false;
                    }

                    if (!pendingDirectCall.empty()) {
                        auto function = functionSignatures.find(pendingDirectCall);
                        if (function == functionSignatures.end()) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生函数调用签名不匹配");
                            rollback();
                            return false;
                        }

                        if (!pendingFunctionLoad.empty() &&
                            pendingFunctionLoad != pendingDirectCall) {
                            failNativeTopLevel(ins, "[emit-llvm] 函数引用与直接调用标注不一致");
                            rollback();
                            return false;
                        }

                        std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                        for (int64_t i = argc - 1; i >= 0; --i) {
                            if (!popValue(ins, arguments[static_cast<size_t>(i)])) {
                                rollback();
                                return false;
                            }
                        }

                        NativeValue callee;
                        if (!popValue(ins, callee)) {
                            rollback();
                            return false;
                        }
                        if (callee.dynamic) {
                            NativeValue closureResult;
                            if (!emitClosureCallTopLevel(ins, callee, arguments, closureResult)) {
                                rollback();
                                return false;
                            }
                            valueStack.push_back(std::move(closureResult));
                            pendingDirectCall.clear();
                            pendingFunctionLoad.clear();
                            break;
                        }
                        if (!callee.functionRef || callee.value != pendingDirectCall) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生函数调用缺少匹配的函数引用");
                            rollback();
                            return false;
                        }

                        std::vector<NativeValue> orderedArguments;
                        std::vector<bool> providedArguments;
                        std::vector<NativeValue> variadicArguments;
                        std::vector<std::pair<std::string, NativeValue>> keywordArguments;
                        std::string bindingError;
                        if (!bindCallArguments(function->second, arguments,
                                orderedArguments, providedArguments, variadicArguments,
                                keywordArguments, bindingError)) {
                            failNativeTopLevel(ins, "[emit-llvm] " + bindingError);
                            rollback();
                            return false;
                        }

                        const bool returnsDynamic = usesZeusValueABI(function->second.returnType);
                        const std::string returnType = returnsDynamic
                            ? "void"
                            : llvmType(function->second.returnType);
                        if (returnType.empty()) {
                            failNativeTopLevel(ins, "[emit-llvm] 原生函数返回类型没有 LLVM ABI 映射");
                            rollback();
                            return false;
                        }

                        std::vector<std::string> nativeArguments;
                        std::vector<std::string> dynamicArguments;
                        std::vector<bool> ownsDynamicArguments;
                        for (size_t i = 0; i < function->second.parameterTypes.size(); ++i) {
                            if (!providedArguments[i]) {
                                NativeValue placeholder;
                                if (!emitOptionalArgumentPlaceholder(
                                        function->second.parameterTypes[i], placeholder,
                                        [this](const std::string& line) { emitLine(line); },
                                        [this]() { return freshTmp(); })) {
                                    failNativeTopLevel(ins,
                                        "[emit-llvm] optional argument has no native ABI placeholder");
                                    rollback();
                                    return false;
                                }
                                if (usesZeusValueABI(function->second.parameterTypes[i])) {
                                    dynamicArguments.push_back(placeholder.value);
                                    ownsDynamicArguments.push_back(false);
                                }
                                nativeArguments.push_back(placeholder.value);
                                continue;
                            }
                            std::string nativeArgument;
                            const std::string& parameterType =
                                function->second.parameterTypes[i];
                            if (usesZeusValueABI(parameterType)) {
                                if (!emitNativeValueAsDynamic(
                                        orderedArguments[i], ins, nativeArgument)) {
                                    rollback();
                                    return false;
                                }
                                dynamicArguments.push_back(nativeArgument);
                                ownsDynamicArguments.push_back(
                                    orderedArguments[i].dynamic ? orderedArguments[i].ownsDynamic : true);
                            } else {
                                if (!emitNativeValueAsType(
                                        orderedArguments[i], llvmType(parameterType), ins,
                                        nativeArgument, parameterType)) {
                                    rollback();
                                    return false;
                                }
                            }
                            nativeArguments.push_back(nativeArgument);
                        }

                        const std::string callResult = freshTmp();
                        std::string callText;
                        if (returnsDynamic) {
                            emitLine(callResult + " = alloca %ZeusValue, align 8");
                            callText = "call void " + llvmCallTarget(function->second.name) +
                                       "(ptr " + callResult;
                        } else {
                            callText = callResult + " = call " + returnType + " " +
                                       llvmCallTarget(function->second.name) + "(";
                        }
                        for (size_t i = 0; i < nativeArguments.size(); ++i) {
                            if (i > 0 || returnsDynamic) callText += ", ";
                            callText += functionParameterABIType(
                                            function->second.parameterTypes[i]) +
                                        " " + nativeArguments[i];
                        }
                        std::string packedArgs;
                        std::string packedKwargs;
                        if (function->second.hasVariadicArgs || function->second.hasVariadicKwargs) {
                            if (!emitVariadicContainers(
                                    function->second.hasVariadicArgs,
                                    function->second.hasVariadicKwargs,
                                    variadicArguments, keywordArguments,
                                    [&](const NativeValue& value, std::string& out, bool& owns) {
                                        return toDynamicValue(value, ins, out, owns);
                                    }, [this]() { return freshTmp(); },
                                    [this](const std::string& line) { emitLine(line); },
                                    packedArgs, packedKwargs)) {
                                rollback();
                                return false;
                            }
                            if (!packedArgs.empty()) {
                                if (!nativeArguments.empty() || returnsDynamic) callText += ", ";
                                callText += "ptr " + packedArgs;
                            }
                            if (!packedKwargs.empty()) {
                                if (!nativeArguments.empty() || returnsDynamic ||
                                    !packedArgs.empty()) callText += ", ";
                                callText += "ptr " + packedKwargs;
                            }
                        }
                        if (function->second.hasDefaultParameters()) {
                            if (!nativeArguments.empty() || returnsDynamic ||
                                !packedArgs.empty() || !packedKwargs.empty()) callText += ", ";
                            callText += "i64 " + std::to_string(providedArgumentMask(providedArguments));
                        }
                        callText += ")";
                        emitLine(callText);
                        if (!packedArgs.empty()) emitRelease(packedArgs);
                        if (!packedKwargs.empty()) emitRelease(packedKwargs);
                        std::set<std::string> releasedDynamicArguments;
                        for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                            if (ownsDynamicArguments[i] &&
                                releasedDynamicArguments.insert(dynamicArguments[i]).second) {
                                emitRelease(dynamicArguments[i]);
                            }
                        }
                        for (const auto& argument : arguments) {
                            if (argument.dynamic && argument.ownsDynamic &&
                                releasedDynamicArguments.insert(argument.value).second) {
                                emitRelease(argument.value);
                            }
                            if (isManagedClassValue(argument) && argument.ownsClass) {
                                emitLine("call void @zeus_class_ref_release(ptr " +
                                         argument.value + ")");
                            }
                        }
                        valueStack.push_back(returnsDynamic
                            ? NativeValue{
                                callResult, "ptr", false, true,
                                function->second.returnType, true
                              }
                            : NativeValue{
                                callResult, returnType, false, false,
                                function->second.returnType, false
                              });
                        valueStack.back().ownsClass =
                            !returnsDynamic && isClassType(function->second.returnType);
                        pendingDirectCall.clear();
                        pendingFunctionLoad.clear();
                        break;
                    }

                    if (!pendingVariadicCall.empty()) {
                        if (!emitVariadicValueCall(ins, argc)) {
                            if (result.error.empty()) {
                                failNativeTopLevel(ins, "[emit-llvm] 原生变长调用参数数量或类型无效");
                            }
                            rollback();
                            return false;
                        }
                        break;
                    }

                    const size_t calleePosition = valueStack.size() >=
                        static_cast<size_t>(argc) + 1
                            ? valueStack.size() - static_cast<size_t>(argc) - 1
                            : valueStack.size();
                    if (calleePosition < valueStack.size() &&
                        valueStack[calleePosition].functionRef &&
                        valueStack[calleePosition].type == "builtin") {
                        if (!emitVariadicValueCall(ins, argc)) {
                            if (result.error.empty()) {
                                failNativeTopLevel(ins,
                                    "[emit-llvm] 原生内置函数参数数量或类型无效");
                            }
                            rollback();
                            return false;
                        }
                        break;
                    }

                    if (calleePosition < valueStack.size() &&
                        valueStack[calleePosition].dynamic &&
                        valueStack.size() >= static_cast<size_t>(argc) + 1) {
                        std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                        for (int64_t i = argc - 1; i >= 0; --i) {
                            if (!popValue(ins, arguments[static_cast<size_t>(i)])) {
                                rollback();
                                return false;
                            }
                        }
                        NativeValue callee;
                        if (!popValue(ins, callee)) {
                            rollback();
                            return false;
                        }
                        NativeValue closureResult;
                        if (!emitClosureCallTopLevel(ins, callee, arguments, closureResult)) {
                            rollback();
                            return false;
                        }
                        valueStack.push_back(std::move(closureResult));
                        break;
                    }

                    failNativeTopLevel(ins, "[emit-llvm] 原生闭包调用缺少 direct_call metadata");
                    rollback();
                    return false;
                }

                case VMOperator::JUMP:
                case VMOperator::LOOP_BREAK: {
                    if (!valueStack.empty()) {
                        failNativeTopLevel(ins,
                                           "[emit-llvm] 标量控制流出口仍有未消费的 VM 栈值");
                        rollback();
                        return false;
                    }

                    int64_t off = 0;
                    if (!tryArgInt(ins, 0, off)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生跳转偏移不是整数");
                        rollback();
                        return false;
                    }

                    const long long target = static_cast<long long>(pc) + 1 + off;
                    emitLine(target == static_cast<long long>(chunk.size())
                        ? "br label " + endLabelFor(ci)
                        : "br label " + labelFor(ci, static_cast<size_t>(target)));
                    blkTerminated = true;
                    blkNeedLabel = true;
                    controlFlowStarted = true;
                    break;
                }

                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::JUMP_IF_TRUE: {
                    NativeValue condition;
                    if (!popValue(ins, condition) || condition.functionRef) {
                        rollback();
                        return false;
                    }

                    if (!valueStack.empty()) {
                        failNativeTopLevel(ins,
                                           "[emit-llvm] 条件分支仍有未消费的 VM 栈值");
                        rollback();
                        return false;
                    }

                    std::string booleanValue;
                    if (condition.dynamic) {
                        const std::string truthy = freshTmp();
                        emitLine(truthy + " = call i32 @zeus_value_truthy(ptr " + condition.value + ")");
                        emitRelease(condition.value);
                        booleanValue = freshTmp();
                        emitLine(booleanValue + " = icmp ne i32 " + truthy + ", 0");
                    } else if (condition.type == "i1") {
                        booleanValue = condition.value;
                    } else if (condition.type == "i64") {
                        booleanValue = freshTmp();
                        emitLine(booleanValue + " = icmp ne i64 " + condition.value + ", 0");
                    } else {
                        failNativeTopLevel(ins, "[emit-llvm] 条件分支要求 bool/int 值");
                        rollback();
                        return false;
                    }

                    int64_t off = 0;
                    if (!tryArgInt(ins, 0, off)) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生条件跳转偏移不是整数");
                        rollback();
                        return false;
                    }

                    const long long target = static_cast<long long>(pc) + 1 + off;
                    const size_t fallthrough = pc + 1;
                    const std::string targetLabel =
                        target == static_cast<long long>(chunk.size())
                            ? endLabelFor(ci)
                            : labelFor(ci, static_cast<size_t>(target));
                    const std::string fallthroughLabel =
                        fallthrough == chunk.size()
                            ? endLabelFor(ci)
                            : labelFor(ci, fallthrough);

                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        emitLine("br i1 " + booleanValue + ", label " +
                                 fallthroughLabel + ", label " + targetLabel);
                    } else {
                        emitLine("br i1 " + booleanValue + ", label " +
                                 targetLabel + ", label " + fallthroughLabel);
                    }

                    blkTerminated = true;
                    blkNeedLabel = true;
                    controlFlowStarted = true;
                    break;
                }

                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO: {
                    NativeValue checked;
                    if (!popValue(ins, checked)) {
                        rollback();
                        return false;
                    }
                    const std::string checkCall = rangeCheckCall(checked, ins);
                    if (!checkCall.empty()) emitLine(checkCall);
                    releaseNativeValue(checked);
                    break;
                }

                default:
                    failNativeTopLevel(ins, "[emit-llvm] 原生 lowering 遇到未处理指令");
                    rollback();
                    return false;
            }
        }

        activeEntryExceptionTarget = -1;
        if (hasControlFlow && exitJump) {
            emitLabelDef(endLabelFor(ci));

            if (!isLast) {
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true;
            }
        }

        for (const auto& value : valueStack) releaseNativeValue(value);
        valueStack.clear();
        if (!pendingVariadicCall.empty() || !pendingDirectCall.empty() ||
            !pendingFunctionLoad.empty()) {
            ZEUS_DEBUG_PRINT(
                "[emit-llvm] native top-level chunk %zu ended with variadic='%s', direct='%s', function='%s'",
                ci, pendingVariadicCall.c_str(),
                pendingDirectCall.c_str(), pendingFunctionLoad.c_str());
            rollback();
            return false;
        }

        return true;
    }

    bool emitChunk(const std::vector<IRType::IRInstr>& chunk, size_t ci, bool isLast) {
        if (!chunk.empty() && chunk.front().op == VMOperator::FUNC_BEGIN) {
            if (!entryLabelEmitted) {
                emitLabelDef("%L0_0");
                entryLabelEmitted = true;
            } else if (blkNeedLabel) {
                emitLabelDef(labelFor(ci, 0));
            }

            // 函数定义不属于 zeus_entry 的执行流；入口只需要跳过该 chunk。
            if (!isLast) {
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true;
            }

            if (reachableFunctionChunks.find(ci) == reachableFunctionChunks.end()) {
                return true;
            }
            return emitNativeFunction(chunk, ci);
        }

        bool classDeclaration = false;
        for (const auto& ins : chunk) {
            if (ins.op == VMOperator::CLASS_BUILD ||
                ins.op == VMOperator::CLASS_BUILD_SHARED) {
                classDeclaration = true;
                break;
            }
        }

        if (classDeclaration) {
            // 静态类函数已经单独生成了 LLVM function；类注册本身属于 VM
            // 运行时协议，AOT 入口不需要再执行 CLASS_BUILD。
            if (!entryLabelEmitted) {
                emitLabelDef("%L0_0");
                entryLabelEmitted = true;
            } else if (blkNeedLabel) {
                emitLabelDef(labelFor(ci, 0));
            }

            if (!isLast) {
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true;
            }
            return true;
        }

        if (chunk.empty()) {
            // 本 chunk 一般无需出码；唯一例外是 chunk 0：entry 块的
            // br %L0_0 要求的 label 必须存在。被跳过的静态类 chunk
            // 后面可能还有的 chunk，也要保留控制流桥接 label。
            if (ci == 0 || blkNeedLabel) {
                emitLabelDef(ci == 0 ? "%L0_0" : labelFor(ci, 0));
                entryLabelEmitted = true;

                if (!isLast) {
                    emitLine("br label " + labelFor(ci + 1, 0));
                    blkTerminated = true;
                    blkNeedLabel = true;
                }
            }

            return result.error.empty();
        }

        if (emitNativeTopLevelChunk(chunk, ci, isLast)) {
            return true;
        }

        std::vector<int64_t> entryExceptionTarget(chunk.size(), -1);
        std::vector<size_t> entryExceptionStack;
        std::vector<size_t> entryCatchStack;
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            if (ins.op == VMOperator::TRY_BEGIN) {
                int64_t offset = 0;
                if (!tryArgInt(ins, 0, offset) || offset <= 0 ||
                    static_cast<uint64_t>(offset) >= chunk.size() - pc ||
                    chunk[pc + static_cast<size_t>(offset)].op != VMOperator::CATCH_BEGIN) {
                    fail(ins, "[emit-llvm] invalid top-level TRY_BEGIN catch target");
                    return false;
                }
                entryHasTry = true;
                needsRuntimeErrorFactory = true;
                entryExceptionStack.push_back(pc + static_cast<size_t>(offset));
            } else if (ins.op == VMOperator::TRY_END) {
                if (entryExceptionStack.empty()) {
                    fail(ins, "[emit-llvm] top-level TRY_END without TRY_BEGIN");
                    return false;
                }
                entryExceptionStack.pop_back();
            } else if (!entryExceptionStack.empty()) {
                entryExceptionTarget[pc] = static_cast<int64_t>(entryExceptionStack.back());
            }
            if (ins.op == VMOperator::CATCH_BEGIN) entryCatchStack.push_back(pc);
            if (ins.op == VMOperator::CATCH_END) {
                if (entryCatchStack.empty()) {
                    fail(ins, "[emit-llvm] top-level CATCH_END without CATCH_BEGIN");
                    return false;
                }
                entryCatchStack.pop_back();
            }
        }
        if (!entryExceptionStack.empty() || !entryCatchStack.empty()) {
            result.error = "[emit-llvm] unterminated top-level try/catch";
            return false;
        }

        // Pass 1：只收集跳转目标。目标 == size 是合法的“离开该 chunk”
        // （如末尾 if 的 false 分支），映射到出口块；条件分支的 fall through
        // size 同理（隐式离开）。直接 fallthrough 不打 label。
        std::set<size_t> labels;
        bool exitJump = false;

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];

            if (ins.op == VMOperator::CATCH_BEGIN) labels.insert(pc);

            if (ins.op == VMOperator::JUMP ||
                ins.op == VMOperator::JUMP_IF_FALSE ||
                ins.op == VMOperator::JUMP_IF_TRUE ||
                ins.op == VMOperator::LOOP_BREAK) {
                int64_t off = 0;

                if (!tryArgInt(ins, 0, off)) {
                    fail(ins, "[emit-llvm] 跳转偏移不是整数");
                    return false;
                }

                long long tgt = (long long)pc + 1 + off;

                if (tgt < 0 || tgt > (long long)chunk.size()) {
                    fail(ins, "[emit-llvm] 跳转目标越界");
                    return false;
                }

                if (tgt == (long long)chunk.size()) {
                    exitJump = true;
                } else {
                    labels.insert((size_t)tgt);
                }

                if ((ins.op == VMOperator::JUMP_IF_FALSE ||
                     ins.op == VMOperator::JUMP_IF_TRUE)) {
                    if (pc + 1 < chunk.size()) {
                        labels.insert(pc + 1); // 条件分支的 fallthrough 后继必须有块
                    } else {
                        exitJump = true; // fall through chunk 尾= 隐式离开
                    }
                }
            }
        }

        // Pass 2：逐条翻译（label 只出现在控制流需要处）。
        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];
            activeEntryExceptionTarget = entryExceptionTarget[pc];
            activeEntryChunk = ci;

            // L0_0 无条件存在（entry 的 br 到它），其余 label 只出现在控制流需要处。
            if (labels.count(pc) || blkNeedLabel || (blkTerminated && pc == 0) ||
                (ci == 0 && pc == 0)) {
                std::string label = labelFor(ci, pc);
                if (!entryLabelEmitted && blkTerminated && pc == 0) {
                    label = "%L0_0";
                    entryLabelEmitted = true;
                }
                emitLabelDef(label);
            }

            if (blkTerminated) {
                continue; // 终结后的不可达指令，跳过
            }

            std::string comment = "; chunk #" + std::to_string(ci) + " pc=" + std::to_string(pc);

            if (ins.line >= 0) comment += " [L" + std::to_string(ins.line) + "]";

            comment += " " + ins.toString();
            body += "  " + comment + "\n";

            switch (ins.op) {
                case VMOperator::TRY_BEGIN:
                case VMOperator::TRY_END:
                case VMOperator::CATCH_END:
                    break;

                case VMOperator::CATCH_BEGIN: {
                    const auto slot = entryExceptionSlots.find(std::make_pair(ci, pc));
                    if (slot == entryExceptionSlots.end()) {
                        emitLine("unreachable");
                        blkTerminated = true;
                        break;
                    }
                    int64_t localSlot = -1;
                    if (tryArgInt(ins, 1, localSlot) && localSlot >= 0) {
                        emitLine("call void @zeus_value_local_store(i32 " +
                            std::to_string(localSlot) + ", ptr " + slot->second + ")");
                        nativeLocals.erase(static_cast<int>(localSlot));
                        nativeLocalSlots.erase(static_cast<int>(localSlot));
                    } else {
                        emitLine("call void @zeus_value_release(ptr " + slot->second + ")");
                    }
                    break;
                }

                case VMOperator::THROW: {
                    const std::string thrown = emitPop();
                    emitLine("call void @zeus_exception_raise_owned(ptr " + thrown + ")");
                    emitLine("unreachable");
                    blkTerminated = true;
                    break;
                }

                case VMOperator::IR_ANNOTATION: {
                    if (annotationKind(ins, "direct_call")) {
                        std::string directCallName;
                        if (!annotationSymbolName(ins, directCallName)) {
                            fail(ins, "[emit-llvm] 直接调用标注缺少函数名");
                            return false;
                        }
                        const auto* target = resolveFunctionSignature(directCallName);
                        pendingDirectCall = target != nullptr &&
                            !hasCompleteFunctionAnnotations(*target)
                                ? std::string()
                                : (target != nullptr ? target->name : directCallName);
                    }
                    break;
                }

                case VMOperator::IMPORT:
                case VMOperator::TYPE_IMPL_BUILD:
                    break;

                case VMOperator::PUSH: {
                    if (!ins.args.empty() &&
                        std::holds_alternative<IRType::NullLiteral>(ins.args[0])) {
                        const std::string nilValue = emitDynamicValueForNull(ins);
                        if (nilValue.empty()) return false;
                        emitPushValue(nilValue);
                        break;
                    }
                    if (!ins.args.empty() &&
                        std::holds_alternative<ZeusType::Bytes>(ins.args[0])) {
                        const std::string bytesValue = emitDynamicValueForBytes(
                            std::get<ZeusType::Bytes>(ins.args[0]), ins);
                        if (bytesValue.empty()) return false;
                        emitPushValue(bytesValue);
                        break;
                    }
                    if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        fail(ins, "[emit-llvm] v0: PUSH 仅支持 Value/字面量");
                        return false;
                    }

                    std::string t = emitDynamicValueForLiteral(std::get<LexerType::Value>(ins.args[0]), ins);

                    if (t.empty()) return false; // emitDynamicValueForLiteral already failed

                    emitPushValue(t);
                    break;
                }

                case VMOperator::PUSH_NAMED:
                    result.error = "[emit-llvm] dynamic stack lowering cannot preserve named argument metadata";
                    return false;

                case VMOperator::LOAD_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        fail(ins, "[emit-llvm] LOAD_VALUE_SHARED 缺少名称");
                        return false;
                    }
                    const std::string value = freshTmp();
                    emitLine(value + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_shared_value_load(ptr " + value +
                             ", ptr " + poolStr(std::get<std::string>(ins.args[0])) + ")");
                    emitPushValue(value);
                    break;
                }

                case VMOperator::POP: {
                    std::string t = emitPop();
                    emitRelease(t);
                    break;
                }

                case VMOperator::SWAP: {
                    const std::string top = emitPop();
                    const std::string below = emitPop();
                    emitPushValue(top);
                    emitPushValue(below);
                    break;
                }

                case VMOperator::LOAD_VALUE: {
                    // 形态：单个 Value 或 字面量。
                    if (ins.args.size() == 1 && std::holds_alternative<LexerType::Value>(ins.args[0])) {
                        std::string t = emitDynamicValueForLiteral(std::get<LexerType::Value>(ins.args[0]), ins);

                        if (t.empty()) return false;

                        emitPushValue(t);
                        break;
                    }

                    // 形态：单个 string 、未解析名或已知动态变长 ABI。
                    if (ins.args.size() == 1 && std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);

                        if (!dynamicVariadicABISymbol(name).empty()) {
                            pendingVariadicCallStack.push_back(name);
                            break;
                        }

                        if (const auto* function = resolveFunctionSignature(name)) {
                            pendingFunctionLoad = function->name;
                            break; // 原生函数引用只作为直接调用的元信息存。
                        }

                        fail(ins, "[emit-llvm] v0: 未知名字 '" + name + "'（freestanding：仅已注册的动态变长 ABI 可用）");
                        return false;
                    }

                    // 形态：scope + index。
                    if (ins.args.size() >= 2 && std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& kind = std::get<std::string>(ins.args[0]);
                        int64_t idx = 0;

                        if (!tryArgInt(ins, 1, idx)) {
                            fail(ins, "[emit-llvm] LOAD 索引不是整数");
                            return false;
                        }

                        if (kind == "global") {
                            if (isFunctionGlobal(static_cast<int>(idx))) {
                                pendingFunctionLoad = functionGlobals[static_cast<int>(idx)];
                                break;
                            }

                            std::string t;
                            auto slot = nativeGlobalSlots.find(static_cast<int>(idx));
                            if (slot != nativeGlobalSlots.end()) {
                                NativeValue slotValue{
                                    freshTmp(), slot->second.type, false, false
                                };
                                emitLine(slotValue.value + " = load " + slotValue.type +
                                         ", ptr " + slot->second.address);
                                if (!emitNativeValueAsDynamic(slotValue, ins, t)) return false;
                            } else {
                                auto native = nativeGlobals.find(static_cast<int>(idx));
                                if (native != nativeGlobals.end()) {
                                    if (!emitNativeValueAsDynamic(native->second, ins, t)) return false;
                                } else {
                                    t = emitGlobalValueLoad(static_cast<int>(idx));
                                }
                            }
                            emitPushValue(t);
                        } else if (kind == "local") {
                            std::string t;
                            auto slot = nativeLocalSlots.find(static_cast<int>(idx));
                            if (slot != nativeLocalSlots.end()) {
                                NativeValue slotValue{
                                    freshTmp(), slot->second.type, false, false
                                };
                                emitLine(slotValue.value + " = load " + slotValue.type +
                                         ", ptr " + slot->second.address);
                                if (!emitNativeValueAsDynamic(slotValue, ins, t)) return false;
                            } else {
                                auto native = nativeLocals.find(static_cast<int>(idx));
                                if (native != nativeLocals.end()) {
                                    if (!emitNativeValueAsDynamic(native->second, ins, t)) return false;
                                } else {
                                    t = emitLocalValueLoad(static_cast<int>(idx));
                                }
                            }
                            emitPushValue(t);
                        } else {
                            fail(ins, "[emit-llvm] v0: LOAD_" + kind + " 暂不支持");
                            return false;
                        }
                        break;
                    }

                    fail(ins, "[emit-llvm] v0: LOAD_VALUE 形态暂不支持");
                    return false;
                }

                case VMOperator::MAKE_FUNCTION: {
                    int64_t protoId = 0;
                    if (!tryArgInt(ins, 0, protoId) ||
                        functionSignaturesByChunk.find(static_cast<size_t>(protoId)) == functionSignaturesByChunk.end()) {
                        fail(ins, "[emit-llvm] MAKE_FUNCTION 没有对应的原生函数签名标注");
                        return false;
                    }
                    // 原生函数定义已经直接生成 LLVM function，动态闭包对象无需创建。
                    break;
                }

                case VMOperator::STORE_VALUE_SHARED:
                case VMOperator::ASSIGN_VALUE_SHARED: {
                    if (ins.args.empty() || !std::holds_alternative<std::string>(ins.args[0])) {
                        fail(ins, "[emit-llvm] shared store 缺少名称");
                        return false;
                    }
                    const std::string value = emitPop();
                    const char* operation = ins.op == VMOperator::STORE_VALUE_SHARED
                        ? "store" : "assign";
                    emitLine(std::string("call void @zeus_shared_value_") + operation +
                        "(ptr " + poolStr(std::get<std::string>(ins.args[0])) +
                        ", ptr " + value + ")");
                    emitRelease(value);
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 ||
                        !std::holds_alternative<std::string>(ins.args[0]) ||
                        !std::holds_alternative<std::string>(ins.args[1])) {
                        fail(ins, "[emit-llvm] DELETE_VAR 参数无效");
                        return false;
                    }
                    const std::string& kind = std::get<std::string>(ins.args[1]);
                    if (kind == "shared") {
                        emitLine("call i32 @zeus_shared_value_delete(ptr " +
                                 poolStr(std::get<std::string>(ins.args[0])) + ")");
                        break;
                    }
                    int64_t index = 0;
                    if (!tryArgInt(ins, 2, index) || index < 0 ||
                        index > std::numeric_limits<int32_t>::max()) {
                        fail(ins, "[emit-llvm] DELETE_VAR 索引无效");
                        return false;
                    }
                    if (kind == "global") {
                        emitLine("call void @zeus_value_global_delete(i32 " +
                                 std::to_string(index) + ")");
                    } else if (kind == "local") {
                        emitLine("call void @zeus_value_local_delete(i32 " +
                                 std::to_string(index) + ")");
                    } else if (kind == "upvalue") {
                        emitLine("call void @zeus_closure_env_delete(ptr %closure.env, i32 " +
                                 std::to_string(index) + ")");
                    } else {
                        fail(ins, "[emit-llvm] DELETE_VAR 变量类型不支持");
                        return false;
                    }
                    break;
                }

                case VMOperator::STORE_VALUE_GLOBAL: {
                    int64_t idx = 0;

                    if (!tryArgInt(ins, 0, idx)) {
                        fail(ins, "[emit-llvm] STORE_GLOBAL 索引不是整数");
                        return false;
                    }

                    if (isFunctionGlobal(static_cast<int>(idx))) {
                        break;
                    }

                    std::string t = emitPop();
                    entryDynamicValueGlobals.insert(static_cast<int>(idx));
                    emitLine("call void @zeus_value_global_store(i32 " + std::to_string(idx) + ", ptr " + t + ")");
                    nativeGlobals.erase(static_cast<int>(idx));
                    nativeGlobalSlots.erase(static_cast<int>(idx));
                    break;
                }

                case VMOperator::ASSIGN_VALUE_GLOBAL: {
                    int64_t idx = 0;

                    if (!tryArgInt(ins, 0, idx)) {
                        fail(ins, "[emit-llvm] ASSIGN_GLOBAL 索引不是整数");
                        return false;
                    }

                    if (isFunctionGlobal(static_cast<int>(idx))) {
                        fail(ins, "[emit-llvm] 原生函数引用不可重新赋值");
                        return false;
                    }

                    std::string t = emitPop();
                    entryDynamicValueGlobals.insert(static_cast<int>(idx));
                    emitLine("call void @zeus_value_global_store(i32 " + std::to_string(idx) + ", ptr " + t + ")");
                    nativeGlobals.erase(static_cast<int>(idx));
                    nativeGlobalSlots.erase(static_cast<int>(idx));
                    break;
                }

                case VMOperator::STORE_VALUE_LOCAL:
                case VMOperator::ASSIGN_VALUE_LOCAL: {
                    int64_t idx = 0;

                    if (!tryArgInt(ins, 0, idx)) {
                        fail(ins, "[emit-llvm] STORE_LOCAL 索引不是整数");
                        return false;
                    }

                    std::string t = emitPop();
                    emitLine("call void @zeus_value_local_store(i32 " + std::to_string(idx) + ", ptr " + t + ")");
                    nativeLocals.erase(static_cast<int>(idx));
                    nativeLocalSlots.erase(static_cast<int>(idx));
                    break;
                }

                case VMOperator::LIST_BUILD:
                case VMOperator::DICT_BUILD: {
                    int64_t count = 0;
                    if (!tryArgInt(ins, 0, count) || count < 0 ||
                        count > std::numeric_limits<int32_t>::max()) {
                        fail(ins, "[emit-llvm] 容器构造元素数量无效");
                        return false;
                    }
                    const bool dictionary = ins.op == VMOperator::DICT_BUILD;
                    const int64_t valueCount = dictionary ? count * 2 : count;
                    std::vector<std::string> values(
                        static_cast<std::size_t>(valueCount));
                    for (int64_t i = valueCount - 1; i >= 0; --i) {
                        values[static_cast<std::size_t>(i)] = emitPop();
                    }
                    const std::string resultValue = ContainerABI::emitBuildCall(
                        dictionary, count, values,
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    for (const auto& value : values) emitRelease(value);
                    emitPushValue(resultValue);
                    break;
                }

                case VMOperator::LIST_LENGTH:
                case VMOperator::DICT_KEYS_BUILD: {
                    const std::string container = emitPop();
                    const std::string symbol = ins.op == VMOperator::LIST_LENGTH
                        ? "@zeus_value_array_length"
                        : "@zeus_value_dict_keys";
                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, { container },
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    emitRelease(container);
                    emitPushValue(resultValue);
                    break;
                }

                case VMOperator::LIST_LOAD_ELEM:
                case VMOperator::LIST_STORE_ELEM:
                case VMOperator::DICT_LOAD_VALUE:
                case VMOperator::DICT_STORE_VALUE: {
                    const bool isStore = ins.op == VMOperator::LIST_STORE_ELEM ||
                        ins.op == VMOperator::DICT_STORE_VALUE;
                    const bool isList = ins.op == VMOperator::LIST_LOAD_ELEM ||
                        ins.op == VMOperator::LIST_STORE_ELEM;
                    std::string storedValue;
                    if (isStore) storedValue = emitPop();
                    const std::string keyOrIndex = emitPop();
                    const std::string container = emitPop();

                    std::string symbol;
                    std::vector<std::string> callArguments;
                    if (isList && !isStore) {
                        symbol = "@zeus_value_array_load";
                        callArguments = { container, keyOrIndex };
                    } else if (isList) {
                        symbol = "@zeus_value_array_store";
                        callArguments = { container, keyOrIndex, storedValue };
                    } else if (!isStore) {
                        symbol = "@zeus_value_subscript_load";
                        callArguments = { container, keyOrIndex };
                    } else {
                        symbol = "@zeus_value_subscript_store";
                        callArguments = { container, keyOrIndex, storedValue };
                    }

                    const std::string resultValue = ContainerABI::emitResultCall(
                        symbol, callArguments,
                        [&]() { return freshTmp(); },
                        [&](const std::string& line) { emitLine(line); });
                    emitRelease(container);
                    emitRelease(keyOrIndex);
                    if (isStore) emitRelease(storedValue);
                    emitPushValue(resultValue);
                    break;
                }

                case VMOperator::LOGICAL_NOT: {
                    const std::string operand = emitPop();
                    const std::string truthy = freshTmp();
                    emitLine(truthy + " = call i32 @zeus_value_truthy(ptr " + operand + ")");
                    const std::string resultBool = freshTmp();
                    emitLine(resultBool + " = icmp eq i32 " + truthy + ", 0");
                    const std::string resultRaw = freshTmp();
                    emitLine(resultRaw + " = zext i1 " + resultBool + " to i32");
                    const std::string resultValue = freshTmp();
                    emitLine(resultValue + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_init_bool(ptr " + resultValue +
                             ", i32 " + resultRaw + ")");
                    emitPushValue(resultValue);
                    emitRelease(operand);
                    break;
                }

                case VMOperator::ADD: {
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_int_add(ptr " + t + ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::SUBTRACT:
                case VMOperator::MULTIPLY:
                case VMOperator::DIVIDE:
                case VMOperator::MODULUS:
                case VMOperator::EQUL:
                case VMOperator::NOT_EQUAL:
                case VMOperator::LOGICAL_AND:
                case VMOperator::LOGICAL_OR: {
                    const int dynamicOpcode = DynamicBinaryABI::opcodeFor(ins.op);
                    if (dynamicOpcode < 0) {
                        fail(ins, "[emit-llvm] dynamic 值暂不支持该二元运算");
                        return false;
                    }
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_dynamic_binary(ptr " + t +
                             ", ptr null, i32 " + std::to_string(dynamicOpcode) +
                             ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::LESS_THAN: {
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_less_than(ptr " + t + ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::GREATER_THAN: {
                    std::string r = emitPop();
                    std::string l = emitPop();
                    std::string t = freshTmp();
                    emitLine(t + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_dynamic_binary(ptr " + t +
                             ", ptr null, i32 " +
                             std::to_string(DynamicBinaryABI::opcodeFor(ins.op)) +
                             ", ptr " + l + ", ptr " + r + ")");
                    emitPushValue(t);
                    emitRelease(l);
                    emitRelease(r);
                    break;
                }

                case VMOperator::JUMP:
                case VMOperator::LOOP_BREAK: {
                    int64_t off = 0;
                    tryArgInt(ins, 0, off); // Pass 1 已验证。
                    long long tgt = (long long)pc + 1 + off;

                    if (tgt == (long long)chunk.size()) {
                        emitLine("br label " + endLabelFor(ci));
                    } else {
                        emitLine("br label " + labelFor(ci, (size_t)tgt));
                    }

                    blkTerminated = true;
                    blkNeedLabel = true;
                    break;
                }

                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::JUMP_IF_TRUE: {
                    int64_t off = 0;
                    tryArgInt(ins, 0, off); // Pass 1 已验证。
                    long long tgt = (long long)pc + 1 + off;
                    size_t fall = pc + 1;

                    std::string c = emitPop();
                    std::string tv = freshTmp();
                    emitLine(tv + " = call i32 @zeus_value_truthy(ptr " + c + ")");
                    emitRelease(c);
                    std::string b = freshTmp();
                    emitLine(b + " = icmp ne i32 " + tv + ", 0");

                    std::string tgtLabel = (tgt == (long long)chunk.size())
                        ? endLabelFor(ci)
                        : labelFor(ci, (size_t)tgt);
                    std::string fallLabel = (fall == chunk.size())
                        ? endLabelFor(ci)
                        : labelFor(ci, fall);

                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        emitLine("br i1 " + b + ", label " + fallLabel + ", label " + tgtLabel);
                    } else {
                        emitLine("br i1 " + b + ", label " + tgtLabel + ", label " + fallLabel);
                    }

                    blkTerminated = true;
                    blkNeedLabel = true;
                    break;
                }

                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO: {
                    const int32_t checkKind =
                        ins.op == VMOperator::COUNT_CHECK_NEG
                            ? ZEUS_RANGE_CHECK_NEGATIVE_END
                            : ZEUS_RANGE_CHECK_ZERO_STEP;
                    const int32_t sourceLine = ins.line > 0 ? ins.line : 0;
                    const std::string checked = freshTmp();
                    emitLine(checked + " = alloca %ZeusValue, align 8");
                    emitLine("call void @zeus_value_pop(ptr " + checked + ")");
                    emitLine("call void @zeus_range_check_value(ptr " + checked +
                             ", i32 " + std::to_string(checkKind) +
                             ", i32 " + std::to_string(sourceLine) + ")");
                    emitLine("call void @zeus_value_push(ptr " + checked + ")");
                    break;
                }

                case VMOperator::LOAD_NATIVE: {
                    int64_t nid = 0;

                    if (!tryArgInt(ins, 0, nid)) {
                        fail(ins, "[emit-llvm] LOAD_NATIVE id 不是整数");
                        return false;
                    }

                    const auto* d = ZeusBuiltins::get((int)nid);

                    if (!d) {
                        fail(ins, "[emit-llvm] LOAD_NATIVE 非法 id");
                        return false;
                    }

                    if (!dynamicVariadicABISymbol(d->name).empty() ||
                        findNativeBuiltinDeclaration(d->name) != nullptr) {
                        pendingVariadicCallStack.push_back(d->name);
                        break;
                    }

                    fail(ins, "[emit-llvm] v0: 原生函数 '" + d->name + "' 暂不支持");
                    return false;
                }

                case VMOperator::CALL_NATIVE: {
                    int64_t argc = 0;

                    if (!tryArgInt(ins, 0, argc)) {
                        fail(ins, "[emit-llvm] CALL_NATIVE 参数不是整数");
                        return false;
                    }

                    if (pendingVariadicCallStack.empty()) {
                        fail(ins, "[emit-llvm] v0: 原生调用需要动态变长 ABI");
                        return false;
                    }

                    if (argc < 0) {
                        fail(ins, "[emit-llvm] v0: 动态变长调用参数数量无效");
                        return false;
                    }

                    {
                        const std::string nativeName = pendingVariadicCallStack.back();
                        pendingVariadicCallStack.pop_back();
                        if (const auto* nativeDeclaration =
                                findNativeBuiltinDeclaration(nativeName)) {
                            pendingDirectCall = nativeDeclaration->name;
                            pendingFunctionLoad.clear();
                            if (!emitDirectCallFromDynamic(ins, argc)) return false;
                        } else {
                            emitDynamicVariadicIntrinsic(argc, nativeName);
                        }
                    }
                    break;
                }

                case VMOperator::CALL_CLOSURE: {
                    int64_t argc = 0;

                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            fail(ins, "[emit-llvm] CallTarget 元信息缺少函数名");
                            return false;
                        }
                    }

                    if (!tryArgInt(ins, 0, argc)) {
                        fail(ins, "[emit-llvm] CALL_CLOSURE 参数不是整数");
                        return false;
                    }

                    // Preserve the enclosing variadic call state for nested calls.
                    // The native result is wrapped as ZeusValue and pushed to the dynamic stack.
                    if (!pendingDirectCall.empty()) {
                        if (!emitDirectCallFromDynamic(ins, argc)) return false;
                        break;
                    }

                    if (pendingVariadicCallStack.empty()) {
                        fail(ins, "[emit-llvm] v0: 闭包调用需要动态变长 ABI");
                        return false;
                    }

                    if (argc < 0) {
                        fail(ins, "[emit-llvm] v0: 动态变长调用参数数量无效");
                        return false;
                    }

                    {
                        const std::string nativeName = pendingVariadicCallStack.back();
                        pendingVariadicCallStack.pop_back();
                        emitDynamicVariadicIntrinsic(argc, nativeName);
                    }
                    break;
                }

                case VMOperator::OBJ_CALL_METHOD: {
                    if (ins.args.size() < 2 ||
                        !std::holds_alternative<std::string>(ins.args[0])) {
                        fail(ins, "[emit-llvm] 动态成员调用缺少方法名");
                        return false;
                    }
                    int64_t argc = 0;
                    if (!tryArgInt(ins, 1, argc) || argc < 0 ||
                        argc > std::numeric_limits<int32_t>::max()) {
                        fail(ins, "[emit-llvm] 动态成员调用参数数量无效");
                        return false;
                    }

                    DynamicMemberBridge::RuntimeStackCallRules rules;
                    rules.popValue = [this]() { return emitPop(); };
                    rules.emitLine = [this](const std::string& line) { emitLine(line); };
                    rules.freshTemporary = [this]() { return freshTmp(); };
                    rules.poolString = [this](const std::string& text) {
                        return poolStr(text);
                    };
                    rules.releaseValue = [this](const std::string& value) {
                        emitRelease(value);
                    };
                    rules.pushValue = [this](const std::string& value) {
                        emitPushValue(value);
                    };

                    std::string error;
                    if (!DynamicMemberBridge::emitRuntimeStackCall(
                            std::get<std::string>(ins.args[0]),
                            static_cast<int32_t>(argc),
                            rules,
                            error)) {
                        fail(ins, "[emit-llvm] " + error);
                        return false;
                    }
                    break;
                }

                default: {
                    fail(ins, "[emit-llvm] v0: 不支持的操作 " + VMO_toString(ins.op));
                    return false;
                }
            }
        }

        activeEntryExceptionTarget = -1;
        if (exitJump) {
            // 出口块：跳到 size / fall through size 的边全部落在这里。
            emitLabelDef(endLabelFor(ci));

            if (!isLast) {
                // 非末 chunk：语义上执行流进入下一 chunk（与解释器按序执行一致）。
                emitLine("br label " + labelFor(ci + 1, 0));
                blkTerminated = true;
                blkNeedLabel = true; // 下一 chunk 首条强制起块
            }
            // 最后一个 chunk：epilogue 的 ret/unreachable 直接成为本块终结。
        }

        return result.error.empty();
    }
}; // class Emitter

} // namespace detail

inline EmitResult emit(const ZeusCompile::Result& r) {
    EmitResult res;
    detail::Emitter e;
    e.programChunks = &r.program;
    // 失败判定统一为 error 字符串（fail() 负责填充）。
    if (r.program.empty()) {
        res.ok = false;
        res.error = "[emit-llvm] 空程序";
        return res;
    }

    if (!e.collectMetadata(r)) {
        res.ok = false;
        res.error = e.result.error;
        res.errorLine = e.result.errorLine;
        return res;
    }

    // Both passes need to know what each global holds before any code is
    // emitted, so the scan runs on its own here.
    e.prescanGlobalTypes();
    e.collectReachableFunctionChunks(r.program);

    for (size_t ci = 0; ci < r.program.size(); ++ci) {
        if (!r.program[ci].empty() &&
            r.program[ci].front().metadata.kind == IRType::IRMetadataKind::ModuleImportInit) {
            if (!e.emitChunk({}, ci, ci + 1 == r.program.size())) {
                res.ok = false;
                res.error = e.result.error;
                res.errorLine = e.result.errorLine;
                res.warnings = e.result.warnings;
                return res;
            }
            continue;
        }
        if (!e.emitChunk(r.program[ci], ci, ci + 1 == r.program.size())) {
            res.ok = false;
            res.error = e.result.error;
            res.errorLine = e.result.errorLine;
            res.warnings = e.result.warnings;
            return res;
        }
    }

    if (!e.pendingVariadicCall.empty() || !e.pendingVariadicCallStack.empty()) {
        res.ok = false;
        res.error = "[emit-llvm] 悬空的动态变长函数加载（LOAD 后无 CALL）";
        res.warnings = e.result.warnings;
        return res;
    }

    if (!e.pendingDirectCall.empty() || !e.pendingFunctionLoad.empty()) {
        res.ok = false;
        res.error = "[emit-llvm] 悬空的函数直接调用标注";
        res.warnings = e.result.warnings;
        return res;
    }

    if (!e.buildDynamicMemberBridge()) {
        res.ok = false;
        res.error = e.result.error;
        res.errorLine = e.result.errorLine;
        res.warnings = e.result.warnings;
        return res;
    }

    size_t runtimeErrorMessageField = 0;
    size_t runtimeErrorTypeField = 0;
    if (e.needsRuntimeErrorFactory) {
        const auto layout = e.classLayouts.find("RuntimeError");
        if (layout == e.classLayouts.end() || layout->second.fieldNames.size() != 2 ||
            layout->second.fieldTypes.size() != 2) {
            res.ok = false;
            res.error = "[emit-llvm] RuntimeError class must have _message and _type string fields";
            return res;
        }
        const auto& names = layout->second.fieldNames;
        const auto message = std::find(names.begin(), names.end(), "_message");
        const auto type = std::find(names.begin(), names.end(), "_type");
        if (message == names.end() || type == names.end() ||
            message == type ||
            e.llvmFieldType(layout->second.fieldTypes[message - names.begin()]) != "ptr" ||
            e.llvmFieldType(layout->second.fieldTypes[type - names.begin()]) != "ptr") {
            res.ok = false;
            res.error = "[emit-llvm] RuntimeError fields have incompatible LLVM types";
            return res;
        }
        runtimeErrorMessageField = static_cast<size_t>(message - names.begin());
        runtimeErrorTypeField = static_cast<size_t>(type - names.begin());
    }

    // 组装：声明+ 字符串池 + 入口。
    std::string out;
    out += "; Generated by Zeus --emit-llvm (v0 naive stack machine)\n";
    out += "; 入口 zeus_entry 由所有 top-level chunk 按序拼接，main 链接 libzeus_rt.lib。\n\n";
    out += "%ZeusValue = type { i32, i32, i64 }\n";
    out += "declare void @zeus_value_init_nil(ptr)\n";
    out += "declare void @zeus_value_init_i64(ptr, i64)\n";
    out += "declare void @zeus_value_init_bool(ptr, i32)\n";
    out += "declare void @zeus_value_from_i64_ptr(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bool_ptr(ptr, ptr)\n";
    out += "declare void @zeus_value_clone(ptr, ptr)\n";
    out += "declare void @zeus_value_from_class_ptr(ptr, ptr, ptr)\n";
    out += "declare ptr @zeus_value_require_class_ptr(ptr, ptr)\n";
    out += "declare void @zeus_class_pointer_invalidate(ptr)\n";
    out += "declare void @zeus_class_ref_register(ptr, ptr)\n";
    out += "declare void @zeus_class_ref_retain(ptr)\n";
    out += "declare void @zeus_class_ref_release(ptr)\n";
    out += "declare i32 @zeus_class_ref_destroy_now(ptr)\n";
    out += "declare void @zeus_class_ref_require_live(ptr)\n";
    out += "declare void @zeus_value_from_string_data(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_from_string_view(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bytes_view(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bytes(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_from_bigint_decimal(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bigdecimal_decimal(ptr, ptr)\n";
    out += "declare i64 @zeus_value_require_i64(ptr)\n";
    out += "declare i32 @zeus_value_require_bool(ptr)\n";
    out += "declare ptr @zeus_value_intern_string_view(ptr)\n";
    out += "declare ptr @zeus_value_intern_bytes_view(ptr)\n";
    out += "declare ptr @zeus_string_view_intern(ptr)\n";
    out += "declare ptr @zeus_string_view_from_data(ptr, i64)\n";
    out += "declare ptr @zeus_bytes_view_intern(ptr)\n";
    out += "declare ptr @zeus_bytes_view_from_data(ptr, i64)\n";
    out += "declare ptr @zeus_value_require_i64_ptr(ptr)\n";
    out += "declare ptr @zeus_value_require_bool_ptr(ptr)\n";
    out += "declare void @zeus_value_release(ptr)\n";
    out += "declare void @zeus_range_check_i64(i64, i32, i32)\n";
    out += "declare void @zeus_range_check_value(ptr, i32, i32)\n";
    out += "declare void @zeus_value_push(ptr)\n";
    out += "declare void @zeus_value_pop(ptr)\n";
    out += "declare void @zeus_value_print(ptr)\n";
    out += "declare void @zeus_value_print_call(ptr, i32, ptr)\n";
    out += "declare void @zeus_value_native_call(ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_dynamic_member_register(ptr, ptr, ptr)\n";
    out += "declare i32 @zeus_value_try_dynamic_member_call(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_value_dynamic_member_call(ptr, ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_dynamic_member_arity_error(ptr, i32, i32)\n";
    out += "declare void @zeus_value_global_store(i32, ptr)\n";
    out += "declare void @zeus_value_global_load(ptr, i32)\n";
    out += "declare void @zeus_global_clear(i32)\n";
    out += "declare void @zeus_value_global_delete(i32)\n";
    out += "declare void @zeus_value_local_delete(i32)\n";
    out += "declare void @zeus_value_delete_object(ptr)\n";
    out += "declare void @zeus_shared_value_store(ptr, ptr)\n";
    out += "declare void @zeus_shared_value_assign(ptr, ptr)\n";
    out += "declare void @zeus_shared_value_load(ptr, ptr)\n";
    out += "declare i32 @zeus_shared_value_delete(ptr)\n";
    out += "declare i64 @zeus_global_load_i64(i32)\n";
    out += "declare i32 @zeus_global_load_bool(i32)\n";
    out += "declare void @zeus_global_store_i64(i32, i64)\n";
    out += "declare void @zeus_global_store_bool(i32, i32)\n";
    out += "declare void @zeus_value_local_store(i32, ptr)\n";
    out += "declare void @zeus_value_local_load(ptr, i32)\n";
    out += "declare ptr @zeus_capture_cell_create(ptr)\n";
    out += "declare ptr @zeus_capture_cell_clone(ptr)\n";
    out += "declare ptr @zeus_capture_cell_retain(ptr)\n";
    out += "declare void @zeus_capture_cell_release(ptr)\n";
    out += "declare void @zeus_capture_cell_load(ptr, ptr)\n";
    out += "declare void @zeus_capture_cell_store(ptr, ptr)\n";
    out += "declare void @zeus_capture_cell_delete(ptr)\n";
    out += "declare ptr @zeus_closure_capture_ref(ptr, i32)\n";
    out += "declare ptr @zeus_closure_capture_copy(ptr, i32)\n";
    out += "declare void @zeus_closure_env_delete(ptr, i32)\n";
    out += "declare void @zeus_closure_create(ptr, ptr, i32, i32, ptr)\n";
    out += "declare void @zeus_closure_create_range(ptr, ptr, i32, i32, i32, ptr)\n";
    out += "declare void @zeus_closure_call(ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_closure_call_named(ptr, ptr, i32, ptr, ptr)\n";
    out += "declare void @zeus_closure_bind_arguments(i32, ptr, i64, i32, i32, i32, ptr, ptr, ptr, ptr, ptr, ptr)\n";
    out += "declare void @zeus_closure_env_load(ptr, ptr, i32)\n";
    out += "declare void @zeus_closure_env_store(ptr, i32, ptr)\n";
    out += "declare void @zeus_gc_collect()\n";
    out += "declare i64 @zeus_gc_live_node_count()\n";
    out += "declare void @zeus_value_int_add(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_less_than(ptr, ptr, ptr)\n";
    out += "declare i32 @zeus_value_truthy(ptr)\n";
    out += "declare ptr @malloc(i64)\n";
    out += "declare void @free(ptr)\n";
    out += "declare void @zeus_class_field_require_initialized(i32, ptr, ptr, i32)\n";
    out += "declare void @zeus_exception_raise(ptr)\n";
    out += "declare void @zeus_exception_raise_owned(ptr)\n";
    out += "declare void @zeus_exception_capture(ptr, ptr)\n";
    out += "declare void @zeus_i64_check_division(i64, i64, i32)\n";
    out += "declare i32 @__CxxFrameHandler3(...)\n";
    out += "declare void @llvm.trap()\n\n";
    DynamicABI::appendLLVMDeclarations(out);
    DynamicBinaryABI::appendLLVMDeclarations(out);
    out += "\n";

    for (const auto& typeName : e.structTypes) {
        auto layout = e.classLayouts.find(typeName);
        if (layout == e.classLayouts.end()) {
            out += e.llvmStructType(typeName) + " = type { i8 }\n";
            continue;
        }

        out += e.llvmStructType(typeName) + " = type {";
        for (size_t i = 0; i < layout->second.fieldTypes.size(); ++i) {
            if (i > 0) out += ",";
            out += " " + e.llvmFieldType(layout->second.fieldTypes[i]);
        }
        out += " }\n";
    }

    if (!e.structTypes.empty()) out += "\n";

    for (const auto& kv : e.strPool) {
        const std::string& s = kv.first;
        const std::string& sym = kv.second;
        out += sym + " = private unnamed_addr constant [" + std::to_string(s.size() + 1) +
               " x i8] c\"" + detail::escapeLLString(s) + "\\00\", align 1\n";
    }

    if (!e.strPool.empty()) out += "\n";
    out += e.dynamicMemberBridge.constants;
    if (!e.dynamicMemberBridge.constants.empty()) out += "\n";

    out += e.externalDeclarations;
    if (e.needsRuntimeErrorFactory) {
        out += "@.zeus_runtime_error_type = private unnamed_addr constant [13 x i8] c\"RuntimeError\\00\", align 1\n\n";
        out += "define void @__zeus_make_runtime_error(ptr %out, ptr %message, i64 %length) {\n";
        out += "entry:\n";
        out += "  %size.ptr = getelementptr %zeus_RuntimeError, ptr null, i32 1\n";
        out += "  %size = ptrtoint ptr %size.ptr to i64\n";
        out += "  %object = call ptr @malloc(i64 %size)\n";
        out += "  %failed = icmp eq ptr %object, null\n";
        out += "  br i1 %failed, label %out.of.memory, label %ready\n";
        out += "out.of.memory:\n";
        out += "  call void @llvm.trap()\n";
        out += "  unreachable\n";
        out += "ready:\n";
        out += "  %message.field = getelementptr inbounds %zeus_RuntimeError, ptr %object, i32 0, i32 " +
            std::to_string(runtimeErrorMessageField) + "\n";
        out += "  %message.view = call ptr @zeus_string_view_from_data(ptr %message, i64 %length)\n";
        out += "  store ptr %message.view, ptr %message.field\n";
        out += "  %type.field = getelementptr inbounds %zeus_RuntimeError, ptr %object, i32 0, i32 " +
            std::to_string(runtimeErrorTypeField) + "\n";
        out += "  %type.view = call ptr @zeus_string_view_from_data(ptr @.zeus_runtime_error_type, i64 12)\n";
        out += "  store ptr %type.view, ptr %type.field\n";
        out += "  call void @zeus_value_from_class_ptr(ptr %out, ptr %object, ptr @.zeus_runtime_error_type)\n";
        out += "  ret void\n";
        out += "}\n\n";
    }
    out += e.emitClassLifetimeFinalizers();
    out += e.nativeFunctions;
    out += e.dynamicMemberBridge.adapters;

    out += "define i32 @zeus_entry() personality ptr @__CxxFrameHandler3 {\nentry:\n";
    // The top-level statements all end up in this one function, so their
    // allocas are hoisted the same way a native function hoists its own: at
    // -O0 an alloca inside a loop grows the frame on every iteration.
    std::string hoistedAllocas;
    const std::string hoistedBody = detail::Emitter::takeAllocas(e.body, 0, hoistedAllocas);
    out += e.takeEntryAllocas();
    out += hoistedAllocas;
    if (!e.dynamicMemberBridge.empty()) {
        out += "  call void @__zeus_register_dynamic_members()\n";
    }
    out += "  br label %L0_0\n";
    out += hoistedBody;

    // 收尾：上一块若已终结（末尾是无条件跳转，如死循环），补不可达块。
    // 判定：取 body 最后一行，看是否为 "br label ..."。
    bool endsWithBr = false;

    {
        // 取 body 最后一行（body 以换行结尾，out 尾部还有 "}\n" 未加——）。
        // 注意此处检查的是 e.body，不是 out）。
        std::string b = e.body;

        while (!b.empty() && (b.back() == '\n' || b.back() == ' ')) {
            b.pop_back();
        }
        // 取最后一行。
        size_t nl = b.rfind('\n');
        std::string last = (nl == std::string::npos) ? b : b.substr(nl + 1);

        // 去前导空白。
        size_t p = last.find_first_not_of(" \t");

        if (p != std::string::npos) last = last.substr(p);

        endsWithBr = (last.rfind("br label ", 0) == 0);
    }

    if (endsWithBr) {
        out += "Lend_unreachable:\n  unreachable\n";
    } else {
        for (const auto& entry : e.nativeGlobalSlots) {
            if (entry.second.type != "ptr" ||
                !e.isClassType(entry.second.sourceType)) {
                continue;
            }
            const std::string value = e.freshTmp();
            out += "  " + value + " = load ptr, ptr " + entry.second.address + "\n";
            out += "  call void @zeus_class_ref_release(ptr " + value + ")\n";
        }
        for (const auto& entry : e.nativeLocalSlots) {
            if (entry.second.type != "ptr" ||
                !e.isClassType(entry.second.sourceType)) {
                continue;
            }
            const std::string value = e.freshTmp();
            out += "  " + value + " = load ptr, ptr " + entry.second.address + "\n";
            out += "  call void @zeus_class_ref_release(ptr " + value + ")\n";
        }
        auto appendTopLevelValueRelease = [&](const detail::Emitter::NativeValue& value) {
            if (value.dynamic && value.ownsDynamic) {
                out += "  call void @zeus_value_release(ptr " + value.value + ")\n";
            } else if (value.ownsClass && e.isManagedClassValue(value)) {
                out += "  call void @zeus_class_ref_release(ptr " + value.value + ")\n";
            }
        };
        for (const auto& entry : e.nativeGlobals) {
            appendTopLevelValueRelease(entry.second);
        }
        for (const auto& entry : e.nativeLocals) {
            appendTopLevelValueRelease(entry.second);
        }
        for (int index : e.entryDynamicValueGlobals) {
            out += "  call void @zeus_global_clear(i32 " +
                std::to_string(index) + ")\n";
        }
        out += "  ret i32 0\n";
    }

    for (size_t i = 0; i < e.entryExceptionPads.size(); ++i) {
        const auto& pad = e.entryExceptionPads[i];
        if (pad.cleanupOnly) {
            const std::string cleanupToken = "%entry_eh_cleanup_token_" +
                std::to_string(i);
            out += pad.switchLabel.substr(1) + ":\n";
            out += "  " + cleanupToken + " = cleanuppad within none []\n";
            for (const auto& cleanupCall : pad.cleanupCalls) {
                out += "  " + cleanupCall;
                if (cleanupCall.rfind("call ", 0) == 0) {
                    out += " [ \"funclet\"(token " + cleanupToken + ") ]";
                }
                out += "\n";
            }
            out += "  cleanupret from " + cleanupToken +
                " unwind to caller\n";
            continue;
        }
        const std::string index = std::to_string(i);
        const std::string switchToken = "%entry_eh_switch_token_" + index;
        const std::string catchToken = "%entry_eh_catch_token_" + index;
        out += pad.switchLabel.substr(1) + ":\n";
        out += "  " + switchToken + " = catchswitch within none [label " +
            pad.padLabel + "] unwind to caller\n";
        out += pad.padLabel.substr(1) + ":\n";
        out += "  " + catchToken + " = catchpad within " + switchToken +
            " [ptr null, i32 64, ptr null]\n";
        out += "  call void @zeus_exception_capture(ptr " + pad.valueSlot +
            ", ptr @__zeus_make_runtime_error) [ \"funclet\"(token " + catchToken + ") ]\n";
        out += "  catchret from " + catchToken + " to label " + pad.resumeLabel + "\n";
        out += pad.resumeLabel.substr(1) + ":\n";
        out += "  br label " + detail::Emitter::labelFor(pad.catchChunk, pad.catchPc) + "\n";
    }

    out += "}\n";

    res.ok = true;
    res.text = out;
    res.warnings = e.result.warnings;
    return res;
}

} // namespace ZeusLLVM
