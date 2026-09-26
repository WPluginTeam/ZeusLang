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
    std::string ownerType;
    std::string operatorSymbol;
    std::string nativeSymbol;
    int captureCount = 0;
    size_t chunkIndex = 0;
    bool isExternal = false;
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
    }
    std::set<std::string> structTypes;
    std::map<std::string, ClassLayout> classLayouts;
    std::string nativeFunctions;
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

    std::string llvmStructType(const std::string& typeName) const {
        std::string name = "%zeus_";
        for (unsigned char c : typeName) {
            name += std::isalnum(c) || c == '_' ? static_cast<char>(c) : '_';
        }
        return name;
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

        collectReachableFunctionChunks(program.program);

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

    static std::string methodNameForSignature(const FunctionSignature& signature) {
        if (signature.ownerType.empty()) return {};
        const std::string prefix = signature.ownerType + "::";
        if (signature.name.rfind(prefix, 0) != 0) return {};
        return signature.name.substr(prefix.size());
    }

    void collectReachableFunctionChunks(
        const std::vector<std::vector<IRType::IRInstr>>& program
    ) {
        dynamicMemberMethodNames.clear();
        dynamicMemberMethodNames.insert("toString");
        std::vector<size_t> pending;
        for (const auto& entry : functionSignaturesByChunk) {
            if (!isBuiltinImplementationFunction(entry.second)) {
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
            const auto function = functionSignatures.find(name);
            if (function == functionSignatures.end() || function->second.isExternal ||
                !isBuiltinImplementationFunction(function->second)) {
                return;
            }
            const size_t targetChunk = function->second.chunkIndex;
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

            bool constructsClass = false;
            bool callsDelete = false;
            bool usesSubscriptLoad = false;
            bool usesSubscriptStore = false;
            std::vector<std::string> methodCalls;
            for (const auto& ins : program[ci]) {
                std::string targetName;
                if (ins.metadata.readDirectCall(targetName)) {
                    addTarget(targetName);
                } else if (annotationKind(ins, "direct_call") &&
                           annotationString(ins, 1, targetName)) {
                    addTarget(targetName);
                }

                if (ins.op == VMOperator::OBJ_BUILD) constructsClass = true;
                if (ins.op == VMOperator::DELETE_VAR) callsDelete = true;
                if (ins.op == VMOperator::DICT_LOAD_VALUE) usesSubscriptLoad = true;
                if (ins.op == VMOperator::DICT_STORE_VALUE) usesSubscriptStore = true;
                if (ins.op == VMOperator::OBJ_CALL_METHOD && !ins.args.empty() &&
                    std::holds_alternative<std::string>(ins.args.front())) {
                    const std::string method = std::get<std::string>(ins.args.front());
                    methodCalls.push_back(method);
                    dynamicMemberMethodNames.insert(method);
                }
            }

            for (const auto& method : methodCalls) {
                for (const auto& function : functionSignatures) {
                    const auto& candidate = function.second;
                    if (!isBuiltinImplementationFunction(candidate) ||
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

            if (constructsClass || callsDelete) {
                for (const auto& function : functionSignatures) {
                    const auto& candidate = function.second;
                    if (isBuiltinImplementationFunction(candidate) &&
                        (candidate.name == candidate.ownerType + "::init" ||
                         (callsDelete && candidate.operatorSymbol == "~"))) {
                        addTarget(candidate.name);
                    }
                }
            }

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
    }

    bool buildDynamicMemberBridge() {
        std::vector<DynamicMemberBridge::MethodBinding> bindings;
        std::set<std::pair<std::string, std::string>> registeredMethods;

        for (const auto& entry : functionSignatures) {
            const FunctionSignature& signature = entry.second;
            const std::string methodName = methodNameForSignature(signature);
            if (methodName.empty() ||
                dynamicMemberMethodNames.find(methodName) == dynamicMemberMethodNames.end() ||
                signature.operatorSymbol.size() > 0 ||
                signature.parameterTypes.empty() ||
                signature.parameterTypes.front() != signature.ownerType ||
                !isClassType(signature.ownerType)) {
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
        body += "  " + s + "\n";
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
    }

    // PUSH/LOAD-字面量 的值部分（不含 push）：返回 box 的 SSA 名，失败返回 ""。
    std::string emitDynamicValueForLiteral(const LexerType::Value& v, const IRType::IRInstr& ins) {
        if (std::holds_alternative<std::string>(v.data)) {
            const std::string& s = std::get<std::string>(v.data);

            if (s == "__NIL__") {
                std::string t = freshTmp();
                emitLine(t + " = alloca %ZeusValue, align 8");
                emitLine("call void @zeus_value_init_nil(ptr " + t + ")");
                return t;
            }

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
            emitLine("call void @zeus_value_release(ptr " + boxed + ")");
            out = NativeValue{ unboxed, "ptr", false, false, classType->second };
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

        if (argc < 0 || called.parameterTypes.size() != static_cast<size_t>(argc)) {
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

        for (size_t i = 0; i < dynamicArguments.size(); ++i) {
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
                           abi != nullptr &&
                           abi->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                    const std::string pointer = freshTmp();
                    emitLine(pointer + " = call ptr @zeus_value_intern_string_view(ptr " +
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
                       abi != nullptr &&
                       abi->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                const std::string nativeString = freshTmp();
                emitLine(nativeString + " = call ptr @zeus_value_intern_string_view(ptr " +
                         dynamicArguments[i] + ")");
                nativeArguments.push_back(nativeString);
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
        const bool returnsStringView = (returnABI != nullptr &&
            returnABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) ||
            (referenceTargetABI != nullptr &&
             referenceTargetABI->valueABI == ZeusBuiltins::NativeValueABI::StringView);
        const bool returnsReferenceClassPointer = isReferenceTypeName(called.returnType) &&
            isClassType(referenceTargetType);
        const bool returnsScalarReference = referenceTargetType == "int" ||
                                            referenceTargetType == "int32" ||
                                            referenceTargetType == "int64" ||
                                            referenceTargetType == "bool";
        if (returnType.empty() || (!returnsDynamic && returnType != "i64" &&
                                   returnType != "i1" && !returnsClassPointer &&
                                   !returnsReferenceClassPointer &&
                                   !(returnType == "ptr" && returnsStringView) &&
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
        } else if (returnsStringView) {
            const std::string dynamicResult = freshTmp();
            emitLine(dynamicResult + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_string_view(ptr " + dynamicResult +
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
                       abi != nullptr &&
                       abi->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                emitLine("call void @zeus_value_from_string_view(ptr " + dynamicValue +
                         ", ptr " + value.value + ")");
            } else {
                fail(ins, "[emit-llvm] 该引用类型不能装箱为 ZeusValue: &" + targetType);
                return false;
            }
            return true;
        }

        const auto* sourceABI = ZeusBuiltins::findNativeTypeABI(value.sourceType);
        if (value.type == "ptr" && sourceABI != nullptr &&
            sourceABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
            dynamicValue = freshTmp();
            emitLine(dynamicValue + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_value_from_string_view(ptr " + dynamicValue +
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
            if (referenceABI != nullptr &&
                referenceABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                nativeValue = freshTmp();
                emitLine(nativeValue + " = call ptr @zeus_value_intern_string_view(ptr " +
                         value.value + ")");
                return true;
            }
        }

        const auto* declaredABI = ZeusBuiltins::findNativeTypeABI(declaredType);
        if (expectedType == "ptr" && declaredABI != nullptr &&
            declaredABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
            nativeValue = freshTmp();
            emitLine(nativeValue + " = call ptr @zeus_value_intern_string_view(ptr " +
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
             constructor->parameterTypes.size() != arguments.size() + 1 ||
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
            const bool isStringViewField = fieldABI != nullptr &&
                fieldABI->valueABI == ZeusBuiltins::NativeValueABI::StringView;
            if (fieldType.empty() || (!isDynamicField &&
                (fieldTypeName.empty() ||
                 (fieldTypeName != "int" && fieldTypeName != "int32" &&
                 fieldTypeName != "int64" && fieldTypeName != "bool" &&
                 !isStringViewField && fieldTypeName != "nil" &&
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
                } else if (fieldDefault.kind == IRType::ClassFieldDefaultKind::String &&
                           isStringViewField) {
                    const std::string symbol = poolStr(fieldDefault.stringValue);
                    const std::string stringPointer = freshTemporary();
                    emit(stringPointer + " = getelementptr inbounds [" +
                         std::to_string(fieldDefault.stringValue.size() + 1) +
                         " x i8], ptr " + symbol + ", i64 0, i64 0");
                    initializer = freshTemporary();
                    emit(initializer + " = call ptr @zeus_string_view_from_data(ptr " +
                         stringPointer + ", i64 " +
                         std::to_string(fieldDefault.stringValue.size()) + ")");
                } else {
                    return failAt(ins, "[emit-llvm] pointer 字段默认值类型不匹配");
                }
            } else {
                return failAt(ins, "[emit-llvm] class 字段 LLVM 类型暂不支持");
            }

            emit("store " + fieldType + " " + initializer + ", ptr " + fieldAddress);
        }

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
            for (size_t i = 0; i < arguments.size(); ++i) {
                std::string argument;
                bool ownsDynamic = false;
                const std::string& declaredType = constructor->parameterTypes[i + 1];
                if (!convertArgument(arguments[i], declaredType, argument, ownsDynamic)) {
                    return false;
                }
                callText += ", " + functionParameterABIType(declaredType) + " " + argument;
                if (usesZeusValueABI(declaredType)) {
                    dynamicArguments.push_back(argument);
                    ownsDynamicArguments.push_back(ownsDynamic);
                }
            }
            callText += ")";
            emit(callText);
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) {
                    emit("call void @zeus_value_release(ptr " + dynamicArguments[i] + ")");
                }
            }
            if (constructorReturnsDynamic) {
                emit("call void @zeus_value_release(ptr " + constructorResult + ")");
            }
        }

        instance = NativeValue{ object, "ptr", false, false, className };
        return true;
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
                if (!usesZeusValueABI(layout->fieldTypes[fieldIndex])) continue;
                const std::string fieldAddress = freshTemporary();
                emit(fieldAddress + " = getelementptr inbounds " +
                     llvmStructType(object.sourceType) + ", ptr " + object.value +
                     ", i32 0, i32 " + std::to_string(fieldIndex));
                emit("call void @zeus_value_release(ptr " + fieldAddress + ")");
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
        Fail&& failAt
    ) {
        if (arguments.empty()) {
            return failAt(ins, "[emit-llvm] class method call is missing its receiver");
        }

        const NativeValue& receiver = arguments.front();
        if (receiver.functionRef || receiver.type != "ptr" ||
            !isClassType(receiver.sourceType)) {
            return failAt(
                ins,
                "[emit-llvm] class method call requires a statically known class receiver"
            );
        }

        const std::string qualifiedName = receiver.sourceType + "::" + methodName;
        const auto function = functionSignatures.find(qualifiedName);
        if (function == functionSignatures.end() ||
            function->second.ownerType != receiver.sourceType ||
            function->second.parameterTypes.size() != arguments.size() ||
            function->second.parameterTypes.empty() ||
            function->second.parameterTypes.front() != receiver.sourceType) {
            return failAt(
                ins,
                "[emit-llvm] no matching class method LLVM signature: " + qualifiedName
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

        std::vector<std::string> nativeArguments;
        std::vector<std::string> cleanupValues;
        nativeArguments.reserve(arguments.size());
        for (size_t i = 0; i < arguments.size(); ++i) {
            const std::string& parameterType = called.parameterTypes[i];
            if (functionParameterABIType(parameterType).empty()) {
                return failAt(ins, "[emit-llvm] class method parameter has no LLVM ABI mapping");
            }

            std::string nativeArgument;
            std::string cleanupValue;
            if (!convertArgument(
                    arguments[i], parameterType, nativeArgument, cleanupValue)) {
                return false;
            }
            nativeArguments.push_back(std::move(nativeArgument));
            if (!cleanupValue.empty()) cleanupValues.push_back(std::move(cleanupValue));
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
        callText += ")";
        emit(callText);

        for (const auto& cleanupValue : cleanupValues) {
            releaseDynamic(cleanupValue);
        }

        if (returnsDynamic) {
            resultValue = NativeValue{
                callResult, "ptr", false, true, called.returnType, true
            };
            return true;
        }

        const auto* returnABI = ZeusBuiltins::findNativeTypeABI(called.returnType);
        if (returnABI != nullptr &&
            returnABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
            const std::string stableString = freshTemporary();
            emit(stableString + " = call ptr @zeus_string_view_intern(ptr " +
                 callResult + ")");
            resultValue = NativeValue{
                stableString, returnType, false, false, called.returnType, false
            };
        } else {
            resultValue = NativeValue{
                callResult, returnType, false, false, called.returnType, false
            };
        }
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
        Fail&& failAt
    ) {
        if (arguments.empty()) {
            return failAt(ins, "dynamic member call is missing its receiver");
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

        for (size_t i = 0; i < dynamicValues.size(); ++i) {
            if (ownsDynamicValues[i]) releaseDynamic(dynamicValues[i]);
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
        functionText += ") {\nentry:\n";
        const size_t nativeEntryBody = functionText.size();

        int nativeTmpCount = 0;
        auto freshNativeTmp = [&]() {
            return std::string("%n") + std::to_string(nativeTmpCount++);
        };
        auto emitNativeLine = [&](const std::string& line) {
            functionText += "  " + line + "\n";
        };

        std::vector<NativeValue> valueStack;
        std::map<int, NativeValue> locals;
        std::map<int, std::string> closureLocalCells;
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
            if (std::holds_alternative<std::string>(literal.data) &&
                std::get<std::string>(literal.data) == "__NIL__") {
                const std::string nilValue = freshNativeTmp();
                emitNativeLine(nilValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_init_nil(ptr " + nilValue + ")");
                value = NativeValue{ nilValue, "ptr", false, true, "nil", true };
                return true;
            }

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
                           abi != nullptr &&
                           abi->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                    emitNativeLine("call void @zeus_value_from_string_view(ptr " +
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
            if (value.type == "ptr" && sourceABI != nullptr &&
                sourceABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                dynamicValue = freshNativeTmp();
                emitNativeLine(dynamicValue + " = alloca %ZeusValue, align 8");
                emitNativeLine("call void @zeus_value_from_string_view(ptr " + dynamicValue +
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
                    if (abi == nullptr ||
                        abi->valueABI != ZeusBuiltins::NativeValueABI::StringView) {
                        return failNative(ins,
                            "[emit-llvm] ZeusValue 不能直接转换为原生 class/struct 指针");
                    }
                    argument = freshNativeTmp();
                    emitNativeLine(argument +
                                   " = call ptr @zeus_value_intern_string_view(ptr " +
                                   value.value + ")");
                } else {
                    return failNative(ins,
                        "[emit-llvm] ZeusValue 不能直接转换为原生 class/struct 指针");
                }
                if (value.ownsDynamic) {
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
            }

            std::string argumentArray = "null";
            if (!dynamicArguments.empty()) {
                argumentArray = freshNativeTmp();
                emitNativeLine(argumentArray + " = alloca ptr, i64 " +
                               std::to_string(dynamicArguments.size()) + ", align 8");
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    const std::string slot = freshNativeTmp();
                    emitNativeLine(slot + " = getelementptr inbounds ptr, ptr " +
                                   argumentArray + ", i64 " + std::to_string(i));
                    emitNativeLine("store ptr " + dynamicArguments[i] + ", ptr " + slot +
                                   ", align 8");
                }
            }

            const std::string output = freshNativeTmp();
            emitNativeLine(output + " = alloca %ZeusValue, align 8");
            emitNativeLine("call void @zeus_closure_call(ptr " + output + ", ptr " +
                           callee.value + ", i32 " +
                           std::to_string(dynamicArguments.size()) + ", ptr " +
                           argumentArray + ")");
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   dynamicArguments[i] + ")");
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

            const bool unconditionalJump =
                ins.op == VMOperator::JUMP || ins.op == VMOperator::LOOP_BREAK;
            const bool conditionalJump =
                ins.op == VMOperator::JUMP_IF_FALSE ||
                ins.op == VMOperator::JUMP_IF_TRUE;
            if (unconditionalJump || conditionalJump) {
                int64_t offset = 0;
                if (!tryArgInt(ins, 0, offset)) {
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
                chunk[branch.first].op == VMOperator::JUMP_IF_TRUE) {
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
                return failNative(ins,
                    "LLVM 函数分支边要求操作数栈为空");
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
                    return value.type == prototype->type &&
                           value.dynamic == prototype->dynamic &&
                           value.sourceType == prototype->sourceType &&
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
                }
            }
            callText += ")";
            emitNativeLine(callText);
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) {
                    emitNativeLine("call void @zeus_value_release(ptr " +
                                   dynamicArguments[i] + ")");
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
            pendingDirectCall.clear();
            return true;
        };

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            if (reachableNativePcs.find(pc) == reachableNativePcs.end()) continue;
            const auto& ins = chunk[pc];

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
                    const auto target = functionSignatures.find(directCallName);
                    pendingDirectCall = target != functionSignatures.end() &&
                        !hasCompleteFunctionAnnotations(target->second)
                            ? std::string()
                            : directCallName;
                }
                if (!recordNativeFallthrough(pc, ins)) return false;
                continue;
            }

            switch (ins.op) {
                case VMOperator::PUSH: {
                    NativeValue value;
                    if (!parseNativeValue(ins, value)) return false;
                    valueStack.push_back(std::move(value));
                    break;
                }

                case VMOperator::LOAD_VALUE: {
                    if (ins.args.size() == 1 && std::holds_alternative<std::string>(ins.args[0])) {
                        const std::string& name = std::get<std::string>(ins.args[0]);
                        if (!dynamicVariadicABISymbol(name).empty()) {
                            valueStack.push_back(NativeValue{ name, "builtin", true });
                            break;
                        }
                        if (functionSignatures.find(name) == functionSignatures.end()) {
                            return failNative(ins, "[emit-llvm] 原生函数无法解析名字 '" + name + "'");
                        }
                        valueStack.push_back(NativeValue{ name, "function", true });
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
                            valueStack.push_back(local->second);
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
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                boxed + ")");
                            valueStack.push_back(NativeValue{
                                unboxed, "ptr", false, false, globalClass->second
                            });
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

                    if (!isList && container.type == "ptr" &&
                        !container.dynamic && isClassType(container.sourceType)) {
                        const size_t parameterCount = isStore ? 3 : 2;
                        const FunctionSignature* operatorSignature =
                            findClassOperatorSignature(
                                container.sourceType, isStore ? "[]=" : "[]",
                                parameterCount);
                        if (operatorSignature == nullptr) {
                            return failNative(ins,
                                "[emit-llvm] class 没有匹配的下标operator");
                        }
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
                                })) {
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
                        symbol = "@zeus_value_dict_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else {
                        symbol = "@zeus_value_dict_store";
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
                    }
                    valueStack.push_back(NativeValue{
                        resultValue, "ptr", false, true, std::string(), true
                    });
                    break;
                }

                case VMOperator::OBJ_BUILD: {
                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc + 1)) {
                        return failNative(ins, "[emit-llvm] class 构造参数数量无效");
                    }

                    std::vector<NativeValue> arguments(static_cast<size_t>(argc));
                    for (int64_t i = argc - 1; i >= 0; --i) {
                        if (!popNative(ins, arguments[static_cast<size_t>(i)])) return false;
                    }

                    NativeValue classReference;
                    if (!popNative(ins, classReference) || classReference.type != "classref") {
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
                    if (!emitClassConstruction(
                            ins,
                            classReference.sourceType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            emitConstructionLabel,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction)) {
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
                    emitNativeLine("call void @zeus_closure_create(ptr " + closure +
                        ", ptr " + closureAdapterSymbol(static_cast<size_t>(childChunk)) +
                        ", i32 " + std::to_string(childSignature->second.parameterTypes.size()) +
                        ", i32 " + std::to_string(cells.size()) + ", ptr " + cellsArray + ")");
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
                        break;
                    }
                    if (value.functionRef) {
                        return failNative(ins, "[emit-llvm] 原生函数不能把函数值存入原生局部变量");
                    }
                    auto existing = locals.find(static_cast<int>(index));
                    if (existing != locals.end() && existing->second.dynamic &&
                        existing->second.ownsDynamic) {
                        emitNativeLine("call void @zeus_value_release(ptr " +
                                       existing->second.value + ")");
                    }
                    if (value.dynamic && !value.ownsDynamic) {
                        NativeValue owned;
                        if (!cloneNativeDynamicValue(ins, value, owned)) return false;
                        locals[static_cast<int>(index)] = std::move(owned);
                    } else {
                        locals[static_cast<int>(index)] = value;
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
                    emitNativeLine("call void @zeus_value_global_store(i32 " +
                                   std::to_string(index) + ", ptr " + dynamicValue + ")");
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 || !std::holds_alternative<std::string>(ins.args[1]) ||
                        std::get<std::string>(ins.args[1]) != "local") {
                        return failNative(ins, "[emit-llvm] class delete 目前要求局部变量");
                    }

                    int64_t index = 0;
                    if (!tryArgInt(ins, 2, index) || index < 0) {
                        return failNative(ins, "[emit-llvm] delete 局部索引无效");
                    }
                    auto objectIt = locals.find(static_cast<int>(index));
                    if (objectIt == locals.end() || objectIt->second.type != "ptr" ||
                        !isClassType(objectIt->second.sourceType)) {
                        return failNative(ins, "[emit-llvm] delete 目标不是 class 实例");
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

                    for (auto localIt = locals.begin(); localIt != locals.end();) {
                        if (localIt->second.type == "ptr" &&
                            localIt->second.sourceType == object.sourceType &&
                            localIt->second.value == object.value) {
                            localIt = locals.erase(localIt);
                        } else {
                            ++localIt;
                        }
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

                    const ClassLayout* layout = findClassLayout(object.sourceType);
                    if (layout == nullptr) {
                        return failNative(ins, "[emit-llvm] 原生函数字段读取的对象不是已知类布局");
                    }

                    const std::string& fieldName = std::get<std::string>(ins.args[0]);
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
                    if (usesZeusValueABI(fieldTypeName)) {
                        valueStack.push_back(NativeValue{
                            fieldAddress, "ptr", false, true, fieldTypeName, false
                        });
                        break;
                    }
                    const std::string temporary = freshNativeTmp();
                    emitNativeLine(
                        temporary + " = load " + fieldType + ", ptr " + fieldAddress
                    );
                    valueStack.push_back(NativeValue{
                        temporary,
                        fieldType,
                        false,
                        false,
                        fieldTypeName
                    });
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
                        break;
                    }

                    std::string storedValue = value.value;
                    const auto* fieldABI = ZeusBuiltins::findNativeTypeABI(fieldTypeName);
                    if (fieldABI != nullptr &&
                        fieldABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
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
                            emitNativeLine(storedValue +
                                           " = call ptr @zeus_string_view_intern(ptr " +
                                           value.value + ")");
                        }
                    } else if (fieldType.empty() || value.type != fieldType ||
                               (isClassType(fieldTypeName) &&
                                value.sourceType != fieldTypeName)) {
                        return failNative(ins, "[emit-llvm] 类字段赋值类型不匹配");
                    }

                    emitNativeLine(
                        "store " + fieldType + " " + storedValue + ", ptr " + fieldAddress
                    );
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
                    }

                    std::string resultValue;
                    if (const auto* nativeDeclaration =
                            findNativeBuiltinDeclaration(callee.value)) {
                        if (!usesDynamicZeusValueSignature(*nativeDeclaration) ||
                            nativeDeclaration->parameterTypes.size() != arguments.size()) {
                            return failNative(ins,
                                "[emit-llvm] @native 内置函数 '" + callee.value +
                                "' 需要匹配的动态 ZeusValue 签名");
                        }
                        resultValue = freshNativeTmp();
                        emitNativeLine(resultValue + " = alloca %ZeusValue, align 8");
                        std::string callText = "call void " +
                            llvmCallTarget(nativeDeclaration->name) +
                            "(ptr " + resultValue;
                        for (const auto& argument : dynamicArguments) {
                            callText += ", ptr " + argument;
                        }
                        callText += ")";
                        emitNativeLine(callText);
                    } else {
                        resultValue = emitDynamicVariadicCall(
                            dynamicVariadicABISymbol(callee.value),
                            callee.value,
                            dynamicArguments,
                            emitNativeLine,
                            freshNativeTmp
                        );
                    }
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

                case VMOperator::CALL_CLOSURE: {
                    if (ins.metadata.kind == IRType::IRMetadataKind::CallTarget) {
                        if (!ins.metadata.readDirectCall(pendingDirectCall)) {
                            return failNative(ins, "[emit-llvm] CallTarget 元信息缺少函数名");
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
                        }

                        const std::string resultValue = emitDynamicVariadicCall(
                            callSymbol,
                            callee.value,
                            dynamicArguments,
                            emitNativeLine,
                            freshNativeTmp
                        );
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
                    if (called.parameterTypes.size() != static_cast<size_t>(argc)) {
                        return failNative(ins, "[emit-llvm] 原生函数调用参数数量不匹配");
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
                    if (!callee.functionRef || callee.value != pendingDirectCall) {
                        return failNative(ins, "[emit-llvm] 原生函数调用缺少匹配的函数引用");
                    }

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
                    for (size_t i = 0; i < arguments.size(); ++i) {
                        if (i > 0 || calledReturnsDynamic) callText += ", ";
                        std::string nativeArgument;
                        bool ownsDynamicArgument = false;
                        if (!emitFunctionABIArgument(
                                ins,
                                arguments[i],
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
                        }
                    }
                    callText += ")";

                    emitNativeLine(callText);
                    const auto* calledReturnABI =
                        ZeusBuiltins::findNativeTypeABI(called.returnType);
                    if (!calledReturnsDynamic && calledReturnABI != nullptr &&
                        calledReturnABI->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                        const std::string stableString = freshNativeTmp();
                        emitNativeLine(stableString +
                                       " = call ptr @zeus_string_view_intern(ptr " +
                                       resultValue + ")");
                        resultValue = stableString;
                    }
                    for (size_t i = 0; i < dynamicCallArguments.size(); ++i) {
                        if (ownsDynamicCallArguments[i]) {
                            emitNativeLine("call void @zeus_value_release(ptr " +
                                           dynamicCallArguments[i] + ")");
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
                    if (arguments[0].dynamic || arguments[0].type != "ptr" ||
                        !isClassType(arguments[0].sourceType)) {
                        auto toDynamic = [&](const NativeValue& value,
                                             std::string& dynamicValue,
                                             bool& ownsDynamic) {
                            return emitNativeDynamicValue(
                                ins, value, dynamicValue, ownsDynamic);
                        };
                        if (!emitDynamicMemberCall(
                                ins,
                                std::get<std::string>(ins.args[0]),
                                arguments,
                                callResult,
                                emitNativeLine,
                                freshNativeTmp,
                                toDynamic,
                                releaseDynamic,
                                failMethod)) {
                            return false;
                        }
                    } else if (!emitKnownClassMethodCall(
                                   ins,
                                   std::get<std::string>(ins.args[0]),
                                   arguments,
                                   callResult,
                                   emitNativeLine,
                                   freshNativeTmp,
                                   convertArgument,
                                   releaseDynamic,
                                   failMethod)) {
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
                        }
                    }
                    valueStack.clear();
                    break;

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
                result.error = "[emit-llvm] loop-carried local changed ABI type or ownership";
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
            "(ptr %out, ptr %env, i32 %argc, ptr %args) {\nentry:\n";
        std::vector<std::string> adapterArguments;
        adapterArguments.reserve(signature.parameterTypes.size());
        int adapterTmp = 0;
        auto freshAdapterTmp = [&]() {
            return std::string("%a") + std::to_string(adapterTmp++);
        };
        auto emitAdapterLine = [&](const std::string& line) {
            adapterText += "  " + line + "\n";
        };
        for (size_t i = 0; i < signature.parameterTypes.size(); ++i) {
            const std::string slot = freshAdapterTmp();
            const std::string dynamicArgument = freshAdapterTmp();
            emitAdapterLine(slot + " = getelementptr inbounds ptr, ptr %args, i64 " +
                            std::to_string(i));
            emitAdapterLine(dynamicArgument + " = load ptr, ptr " + slot + ", align 8");

            const std::string& declaredType = signature.parameterTypes[i];
            const std::string abiType = functionParameterABIType(declaredType);
            std::string nativeArgument = dynamicArgument;
            if (!usesZeusValueABI(declaredType)) {
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
                    if (abi != nullptr &&
                        abi->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                        nativeArgument = freshAdapterTmp();
                        emitAdapterLine(nativeArgument +
                            " = call ptr @zeus_value_intern_string_view(ptr " +
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
                if (adapterReturnType == "ptr" && abi != nullptr &&
                    abi->valueABI == ZeusBuiltins::NativeValueABI::StringView) {
                    emitAdapterLine("call void @zeus_value_from_string_view(ptr %out, ptr " +
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
                case VMOperator::POP:
                case VMOperator::CLEAR:
                case VMOperator::SWAP:
                case VMOperator::LOAD_VALUE:
                case VMOperator::OBJ_BUILD:
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
                case VMOperator::JUMP:
                case VMOperator::JUMP_IF_TRUE:
                case VMOperator::JUMP_IF_FALSE:
                case VMOperator::LOOP_BREAK:
                case VMOperator::COUNT_CHECK_NEG:
                case VMOperator::COUNT_CHECK_STEP_ZERO:
                    hasControlFlow = hasControlFlow ||
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
        };

        auto failNativeTopLevel = [&](const IRType::IRInstr& ins, const std::string& message) {
            result.error = message + " (" + ins.toString() + ")";
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
            slots.emplace(index, std::move(slot));
            return true;
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
                emitLine("store " + value.type + " " + value.value +
                         ", ptr " + slots[index].address);
            }
            values.clear();
            return true;
        };

        std::set<size_t> labels;
        bool exitJump = false;
        if (hasControlFlow) {
            for (size_t pc = 0; pc < chunk.size(); ++pc) {
                const auto& ins = chunk[pc];
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

            std::vector<std::string> dynamicArguments(arguments.size());
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (!emitNativeValueAsDynamic(arguments[i], ins, dynamicArguments[i])) return false;
            }
            std::string resultValue;
            if (const auto* nativeDeclaration =
                    findNativeBuiltinDeclaration(callee.value)) {
                if (!usesDynamicZeusValueSignature(*nativeDeclaration) ||
                    nativeDeclaration->parameterTypes.size() != arguments.size()) {
                    failNativeTopLevel(ins,
                        "[emit-llvm] @native 内置函数 '" + callee.value +
                        "' 需要匹配的动态 ZeusValue 签名");
                    return false;
                }
                resultValue = freshTmp();
                emitLine(resultValue + " = alloca %ZeusValue, align 8");
                std::string callText = "call void " +
                    llvmCallTarget(nativeDeclaration->name) + "(ptr " + resultValue;
                for (const auto& argument : dynamicArguments) {
                    callText += ", ptr " + argument;
                }
                callText += ")";
                emitLine(callText);
            } else {
                const std::string callSymbol = dynamicVariadicABISymbol(callee.value);
                if (callSymbol.empty()) {
                    failNativeTopLevel(ins, "[emit-llvm] 函数没有动态变长 ABI");
                    return false;
                }
                resultValue = emitDynamicVariadicCall(
                    callSymbol,
                    callee.value,
                    dynamicArguments,
                    [this](const std::string& line) { emitLine(line); },
                    [this]() { return freshTmp(); }
                );
            }
            for (size_t i = 0; i < arguments.size(); ++i) {
                if (arguments[i].dynamic) releaseNativeValue(arguments[i]);
                else emitRelease(dynamicArguments[i]);
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
            if (!dynamicArguments.empty()) {
                argumentArray = freshTmp();
                emitLine(argumentArray + " = alloca ptr, i64 " +
                         std::to_string(dynamicArguments.size()) + ", align 8");
                for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                    const std::string slot = freshTmp();
                    emitLine(slot + " = getelementptr inbounds ptr, ptr " +
                             argumentArray + ", i64 " + std::to_string(i));
                    emitLine("store ptr " + dynamicArguments[i] + ", ptr " + slot +
                             ", align 8");
                }
            }
            const std::string output = freshTmp();
            emitLine(output + " = alloca %ZeusValue, align 8");
            emitLine("call void @zeus_closure_call(ptr " + output + ", ptr " +
                     callee.value + ", i32 " +
                     std::to_string(dynamicArguments.size()) + ", ptr " +
                     argumentArray + ")");
            for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                if (ownsDynamicArguments[i]) emitRelease(dynamicArguments[i]);
            }
            if (callee.ownsDynamic) releaseNativeValue(callee);
            callResult = NativeValue{ output, "ptr", false, true, std::string(), true };
            return true;
        };

        auto emitBinary = [&](const IRType::IRInstr& ins) {
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
            valueStack.push_back(NativeValue{ temporary, "ptr", false, true });
            return true;
        };

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];

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
                case VMOperator::IR_ANNOTATION:
                    if (annotationKind(ins, "direct_call")) {
                        std::string directCallName;
                        if (!annotationSymbolName(ins, directCallName)) {
                            failNativeTopLevel(ins, "[emit-llvm] 直接调用标注缺少函数名");
                            rollback();
                            return false;
                        }
                        const auto target = functionSignatures.find(directCallName);
                        pendingDirectCall = target != functionSignatures.end() &&
                            !hasCompleteFunctionAnnotations(target->second)
                                ? std::string()
                                : directCallName;
                    }
                    break;

                case VMOperator::IMPORT:
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

                case VMOperator::SWAP:
                    if (valueStack.size() < 2) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生值栈无法 SWAP");
                        rollback();
                        return false;
                    }
                    std::swap(valueStack[valueStack.size() - 1], valueStack[valueStack.size() - 2]);
                    break;

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
                        } else if (functionSignatures.find(name) != functionSignatures.end()) {
                            pendingFunctionLoad = name;
                            valueStack.push_back(NativeValue{ name, "function", true, false });
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
                            valueStack.push_back(value->second);
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
                            valueStack.push_back(value->second);
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
                    if (!tryArgInt(ins, 0, argc) || argc < 0 ||
                        valueStack.size() < static_cast<size_t>(argc + 1)) {
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
                    if (!popValue(ins, classReference) || classReference.type != "classref") {
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
                    if (!emitClassConstruction(
                            ins,
                            classReference.sourceType,
                            arguments,
                            instance,
                            emitConstructionLine,
                            emitConstructionLabel,
                            freshConstructionTemporary,
                            convertConstructorArgument,
                            failConstruction)) {
                        rollback();
                        return false;
                    }
                    valueStack.push_back(std::move(instance));
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
                        cleanupValue = value.dynamic && value.ownsDynamic
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
                    if (arguments[0].dynamic || arguments[0].type != "ptr" ||
                        !isClassType(arguments[0].sourceType)) {
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
                                std::get<std::string>(ins.args[0]),
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
                                   std::get<std::string>(ins.args[0]),
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
                    if (slot != nativeGlobalSlots.end()) {
                        if (!isNativeSlotValue(value) ||
                            slot->second.type != value.type ||
                            slot->second.sourceType != value.sourceType) {
                            failNativeTopLevel(ins,
                                               "[emit-llvm] 控制流全局槽位类型不匹配");
                            rollback();
                            return false;
                        }
                        emitLine("store " + value.type + " " + value.value +
                                 ", ptr " + slot->second.address);
                    } else if (hasControlFlow && isNativeSlotValue(value)) {
                        if (!ensureSlot(nativeGlobalSlots, static_cast<int>(index),
                                        value.type, value.sourceType, ins)) {
                            rollback();
                            return false;
                        }
                        emitLine("store " + value.type + " " + value.value +
                                 ", ptr " + nativeGlobalSlots[static_cast<int>(index)].address);
                    } else if (value.dynamic) {
                        emitLine("call void @zeus_value_global_store(i32 " +
                                 std::to_string(index) + ", ptr " + value.value + ")");
                        nativeGlobals.erase(static_cast<int>(index));
                    } else {
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
                            // The slot table is the only owner now: keeping an
                            // SSA copy here would shadow writes made by native
                            // functions called later.
                            nativeGlobals.erase(static_cast<int>(index));
                        } else {
                            nativeGlobals[static_cast<int>(index)] = value;
                        }
                    }
                    break;
                }

                case VMOperator::DELETE_VAR: {
                    if (ins.args.size() < 3 ||
                        !std::holds_alternative<std::string>(ins.args[1]) ||
                        std::get<std::string>(ins.args[1]) != "global") {
                        failNativeTopLevel(ins,
                                           "[emit-llvm] 顶层 class delete 目前要求全局变量");
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
                    NativeValue object;
                    const auto slot = nativeGlobalSlots.find(globalIndex);
                    if (slot != nativeGlobalSlots.end()) {
                        if (slot->second.type != "ptr" ||
                            !isClassType(slot->second.sourceType)) {
                            failNativeTopLevel(ins,
                                               "[emit-llvm] delete 目标不是 class 实例");
                            rollback();
                            return false;
                        }
                        object = NativeValue{
                            freshTmp(), "ptr", false, false, slot->second.sourceType
                        };
                        emitLine(object.value + " = load ptr, ptr " + slot->second.address);
                    } else {
                        const auto value = nativeGlobals.find(globalIndex);
                        if (value == nativeGlobals.end() || value->second.type != "ptr" ||
                            !isClassType(value->second.sourceType)) {
                            failNativeTopLevel(ins,
                                               "[emit-llvm] delete 目标不是可静态管理的 class 实例");
                            rollback();
                            return false;
                        }
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

                    std::set<int> clearedGlobals{ globalIndex };
                    for (auto global = nativeGlobals.begin(); global != nativeGlobals.end();) {
                        if (global->second.type == "ptr" &&
                            global->second.sourceType == object.sourceType &&
                            global->second.value == object.value) {
                            clearedGlobals.insert(global->first);
                            global = nativeGlobals.erase(global);
                        } else {
                            ++global;
                        }
                    }

                    for (const auto& globalSlot : nativeGlobalSlots) {
                        if (globalSlot.second.type != "ptr" ||
                            globalSlot.second.sourceType != object.sourceType) {
                            continue;
                        }
                        const std::string current = freshTmp();
                        const std::string isDeletedObject = freshTmp();
                        const std::string cleared = freshTmp();
                        emitLine(current + " = load ptr, ptr " + globalSlot.second.address);
                        emitLine(isDeletedObject + " = icmp eq ptr " + current +
                                 ", " + object.value);
                        emitLine(cleared + " = select i1 " + isDeletedObject +
                                 ", ptr null, ptr " + current);
                        emitLine("store ptr " + cleared + ", ptr " + globalSlot.second.address);
                    }

                    for (const int clearedIndex : clearedGlobals) {
                        const std::string nilValue = freshTmp();
                        emitLine(nilValue + " = alloca %ZeusValue, align 8");
                        emitLine("call void @zeus_value_init_nil(ptr " + nilValue + ")");
                        emitLine("call void @zeus_value_global_store(i32 " +
                                 std::to_string(clearedIndex) + ", ptr " + nilValue + ")");
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
                    if (slot != nativeLocalSlots.end()) {
                        if (!isNativeSlotValue(value) ||
                            slot->second.type != value.type ||
                            slot->second.sourceType != value.sourceType) {
                            failNativeTopLevel(ins,
                                               "[emit-llvm] 控制流局部槽位类型不匹配");
                            rollback();
                            return false;
                        }
                        emitLine("store " + value.type + " " + value.value +
                                 ", ptr " + slot->second.address);
                    } else if (hasControlFlow && isNativeSlotValue(value)) {
                        if (!ensureSlot(nativeLocalSlots, static_cast<int>(index),
                                        value.type, value.sourceType, ins)) {
                            rollback();
                            return false;
                        }
                        emitLine("store " + value.type + " " + value.value +
                                 ", ptr " + nativeLocalSlots[static_cast<int>(index)].address);
                    } else if (value.dynamic) {
                        emitLine("call void @zeus_value_local_store(i32 " +
                                 std::to_string(index) + ", ptr " + value.value + ")");
                        nativeLocals.erase(static_cast<int>(index));
                    } else {
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

                    if (!isList && container.type == "ptr" &&
                        !container.dynamic && isClassType(container.sourceType)) {
                        const size_t parameterCount = isStore ? 3 : 2;
                        const FunctionSignature* operatorSignature =
                            findClassOperatorSignature(
                                container.sourceType, isStore ? "[]=" : "[]",
                                parameterCount);
                        if (operatorSignature == nullptr) {
                            failNativeTopLevel(ins,
                                "[emit-llvm] class 没有匹配的下标operator");
                            rollback();
                            return false;
                        }
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
                            cleanupValue = value.dynamic && value.ownsDynamic
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
                        symbol = "@zeus_value_dict_load";
                        callArguments = { dynamicOperands[0], dynamicOperands[1] };
                    } else {
                        symbol = "@zeus_value_dict_store";
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
                    }

                    int64_t argc = 0;
                    if (!tryArgInt(ins, 0, argc) || argc < 0) {
                        failNativeTopLevel(ins, "[emit-llvm] 原生 CALL_CLOSURE 参数不是整数");
                        rollback();
                        return false;
                    }

                    if (!pendingDirectCall.empty()) {
                        auto function = functionSignatures.find(pendingDirectCall);
                        if (function == functionSignatures.end() ||
                            function->second.parameterTypes.size() != static_cast<size_t>(argc)) {
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
                        for (size_t i = 0; i < arguments.size(); ++i) {
                            std::string nativeArgument;
                            const std::string& parameterType =
                                function->second.parameterTypes[i];
                            if (usesZeusValueABI(parameterType)) {
                                if (!emitNativeValueAsDynamic(
                                        arguments[i], ins, nativeArgument)) {
                                    rollback();
                                    return false;
                                }
                                dynamicArguments.push_back(nativeArgument);
                                ownsDynamicArguments.push_back(
                                    arguments[i].dynamic ? arguments[i].ownsDynamic : true);
                            } else {
                                if (!emitNativeValueAsType(
                                        arguments[i], llvmType(parameterType), ins,
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
                        callText += ")";
                        emitLine(callText);
                        for (size_t i = 0; i < dynamicArguments.size(); ++i) {
                            if (ownsDynamicArguments[i]) emitRelease(dynamicArguments[i]);
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

        // Pass 1：只收集跳转目标。目标 == size 是合法的“离开该 chunk”
        // （如末尾 if 的 false 分支），映射到出口块；条件分支的 fall through
        // size 同理（隐式离开）。直接 fallthrough 不打 label。
        std::set<size_t> labels;
        bool exitJump = false;

        for (size_t pc = 0; pc < chunk.size(); ++pc) {
            const auto& ins = chunk[pc];

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
                case VMOperator::IR_ANNOTATION: {
                    if (annotationKind(ins, "direct_call")) {
                        std::string directCallName;
                        if (!annotationSymbolName(ins, directCallName)) {
                            fail(ins, "[emit-llvm] 直接调用标注缺少函数名");
                            return false;
                        }
                        const auto target = functionSignatures.find(directCallName);
                        pendingDirectCall = target != functionSignatures.end() &&
                            !hasCompleteFunctionAnnotations(target->second)
                                ? std::string()
                                : directCallName;
                    }
                    break;
                }

                case VMOperator::IMPORT:
                    break;

                case VMOperator::PUSH: {
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

                        if (functionSignatures.find(name) != functionSignatures.end()) {
                            pendingFunctionLoad = name;
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
                        symbol = "@zeus_value_dict_load";
                        callArguments = { container, keyOrIndex };
                    } else {
                        symbol = "@zeus_value_dict_store";
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
                case VMOperator::NOT_EQUAL: {
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
                            if (!usesDynamicZeusValueSignature(*nativeDeclaration)) {
                                fail(ins, "[emit-llvm] @native 内置函数 '" + nativeName +
                                     "' 目前需要使用动态 ZeusValue 参数和返回值");
                                return false;
                            }
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
        return res;
    }

    // Both passes need to know what each global holds before any code is
    // emitted, so the scan runs on its own here.
    e.prescanGlobalTypes();

    for (size_t ci = 0; ci < r.program.size(); ++ci) {
        if (!r.program[ci].empty() &&
            r.program[ci].front().metadata.kind == IRType::IRMetadataKind::ModuleImportInit) {
            if (!e.emitChunk({}, ci, ci + 1 == r.program.size())) {
                res.ok = false;
                res.error = e.result.error;
                res.warnings = e.result.warnings;
                return res;
            }
            continue;
        }
        if (!e.emitChunk(r.program[ci], ci, ci + 1 == r.program.size())) {
            res.ok = false;
            res.error = e.result.error;
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
        res.warnings = e.result.warnings;
        return res;
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
    out += "declare void @zeus_value_from_string_data(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_from_string_view(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bytes(ptr, ptr, i64)\n";
    out += "declare void @zeus_value_from_bigint_decimal(ptr, ptr)\n";
    out += "declare void @zeus_value_from_bigdecimal_decimal(ptr, ptr)\n";
    out += "declare i64 @zeus_value_require_i64(ptr)\n";
    out += "declare i32 @zeus_value_require_bool(ptr)\n";
    out += "declare ptr @zeus_value_intern_string_view(ptr)\n";
    out += "declare ptr @zeus_string_view_intern(ptr)\n";
    out += "declare ptr @zeus_string_view_from_data(ptr, i64)\n";
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
    out += "declare ptr @zeus_closure_capture_ref(ptr, i32)\n";
    out += "declare ptr @zeus_closure_capture_copy(ptr, i32)\n";
    out += "declare void @zeus_closure_create(ptr, ptr, i32, i32, ptr)\n";
    out += "declare void @zeus_closure_call(ptr, ptr, i32, ptr)\n";
    out += "declare void @zeus_closure_env_load(ptr, ptr, i32)\n";
    out += "declare void @zeus_closure_env_store(ptr, i32, ptr)\n";
    out += "declare void @zeus_gc_collect()\n";
    out += "declare i64 @zeus_gc_live_node_count()\n";
    out += "declare void @zeus_value_int_add(ptr, ptr, ptr)\n";
    out += "declare void @zeus_value_less_than(ptr, ptr, ptr)\n";
    out += "declare i32 @zeus_value_truthy(ptr)\n";
    out += "declare ptr @malloc(i64)\n";
    out += "declare void @free(ptr)\n";
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
    out += e.nativeFunctions;
    out += e.dynamicMemberBridge.adapters;

    out += "define i32 @zeus_entry() {\nentry:\n";
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
        out += "  ret i32 0\n";
    }

    out += "}\n";

    res.ok = true;
    res.text = out;
    res.warnings = e.result.warnings;
    return res;
}

} // namespace ZeusLLVM
