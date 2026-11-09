#pragma once

#include "ZeusCallSetupScope.hpp"
#include "Dispatch/ZeusVMDispatchConfig.hpp"

#include <string>
#include <stack>
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <iostream>
#include <vector>
#include <deque>
#include <cstdio>
#include <type_traits>
#include <stdexcept>
#include <optional>
#include <shared_mutex>
#include <limits>
#include <cstring>
#include <memory>
#include <functional>

// Windows Socket headers
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define SOCKET int
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#endif

#include "../ZeusRuntimeLib/ZeusVariant.hpp"
#include "../ZeusRuntimeLib/ZeusObjectPayload.hpp"
#include "ZeusVMOperator.hpp"
#include "../ZeusRuntimeLib/ZeusVMException.hpp"
#include "../ZeusRuntimeLib/ZeusSharedHeap.hpp"
#include "../ZeusIR/IRType.hpp"
#include "../ZeusRuntimeLib/ZeusCell.hpp"
#include "ZeusBuiltins.hpp"
#include "../ZeusRuntimeLib/ZeusVMUtils.hpp"
#include "ZeusVMAddress.hpp"
#include "ZeusVMSlots.hpp"
#include "ZeusVMCopy.hpp"
#include "ZeusVMRoots.hpp"
#include "../ZeusTyping/GenericTypeName.hpp"
#include "ZeusHotspot.hpp"
#include "../ZeusRuntimeLib/ZeusGC.hpp"
#include "../ZeusBuiltins/NativeTypeABI.hpp"
#include "../ZeusRuntimeLib/ZeusRuntimeType.hpp"
#if 0
/* Computed-goto dispatch removed for MSVC builds. */
#endif
#ifdef ZEUS_DEBUG_FILE_CHANNEL
#undef ZEUS_DEBUG_FILE_CHANNEL
#endif
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_VM
#include "../ZeusDebug/Utils.hpp"
#include "../ZeusDebug/BreakpointConfig.hpp"
#include "../ZeusDebug/VMSnapshot.hpp"
#include "../ZeusDebug/VMSnapshotCapture.hpp"

// MSVC 报告 C4828 当源文件包含在当前源字符集下不可识别的字节序列。
// 目前文件中存在历史遗留的编码噪音（注释中的 mojibake），这些字符不会影响运行逻辑，
// 但会触发编译器警告。为了最小侵入，这里临时禁用该警告。
// 建议后续将该文件及整个仓库统一保存为 UTF-8（无 BOM），以彻底根除警告。
#pragma warning(push)
#pragma warning(disable:4828)
 

// Helper: 从 LexerType::Value 中提取整数（用于参数/索引处理）
namespace VMHelpers {
    inline int getIntFromValue(const LexerType::Value& val) {
        if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
            const auto& big = std::get<ZeusType::BigInteger>(val.data);
            if (!big.fitsInInt64()) {
                throw std::overflow_error("BigInteger too large for int");
            }
            int64_t i64 = big.toInt64();
            if (i64 < INT_MIN || i64 > INT_MAX) {
                throw std::overflow_error("Value too large for int");
            }
            return static_cast<int>(i64);
        }
        throw std::runtime_error("Value is not an integer");
    }
}



class VM {
    public: 
    ZeusType::VMStatus vmstat = ZeusType::VMStatus::Running;

    int __line = 0;  // 已弃用：请使用 currentChunk 表示当前 chunk 的行/上下文
    int __sourceFileId = 0;  // 当前执行指令的源文件ID（用于错误报告）
    int currentChunk = -1;  // 当前执行的 chunk 索引（用于错误/调试上下文）
    
    // 外部停止标志（用于线程 eliminate）
    std::atomic<bool>* externalStopFlag = nullptr;

    // 可选：使用 std::pmr 进行单体分配以减少内存分配开销（当前未启用）
    // std::pmr::monotonic_buffer_resource globalHeap;
    // std::pmr::vector<int>               heapObjects{&globalHeap};

    // operand stack (runtime evaluation stack)
    // 栈条目携带所有权标记，方便在统一收缩时释放对象/闭包
    class StackWithOwner {
    public:
        std::vector<ZeusType::UniversalVariant> data;
        std::vector<uint8_t> owner; // bit0: owns ObjectHandle, bit1: owns ClosureHandle
        std::function<void(const ZeusType::UniversalVariant&)> temporaryRoot;

        void pinTemporary(size_t index) {
            if (owner[index] && temporaryRoot) temporaryRoot(data[index]);
        }

        static uint8_t calcOwner(const ZeusType::UniversalVariant& v) {
            if (std::holds_alternative<ZeusType::ObjectHandle>(v)) return 0x1;
            if (std::holds_alternative<ZeusType::ClosureHandle>(v)) return 0x2;
            if (std::holds_alternative<ZeusType::Variant>(v)) return 0x4;
            if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(v)) return 0x4;
            return 0;
        }

        size_t size() const { return data.size(); }
        bool empty() const { return data.empty(); }
        void clear() { data.clear(); owner.clear(); }
        ZeusType::UniversalVariant& back() { return data.back(); }
        const ZeusType::UniversalVariant& back() const { return data.back(); }
        ZeusType::UniversalVariant& operator[](size_t i) { return data[i]; }
        const ZeusType::UniversalVariant& operator[](size_t i) const { return data[i]; }
        auto begin() { return data.begin(); }
        auto end() { return data.end(); }
        auto begin() const { return data.begin(); }
        auto end() const { return data.end(); }
        auto cbegin() const { return data.cbegin(); }
        auto cend() const { return data.cend(); }

        void reserve(size_t n) { data.reserve(n); owner.reserve(n); }
        void resize(size_t n) { data.resize(n); owner.resize(n); }

        void push_back(const ZeusType::UniversalVariant& v) {
            data.push_back(v);
            owner.push_back(calcOwner(v));
        }
        void push_back(ZeusType::UniversalVariant&& v) {
            owner.push_back(calcOwner(v));
            data.push_back(std::move(v));
        }
        // 插入到指定位置（索引）
        void insert(size_t idx, const ZeusType::UniversalVariant& v) {
            data.insert(data.begin() + idx, v);
            owner.insert(owner.begin() + idx, calcOwner(v));
        }
        void insert(std::vector<ZeusType::UniversalVariant>::iterator it, const ZeusType::UniversalVariant& v) {
            size_t idx = static_cast<size_t>(it - data.begin());
            insert(idx, v);
        }
        void erase(size_t idx) {
            pinTemporary(idx);
            data.erase(data.begin() + idx);
            owner.erase(owner.begin() + idx);
        }
        void pop_back() {
            pinTemporary(data.size() - 1);
            data.pop_back();
            owner.pop_back();
        }
        uint8_t ownerFlag(size_t idx) const { return owner[idx]; }
        void setOwnerFlag(size_t idx, uint8_t f) { owner[idx] = f; }
        void set(size_t idx, const ZeusType::UniversalVariant& v) {
            pinTemporary(idx);
            data[idx] = v;
            owner[idx] = calcOwner(v);
        }
        void set(size_t idx, ZeusType::UniversalVariant&& v) {
            pinTemporary(idx);
            owner[idx] = calcOwner(v);
            data[idx] = std::move(v);
        }
    } stack;

    // 变量槽：global/local/upvalue 存储对应 Cell* 引用
    std::vector<Cell*> global;   // 全局变量槽
    std::vector<Cell*> local;    // 局部变量槽
    std::vector<Cell*> upvalue;  // 上值（闭包捕获）槽
    ZeusVMSlots::Registry addressSlots;
    
    // 子线程继承的资源边界（析构时跳过这些资源的释放）
    size_t inheritedGlobalSize = 0;       // 继承的 global 槽数
    size_t packageGlobalCount = 0;        // 编译结果中的包全局槽位前缀
    size_t mainGlobalStart = 0;           // 主程序槽位起点（namespace import 别名之后）
    size_t mainGlobalEnd = 0;             // 合并包专属别名前的终点
    size_t teardownLayoutGlobalCount = 0;
    bool hasGlobalTeardownLayout = false;
    size_t inheritedClosuresSize = 0;     // 继承的 closures 数

    // 调用深度计数器：通过栈帧数量跟踪（不再受C++栈限制）
    static constexpr int MAX_CALL_DEPTH = 100000;  // 最大调用深度（栈帧模式支持深度递归）
    
    // 标记：execute()是否创建了新栈帧（CALL_CLOSURE会设置此标志）
    bool justPushedFrame = false;

    // 析构函数：清理所有分配的内存
    VM() {
        stack.temporaryRoot = [this](const ZeusType::UniversalVariant& value) {
            pinVMTemporaryRoot(value);
        };
        ZeusVMRoots::Snapshot roots;
        roots.objectTable = &objects;
        ZeusVMRoots::Registry::publish(this, std::move(roots));
    }

    ~VM() {
        ZEUS_DEBUG_PRINT("[~VM] Destructor: inheritedGlobalSize=%zu, global=%zu, local=%zu, upvalue=%zu",
            inheritedGlobalSize, global.size(), local.size(), upvalue.size());

        // 重要：释放栈上仍然存活的引用（尤其是子线程 VM 退出时）
        // 否则线程执行结束但栈残留的 ObjectHandle/Variant/NamedArg 会永久多 retain 一次，导致对象/闭包泄露。
        shrinkStackWithRelease(0);
        stack.clear();
         
        // Detach the environment before user destruction can re-enter the VM.
        releaseCurrentFrameEnvironment();

        auto releaseGlobalSlot = [&](size_t index) {
            Cell* owned = global[index];
            global[index] = nullptr;
            if (owned) releaseCellAndContent(owned);
        };

        if (!hasGlobalTeardownLayout) {
            for (size_t i = inheritedGlobalSize; i < global.size(); ++i) {
                releaseGlobalSlot(i);
            }
        } else {
            const size_t firstOwnedGlobal = std::min(inheritedGlobalSize, global.size());
            const size_t layoutEnd = std::min(teardownLayoutGlobalCount, global.size());
            const size_t packageGlobalEnd = std::min(packageGlobalCount, layoutEnd);
            const size_t programGlobalStart = std::min(mainGlobalStart, layoutEnd);
            const size_t programGlobalEnd = std::min(mainGlobalEnd, layoutEnd);
            auto releaseLateProgramGlobals = [&]() {
                for (size_t i = std::max(firstOwnedGlobal, teardownLayoutGlobalCount);
                     i < global.size(); ++i) {
                    releaseGlobalSlot(i);
                }
            };

            // Release root program globals in their existing slot order while
            // root namespace aliases and package globals remain available.
            for (size_t i = std::max(firstOwnedGlobal, programGlobalStart);
                 i < programGlobalEnd; ++i) {
                releaseGlobalSlot(i);
            }
            releaseLateProgramGlobals();

            // Namespace import aliases are pre-registered before root program
            // globals. Release that prefix only after their finalizers finish.
            for (size_t i = std::max(firstOwnedGlobal, packageGlobalEnd);
                 i < programGlobalStart; ++i) {
                releaseGlobalSlot(i);
            }
            releaseLateProgramGlobals();

            for (size_t i = firstOwnedGlobal; i < packageGlobalEnd; ++i) {
                releaseGlobalSlot(i);
                releaseLateProgramGlobals();
            }

            // Package namespace aliases are appended after the root program
            // globals and remain available until all package finalizers finish.
            const size_t packageAliasStart = std::max(firstOwnedGlobal, programGlobalEnd);
            for (size_t i = packageAliasStart; i < layoutEnd; ++i) {
                releaseGlobalSlot(i);
                releaseLateProgramGlobals();
            }
        }
        global.clear();
        
        // Detach all local closures before releasing captures. Captures may
        // contain a container that points back to this or another closure.
        std::vector<ClosureReleasePayload> closurePayloads;
        {
            std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
            std::unique_lock<std::shared_mutex> lock(closuresMutex);
            for (size_t i = inheritedClosuresSize; i < closures.size(); ++i) {
                auto& closure = closures[i];
                if (closure.isFreed()) continue;
                closurePayloads.push_back({
                    std::move(closure.upvalues), std::move(closure.boundThis)
                });
                closure.upvalues.clear();
                closure.boundThis.reset();
                closure.protoId = -1;
                closure.refCount = 0;
            }
        }
        for (auto& payload : closurePayloads) {
            releaseClosurePayload(payload);
        }
        closures.clear();
        ZeusVMRoots::Registry::remove(this);
    }

    int signedIntegerBitsFor(const std::string& typeName) const {
        const auto* abi = ZeusBuiltins::findNativeSignedIntegerABI(typeName);
        return abi ? abi->integerBits : 0;
    }

    int unsignedIntegerBitsFor(const std::string& typeName) const {
        const auto* abi = ZeusBuiltins::findNativeUnsignedIntegerABI(typeName);
        return abi ? abi->integerBits : 0;
    }

    uint32_t nativeFloatingPointBitsFor(const std::string& typeName) const {
        return ZeusBuiltins::nativeFloatingPointBits(typeName);
    }

    ZeusType::UniversalVariant normalizeNativeScalarValue(
        const std::string& typeName,
        const ZeusType::UniversalVariant& value
    ) const {
        if (const int bits = signedIntegerBitsFor(typeName)) {
            return ZeusSignedInteger::normalize(value, bits);
        }
        if (const int bits = unsignedIntegerBitsFor(typeName)) {
            return ZeusUnsignedInteger::normalize(value, bits);
        }
        if (nativeFloatingPointBitsFor(typeName) == 32) {
            return ZeusNativeFloat::normalize(value);
        }
        if (nativeFloatingPointBitsFor(typeName) == 64) {
            return ZeusNativeDouble::normalize(value);
        }
        return value;
    }

    void configureNativeScalarCell(Cell* cell, const std::string& typeName) const {
        if (!cell) return;
        cell->signedIntegerBits = signedIntegerBitsFor(typeName);
        cell->unsignedIntegerBits = unsignedIntegerBitsFor(typeName);
        cell->nativeFloatingPointBits = nativeFloatingPointBitsFor(typeName);
    }

    bool validateNativeScalarReturn(const std::vector<IRType::IRInstr>& code,
                                    const IRType::IRInstr& returningInstruction) {
        for (const auto& instruction : code) {
            if (instruction.metadata.kind != IRType::IRMetadataKind::FunctionSignature) continue;
            const std::string& returnType = instruction.metadata.typeName;
            const bool nativeScalar = ZeusBuiltins::findNativeScalarCanonical(returnType) != nullptr;
            try {
                if (nativeScalar) {
                    if (stack.empty()) throw std::runtime_error("Native integer function returned no value");
                    stack.back() = normalizeNativeScalarValue(returnType, stack.back());
                }
            } catch (const std::exception& error) {
                VMException::VMException(error.what(), __line, returningInstruction)._throw(vmstat, currentChunk);
                return false;
            }
            return true;
        }
        return true;
    }

    Cell* newCell(const ZeusType::UniversalVariant& v, uint8_t flags = 0, const char* createdBy = "unknown") {
        Cell* c = new Cell{v, flags, __nextCellId()};
        c->createdAtMs = __getCurrentTimeMs();
        c->createdBy = createdBy;
        HeapStats::recordCreate();
        HeapStats::registerCell(c);
        
        // 追踪ObjectHandle Cell的创建（调试通道）
        if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
            auto& oh = std::get<ZeusType::ObjectHandle>(v);
            ZEUS_DEBUG_PRINT("[CELL-TRACK] Created ObjectHandle Cell #%u for obj#%d by %s", c->debugId, oh.id, createdBy);
        }
        
        return c;
    }

    Cell* cloneCell(const Cell* src, const char* createdBy = "clone") {
        if (!src) {
            return newCell(LexerType::Value(0), 0, createdBy);
        }

        Cell* c = new Cell{loadCellValue(src), src->flags, __nextCellId()};
        c->signedIntegerBits = src->signedIntegerBits;
        c->unsignedIntegerBits = src->unsignedIntegerBits;
        c->nativeFloatingPointBits = src->nativeFloatingPointBits;
        c->createdAtMs = __getCurrentTimeMs();
        c->createdBy = createdBy;
        HeapStats::recordCreate();  // 修复：cloneCell 也需要记录创建
        HeapStats::registerCell(c);
        // A copied capture owns a separate value edge even when the value is
        // a shared container. Retain its nested VM handles with that edge.
        const auto clonedValue = c->readValue();
        ZeusGC::retainVariantDeep(*this, clonedValue);
        
        // 追踪ObjectHandle Cell的克隆（调试通道）
        const auto sourceValue = src->readValue();
        if (std::holds_alternative<ZeusType::ObjectHandle>(sourceValue)) {
            const auto& oh = std::get<ZeusType::ObjectHandle>(sourceValue);
            ZEUS_DEBUG_PRINT("[CELL-TRACK] Cloned ObjectHandle Cell #%u for obj#%d (from #%u) by %s", 
                c->debugId, oh.id, src->debugId, createdBy);
        }
        
        return c;
    }

    // --- Native 调用相关 ---
    struct NativeCallContext {
        int argc = 0;
        size_t calleeIndex = 0;          // callee 在栈中的起始索引
        int nativeId = -1;               // 内建函数的 id
        const ZeusBuiltins::NativeFuncDescriptor* desc = nullptr;
        const ZeusBuiltins::DynamicNativeImportDescriptor* dynamicImport = nullptr;
        ZeusBuiltins::NativeFuncDescriptor injectedDescriptor;
    };

    bool prepareNativeCall(const IRType::IRInstr& instr, NativeCallContext& ctx) {
        if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
            VMException::VMException exp("CALL_NATIVE missing argc", __line, instr);
            exp._throw(vmstat, currentChunk);
            return false;
        }

        ctx.argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));

        if (stack.size() < (size_t)ctx.argc + 1) {
            VMException::VMException exp("CALL_NATIVE stack not enough", __line, instr);
            exp._throw(vmstat, currentChunk);
            return false;
        }

        ctx.calleeIndex = stack.size() - (ctx.argc + 1);
        auto& callee = stack[ctx.calleeIndex];

        if (!std::holds_alternative<ZeusType::NativeFuncHandle>(callee)) {
            VMException::VMException exp("CALL_NATIVE callee not native handle", __line, instr);
            exp._throw(vmstat, currentChunk);
            return false;
        }

        ctx.nativeId = std::get<ZeusType::NativeFuncHandle>(callee).id;
        if (ctx.nativeId >= 0) {
            ctx.desc = ZeusBuiltins::get(ctx.nativeId);
        } else if (const auto* injection =
                       ZeusBuiltins::nativeInjectionFromHandle(ctx.nativeId)) {
            ctx.injectedDescriptor = ZeusBuiltins::NativeFuncDescriptor{
                std::string(injection->declarationName),
                injection->callback,
                injection->minArity,
                injection->maxArity};
            ctx.desc = &ctx.injectedDescriptor;
        } else if (const auto* dynamicImport =
                       ZeusBuiltins::dynamicNativeImportFromHandle(ctx.nativeId)) {
            ctx.dynamicImport = dynamicImport;
            ctx.injectedDescriptor = ZeusBuiltins::NativeFuncDescriptor{
                dynamicImport->declarationName,
                nullptr,
                static_cast<int>(dynamicImport->parameterTypes.size()),
                static_cast<int>(dynamicImport->parameterTypes.size())};
            ctx.desc = &ctx.injectedDescriptor;
        }

        if (!ctx.desc) {
            VMException::VMException exp("CALL_NATIVE invalid id: " + std::to_string(ctx.nativeId), __line, instr);
            exp._throw(vmstat, currentChunk);
            return false;
        }

        return true;
    }

    bool validateNativeArity(const NativeCallContext& ctx, const IRType::IRInstr& instr) {
        if (ctx.argc < ctx.desc->minArity) {
            VMException::VMException exp("CALL_NATIVE too few args for " + ctx.desc->name, __line, instr);
            exp._throw(vmstat, currentChunk);
            return false;
        }

        if (ctx.desc->maxArity >= 0 && ctx.argc > ctx.desc->maxArity) {
            VMException::VMException exp("CALL_NATIVE too many args for " + ctx.desc->name, __line, instr);
            exp._throw(vmstat, currentChunk);
            return false;
        }

        return true;
    }

    ZeusType::UniversalVariant invokeNative(const NativeCallContext& ctx) {
        if (ctx.desc && ctx.desc->name == "__gc_collect") {
            collectGarbageSweep();
            return LexerType::Value(
                static_cast<int64_t>(ZeusGC::liveNodeCount()));
        }
        std::vector<ZeusType::UniversalVariant> args;
        args.reserve(ctx.argc);

        for (int i = 0; i < ctx.argc; ++i) {
            args.push_back(stack[ctx.calleeIndex + 1 + i]);
        }

        ZeusType::UniversalVariant ret = LexerType::Value(0);

        // Array copies share mutable storage. Their child references are
        // retained once per live VM Array value, so mutations must update all
        // current owners before normal stack cleanup releases its own value.
        const std::string& builtinName = ctx.desc->name;
        const bool mutatesArrayArgument =
            builtinName == "__array_push" || builtinName == "__array_pop" ||
            builtinName == "__array_set" || builtinName == "__array_clear";
        bool hasArrayArgumentSnapshot = false;
        size_t arrayArgumentOwnerCount = 0;
        std::vector<ZeusType::UniversalVariant> arrayValuesBeforeCall;
        if (mutatesArrayArgument && !args.empty()) {
            const auto* array = ZeusBuiltins::getArrayPtr(args[0]);
            if (array && array->impl) {
                arrayValuesBeforeCall = array->impl->data;
                hasArrayArgumentSnapshot = true;
                arrayArgumentOwnerCount = array->impl->valueRefCount.load(
                    std::memory_order_relaxed);
            }
        }

        try {
            // 特殊处理需要访问VM内部状态的内置函数
            if (ctx.dynamicImport) {
                publishVMRoots();
                ret = ZeusBuiltins::invokeDynamicNativeImport(*ctx.dynamicImport, args);
            }
            else if (ctx.desc->name == "__getglobal") {
                // __getglobal(index) -> value
                if (args.size() != 1) {
                    throw std::runtime_error("__getglobal expects 1 argument");
                }
                
                // 从 UniversalVariant 中提取整数
                int index = -1;
                if (std::holds_alternative<LexerType::Value>(args[0])) {
                    index = VMHelpers::getIntFromValue(std::get<LexerType::Value>(args[0]));
                } else if (std::holds_alternative<ZeusType::BigInteger>(args[0])) {
                    const auto& big = std::get<ZeusType::BigInteger>(args[0]);
                    index = static_cast<int>(big.toInt64());
                } else {
                    throw std::runtime_error("__getglobal: index must be an integer");
                }
                
                if (index < 0 || (size_t)index >= global.size()) {
                    throw std::runtime_error("__getglobal: index out of range: " + std::to_string(index));
                }
                if (!global[index]) {
                    throw std::runtime_error("__getglobal: global slot " + std::to_string(index) + " is null");
                }
                ret = global[index]->readValue();
            }
            else if (ctx.desc->name == "__setglobal") {
                // __setglobal(index, value) -> null
                if (args.size() != 2) {
                    throw std::runtime_error("__setglobal expects 2 arguments");
                }
                
                // 从 UniversalVariant 中提取整数
                int index = -1;
                auto& indexArg = args[0];
                
                if (std::holds_alternative<LexerType::Value>(indexArg)) {
                    index = VMHelpers::getIntFromValue(std::get<LexerType::Value>(indexArg));
                } else if (std::holds_alternative<ZeusType::BigInteger>(indexArg)) {
                    const auto& big = std::get<ZeusType::BigInteger>(indexArg);
                    index = static_cast<int>(big.toInt64());
                } else {
                    throw std::runtime_error("__setglobal: index must be an integer (BigInteger or Value)");
                }
                
                if (index < 0 || (size_t)index >= global.size()) {
                    throw std::runtime_error("__setglobal: index out of range: " + std::to_string(index));
                }
                if (!global[index]) {
                    throw std::runtime_error("__setglobal: global slot " + std::to_string(index) + " is null");
                }
                assignCellValue(global[index], args[1]);
                ret = LexerType::Value(0); // return null
            }
            else if (ctx.desc->name == "__get_module") {
                // __get_module(modulePath) -> ModuleProxy
                // 创建一个模块代理对象，用于引用映射而非值复制
                // 例如: __get_module("zeus.cppfunctions")
                // 返回 ModuleProxy{"zeus.cppfunctions"}
                // 访问 proxy.io 时会自动映射到 "zeus.cppfunctions.io" 全局变量
                if (args.size() != 1) {
                    throw std::runtime_error("__get_module expects 1 argument");
                }
                
                // 提取模块路径
                std::string modulePath;
                if (std::holds_alternative<std::string>(args[0])) {
                    modulePath = std::get<std::string>(args[0]);
                } else if (std::holds_alternative<LexerType::Value>(args[0])) {
                    const auto& val = std::get<LexerType::Value>(args[0]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        modulePath = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("__get_module: modulePath must be a string (got non-string Value)");
                    }
                } else {
                    throw std::runtime_error("__get_module: modulePath must be a string");
                }
                
                // 创建并返回ModuleProxy对象
                ZeusType::ModuleProxy proxy;
                proxy.modulePath = modulePath;
                ret = ZeusType::UniversalVariant(proxy);
            }
            else if (ctx.desc->name == "__loadglobal") {
                // __loadglobal(name) -> value
                // 通过符号名从globalNames查找索引,然后返回 global[index] 的运行期值
                if (args.size() != 1) {
                    throw std::runtime_error("__loadglobal expects 1 argument");
                }
                
                // 提取符号名 - 可能是std::string或LexerType::Value
                std::string name;
                if (std::holds_alternative<std::string>(args[0])) {
                    name = std::get<std::string>(args[0]);
                } else if (std::holds_alternative<LexerType::Value>(args[0])) {
                    const auto& val = std::get<LexerType::Value>(args[0]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        name = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("__loadglobal: name must be a string (got non-string Value)");
                    }
                } else {
                    throw std::runtime_error("__loadglobal: name must be a string");
                }
                
                // 从 globalNames 查找索引（优先使用 sharedGlobalNames）
                int index = -1;
                size_t namesCount = getGlobalNameCount();

                for (size_t i = 0; i < namesCount; ++i) {

                    if (getGlobalName((int)i) == name) {
                        index = (int)i;
                        break;
                    }
                }
                
                if (index < 0) {
                    throw std::runtime_error("__loadglobal: global variable '" + name + "' not found");
                }
                
                if ((size_t)index >= global.size()) {
                    throw std::runtime_error("__loadglobal: index out of range for '" + name + "': " + std::to_string(index));
                }

                if (!global[index]) {
                    throw std::runtime_error("__loadglobal: global slot for '" + name + "' is null");
                }
                
                ret = global[index]->readValue();
                
                // Debug: 输出 __loadglobal 返回的值类型
                ZEUS_DEBUG_PRINT("[__loadglobal] name='%s' index=%d -> %s", 
                    name.c_str(), index, ZeusVMUtils::valueToDebugString(ret).c_str());
            }
            else if (builtinName == "__zeus_object_clone") {
                if (args.size() != 1) {
                    throw std::runtime_error(
                        "__zeus_object_clone expects exactly one value");
                }
                ret = ZeusVMCopy::copy(*this, args[0]);
            }
            else if (builtinName == "__zeus_object_assign") {
                if (args.size() != 2) {
                    throw std::runtime_error(
                        "__zeus_object_assign expects exactly two values");
                }
                const auto* destinationHandle =
                    std::get_if<ZeusType::ObjectHandle>(&args[0]);
                const auto* sourceHandle =
                    std::get_if<ZeusType::ObjectHandle>(&args[1]);
                if (!destinationHandle) {
                    ret = args[1];
                    ZeusGC::retainVariantDeep(*this, ret);
                } else {
                    auto* destination = getValidObject(*destinationHandle);
                    if (!destination) {
                        throw std::runtime_error(
                            "__zeus_object_assign received a stale slot object");
                    }
                    if (!sourceHandle) {
                        throw std::runtime_error(
                            "__zeus_object_assign requires matching class instances");
                    }
                    const auto* source = getValidObject(*sourceHandle);
                    if (!source || source->classId != destination->classId) {
                        throw std::runtime_error(
                            "__zeus_object_assign requires matching class instances");
                    }
                    if (destinationHandle->id == sourceHandle->id &&
                        destinationHandle->generation == sourceHandle->generation) {
                        ret = *destinationHandle;
                        ZeusGC::retainVariantDeep(*this, ret);
                    } else {
                        ZeusType::ObjectInstance replacement;
                        replacement.classId = source->classId;
                        replacement.flags = source->flags;
                        const auto replacementValues = snapshotObjectFields(*source);
                        const auto* sourceClass = getClass(source->classId);
                        if (sourceClass) {
                            ZeusType::initializeDeclaredFields(replacement, *sourceClass);
                        }
                        std::vector<ZeusType::UniversalVariant> retainedFields;
                        retainedFields.reserve(replacementValues.size());
                        try {
                            for (const auto& field : replacementValues) {
                                retainedFields.push_back(field.second);
                                ZeusGC::retainVariantDeep(*this, retainedFields.back());
                                writeObjectField(replacement, field.first, field.second);
                            }
                            if (!invokeObjectDestructor(*destinationHandle) ||
                                vmstat != ZeusType::VMStatus::Running) {
                                throw std::runtime_error(
                                    "StaticArray slot destructor did not complete");
                            }
                            destination = getValidObject(*destinationHandle);
                            if (!destination) {
                                throw std::runtime_error(
                                    "StaticArray slot was invalidated by its destructor");
                            }
                            const auto oldFields = snapshotObjectFields(*destination);
                            auto oldDeclaredFields = std::move(destination->declaredFields);
                            auto oldExtensions = std::move(destination->extensionValues);
                            destination->declaredFields = std::move(replacement.declaredFields);
                            destination->extensionValues = std::move(replacement.extensionValues);
                            destination->flags = std::move(replacement.flags);
                            destination->destructorCalled = false;
                            retainedFields.clear();
                            for (auto& field : oldFields) {
                                ZeusGC::releaseVariantDeep(*this, field.second);
                            }
                            oldDeclaredFields.reset();
                            oldExtensions.clear();
                            ret = *destinationHandle;
                            ZeusGC::retainVariantDeep(*this, ret);
                        } catch (...) {
                            for (const auto& field : retainedFields) {
                                ZeusGC::releaseVariantDeep(*this, field);
                            }
                            throw;
                        }
                    }
                }
            }
            else if (builtinName == "__zeus_object_invalidate") {
                if (args.size() != 1) {
                    throw std::runtime_error(
                        "__zeus_object_invalidate expects exactly one value");
                }
                if (const auto* handle =
                        std::get_if<ZeusType::ObjectHandle>(&args[0])) {
                    if (getValidObject(*handle)) {
                        if (!invokeObjectDestructor(*handle) ||
                            vmstat != ZeusType::VMStatus::Running) {
                            throw std::runtime_error(
                                "StaticArray slot destructor did not complete");
                        }
                        forceBreakClosureCycles(*handle);
                        destroyObjectSource(*handle);
                    }
                }
                ret = ZeusType::UniversalVariant(ZeusType::Variant(ZeusType::Null()));
            }
            else if (ctx.desc->name == "__dict_clone") {
                ret = ZeusBuiltins::builtin_dict_clone(args);
                ZeusGC::retainVariantDeep(*this, ret);
            }
            else if (ctx.desc->name == "__dict_delete") {
                std::optional<ZeusType::UniversalVariant> removedValue;
                size_t dictOwnerCount = 0;
                if (const auto* dict = ZeusContainerOps::dictValue(args[0]);
                    dict && dict->impl) {
                    const std::string key = ZeusContainerOps::normalizedDictKey(args[1]);
                    const auto found = dict->impl->data.find(key);
                    if (found != dict->impl->data.end()) {
                        removedValue = found->second;
                        dictOwnerCount = dict->impl->valueRefCount.load(
                            std::memory_order_relaxed);
                    }
                }
                ret = ZeusBuiltins::builtin_dict_delete(args);
                if (removedValue.has_value()) {
                    // Deletion removes this edge from every VM value sharing the
                    // dictionary storage. The removed item becomes the native call's
                    // result, which has one independent owner.
                    ZeusGC::retainVariantDeep(*this, ret);
                    for (size_t i = 0; i < dictOwnerCount; ++i) {
                        ZeusGC::releaseVariantDeep(*this, *removedValue);
                    }
                }
            }
            else if (ctx.desc->name == "hasKey") {
                // hasKey(dict, key) -> bool
                // 检查字典是否包含指定的键
                if (args.size() != 2) {
                    throw std::runtime_error("hasKey expects 2 arguments");
                }
                
                // 获取key字符串 (对两种类型都适用)
                std::string keyStr;
                if (std::holds_alternative<std::string>(args[1])) {
                    keyStr = std::get<std::string>(args[1]);
                } else if (std::holds_alternative<LexerType::Value>(args[1])) {
                    const auto& val = std::get<LexerType::Value>(args[1]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        keyStr = std::get<std::string>(val.data);
                    } else if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                        keyStr = std::get<ZeusType::BigInteger>(val.data).toString();
                    } else {
                        throw std::runtime_error("hasKey: key must be a string");
                    }
                } else {
                    throw std::runtime_error("hasKey: key must be a string");
                }
                
                // 获取字典对象并检查key
                // 字典可能以两种形式传递:
                // 1. ObjectHandle (OBJ_BUILD创建的Namespace对象)
                // 2. Variant(Object) (DICT_BUILD创建的原生字典)
                bool exists = false;
                
                if (std::holds_alternative<ZeusType::ObjectHandle>(args[0])) {
                    // ObjectHandle - 从 objects 数组获取
                    const auto& objectHandle = std::get<ZeusType::ObjectHandle>(args[0]);
                    auto* objPtr = getValidObject(objectHandle);
                    if (!objPtr) {
                        throw std::runtime_error("hasKey: invalid or stale object handle");
                    }
                    const auto& obj = *objPtr;
                    exists = hasObjectField(obj, keyStr);
                } 
                else if (std::holds_alternative<ZeusType::Variant>(args[0])) {
                    // Variant(Object) - DICT_BUILD创建的
                    const auto& variant = std::get<ZeusType::Variant>(args[0]);
                    if (!std::holds_alternative<ZeusType::Object>(variant)) {
                        throw std::runtime_error("hasKey: first argument must be a dictionary");
                    }
                    const auto& obj = std::get<ZeusType::Object>(variant);
                    if (!obj.impl) {
                        throw std::runtime_error("hasKey: null object data");
                    }
                    // ObjectData有data成员: std::map<std::string, UniversalVariant>
                    exists = obj.impl->data.find(keyStr) != obj.impl->data.end();
                }
                else {
                    throw std::runtime_error("hasKey: first argument must be a dictionary (ObjectHandle or Variant(Object))");
                }
                
                ret = ZeusType::UniversalVariant(exists);
            }
            else if (ctx.desc->name == "__storeglobal") {
                // __storeglobal(name, value) -> null
                // 通过符号名从globalNames查找索引,然后设置 global[index] 的运行期值
                if (args.size() != 2) {
                    throw std::runtime_error("__storeglobal expects 2 arguments");
                }
                
                // 提取符号名 - 可能是std::string或LexerType::Value
                std::string name;
                if (std::holds_alternative<std::string>(args[0])) {
                    name = std::get<std::string>(args[0]);
                } else if (std::holds_alternative<LexerType::Value>(args[0])) {
                    const auto& val = std::get<LexerType::Value>(args[0]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        name = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("__storeglobal: name must be a string (got non-string Value)");
                    }
                } else {
                    throw std::runtime_error("__storeglobal: name must be a string");
                }
                
                // 从globalNames查找索引
                int index = -1;
                for (size_t i = 0; i < globalNames.size(); ++i) {
                    if (globalNames[i] == name) {
                        index = (int)i;
                        break;
                    }
                }
                
                if (index < 0) {
                    throw std::runtime_error("__storeglobal: global variable '" + name + "' not found");
                }
                
                if ((size_t)index >= global.size()) {
                    throw std::runtime_error("__storeglobal: index out of range for '" + name + "': " + std::to_string(index));
                }
                if (!global[index]) {
                    throw std::runtime_error("__storeglobal: global slot for '" + name + "' is null");
                }
                assignCellValue(global[index], args[1]);
                ret = LexerType::Value(0); // return null
            }
            else if (ctx.desc->name == "getattr") {
                // getattr(obj, "attrName") -> value
                if (args.size() != 2) {
                    throw std::runtime_error("getattr expects 2 arguments");
                }
                
                auto& objVar = args[0];
                
                // 提取属性名(可能是string或LexerType::Value包装的string)
                std::string attrName;
                if (std::holds_alternative<std::string>(args[1])) {
                    attrName = std::get<std::string>(args[1]);
                } else if (std::holds_alternative<LexerType::Value>(args[1])) {
                    auto& val = std::get<LexerType::Value>(args[1]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        attrName = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("getattr: attribute name must be a string");
                    }
                } else {
                    throw std::runtime_error("getattr: attribute name must be a string");
                }
                
                if (!std::holds_alternative<ZeusType::ObjectHandle>(objVar)) {
                    throw std::runtime_error("getattr: first argument must be an object");
                }
                
                const auto& objectHandle = std::get<ZeusType::ObjectHandle>(objVar);
                auto* objPtr = getValidObject(objectHandle);
                if (!objPtr) {
                    throw std::runtime_error("getattr: invalid or stale object handle");
                }
                
                auto& obj = *objPtr;
                if (hasObjectField(obj, attrName)) {
                    ret = readObjectField(obj, attrName);
                } else {
                    throw std::runtime_error("getattr: attribute '" + attrName + "' not found");
                }
            }
            else if (ctx.desc->name == "__objget") {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                // __objget(obj, "field", "key") -> value
                // 直接从obj.field字典中获取key的值，不拷贝整个字典
                // 用法：__objget(this, "data", "name") 等价于 this.data["name"]
                if (args.size() != 3) {
                    throw std::runtime_error("__objget expects 3 arguments");
                }
                
                auto& objVar = args[0];
                
                // 提取字段名
                std::string fieldName;
                if (std::holds_alternative<std::string>(args[1])) {
                    fieldName = std::get<std::string>(args[1]);
                } else if (std::holds_alternative<LexerType::Value>(args[1])) {
                    auto& val = std::get<LexerType::Value>(args[1]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        fieldName = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("__objget: field name must be a string");
                    }
                } else {
                    throw std::runtime_error("__objget: field name must be a string");
                }
                
                // 提取key
                std::string keyName;
                if (std::holds_alternative<std::string>(args[2])) {
                    keyName = std::get<std::string>(args[2]);
                } else if (std::holds_alternative<LexerType::Value>(args[2])) {
                    auto& val = std::get<LexerType::Value>(args[2]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        keyName = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("__objget: key must be a string");
                    }
                } else {
                    throw std::runtime_error("__objget: key must be a string");
                }
                
                if (!std::holds_alternative<ZeusType::ObjectHandle>(objVar)) {
                    throw std::runtime_error("__objget: first argument must be an object");
                }
                
                const auto& objectHandle = std::get<ZeusType::ObjectHandle>(objVar);
                auto* objPtr = getValidObject(objectHandle);
                if (!objPtr) {
                    throw std::runtime_error("__objget: invalid or stale object handle");
                }
                
                auto& obj = *objPtr;
                if (!hasObjectField(obj, fieldName)) {
                    throw std::runtime_error("__objget: field '" + fieldName + "' not found");
                }
                auto fieldValue = readObjectField(obj, fieldName);
                
                // field应该是Variant(Object)类型
                if (!std::holds_alternative<ZeusType::Variant>(fieldValue)) {
                    throw std::runtime_error("__objget: field '" + fieldName + "' is not a dictionary");
                }
                
                const auto& variant = std::get<ZeusType::Variant>(fieldValue);
                if (!std::holds_alternative<ZeusType::Object>(variant)) {
                    throw std::runtime_error("__objget: field '" + fieldName + "' is not a dictionary object");
                }
                
                const auto& dictObj = std::get<ZeusType::Object>(variant);
                if (!dictObj.impl) {
                    throw std::runtime_error("__objget: dictionary has null impl");
                }
                
                // 从字典中获取key
                auto keyIt = dictObj.impl->data.find(keyName);
                if (keyIt != dictObj.impl->data.end()) {
                    ret = keyIt->second;
                } else {
                    ret = LexerType::Value(0);  // 返回nil如果key不存在
                }
            }
            else if (ctx.desc->name == "__objset") {
                std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
                // __objset(obj, "field", "key", value) -> null
                // 直接设置obj.field字典中key的值，不拷贝整个字典
                // 用法：__objset(this, "data", "name", "value") 等价于 this.data["name"] = "value"
                if (args.size() != 4) {
                    throw std::runtime_error("__objset expects 4 arguments");
                }
                
                auto& objVar = args[0];
                
                // 提取字段名
                std::string fieldName;
                if (std::holds_alternative<std::string>(args[1])) {
                    fieldName = std::get<std::string>(args[1]);
                } else if (std::holds_alternative<LexerType::Value>(args[1])) {
                    auto& val = std::get<LexerType::Value>(args[1]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        fieldName = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("__objset: field name must be a string");
                    }
                } else {
                    throw std::runtime_error("__objset: field name must be a string");
                }
                
                // 提取key
                std::string keyName;
                if (std::holds_alternative<std::string>(args[2])) {
                    keyName = std::get<std::string>(args[2]);
                } else if (std::holds_alternative<LexerType::Value>(args[2])) {
                    auto& val = std::get<LexerType::Value>(args[2]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        keyName = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("__objset: key must be a string");
                    }
                } else {
                    throw std::runtime_error("__objset: key must be a string");
                }
                
                auto& value = args[3];
                
                if (!std::holds_alternative<ZeusType::ObjectHandle>(objVar)) {
                    throw std::runtime_error("__objset: first argument must be an object");
                }
                
                const auto& objectHandle = std::get<ZeusType::ObjectHandle>(objVar);
                auto* objPtr = getValidObject(objectHandle);
                if (!objPtr) {
                    throw std::runtime_error("__objset: invalid or stale object handle");
                }
                
                auto& obj = *objPtr;
                if (!hasObjectField(obj, fieldName)) {
                    throw std::runtime_error("__objset: field '" + fieldName + "' not found");
                }
                auto fieldValue = readObjectField(obj, fieldName);
                
                // field应该是Variant(Object)类型
                if (!std::holds_alternative<ZeusType::Variant>(fieldValue)) {
                    throw std::runtime_error("__objset: field '" + fieldName + "' is not a dictionary");
                }
                
                auto& variant = std::get<ZeusType::Variant>(fieldValue);
                if (!std::holds_alternative<ZeusType::Object>(variant)) {
                    throw std::runtime_error("__objset: field '" + fieldName + "' is not a dictionary object");
                }
                
                auto& dictObj = std::get<ZeusType::Object>(variant);
                if (!dictObj.impl) {
                    throw std::runtime_error("__objset: dictionary has null impl");
                }
                
                // 设置字典中的key
                dictObj.impl->setValue(keyName, value);
                ret = LexerType::Value(0);  // 返回nil
            }
            else if (ctx.desc->name == "setattr") {
                // setattr(obj, "attrName", value) -> null
                if (args.size() != 3) {
                    throw std::runtime_error("setattr expects 3 arguments");
                }
                
                auto& objVar = args[0];
                
                // 提取属性名(可能是string或LexerType::Value包装的string)
                std::string attrName;
                if (std::holds_alternative<std::string>(args[1])) {
                    attrName = std::get<std::string>(args[1]);
                } else if (std::holds_alternative<LexerType::Value>(args[1])) {
                    auto& val = std::get<LexerType::Value>(args[1]);
                    if (std::holds_alternative<std::string>(val.data)) {
                        attrName = std::get<std::string>(val.data);
                    } else {
                        throw std::runtime_error("setattr: attribute name must be a string");
                    }
                } else {
                    throw std::runtime_error("setattr: attribute name must be a string");
                }
                
                auto& value = args[2];
                
                if (!std::holds_alternative<ZeusType::ObjectHandle>(objVar)) {
                    throw std::runtime_error("setattr: first argument must be an object");
                }
                
                const auto& objectHandle = std::get<ZeusType::ObjectHandle>(objVar);
                auto* objPtr = getValidObject(objectHandle);
                if (!objPtr) {
                    throw std::runtime_error("setattr: invalid or stale object handle");
                }
                
                auto& obj = *objPtr;
                
                writeObjectField(obj, attrName, value);
                
                ret = LexerType::Value(0); // return null
            }
            else if (ctx.desc->name == "__string_encode") {
                // __string_encode(string, encoding="utf-8")
                if (args.empty()) {
                    throw std::runtime_error("__string_encode expects at least 1 argument");
                }

                LexerType::Value strVal(0);
                if (!ZeusVMUtils::convertToValue(args[0], strVal) || strVal.getStringType() != "string") {
                    throw std::runtime_error("__string_encode: first argument must be string");
                }

                const std::string& str = std::get<std::string>(strVal.data);
                ZeusType::Bytes bytes;

                for (char c : str) {
                    bytes.data.push_back(static_cast<uint8_t>(c));
                }

                ret = ZeusType::Variant(bytes);
            }
            // Bytes相关函数已移至ZeusBuiltins.hpp统一实现
            // Socket相关函数已移至ZeusBuiltins.hpp统一实现
            // 所有内置函数现在统一通过注册表调用
            else {
                // 普通内置函数,调用注册的函数指针
                publishVMRoots();
                ret = ctx.desc->fn(args);

                if (hasArrayArgumentSnapshot) {
                    if (builtinName == "__array_push" && args.size() == 2) {
                        for (size_t i = 0; i < arrayArgumentOwnerCount; ++i) {
                            ZeusGC::retainVariantDeep(*this, args[1]);
                        }
                    } else if (builtinName == "__array_set" && args.size() == 3) {
                        int index = 0;
                        ZeusVMUtils::tryGetInt(args[1], index);
                        if (index >= 0 && index < static_cast<int>(arrayValuesBeforeCall.size())) {
                            for (size_t i = 0; i < arrayArgumentOwnerCount; ++i) {
                                ZeusGC::retainVariantDeep(*this, args[2]);
                                ZeusGC::releaseVariantDeep(
                                    *this, arrayValuesBeforeCall[index]);
                            }
                        }
                    } else if (builtinName == "__array_clear") {
                        for (size_t i = 0; i < arrayArgumentOwnerCount; ++i) {
                            for (const auto& value : arrayValuesBeforeCall) {
                                ZeusGC::releaseVariantDeep(*this, value);
                            }
                        }
                    } else if (builtinName == "__array_pop" &&
                               !arrayValuesBeforeCall.empty()) {
                        ZeusGC::retainVariantDeep(*this, ret);
                        for (size_t i = 0; i < arrayArgumentOwnerCount; ++i) {
                            ZeusGC::releaseVariantDeep(
                                *this, arrayValuesBeforeCall.back());
                        }
                    }
                } else if (builtinName == "__array_get" || builtinName == "__array_slice") {
                    // These builtins return values copied out of array storage.
                    ZeusGC::retainVariantDeep(*this, ret);
                }
            }
        }
        catch (const std::bad_alloc&) {
            throw;
        }
        catch (const std::exception& e) {
            IRType::IRInstr errInstr{VMOperator::CALL_NATIVE, {}};
            errInstr.line = __line;
            errInstr.sourceFileId = __sourceFileId;
            VMException::VMException exp(std::string("Native exception in ") + ctx.desc->name + ": " + e.what(), __line, errInstr);
            exp._throw(vmstat, currentChunk);
        }

        return ret;
    }

    void cleanupNativeCall(const NativeCallContext& ctx, ZeusType::UniversalVariant ret) {
        // Drop callee + args (stack-owned) and push return value.
        size_t newSize = stack.size() - static_cast<size_t>(ctx.argc + 1);
        shrinkStackWithRelease(newSize);
        stack.push_back(std::move(ret));
    }

    public:



    // 参数原型与闭包捕获描述
    struct CaptureDesc { 
        std::string fromKind; 
        int         srcIndex; 
        int         dstIndex;
        std::string name; 
        int modeFlag = 0; /*0 copy 1 ref*/ 
    };


    struct Prototype { 
        std::vector<IRType::IRInstr> body; 
        std::vector<CaptureDesc> captures; 
        std::string functionName;
        std::string ownerType;
        std::vector<std::string> paramTypes;
        std::vector<std::string> typeParameters;
        std::vector<bool> paramIsReference;
        bool isStaticMethod = false;
        int paramCount = 0;
        std::vector<std::string> paramNames; // 参数名列表(用于命名参数匹配)
        bool hasVariadicArgs = false;   // 是否有*args
        bool hasVariadicKwargs = false; // 是否有**kwargs
        std::string variadicArgsName;   // *args的参数名
        std::string variadicKwargsName; // **kwargs的参数名
        std::vector<bool> paramHasDefault; // defaults are evaluated by bytecode at call time
        std::vector<ZeusType::UniversalVariant> paramDefaultValues; // legacy literal defaults
        bool hasRuntimeDefaultPrologue = false;
        int minRequiredParams = -1; // 最少必需参数数量(-1表示未设置,默认所有参数都是必需的)
    };



    struct Closure { 
        int protoId; 
        std::vector<Cell*> upvalues; 
        std::optional<ZeusType::ObjectHandle> boundThis; // 绑定的this对象（用于方法引用）
        int refCount = 1;  // 引用计数（初始为1）
        bool autoRelease = false; // 是否应在调用后自动释放（例如临时绑定的实例方法）
        
        // 增加引用计数
        void retain() { ++refCount; }
        
        // 减少引用计数，返回是否应该释放
        bool release() { return --refCount <= 0; }
        
        // 检查是否已释放
        bool isFreed() const { return refCount <= 0; }
    };

    // Closure edges must be detached before their captured values are
    // released. A captured container can point back to this or another closure.
    struct ClosureReleasePayload {
        std::vector<Cell*> upvalues;
        std::optional<ZeusType::ObjectHandle> boundThis;
    };


    // 调用栈帧：用于非递归函数调用
    struct CallFrame {
        const std::vector<IRType::IRInstr>* code;                     // 执行的代码块
        size_t                              pc;                       // 程序计数器
        std::vector<Cell*>                  savedLocal;               // 保存的局部变量
        std::vector<Cell*>                  savedUpvalue;             // 保存的upvalue
        size_t                              stackBase;                // 栈基址（用于返回时恢复）
        int                                 savedLine;                // 保存的行号
        int                                 calleeAutoReleaseId = -1; // 如果为函数调用传递的临时闭包, 返回时自动 release id
        std::vector<bool>                   paramProvided;           // supplied positional/named arguments for default prologue
        std::vector<bool>                   referenceParameterSlots;
        std::optional<ZeusType::ObjectHandle> constructorResult;
        size_t                              pendingObjectCloneId = 0;
        int                                 debugLastSourceFileId = -1;
        int                                 debugLastLine = -1;
        size_t                              debugLastPc = 0;
        bool                                debugHasSourcePosition = false;
    };

    struct PendingObjectClone {
        NativeCallContext nativeCall;
        std::unique_ptr<ZeusVMCopy::Context<VM>> copyContext;
    };

    std::vector<Prototype> prototypes; // 为外部函数/闭包注册的原型列表
    size_t                 prototypesCount = 0;        // 原型数量（调试用）
    std::vector<int>       chunkToProtoId;   // chunkIndex -> protoId（若无则为 -1）
    std::unordered_map<std::string, int> functionNameToProtoId;
    std::vector<Closure>   closures;     // 已创建的闭包集合
    size_t                 closuresCount = 0;          // 闭包计数器（用于统计/调试）
    size_t                 freeClosureSlotsCount = 0;  // 空闲闭包槽计数（用于统计/调试）
    std::vector<CallFrame> callFrames; // 调用栈（支持深度递归）
    std::unordered_map<size_t, PendingObjectClone> pendingObjectClones;
    size_t nextPendingObjectCloneId = 1;
    
    // ===== 异常处理栈 =====
    struct ExceptionHandler {
        size_t catchPc;                    // catch 块的起始 PC
        size_t callFrameDepth;             // 注册时的调用栈深度
        const std::vector<IRType::IRInstr>* code;  // 当前代码块
        size_t stackBase;                  // 注册时的栈基址（用于恢复）
        size_t localSize;                  // 注册时的局部变量数（用于恢复）
        // 注册时的 with 块深度：异常展开时要把超出的 with 块逐个退出，
        // 否则块内的 operator[~with] 永远不会执行（线程不汇合、句柄不关闭）。
        size_t withDepth = 0;
    };
    std::vector<ExceptionHandler> exceptionHandlers;  // 异常处理器栈
    ZeusType::UniversalVariant currentException;      // 当前捕获的异常
    bool hasUnhandledException = false;               // 是否有未处理的异常
    
    // ===== with 对象栈 =====
    struct WithContext {
        ZeusType::ObjectHandle objHandle;  // with 对象的句柄
        bool hasExitMethod;                // 是否有 operator[~with] 方法
    };
    std::vector<WithContext> withContextStack;  // with 对象栈
    
    // 类/对象/捕获等运行时信息集合
    std::vector<ZeusType::ClassInfo> classes;
    std::unordered_map<std::string, ZeusType::ClassInfo> typeImplementations;
    std::deque<ZeusType::ObjectInstance> objects;  // 使用 deque 避免重分配时指针失效
    mutable std::shared_mutex objectsMutex;  // 读写锁，用于线程安全的对象访问
    std::vector<ZeusType::NamespaceData> namespaces;  // 包命名空间集合
    
    // GC: 空闲槽位列表（用于复用已释放的槽位）
    std::vector<int> freeObjectSlots;   // 已释放的对象槽位
    size_t freeObjectSlotsCount = 0;     // 空闲对象槽位计数（调试用）
    std::vector<int> freeClosureSlots;  // 已释放的闭包槽位
    mutable std::shared_mutex closuresMutex;  // 读写锁，用于线程安全的闭包访问
    
    // 共享对象系统（用于线程访问主 VM 的对象）
    std::vector<ZeusType::ClassInfo>* sharedClasses = nullptr;
    std::deque<ZeusType::ObjectInstance>* sharedObjects = nullptr;  // 使用 deque 指针
    mutable std::shared_mutex* sharedObjectsMutex = nullptr;  // 共享读写锁
    std::vector<int>* sharedFreeObjectSlots = nullptr;  // GC: 共享的空闲槽位
    
    // 共享原型系统（子线程使用指针引用主 VM 的原型，避免复制）
    std::vector<Prototype>* sharedPrototypes = nullptr;
    std::vector<std::string>* sharedGlobalNames = nullptr;
    
    // 共享闭包和名称映射（子线程使用指针引用，避免复制大型数据结构）
    std::vector<Closure>* sharedClosures = nullptr;
    std::vector<int>* sharedFreeClosureSlots = nullptr;  // GC: 共享的闭包空闲槽位
    std::shared_mutex* sharedClosuresMutex = nullptr;    // 用于同步闭包访问
    std::unordered_map<std::string,int>* sharedNameToClosure = nullptr;
    std::unordered_map<std::string,int>* sharedNameToClass = nullptr;
    
    public: bool enableLateResolve = true;     // 是否启用延迟解析（允许在运行时通过名字解析闭包/类）
    std::unordered_map<std::string,int> nameToClosure; // 名字->闭包句柄映射，用于延迟解析
    std::unordered_map<std::string,int> nameToClass;   // 名字->类ID映射，用于class的延迟解析（向前引用）
    // TODO: 预留空间 - 未来可添加 declaredClasses 集合来支持 --strict-decl 模式下的 class 声明检查
    // std::unordered_set<std::string> declaredClasses; // 已声明的类名集合（用于 decl class Name; 语法）
    std::vector<std::string> globalNames; // 全局槽索引 -> 名称
    ZeusDebug::BreakpointConfig breakpointConfig;
    ZeusDebug::VMSnapshotWriter snapshotWriter;

    public:
    void setGlobalNames(const std::vector<std::string>& names) { 
        globalNames = names;
        // Pre-allocate global slots to match globalNames size
        if (global.size() < globalNames.size()) {
            global.resize(globalNames.size(), nullptr);
        }
        ZEUS_DEBUG_PRINT("[VM] setGlobalNames: %zu names, global.size()=%zu", globalNames.size(), global.size());
        for (size_t i = 0; i < globalNames.size() && i < 20; ++i) {
            ZEUS_DEBUG_PRINT("[VM]   global[%zu] = %s", i, globalNames[i].c_str());
        }
    }

    void setGlobalTeardownLayout(
        size_t packageCount,
        size_t programStart,
        size_t programEnd
    ) {
        if (packageCount > programStart || programStart > programEnd ||
            programEnd > global.size()) {
            throw std::invalid_argument("invalid VM global teardown layout");
        }
        packageGlobalCount = packageCount;
        mainGlobalStart = programStart;
        mainGlobalEnd = programEnd;
        teardownLayoutGlobalCount = global.size();
        hasGlobalTeardownLayout = true;
    }

    void configureBreakpoints(
        const ZeusDebug::BreakpointConfig& config,
        const std::string& sourcePath
    ) {
        breakpointConfig = config;
        if (!breakpointConfig.enabled()) return;
        if (breakpointConfig.sourcePath.empty()) {
            breakpointConfig.sourcePath = sourcePath;
        }
        if (breakpointConfig.snapshotDirectory.empty()) {
            breakpointConfig.snapshotDirectory =
                ZeusDebug::defaultSnapshotDirectory(breakpointConfig.sourcePath);
        }
        snapshotWriter.setDirectory(breakpointConfig.snapshotDirectory);
    }

    // 调试用：统计字典相关操作的配对与写入次数
    size_t debugDictInitialPairs = 0; // 最近一次 DICT_BUILD 的 pair 数（用于调试统计）
    size_t debugDictStoreCount   = 0; // DICT_STORE_VALUE 执行次数（用于调试统计）

    // 辅助函数：获取对象实例（优先使用共享系统）
    ZeusType::ObjectInstance* getObject(int oid) {
        // 先检查共享对象系统
        if (sharedObjects && sharedObjectsMutex) {
            std::shared_lock<std::shared_mutex> lock(*sharedObjectsMutex);
            if (oid >= 0 && oid < (int)sharedObjects->size()) {
                return &(*sharedObjects)[oid];
            }
        } else if (sharedObjects) {
            if (oid >= 0 && oid < (int)sharedObjects->size()) {
                return &(*sharedObjects)[oid];
            }
        }
        
        // 再检查本地对象
        if (oid >= 0 && oid < (int)objects.size()) {
            return &objects[oid];
        }
        
        return nullptr;
    }
    
    // 验证 ObjectHandle 是否仍然有效（检查 generation 匹配）
    // 返回对象指针，如果 handle 无效则返回 nullptr
    ZeusType::ObjectInstance* getValidObject(const ZeusType::ObjectHandle& handle) {
        auto* obj = getObject(handle.id);

        if (!obj) return nullptr;

        // What counts as a live handle is decided in ZeusVMUtils, so the shared
        // heap path and this one cannot drift apart. Only the reporting is
        // local, because only this path can name the slot it looked in.
        if (!ZeusVMUtils::isValidObjectHandle(*obj, handle)) {
            ZEUS_DEBUG_PRINT("[GC] Stale object reference: handle.id=%d handle.gen=%d obj.gen=%d",
                             handle.id, handle.generation, obj->generation);
            return nullptr;
        }

        return obj;
    }
    
    // 辅助函数：获取类信息（优先使用共享系统）
    ZeusType::ClassInfo* getClass(int classId) {
        if (sharedClasses && classId >= 0 && classId < (int)sharedClasses->size()) {
            return &(*sharedClasses)[classId];
        }
        
        if (classId >= 0 && classId < (int)classes.size()) {
            return &classes[classId];
        }
        
        return nullptr;
    }

    bool hasObjectField(
        const ZeusType::ObjectInstance& object,
        const std::string& name
    ) {
        const auto* classInfo = getClass(object.classId);
        return classInfo
            ? ZeusType::objectHasField(object, *classInfo, name)
            : object.extensionValues.find(name) != object.extensionValues.end();
    }

    ZeusType::UniversalVariant readObjectField(
        const ZeusType::ObjectInstance& object,
        const std::string& name
    ) {
        const auto* classInfo = getClass(object.classId);
        if (classInfo) return ZeusType::readObjectField(object, *classInfo, name);
        const auto field = object.extensionValues.find(name);
        if (field == object.extensionValues.end()) {
            throw std::out_of_range("object field not found: " + name);
        }
        return field->second;
    }

    void writeObjectField(
        ZeusType::ObjectInstance& object,
        const std::string& name,
        const ZeusType::UniversalVariant& value
    ) {
        const auto* classInfo = getClass(object.classId);
        if (classInfo) {
            const auto fieldType = classInfo->explicitFieldTypes.find(name);
            if (fieldType != classInfo->explicitFieldTypes.end()) {
                if (ZeusBuiltins::findNativeScalarCanonical(fieldType->second)) {
                    ZeusType::writeObjectField(object, *classInfo, name,
                        normalizeNativeScalarValue(fieldType->second, value));
                    return;
                }
            }
            ZeusType::writeObjectField(object, *classInfo, name, value);
            return;
        }
        object.extensionValues[name] = value;
    }

    std::vector<std::pair<std::string, ZeusType::UniversalVariant>>
    snapshotObjectFields(const ZeusType::ObjectInstance& object) {
        const auto* classInfo = getClass(object.classId);
        if (classInfo) return ZeusType::snapshotObjectFields(object, *classInfo);
        return std::vector<std::pair<std::string, ZeusType::UniversalVariant>>(
            object.extensionValues.begin(), object.extensionValues.end());
    }

    // Turns a source-level field default into the runtime value a fresh
    // instance starts with.
    //
    // The IR generator already classifies every literal default it can express,
    // so the VM reads that description rather than re-deriving the value from
    // the class object. The class object's copy is only trustworthy for a class
    // compiled into the main chunk: a class arriving from an imported package
    // carries unrelated values there, so relying on it silently produced
    // garbage field values for every package class.
    //
    // Returns false for a default the generator could not classify, which
    // includes every field that has an initializer function because those are
    // computed at construction time. The caller then leaves the existing
    // default in place.
    static bool classFieldDefaultValue(
        const IRType::ClassFieldDefault& declared,
        ZeusType::UniversalVariant& out
    ) {
        switch (declared.kind) {
            case IRType::ClassFieldDefaultKind::Integer:
                out = LexerType::Value(
                    ZeusType::BigInteger(declared.integerValue));
                return true;
            case IRType::ClassFieldDefaultKind::Boolean:
                out = LexerType::Value(declared.booleanValue);
                return true;
            case IRType::ClassFieldDefaultKind::String:
                out = LexerType::Value(declared.stringValue);
                return true;
            case IRType::ClassFieldDefaultKind::Bytes: {
                ZeusType::Bytes bytes;
                bytes.data.assign(
                    declared.bytesValue.begin(), declared.bytesValue.end());
                out = ZeusType::Variant(std::move(bytes));
                return true;
            }
            case IRType::ClassFieldDefaultKind::Null:
                out = ZeusType::Variant(ZeusType::Null());
                return true;
            case IRType::ClassFieldDefaultKind::EmptyArray: {
                ZeusType::Array array;
                array.impl = ZeusGC::makeArrayData();
                out = ZeusType::Variant(std::move(array));
                return true;
            }
            case IRType::ClassFieldDefaultKind::EmptyDict: {
                ZeusType::Object dictionary;
                dictionary.impl = ZeusGC::makeObjectData();
                out = ZeusType::Variant(std::move(dictionary));
                return true;
            }
            case IRType::ClassFieldDefaultKind::Zero:
            case IRType::ClassFieldDefaultKind::Unsupported:
                break;
        }
        return false;
    }

    bool applyClassFieldTypeMetadata(
        const IRType::IRMetadata& metadata,
        const std::string& className,
        ZeusType::ClassInfo& info,
        std::string& error
    ) {
        if (metadata.kind != IRType::IRMetadataKind::ClassLayout) return true;

        std::string metadataClassName;
        std::vector<std::string> fieldNames;
        std::vector<std::string> fieldTypes;
        std::vector<bool> constFields;
        std::vector<IRType::ClassFieldDefault> defaults;
        std::vector<std::string> initializerNames;
        std::vector<bool> explicitTypes;
        if (!metadata.readClassLayout(
                metadataClassName, fieldNames, fieldTypes,
                &constFields, &defaults, nullptr, &explicitTypes,
                &initializerNames) ||
            metadataClassName != className) {
            error = "invalid field type metadata for class '" + className + "'";
            return false;
        }

        info.fieldOrder = fieldNames;
        for (size_t index = 0; index < fieldNames.size(); ++index) {
            ZeusType::FieldDef& field = info.fields[fieldNames[index]];
            if (!constFields.empty()) field.isConst = constFields[index];
            if (!explicitTypes.empty() && explicitTypes[index] &&
                !fieldTypes[index].empty()) {
                info.explicitFieldTypes[fieldNames[index]] = fieldTypes[index];
            }

            // A field with an initializer function is filled in at construction
            // time, so the literal default recorded beside it never applies.
            const bool hasInitializer =
                !initializerNames.empty() && !initializerNames[index].empty();
            if (!hasInitializer && index < defaults.size()) {
                ZeusType::UniversalVariant declaredDefault;
                if (classFieldDefaultValue(defaults[index], declaredDefault)) {
                    field.defaultValue = std::move(declaredDefault);
                }
            }

            if (hasInitializer) {
                const auto proto = functionNameToProtoId.find(initializerNames[index]);
                if (proto == functionNameToProtoId.end()) {
                    error = "field initializer function '" + initializerNames[index] +
                        "' was not registered for class '" + className + "'";
                    return false;
                }
                const Prototype* initializer = getPrototype(proto->second);
                if (!initializer || initializer->paramCount != 1 ||
                    !initializer->captures.empty()) {
                    error = "invalid field initializer prototype '" +
                        initializerNames[index] + "' for class '" + className + "'";
                    return false;
                }
                field.initializerProtoId = proto->second;
                info.fieldInitializerNames[fieldNames[index]] = initializerNames[index];
            }
        }
        ZeusType::rebuildClassFieldIndices(info);
        return true;
    }

    bool runFieldInitializer(
        int protoId,
        int classId,
        const ZeusType::UniversalVariant& receiver,
        const ZeusType::ObjectHandle& objectHandle,
        const std::string& className,
        const std::string& fieldName,
        const std::shared_ptr<ZeusType::ObjectData>& sharedData,
        std::string& error
    ) {
        const Prototype* initializer = getPrototype(protoId);
        if (!initializer || initializer->paramCount != 1 ||
            !initializer->captures.empty()) {
            error = "invalid field initializer prototype for '" + className +
                "." + fieldName + "'";
            return false;
        }

        const auto* classInfo = getClass(classId);
        if (!classInfo) {
            error = "field initializer class is unavailable: '" + className + "'";
            return false;
        }
        std::string expectedType;
        const auto expectedField = classInfo->explicitFieldTypes.find(fieldName);
        if (expectedField != classInfo->explicitFieldTypes.end()) {
            expectedType = expectedField->second;
        }

        Closure closure;
        closure.protoId = protoId;
        const int closureId = allocClosure(std::move(closure));
        const size_t stackBase = stack.size();
        stack.push_back(ZeusType::ClosureHandle{closureId});
        if (std::holds_alternative<ZeusType::ObjectHandle>(receiver)) {
            retainObject(std::get<ZeusType::ObjectHandle>(receiver));
        } else {
            ZeusGC::retainVariantDeep(*this, receiver);
        }
        stack.push_back(receiver);

        if (!executeClosureSync(closureId, 1)) {
            shrinkStackWithRelease(stackBase);
            if (vmstat == ZeusType::VMStatus::Running) {
                error = "field initializer failed for '" + className + "." + fieldName + "'";
            }
            return false;
        }

        StackItem result = popStackItem();
        if (!expectedType.empty() &&
            !isValueCompatibleWithType(result.value, expectedType)) {
            const std::string actualType = runtimeValueTypeName(result.value);
            dropStackItem(result);
            error = "TypeError: field '" + className + "." + fieldName +
                "' expects '" + expectedType + "', got '" + actualType + "'";
            return false;
        }

        ZeusType::ObjectInstance* object = getMutableObject(objectHandle.id);
        if (!object || object->generation != objectHandle.generation ||
            object->classId != classId) {
            dropStackItem(result);
            error = "field initializer receiver became invalid for '" +
                className + "." + fieldName + "'";
            return false;
        }

        if (!hasObjectField(*object, fieldName)) {
            dropStackItem(result);
            error = "field initializer target is missing: '" +
                className + "." + fieldName + "'";
            return false;
        }
        auto stored = readObjectField(*object, fieldName);
        ZeusGC::retainVariantDeep(*this, result.value);
        try {
            writeObjectField(*object, fieldName, result.value);
        } catch (const std::exception& exception) {
            ZeusGC::releaseVariantDeep(*this, result.value);
            dropStackItem(result);
            error = "field initializer could not store '" + className + "." +
                fieldName + "': " + exception.what();
            return false;
        }
        ZeusGC::releaseVariantDeep(*this, stored);

        if (sharedData) {
            sharedData->setValue(fieldName, readObjectField(*object, fieldName));
        }
        dropStackItem(result);
        return true;
    }

    std::string serializedSharedFieldType(
        const ZeusType::SharedObjectHandle& handle,
        const std::string& fieldName
    ) const {
        auto cell = ZeusShared::getGlobalHeap().find(
            handle.name, handle.generation);
        if (!cell) return {};

        const ZeusType::UniversalVariant value = cell->read();
        const auto* variant = std::get_if<ZeusType::Variant>(&value);
        if (!variant) return {};
        const auto* object = std::get_if<ZeusType::Object>(variant);
        if (!object || !object->impl) return {};

        const auto fieldType = object->impl->data.find(
            "__field_type__" + fieldName);
        if (fieldType == object->impl->data.end()) return {};
        const auto* literal = std::get_if<LexerType::Value>(&fieldType->second);
        if (!literal) return {};
        const auto* typeName = std::get_if<std::string>(&literal->data);
        return typeName ? *typeName : std::string();
    }

    static bool mergeTypeImplementationMethods(
        ZeusType::ClassInfo& destination,
        const ZeusType::ClassInfo& source,
        std::string& conflict
    ) {
        const auto merge = [&](auto& target, const auto& additions) {
            for (const auto& entry : additions) {
                if (!target.emplace(entry.first, entry.second).second) {
                    conflict = entry.first;
                    return false;
                }
            }
            return true;
        };
        return merge(destination.instanceMethods, source.instanceMethods) &&
               merge(destination.staticMethods, source.staticMethods) &&
               merge(destination.operatorMethods, source.operatorMethods);
    }

    std::string dictionaryKeyString(
        const ZeusType::UniversalVariant& value
    ) {
        auto primitiveString = [](const ZeusType::UniversalVariant& candidate,
                                  std::string& result) -> bool {
            if (const auto* literal =
                    std::get_if<LexerType::Value>(&candidate)) {
                if (const auto* text = std::get_if<std::string>(&literal->data)) {
                    result = *text;
                    return true;
                }
                if (const auto* integer =
                        std::get_if<ZeusType::BigInteger>(&literal->data)) {
                    result = integer->toString();
                    return true;
                }
                if (const auto* decimal =
                        std::get_if<ZeusType::BigDecimal>(&literal->data)) {
                    result = decimal->toString();
                    return true;
                }
                if (const auto* boolean = std::get_if<bool>(&literal->data)) {
                    result = *boolean ? "true" : "false";
                    return true;
                }
            }

            if (const auto* text = std::get_if<std::string>(&candidate)) {
                result = *text;
                return true;
            }
            if (const auto* integer =
                    std::get_if<ZeusType::BigInteger>(&candidate)) {
                result = integer->toString();
                return true;
            }
            if (const auto* decimal =
                    std::get_if<ZeusType::BigDecimal>(&candidate)) {
                result = decimal->toString();
                return true;
            }
            if (const auto* boolean = std::get_if<bool>(&candidate)) {
                result = *boolean ? "true" : "false";
                return true;
            }
            if (const auto* character = std::get_if<char>(&candidate)) {
                result.assign(1, *character);
                return true;
            }
            return false;
        };

        std::string result;
        if (primitiveString(value, result)) return result;

        if (const auto* handle = std::get_if<ZeusType::ObjectHandle>(&value)) {
            auto* object = getValidObject(*handle);
            if (object) {
                auto* classInfo = getClass(object->classId);
                if (classInfo && classInfo->name == "String") {
                    if (hasObjectField(*object, "value") &&
                        primitiveString(readObjectField(*object, "value"), result)) {
                        return result;
                    }
                }
            }
        }

        return "<obj>";
    }
    
    // 辅助函数：获取原型（优先使用共享系统）
    const Prototype* getPrototype(int protoId) const {
        if (sharedPrototypes && protoId >= 0 && protoId < (int)sharedPrototypes->size()) {
            return &(*sharedPrototypes)[protoId];
        }
        
        if (protoId >= 0 && protoId < (int)prototypes.size()) {
            return &prototypes[protoId];
        }
        
        return nullptr;
    }
    
    // 辅助函数：获取原型数量
    size_t getPrototypeCount() const {
        if (sharedPrototypes) return sharedPrototypes->size();
        return prototypes.size();
    }
    
    // 辅助函数：获取全局名称
    const std::string& getGlobalName(int index) const {
        if (sharedGlobalNames && index >= 0 && index < (int)sharedGlobalNames->size()) {
            return (*sharedGlobalNames)[index];
        }
        
        if (index >= 0 && index < (int)globalNames.size()) {
            return globalNames[index];
        }
        
        static const std::string empty;
        return empty;
    }
    
    // 辅助函数：获取全局名称数量
    size_t getGlobalNameCount() const {
        if (sharedGlobalNames) return sharedGlobalNames->size();
        return globalNames.size();
    }
    
    // 辅助函数：获取对象数量
    size_t getObjectCount() const {
        if (sharedObjects) return sharedObjects->size();
        return objects.size();
    }
    
    // 辅助函数：获取类数量
    size_t getClassCount() const {
        if (sharedClasses) return sharedClasses->size();
        return classes.size();
    }
    
    // 辅助函数：获取闭包（优先使用共享系统，再查本地）
    // 注意：对于继承的槽位，应该使用 sharedClosures
    Closure* getClosure(int closureId) {
        // 先查共享（主线程的闭包）- 对于继承范围内的 ID
        if (sharedClosures && closureId >= 0 && closureId < (int)inheritedClosuresSize) {
            if (closureId < (int)sharedClosures->size()) {
                return &(*sharedClosures)[closureId];
            }
        }
        
        // 再查本地（子线程新创建的闭包）- 超出继承范围的 ID
        if (closureId >= 0 && closureId < (int)closures.size()) {
            return &closures[closureId];
        }
        
        // 回退：查共享（用于没有设置 inheritedClosuresSize 的情况）
        if (sharedClosures && closureId >= 0 && closureId < (int)sharedClosures->size()) {
            return &(*sharedClosures)[closureId];
        }
        
        return nullptr;
    }
    
    const Closure* getClosure(int closureId) const {
        // 先查共享（主线程的闭包）- 对于继承范围内的 ID
        if (sharedClosures && closureId >= 0 && closureId < (int)inheritedClosuresSize) {
            if (closureId < (int)sharedClosures->size()) {
                return &(*sharedClosures)[closureId];
            }
        }
        
        // 再查本地（子线程新创建的闭包）- 超出继承范围的 ID
        if (closureId >= 0 && closureId < (int)closures.size()) {
            return &closures[closureId];
        }
        
        // 回退：查共享
        if (sharedClosures && closureId >= 0 && closureId < (int)sharedClosures->size()) {
            return &(*sharedClosures)[closureId];
        }
        
        return nullptr;
    }
    
    // 辅助函数：获取闭包数量
    size_t getClosureCount() const {
        return closures.size();
    }
    
    // 辅助函数：查找闭包 ID（通过名称）
    int findClosureByName(const std::string& name) const {
        // 先查本地
        auto it = nameToClosure.find(name);
        if (it != nameToClosure.end()) {
            return it->second;
        }
        
        // 再查共享
        if (sharedNameToClosure) {
            auto sit = sharedNameToClosure->find(name);
            if (sit != sharedNameToClosure->end()) {
                return sit->second;
            }
        }
        
        return -1;
    }
    
    // 辅助函数：查找类 ID（通过名称）
    int findClassByName(const std::string& name) const {
        // 先查本地
        auto it = nameToClass.find(name);
        if (it != nameToClass.end()) {
            return it->second;
        }
        
        // 再查共享
        if (sharedNameToClass) {
            auto sit = sharedNameToClass->find(name);
            if (sit != sharedNameToClass->end()) {
                return sit->second;
            }
        }
        
        return -1;
    }

    // ========== GC: 对象引用计数管理 ==========
    
    // 获取对象的可变引用（支持共享对象系统）
    ZeusType::ObjectInstance* getMutableObject(int oid) {
        if (sharedObjects && oid >= 0 && oid < (int)sharedObjects->size()) {
            return &(*sharedObjects)[oid];
        }
        
        if (oid >= 0 && oid < (int)objects.size()) {
            return &objects[oid];
        }
        
        return nullptr;
    }
    
    // 检查对象是否在共享系统中
    bool isSharedObject(int oid) const {
        return sharedObjects && oid >= 0 && oid < (int)sharedObjects->size();
    }
    
    // 创建新对象，返回 ObjectHandle（包含 id 和 generation）
    // 优先复用空闲槽位，复用时递增 generation 以检测悬空引用
    ZeusType::ObjectHandle allocObject(ZeusType::ObjectInstance&& obj) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        if (auto* classInfo = getClass(obj.classId)) {
            if (classInfo->fieldIndices.size() != classInfo->fields.size() ||
                classInfo->fieldOrder.size() != classInfo->fields.size()) {
                ZeusType::rebuildClassFieldIndices(*classInfo);
            }
            ZeusType::initializeDeclaredFields(obj, *classInfo);
        }
        obj.refCount = 1;
        obj.markedForDeletion = false;
        obj.destructorCalled = false;
        obj.finalizing = false;
        
        // 如果有共享对象系统，在共享系统中分配
        if (sharedObjects && sharedObjectsMutex) {
            std::unique_lock<std::shared_mutex> lock(*sharedObjectsMutex);
            
            // 优先复用共享的空闲槽位
            if (sharedFreeObjectSlots && !sharedFreeObjectSlots->empty()) {
                int slot = sharedFreeObjectSlots->back();
                sharedFreeObjectSlots->pop_back();
                
                // 递增 generation 以使旧引用失效
                int newGen = (*sharedObjects)[slot].generation + 1;
                obj.generation = newGen;
                (*sharedObjects)[slot] = std::move(obj);
                
                ZEUS_DEBUG_PRINT("[GC] Reused shared object slot #%d (gen=%d)", slot, newGen);
                lock.unlock();
                const ZeusType::ObjectHandle handle{slot, newGen};
                pinVMObjectRoot(handle);
                return handle;
            }
            
            int objId = (int)sharedObjects->size();
            obj.generation = 0;
            sharedObjects->push_back(std::move(obj));
            ZEUS_DEBUG_PRINT("[GC] Allocated shared object #%d", objId);
            lock.unlock();
                const ZeusType::ObjectHandle handle{objId, 0};
                pinVMObjectRoot(handle);
                return handle;
        }
        
        // 使用写锁保护 objects 的修改（包括槽位复用和新分配）
        std::unique_lock<std::shared_mutex> lock(objectsMutex);
        
        // 尝试复用空闲槽位
        if (!freeObjectSlots.empty()) {
            int slot = freeObjectSlots.back();
            freeObjectSlots.pop_back();
            if (freeObjectSlotsCount > 0) freeObjectSlotsCount--;
            
            // 递增 generation 以使旧引用失效
            int newGen = objects[slot].generation + 1;
            obj.generation = newGen;
            objects[slot] = std::move(obj);
            
            ZEUS_DEBUG_PRINT("[GC] Reused object slot #%d (gen=%d)", slot, newGen);
            lock.unlock();
                const ZeusType::ObjectHandle handle{slot, newGen};
                pinVMObjectRoot(handle);
                return handle;
        }
        
        // 没有空闲槽位，分配新槽位
        int objId = (int)objects.size();
        obj.generation = 0;
        objects.push_back(std::move(obj));
        lock.unlock();
                const ZeusType::ObjectHandle handle{objId, 0};
                pinVMObjectRoot(handle);
                return handle;
    }
    
    // 释放 Cell 及其内容（如果内容是 ClosureHandle 或 ObjectHandle，也会释放）
    // 用于函数返回时清理 local 变量
    void releaseCellAndContent(Cell* cell) {
        if (!cell) return;

        // 仅当 Cell 即将被销毁（refCount==1）时，才释放其内容的引用。
        // 否则该 Cell 仍被其他持有者共享（例如多个 closure upvalue 槽），提前释放内容会造成悬垂引用。
        if (cell->refCount == 1) {
            const auto value = cell->readValue();
            ZeusGC::releaseVariantDeep(*this, value);
        }

        // 然后释放 Cell 本身（refCount--，到0时 delete）
        cell->release();
    }

    bool detachClosureForRelease(
        int closureId,
        ClosureReleasePayload& payload,
        bool force = false
    ) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::unique_lock<std::shared_mutex> lock(closuresMutex);
        Closure* closure = getClosure(closureId);
        if (!closure || closure->isFreed()) return false;

        ZEUS_DEBUG_PRINT("[GC] releaseClosure #%d: refCount=%d before release",
            closureId, closure->refCount);

        if (!force && !closure->release()) {
            ZEUS_DEBUG_PRINT("[GC] Closure #%d release -> refCount=%d",
                closureId, closure->refCount);
            return false;
        }

        if (isSharedClosure(closureId)) {
            closure->refCount = 1;
            ZEUS_DEBUG_PRINT("[GC] Closure #%d is shared; pinning (refCount=1)", closureId);
            return false;
        }

        // Mark it freed while holding the table lock, then release captured
        // values after unlocking. Recursive edges now observe a dead closure.
        payload.upvalues = std::move(closure->upvalues);
        payload.boundThis = std::move(closure->boundThis);
        closure->upvalues.clear();
        closure->boundThis.reset();
        closure->protoId = -1;
        closure->refCount = 0;
        if (closureId >= 0 && closureId < static_cast<int>(closures.size())) {
            freeClosureSlots.push_back(closureId);
            freeClosureSlotsCount++;
            ZEUS_DEBUG_PRINT("[GC] Added local closure slot #%d to free list (size=%zu)",
                closureId, freeClosureSlots.size());
        }
        return true;
    }

    void releaseClosurePayload(ClosureReleasePayload& payload) {
        for (Cell* cell : payload.upvalues) {
            if (cell) releaseCellAndContent(cell);
        }
        payload.upvalues.clear();
        if (payload.boundThis.has_value()) {
            releaseObject(*payload.boundThis);
            payload.boundThis.reset();
        }
    }

    // 增加对象引用计数
    void retainObject(int objId) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        auto* obj = getMutableObject(objId);
        
        if (obj && !obj->markedForDeletion) {
            obj->retain();
            pinVMObjectRoot(ZeusType::ObjectHandle{objId, obj->generation});
            ZEUS_DEBUG_PRINT("[GC] Object #%d retain -> refCount=%d", objId, obj->refCount);
            if (obj->classId == 20 || obj->classId == 17 || obj->classId == 18) {
                ZEUS_DEBUG_PRINT("[GC][TRACE] classId=%d retain -> refCount=%d", obj->classId, obj->refCount);
            }
        }
    }

    void retainObject(const ZeusType::ObjectHandle& handle) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        auto* obj = getMutableObject(handle.id);
        if (!obj || obj->generation != handle.generation ||
            obj->markedForDeletion || obj->classId < 0) {
            ZEUS_DEBUG_PRINT("[GC] Ignored stale object retain: id=%d handle.gen=%d",
                handle.id, handle.generation);
            return;
        }
        obj->retain();
        pinVMObjectRoot(handle);
        ZEUS_DEBUG_PRINT("[GC] Object #%d retain -> refCount=%d (gen=%d)",
            handle.id, obj->refCount, handle.generation);
    }

    // 释放对象中嵌套持有的其他对象和资源
    void releaseNestedObjects(ZeusType::ObjectInstance& obj) {
        const auto fields = snapshotObjectFields(obj);
        for (const auto& field : fields) {
            ZeusGC::releaseVariantDeep(*this, field.second);
            // 注意: SocketHandle 的实际资源清理应在 operator[~] 中进行
            // 这里只记录日志，不做强制清理（避免关闭未关闭的连接造成问题）
        }
    }

    // Invoke the interpreted destructor while the object and its fields are
    // still live. Both explicit delete and the last-reference path use this
    // function so operator[~] cannot run twice for one allocation.
    bool invokeObjectDestructor(const ZeusType::ObjectHandle& handle) {
        auto* object = getValidObject(handle);
        if (!object || object->destructorCalled) return object != nullptr;

        auto* classInfo = getClass(object->classId);
        if (!classInfo) return true;
        const auto destructor = classInfo->operatorMethods.find("[~]");
        if (destructor == classInfo->operatorMethods.end()) return true;

        const Prototype* prototype = getPrototype(destructor->second);
        if (!prototype) return true;

        object->destructorCalled = true;
        ZEUS_DEBUG_PRINT("[GC] Calling operator[~] for object #%d gen=%d protoId=%d",
            handle.id, handle.generation, destructor->second);

        Closure closure;
        closure.protoId = destructor->second;
        retainObject(handle); // closure owns boundThis until releaseClosure
        closure.boundThis = handle;

        Cell* thisCell = nullptr;
        if (!prototype->captures.empty()) {
            int maxDst = 0;
            for (const auto& capture : prototype->captures) {
                if (capture.dstIndex > maxDst) maxDst = capture.dstIndex;
            }
            closure.upvalues.resize(maxDst + 1, nullptr);
            thisCell = newCell(handle, 0, "OBJECT_FINALIZER");
            for (const auto& capture : prototype->captures) {
                if (capture.dstIndex >= 0 &&
                    static_cast<size_t>(capture.dstIndex) < closure.upvalues.size()) {
                    closure.upvalues[capture.dstIndex] = thisCell;
                    thisCell->retain();
                }
            }
        }

        const int closureId = allocClosure(std::move(closure));
        const size_t stackBase = stack.size();
        stack.push_back(ZeusType::ClosureHandle{closureId});
        struct ThisHoldGuard {
            VM* vm;
            ZeusType::ObjectHandle handle;
            ~ThisHoldGuard() {
                if (vm && handle.id >= 0) vm->releaseObject(handle);
            }
        } thisHold{this, handle};
        retainObject(handle);

        const bool completed = executeClosureSync(closureId, 0);
        if (stack.size() > stackBase) {
            shrinkStackWithRelease(stackBase);
        }
        if (thisCell) thisCell->release();
        releaseClosure(closureId);
        return completed;
    }
    
    // 强制打断对象的闭包循环引用（递归处理嵌套对象）
    // 在 DELETE_VAR 删除对象时调用，用于打断 using &this 产生的循环引用：
    // Object -> ClosureHandle -> Closure.upvalues -> Cell -> ObjectHandle -> Object
    // 也处理嵌套对象中的闭包（如 this.thread.target）
    //
    // 重要：这里不能在遍历对象字段时直接 releaseObject / releaseCellAndContent，
    // 因为这可能导致当前正在遍历的对象被提前清空（迭代器失效 -> 崩溃）。
    // 因此采用“先断边、后延迟释放”的策略：在递归遍历期间只清除 closure 的引用关系，
    // 把需要释放的 Cell/对象收集起来，待遍历完成后再统一释放。
    struct PendingCycleBreakRelease {
        std::unordered_set<int> processedClosures;
        std::vector<Cell*> upvalueCellsToRelease; // one entry per upvalue slot (respect retain multiplicity)
        std::vector<ZeusType::ObjectHandle> boundThisObjToRelease;
    };

    void forceBreakClosureCycles(
        const ZeusType::ObjectHandle& handle,
        std::set<std::pair<int, int>>& visited,
        PendingCycleBreakRelease& pending
    ) {
        const auto identity = std::make_pair(handle.id, handle.generation);
        if (!visited.insert(identity).second) return;

        auto* obj = getValidObject(handle);
        if (!obj) return;

        ZEUS_DEBUG_PRINT("[GC] forceBreakClosureCycles: breaking cycles for object #%d gen=%d",
            handle.id, handle.generation);

        const auto fields = snapshotObjectFields(*obj);
        for (const auto& kv : fields) {
            if (std::holds_alternative<ZeusType::ClosureHandle>(kv.second)) {
                int cloId = std::get<ZeusType::ClosureHandle>(kv.second).id;
                if (pending.processedClosures.count(cloId)) continue;

                Closure* clo = getClosure(cloId);
                if (!clo || clo->isFreed()) continue;
                pending.processedClosures.insert(cloId);

                ZEUS_DEBUG_PRINT("[GC] forceBreakClosureCycles: breaking closure #%d (field '%s')",
                    cloId, kv.first.c_str());

                // Detach upvalues (but defer releasing the Cell refs)
                for (auto* cell : clo->upvalues) {
                    if (cell) pending.upvalueCellsToRelease.push_back(cell);
                }
                clo->upvalues.clear();

                // Detach boundThis (but defer releasing the object ref)
                if (clo->boundThis.has_value()) {
                    ZEUS_DEBUG_PRINT("[GC] forceBreakClosureCycles: clearing boundThis for closure #%d", cloId);
                    pending.boundThisObjToRelease.push_back(*clo->boundThis);
                    clo->boundThis.reset();
                }
            } else if (std::holds_alternative<ZeusType::ObjectHandle>(kv.second)) {
                const auto& nestedHandle = std::get<ZeusType::ObjectHandle>(kv.second);
                forceBreakClosureCycles(nestedHandle, visited, pending);
            }
        }
    }
    
    // 便捷重载：从单个对象开始打断循环
    void forceBreakClosureCycles(const ZeusType::ObjectHandle& handle) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::set<std::pair<int, int>> visited;
        PendingCycleBreakRelease pending;
        forceBreakClosureCycles(handle, visited, pending);

        // Now it is safe to release, after all traversals are done.
        for (Cell* cell : pending.upvalueCellsToRelease) {
            if (cell) releaseCellAndContent(cell);
        }
        for (const auto& objectHandle : pending.boundThisObjToRelease) {
            releaseObject(objectHandle);
        }
    }
    
    // Explicit delete invalidates the source object immediately. The storage
    // remains as a tombstone until every old handle releases its reference.
    void destroyObjectSource(const ZeusType::ObjectHandle& handle) {
        auto* obj = getValidObject(handle);
        if (!obj) return;
        auto* classInfo = getClass(obj->classId);
        if (classInfo && classInfo->name == "Namespace") return;

        // Keep a temporary reference while dropping fields: a self-reference
        // or closure field may release the object recursively.
        retainObject(handle);
        auto fields = snapshotObjectFields(*obj);
        std::vector<ZeusType::UniversalVariant> nestedValues;
        nestedValues.reserve(fields.size());
        for (auto& entry : fields) {
            nestedValues.push_back(std::move(entry.second));
        }
        ZeusType::clearObjectFieldStorage(*obj);
        obj->markedForDeletion = true;
        for (auto& value : nestedValues) {
            ZeusGC::releaseVariantDeep(*this, value);
        }
        releaseObject(handle); // release the temporary reference
    }

    // 减少对象引用计数，必要时释放
    void releaseObject(int objId) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        auto* obj = getMutableObject(objId);

        if (!obj) return;
        if (obj->refCount <= 0 && (obj->markedForDeletion || obj->finalizing)) return;

        // 永久保留 Namespace 对象，避免全局命名空间被误释放
        if (obj->classId >= 0 && obj->classId < (int)classes.size()) {
            const auto& ci = classes[obj->classId];
            if (ci.name == "Namespace") {
                ZEUS_DEBUG_PRINT("[GC] Skip release for Namespace object #%d (pinned)", objId);
                return;
            }
        }

        if (obj->release()) {
            if (obj->finalizing) return;
            const ZeusType::ObjectHandle handle{objId, obj->generation};
            obj->finalizing = true;
            ZEUS_DEBUG_PRINT("[GC] Object #%d released (refCount=0), freeing slot", objId);
            if (obj->classId == 20 || obj->classId == 17 || obj->classId == 18) {
                ZEUS_DEBUG_PRINT("[GC][TRACE] classId=%d release to zero", obj->classId);
            }

            // Match the LLVM lifetime manager: user destruction happens once
            // when the last live reference disappears. Explicit delete has
            // already run the destructor and marked the source as deleted.
            if (!obj->markedForDeletion) {
                invokeObjectDestructor(handle);
                obj = getMutableObject(objId);
                if (!obj || obj->generation != handle.generation) return;
                if (!obj->markedForDeletion) destroyObjectSource(handle);
                obj = getMutableObject(objId);
                if (!obj || obj->generation != handle.generation) return;
            }
            
            // 先释放嵌套对象
            releaseNestedObjects(*obj);
            
            // 清空对象
            obj->clear();
            
            // 将槽位加入空闲列表
            if (isSharedObject(objId)) {
                // 共享对象：加入共享的空闲列表（需要加锁）
                if (sharedFreeObjectSlots && sharedObjectsMutex) {
                    std::unique_lock<std::shared_mutex> lock(*sharedObjectsMutex);
                    sharedFreeObjectSlots->push_back(objId);
                    ZEUS_DEBUG_PRINT("[GC] Added shared slot #%d to free list", objId);
                }
            } else {
                // 本地对象
                freeObjectSlots.push_back(objId);
                freeObjectSlotsCount++;
            }
        } else {
            ZEUS_DEBUG_PRINT("[GC] Object #%d release -> refCount=%d", objId, obj->refCount);
            if (obj->classId == 20 || obj->classId == 17 || obj->classId == 18) {
                ZEUS_DEBUG_PRINT("[GC][TRACE] classId=%d release -> refCount=%d", obj->classId, obj->refCount);
            }
        }
    }
    
    
    // ========== GC: 闭包引用计数管理 ==========
    
    // 创建新闭包，返回闭包 ID（优先复用空闲槽位，线程安全）
    void releaseObject(const ZeusType::ObjectHandle& handle) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        auto* obj = getMutableObject(handle.id);
        if (!obj || obj->generation != handle.generation) {
            ZEUS_DEBUG_PRINT("[GC] Ignored stale object release: id=%d handle.gen=%d current.gen=%d",
                handle.id, handle.generation, obj ? obj->generation : -1);
            return;
        }
        releaseObject(handle.id);
    }

    int allocClosure(Closure&& clo) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::unique_lock<std::shared_mutex> lock(closuresMutex);
        clo.refCount = 1;
        
        if (!freeClosureSlots.empty()) {
            int slot = freeClosureSlots.back();
            freeClosureSlots.pop_back();
            if (freeClosureSlotsCount > 0) freeClosureSlotsCount--;
            closures[slot] = std::move(clo);
            ZEUS_DEBUG_PRINT("[GC] Reused closure slot #%d", slot);
            pinVMClosureRoot(slot);
            return slot;
        }

        int cloId = (int)closures.size();
        closures.push_back(std::move(clo));
        closuresCount++;
        ZEUS_DEBUG_PRINT("[GC] allocClosure: new closure id=%d protoId=%d", cloId, closures[cloId].protoId);
        pinVMClosureRoot(cloId);
        return cloId;
    }    // 调试: 输出当前闭包列表的详情（用于内置的 __closure_details 调用）
    std::string dumpClosureDetails() const {
        std::ostringstream oss;
        oss << "CLOSURE DETAILS: total=" << closures.size() << ", count=" << closuresCount << "\r\n";
        for (size_t i = 0; i < closures.size(); ++i) {
            const auto &c = closures[i];
            oss << "#" << i << " proto=" << c.protoId << " ref=" << c.refCount << " freed=" << (c.isFreed() ? 1 : 0) << " upvals=" << c.upvalues.size();
            if (c.boundThis.has_value()) {
                int oid = c.boundThis->id;
                std::string cname = "unknown";
                const ZeusType::ObjectInstance* obj = nullptr;
                if (sharedObjects && oid >= 0 && oid < (int)sharedObjects->size()) {
                    obj = &(*sharedObjects)[oid];
                } else if (oid >= 0 && oid < (int)objects.size()) {
                    obj = &objects[oid];
                }
                if (obj && c.boundThis->generation == obj->generation &&
                    !obj->markedForDeletion && obj->classId >= 0 &&
                    obj->classId < (int)classes.size()) {
                    cname = classes[obj->classId].name;
                }
                oss << " boundThis=" << oid << ":" << cname;
            }
            oss << "\r\n";
        }
        return oss.str();
    }

    #include "ZeusVMRootSnapshots.inl"

    // ========== Mark/Sweep GC (补充) ==========
    // 现有引用计数在一些路径下会留下“无根 refCount=1”的对象（例如栈/Cell 所有权不对称时）。
    // 这里提供一个保守的 mark-sweep，用于在安全点（RETURN/DELETE_VAR）回收真正不可达的对象/闭包。
    void collectGarbageSweep() {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        publishVMRoots();
        auto& gcObjects = sharedObjects ? *sharedObjects : objects;
        // Mark vectors sized to current storage.
        std::vector<uint8_t> markedObj(gcObjects.size(), 0);
        std::vector<int> objectGenerations;
        objectGenerations.reserve(gcObjects.size());
        for (const auto& object : gcObjects) objectGenerations.push_back(object.generation);
        std::vector<uint8_t> markedClo(closures.size(), 0);
        std::unordered_set<const void*> markedNativeClo;
        std::unordered_set<const void*> markedContainers;
        std::vector<std::shared_ptr<void>> nativeClosureRoots;

        auto markObject = [&](const ZeusType::ObjectHandle& objectHandle,
                              auto&& markObjectRef, auto&& markVariantRef) -> void {
            auto* object = ZeusVMUtils::getValidObject(gcObjects, objectHandle);
            if (!object) return;
            const size_t objId = static_cast<size_t>(objectHandle.id);
            if (markedObj[objId]) return;
            markedObj[objId] = 1;
            const auto fields = snapshotObjectFields(*object);
            for (const auto& kv : fields) {
                markVariantRef(kv.second, markVariantRef, markObjectRef);
            }
        };

        auto markClosure = [&](int cloId, auto&& markClosureRef, auto&& markVariantRef, auto&& markObjectRef) -> void {
            if (cloId < 0 || cloId >= (int)closures.size()) return;
            Closure* clo = getClosure(cloId);
            if (!clo || clo->isFreed() || clo->protoId < 0) return;
            if (markedClo[(size_t)cloId]) return;
            markedClo[(size_t)cloId] = 1;

            // mark closure upvalues
            for (auto* cell : clo->upvalues) {
                if (!cell) continue;
                const auto value = cell->readValue();
                markVariantRef(value, markVariantRef, markObjectRef);
            }
            // mark boundThis
            if (clo->boundThis.has_value()) {
                markObjectRef(*clo->boundThis, markObjectRef, markVariantRef);
            }
        };

        auto markVariant = [&](const ZeusType::UniversalVariant& v, auto&& markVariantRef, auto&& markObjectRef) -> void {
            if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
                const auto& objectHandle = std::get<ZeusType::ObjectHandle>(v);
                markObjectRef(objectHandle, markObjectRef, markVariantRef);
                return;
            }
            if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
                int cid = std::get<ZeusType::ClosureHandle>(v).id;
                markClosure(cid, markClosure, markVariantRef, markObjectRef);
                return;
            }
            if (std::holds_alternative<ZeusType::NativeClosureHandle>(v)) {
                const auto& native = std::get<ZeusType::NativeClosureHandle>(v);
                if (native.state && markedNativeClo.insert(native.state.get()).second) {
                    nativeClosureRoots.push_back(native.state);
                    ZeusGC::traceNodeValues(native.state, [&](const ZeusType::UniversalVariant& captured) {
                        markVariantRef(captured, markVariantRef, markObjectRef);
                    });
                }
                return;
            }
            if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(v)) {
                auto ptr = std::get<std::shared_ptr<ZeusType::NamedArg>>(v);
                if (ptr) markVariantRef(ptr->value, markVariantRef, markObjectRef);
                return;
            }
            if (!std::holds_alternative<ZeusType::Variant>(v)) return;

            const auto& var = std::get<ZeusType::Variant>(v);
            std::visit([&](const auto& inner) {
                using T = std::decay_t<decltype(inner)>;
                if constexpr (std::is_same_v<T, ZeusType::Array>) {
                    if (!inner.impl) return;
                    if (!markedContainers.insert(inner.impl.get()).second) return;
                    for (const auto& it : inner.impl->data) {
                        markVariantRef(it, markVariantRef, markObjectRef);
                    }
                } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
                    if (!inner.impl) return;
                    if (!markedContainers.insert(inner.impl.get()).second) return;
                    for (const auto& kv : inner.impl->data) {
                        markVariantRef(kv.second, markVariantRef, markObjectRef);
                    }
                } else {
                    // primitives / Bytes / Function / Null: no-op
                }
            }, var);
        };

        ZeusGC::traceRoots([&](const ZeusType::UniversalVariant& value) {
            markVariant(value, markVariant, markObject);
        });

        // Roots: stack
        for (size_t i = 0; i < stack.size(); ++i) {
            markVariant(stack[i], markVariant, markObject);
        }

        auto markCells = [&](const std::vector<Cell*>& cells) {
            for (auto* cell : cells) {
                if (!cell) continue;
                const auto value = cell->readValue();
                markVariant(value, markVariant, markObject);
            }
        };
        markCells(global);
        markCells(local);
        markCells(upvalue);

        // Calling a function moves the caller's cells into its call frame.
        // A collection inside that callee must keep suspended callers alive,
        // including constructors whose result has not reached the stack yet.
        for (const auto& frame : callFrames) {
            markCells(frame.savedLocal);
            markCells(frame.savedUpvalue);
            if (frame.constructorResult) {
                markObject(*frame.constructorResult, markObject, markVariant);
            }
        }
        markVariant(currentException, markVariant, markObject);

        // Roots: shared heap bindings. A class instance referenced only by a
        // shared variable lives in the shared object table; without this it
        // would be reclaimed here and run operator[~] while other threads are
        // still using it. Only object handles need marking, but the shared cell
        // may also hold a container with nested handles, so reuse markVariant.
        ZeusShared::getGlobalHeap().visitValues(
            [&](const ZeusType::UniversalVariant& value) {
                markVariant(value, markVariant, markObject);
            });

        // Peer VMs publish value snapshots at their own safe points. Allocation
        // and retain barriers add transient roots in between publications.
        // No peer execution vector or borrowed VM pointer is read here.
        ZeusVMRoots::Registry::visit(objectTableIdentity(), this,
            [&](const ZeusVMRoots::Snapshot& roots) {
                for (const auto& entry : roots.objects) {
                    markObject(entry, markObject, markVariant);
                }
                for (const auto& closure : roots.closures) {
                    if (closure.first == &closures) {
                        markClosure(closure.second, markClosure, markVariant, markObject);
                    }
                }
                for (const auto& native : roots.nativeNodes) {
                    if (auto owner = native.second.lock()) {
                        if (markedNativeClo.insert(native.first).second) {
                            ZeusGC::traceNodeValues(owner,
                                [&](const ZeusType::UniversalVariant& captured) {
                                    markVariant(captured, markVariant, markObject);
                                });
                            nativeClosureRoots.push_back(std::move(owner));
                        }
                    }
                }
            });

        // Detach unreachable closures before releasing their captured values.
        // A captured array/dictionary may hold a ClosureHandle back edge.
        std::vector<ClosureReleasePayload> closurePayloads;
        for (int cid = (int)inheritedClosuresSize; cid < (int)closures.size(); ++cid) {
            if (markedClo[(size_t)cid]) continue;
            ClosureReleasePayload payload;
            if (detachClosureForRelease(cid, payload, true)) {
                closurePayloads.push_back(std::move(payload));
            }
        }
        for (auto& payload : closurePayloads) {
            releaseClosurePayload(payload);
        }

        // Sweep objects (skip pinned Namespace; treat shared objects via isSharedObject)
        for (int oid = 0; oid < static_cast<int>(markedObj.size()); ++oid) {
            auto& obj = gcObjects[(size_t)oid];
            // A destructor may allocate into a recycled slot while sweeping.
            // Such a new object was not part of this mark pass.
            if (obj.generation != objectGenerations[(size_t)oid]) continue;
            if (obj.classId < 0 || obj.markedForDeletion) continue;
            if (markedObj[(size_t)oid]) continue;

            // 永久保留 Namespace 对象，避免全局命名空间被误释放
            if (const auto* type = getClass(obj.classId)) {
                if (type->name == "Namespace") continue;
            }

            // Let the common finalizer run user destruction and release fields
            // before recycling this unreachable slot. Normal reference counts
            // may be nonzero here because this pass also reclaims cycles.
            const ZeusType::ObjectHandle handle{oid, obj.generation};
            obj.refCount = 1;
            releaseObject(handle);
        }

        // VM-owned cells and objects have now released their unreachable values.
        // The shared collector can safely break container and native-closure
        // cycles after every VM-local owner in this sweep has been removed.
        ZeusGC::collect(nativeClosureRoots);
    }
    
    // 增加闭包引用计数（线程安全）
    void retainClosure(int cloId) {
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::unique_lock<std::shared_mutex> lock(closuresMutex);
        Closure* clo = getClosure(cloId);
        if (clo && !clo->isFreed()) {
            clo->retain();
            pinVMClosureRoot(cloId);
            ZEUS_DEBUG_PRINT("[GC] Closure #%d retain -> refCount=%d", cloId, clo->refCount);
        }
    }
    
    // 判断闭包是否来自共享系统
    bool isSharedClosure(int cloId) const {
        // 如果有 sharedClosures 且 cloId 在继承范围内，是共享的
        if (sharedClosures && cloId >= 0 && cloId < (int)inheritedClosuresSize) {
            return true;
        }
        // 否则是本地闭包
        return false;
    }
    
    // 减少闭包引用计数，必要时释放（线程安全）
    void releaseClosure(int cloId) {
        ClosureReleasePayload payload;
        if (detachClosureForRelease(cloId, payload)) {
            releaseClosurePayload(payload);
        }
    }

    // 对象垃圾回收：标记删除（兼容旧代码）
    void markObjectForDeletion(int objId) {
        auto* object = getMutableObject(objId);
        if (!object || object->markedForDeletion || object->classId < 0) return;
        ZEUS_DEBUG_PRINT("[GC] Mark object #%d (class=%d) for deletion",
            objId, object->classId);
        const ZeusType::ObjectHandle handle{objId, object->generation};
        object->refCount = 1;
        releaseObject(handle);
    }

    // 对象垃圾回收：清理已标记的对象（惰性压缩）- 保留但不再需要
    void compactObjects() {
        // 使用空闲槽位复用后，压缩不再必要
        // 保留此函数以兼容旧代码
    }

    private:
    void tryRegisterGlobalClosure(int slot, const ZeusType::UniversalVariant& v) {
    // 仅在启用延迟解析时才注册全局闭包/类名
        if (!enableLateResolve) {
            return;
        }

    // 检查索引与 globalNames 的有效性
        if (slot < 0 || slot >= (int)globalNames.size()) {
            return;
        }

        const std::string& name = globalNames[slot];
        if (name.empty()) {
            return;
        }

    // 注册闭包名映射
        if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
            int cid = std::get<ZeusType::ClosureHandle>(v).id;

            nameToClosure[name] = cid;

            vmMessage("[late-reg] register closure name '" + name + "' -> #" + std::to_string(cid));
        }
        
    // 注册类名映射
        if (std::holds_alternative<ZeusType::ClassHandle>(v)) {
            int classId = std::get<ZeusType::ClassHandle>(v).id;

            nameToClass[name] = classId;

            vmMessage("[late-reg] register class name '" + name + "' -> #" + std::to_string(classId));
        }
    }

    public:

    void printStack() {
        ZEUS_DEBUG_EXEC({
            std::string line = "[STACK    ] : Stack [ ";
            for (const auto& val : stack) {
                line += ZeusVMUtils::valueToDebugString(val);
                line += ' ';
            }
            line += ']';
            ZEUS_DEBUG_PRINT("%s", line.c_str());
        });
    }

    void vmMessage(const std::string& msg) {
        ZEUS_DEBUG_PRINT("[VM] %s", msg.c_str());
    }

    void printVarStack (std::vector<Cell*>& varStack) {
        ZEUS_DEBUG_EXEC({
            std::string line = "VarStack [ ";
            for (const auto& cell : varStack) {
                if (!cell) { line += "<Cell null> "; continue; }
                line += "<Cell #" + std::to_string(cell->debugId) + ": ";
                const auto value = cell->readValue();
                line += ZeusVMUtils::valueToDebugString(value);
                if (cell->flags & 0x1) line += " const";
                line += "> ";
            }
            line += ']';
            ZEUS_DEBUG_PRINT("%s", line.c_str());
        });
    }

    void printVarStack (int id) {
        switch(id) {
            case 0: {
                ZEUS_DEBUG_PRINT("[GLOBAL   ] : ");
                printVarStack(global);
                break;
            }
            case 1: {
                ZEUS_DEBUG_PRINT("[LOCAL    ] : ");
                printVarStack(local);
                break;
            }
            case 2: {
                ZEUS_DEBUG_PRINT("[UPVALUE  ] : ");
                printVarStack(upvalue);
                break;
            }
        }
    }

    void printGlobals() { printVarStack(0); }
    void printLocals()  { printVarStack(1); }




    
    // 将字节码注册为函数原型并映射参数信息
    bool registerPrototypes(
        const std::vector<std::vector<IRType::IRInstr>>& bytecodes,
        std::string* error = nullptr) {

        if (error) error->clear();

        ZEUS_DEBUG_PRINT("[registerPrototypes] Total bytecodes chunks: %zu", bytecodes.size());
        
        chunkToProtoId.assign(bytecodes.size(), -1);
        functionNameToProtoId.clear();

        int protoCounter = 0;
        for (size_t i = 0; i < bytecodes.size(); ++i) {
            const auto& bc = bytecodes[i];
            for (const auto& instruction : bc) {
                if (instruction.metadata.kind !=
                        IRType::IRMetadataKind::ExternalFunctionSignature ||
                    instruction.metadata.nativeLibraryPath.empty()) {
                    continue;
                }
                const auto& metadata = instruction.metadata;
                ZeusBuiltins::registerDynamicNativeImport(
                    ZeusBuiltins::DynamicNativeImportDescriptor{
                        metadata.symbol.name,
                        metadata.nativeSymbol,
                        metadata.nativeLibraryPath,
                        metadata.typeName,
                        metadata.typeNames,
                        metadata.parameterIsReference,
                        metadata.nativeReferenceABI
                    });
            }
            bool isFuncBegin = (!bc.empty() && bc.front().op == VMOperator::FUNC_BEGIN);
            ZEUS_DEBUG_PRINT("[registerPrototypes] Chunk #%zu: %zu instructions, FUNC_BEGIN=%d", 
                i, bc.size(), isFuncBegin ? 1 : 0);
            if (isFuncBegin) {
                Prototype proto;
                proto.body = bc;
                for (const auto& bodyInstr : bc) {
                    if (bodyInstr.op == VMOperator::IR_ANNOTATION &&
                        !bodyInstr.args.empty() &&
                        std::holds_alternative<std::string>(bodyInstr.args[0]) &&
                        std::get<std::string>(bodyInstr.args[0]) == "generic_type_parameters") {
                        for (size_t pi = 1; pi < bodyInstr.args.size(); ++pi) {
                            if (std::holds_alternative<std::string>(bodyInstr.args[pi])) {
                                proto.typeParameters.push_back(
                                    std::get<std::string>(bodyInstr.args[pi]));
                            }
                        }
                    }
                    if (bodyInstr.op == VMOperator::IR_ANNOTATION &&
                        bodyInstr.metadata.kind == IRType::IRMetadataKind::FunctionSignature) {
                        std::string returnType;
                        int captureCount = 0;
                        bodyInstr.metadata.readFunctionSignature(
                            proto.functionName,
                            returnType,
                            captureCount,
                            proto.paramTypes,
                            &proto.ownerType,
                            nullptr,
                            nullptr,
                            nullptr,
                            &proto.paramIsReference
                        );
                        proto.isStaticMethod = bodyInstr.metadata.isStaticMethod;
                    }
                    if (bodyInstr.op == VMOperator::JUMP_IF_PARAM_PROVIDED) {
                        proto.hasRuntimeDefaultPrologue = true;
                    }
                }

                if (!bc.front().args.empty()) {
                    // 如果 IRGenerator 在 FUNC_BEGIN 中放入参数个数（以 LexerType::Value 形式），则读取之
                    if (std::holds_alternative<LexerType::Value>(bc.front().args[0])) {
                        proto.paramCount = VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[0]));
                    }
                    // 读取变长参数元数据
                    if (bc.front().args.size() >= 5) {
                        if (std::holds_alternative<LexerType::Value>(bc.front().args[1])) {
                            proto.hasVariadicArgs = (VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[1])) != 0);
                        }
                        if (std::holds_alternative<LexerType::Value>(bc.front().args[2])) {
                            proto.hasVariadicKwargs = (VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[2])) != 0);
                        }
                        if (std::holds_alternative<std::string>(bc.front().args[3])) {
                            proto.variadicArgsName = std::get<std::string>(bc.front().args[3]);
                        }
                        if (std::holds_alternative<std::string>(bc.front().args[4])) {
                            proto.variadicKwargsName = std::get<std::string>(bc.front().args[4]);
                        }
                    }
                    
                    // 读取默认参数信息
                    if (bc.front().args.size() >= 7) {
                        if (std::holds_alternative<LexerType::Value>(bc.front().args[5])) {
                            proto.minRequiredParams = VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[5]));
                        }
                        if (std::holds_alternative<LexerType::Value>(bc.front().args[6])) {
                            int defaultCount = VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[6]));
                            proto.paramHasDefault.assign(proto.paramCount, false);
                            proto.paramDefaultValues.resize(proto.paramCount);

                            // Read parameter index/value pairs; new code evaluates the expression in its prologue.
                            size_t argIdx = 7;
                            for (int i = 0; i < defaultCount && argIdx + 1 < bc.front().args.size(); ++i) {
                                if (std::holds_alternative<LexerType::Value>(bc.front().args[argIdx])) {
                                    int paramIdx = VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[argIdx]));
                                    if (paramIdx >= 0 && paramIdx < proto.paramCount) {
                                        proto.paramHasDefault[paramIdx] = true;
                                        if (!proto.hasRuntimeDefaultPrologue &&
                                            std::holds_alternative<LexerType::Value>(bc.front().args[argIdx + 1])) {
                                            const auto& defVal = std::get<LexerType::Value>(bc.front().args[argIdx + 1]);
                                            if (std::holds_alternative<std::string>(defVal.data) &&
                                                std::get<std::string>(defVal.data) == "__NIL__") {
                                                proto.paramDefaultValues[paramIdx] = ZeusType::Variant(ZeusType::Null());
                                            } else {
                                                proto.paramDefaultValues[paramIdx] = defVal;
                                            }
                                        }
                                    }
                                }
                                argIdx += 2;
                            }
                            
                            // 读取参数名列表 (在默认值之后)
                            // argIdx现在指向参数名数量的位置
                            if (argIdx < bc.front().args.size() && std::holds_alternative<LexerType::Value>(bc.front().args[argIdx])) {
                                int paramNameCount = VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[argIdx]));
                                argIdx++;
                                
                                // 读取各参数名
                                for (int i = 0; i < paramNameCount && argIdx < bc.front().args.size(); ++i) {
                                    if (std::holds_alternative<std::string>(bc.front().args[argIdx])) {
                                        proto.paramNames.push_back(std::get<std::string>(bc.front().args[argIdx]));
                                    }
                                    argIdx++;
                                }
                                
                                ZEUS_DEBUG_PRINT("[registerPrototypes] proto paramNames: count=%d", (int)proto.paramNames.size());
                                for (size_t i = 0; i < proto.paramNames.size(); ++i) {
                                    ZEUS_DEBUG_PRINT("  param[%zu]: %s", i, proto.paramNames[i].c_str());
                                }
                            }
                        }
                    }
                }

                for (const auto& ins : bc) {
                    if (ins.op == VMOperator::CAPTURE_OUTSIDE_VAR) {
                        CaptureDesc cap;
                        cap.fromKind = std::get<std::string>(ins.args[0]);
                        cap.srcIndex = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[1]));
                        cap.dstIndex = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[2]));
                        cap.name = std::get<std::string>(ins.args[3]);
                        if (ins.args.size() > 4 && std::holds_alternative<LexerType::Value>(ins.args[4])) {
                            cap.modeFlag = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[4]));
                        }
                        
                        proto.captures.push_back(cap);
                    }
                }
                prototypes.push_back(std::move(proto));
                prototypesCount++;
                // DEBUG: print proto captures summary
                {
                    int pid = (int)prototypes.size() - 1;
                    auto &p = prototypes[pid];
                    ZEUS_DEBUG_PRINT("[registerPrototypes] registered protoId=%d captures=%zu", pid, p.captures.size());
                    for (size_t ci = 0; ci < p.captures.size(); ++ci) {
                        auto &c = p.captures[ci];
                        ZEUS_DEBUG_PRINT("  cap[%zu]: name=%s from=%s src=%d dst=%d mode=%d", ci, c.name.c_str(), c.fromKind.c_str(), c.srcIndex, c.dstIndex, c.modeFlag);
                    }
                }
                chunkToProtoId[i] = protoCounter++;
                if (!prototypes.back().functionName.empty()) {
                    functionNameToProtoId[prototypes.back().functionName] =
                        chunkToProtoId[i];
                }
            }
        }

        return true;
    }



    // truthiness: false/0/"" -> false; others true
    #include "../ZeusRuntimeLib/ZeusVMUtils.hpp"  // 包含 VM 工具函数（调试与值转换等）
    #pragma warning(pop)

    static bool truthy(const ZeusType::UniversalVariant& v) {
        if (std::holds_alternative<LexerType::Value>(v)) {
            const auto& val = std::get<LexerType::Value>(v);
            if (std::holds_alternative<bool>(val.data)) return std::get<bool>(val.data);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                return std::get<ZeusType::BigInteger>(val.data) != ZeusType::BigInteger(0);
            }
            if (std::holds_alternative<ZeusType::BigDecimal>(val.data)) return std::get<ZeusType::BigDecimal>(val.data) != ZeusType::BigDecimal(0);
            if (std::holds_alternative<std::string>(val.data)) return !std::get<std::string>(val.data).empty();
        } else if (std::holds_alternative<ZeusType::BigInteger>(v)) {
            return std::get<ZeusType::BigInteger>(v) != ZeusType::BigInteger(0);
        } else if (std::holds_alternative<ZeusType::BigDecimal>(v)) {
            return std::get<ZeusType::BigDecimal>(v) != ZeusType::BigDecimal(0);
        } else if (std::holds_alternative<bool>(v)) {
            return std::get<bool>(v);
        } else if (std::holds_alternative<std::string>(v)) {
            return !std::get<std::string>(v).empty();
        } else if (std::holds_alternative<ZeusType::Variant>(v)) {
            const ZeusType::Variant &inner = std::get<ZeusType::Variant>(v);
            if (std::holds_alternative<ZeusType::Null>(inner)) {
                return false; // nil 表示空值
            }
        }
    return true; // 表示该节点已被优化为常量（可替换为单个值）
    }
    // 注意：一些通用工具函数已放在 `ZeusVMUtils.hpp`

    // 将栈缩小到指定大小，同时释放被移除槽位中的对象/闭包引用
    void shrinkStackWithRelease(size_t newSize) {
        if (newSize >= stack.size()) {
            stack.resize(newSize);
            return;
        }
        for (size_t i = newSize; i < stack.size(); ++i) {
            uint8_t flag = stack.ownerFlag(i);
            if (flag & 0x4) {
                ZeusGC::releaseVariantDeep(*this, stack[i]);
                continue;
            }
            if (flag & 0x1) {
                ZeusGC::releaseVariantIfObject(*this, stack[i]);
                continue;
            }
            if (flag & 0x2) {
                if (std::holds_alternative<ZeusType::ClosureHandle>(stack[i])) {
                    int cid = std::get<ZeusType::ClosureHandle>(stack[i]).id;
                    // 避免释放共享/全局闭包（主线程持有），仅释放本地创建的临时闭包
                    if (!isSharedClosure(cid)) {
                        releaseClosure(cid);
                    } else {
                        ZEUS_DEBUG_PRINT("[GC] shrinkStack skip release shared closure #%d", cid);
                    }
                }
                continue;
            }
        }
        stack.resize(newSize);
    }

    void assignSharedCellValue(
        const std::shared_ptr<ZeusShared::SharedCell>& cell,
        const ZeusType::UniversalVariant& value
    ) {
        // All VM shared-slot access takes the graph lock before the cell lock.
        // Publish before releasing: destruction may re-enter Zeus and read the
        // binding, which must already name the replacement instance.
        std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
        std::lock_guard<std::recursive_mutex> lock(cell->mutex);
        const auto previous = cell->read();
        ZeusGC::retainVariantDeep(*this, value);
        cell->write(value);
        ZeusGC::releaseVariantDeep(*this, previous);
    }

    struct StackItem {
        ZeusType::UniversalVariant value;
        uint8_t owner = 0;
    };

    StackItem popStackItem() {
        StackItem it;
        if (stack.empty()) return it;
        size_t idx = stack.size() - 1;
        it.value = stack.back();
        it.owner = stack.ownerFlag(idx);
        stack.pop_back();
        return it;
    }

    void dropStackItem(const StackItem& it) {
        if (it.owner & 0x4) {
            ZeusGC::releaseVariantDeep(*this, it.value);
            return;
        }
        if (it.owner & 0x1) {
            ZeusGC::releaseVariantIfObject(*this, it.value);
            return;
        }
        if (it.owner & 0x2) {
            if (std::holds_alternative<ZeusType::ClosureHandle>(it.value)) {
                int cid = std::get<ZeusType::ClosureHandle>(it.value).id;
                if (!isSharedClosure(cid)) {
                    releaseClosure(cid);
                } else {
                    ZEUS_DEBUG_PRINT("[GC] dropStackItem skip release shared closure #%d", cid);
                }
            }
            return;
        }
    }

    void dropStackBack() {
        auto it = popStackItem();
        dropStackItem(it);
    }

    void assignCellValue(Cell* cell, const ZeusType::UniversalVariant& newVal) {
        if (!cell) return;
        cell->validateValue(newVal);

        const auto oldVal = cell->readValue();
        ZeusGC::releaseVariantDeep(*this, oldVal);
        ZeusGC::retainVariantDeep(*this, newVal);

        cell->writeValue(newVal);
    }

    enum class ReferenceWriteResult { NotReference, Stored, ConstTarget, Invalid };

    ZeusType::UniversalVariant loadCellValue(const Cell* cell) const {
        if (!cell) return ZeusType::Variant(ZeusType::Null());
        ZeusType::UniversalVariant current = cell->readValue();
        for (int depth = 0; depth < 64; ++depth) {
            const auto* reference = std::get_if<ZeusType::ReferenceHandle>(&current);
            if (!reference) return current;
            if (!reference->address) {
                throw std::runtime_error("cannot load through a null reference");
            }
            if (reference->typeName == "cell") {
                const auto* target = static_cast<const Cell*>(reference->address);
                if (!target) throw std::runtime_error("reference points to a missing VM cell");
                current = target->readValue();
                continue;
            }
            if (reference->typeName == "shared") {
                const auto shared = ZeusShared::getGlobalHeap().findByAddress(
                    static_cast<const ZeusShared::SharedCell*>(reference->address),
                    reference->generation);
                if (!shared) throw std::runtime_error("reference points to a missing shared VM cell");
                return shared->read();
            }
            if (reference->typeName == "int") {
                return ZeusType::BigInteger(*static_cast<const int64_t*>(reference->address));
            }
            if (reference->typeName == "bool") {
                return *static_cast<const bool*>(reference->address);
            }
            // Preserve opaque reference types until their VM representation is registered.
            return current;
        }
        throw std::runtime_error("cyclic reference chain while loading VM cell");
    }

    ReferenceWriteResult writeThroughReference(
        Cell* binding,
        const ZeusType::UniversalVariant& newValue
    ) {
        if (!binding) return ReferenceWriteResult::Invalid;
        const auto bindingValue = binding->readValue();
        const auto* reference = std::get_if<ZeusType::ReferenceHandle>(&bindingValue);
        if (!reference) return ReferenceWriteResult::NotReference;
        if (!reference->address) return ReferenceWriteResult::Invalid;

        if (reference->typeName == "cell") {
            auto* target = static_cast<Cell*>(reference->address);
            if (!target) return ReferenceWriteResult::Invalid;
            if (target->flags & 0x1) return ReferenceWriteResult::ConstTarget;
            assignCellValue(target, newValue);
            return ReferenceWriteResult::Stored;
        }
        if (reference->typeName == "int") {
            int64_t value = 0;
            if (!ZeusVMUtils::tryGetInt64(newValue, value)) return ReferenceWriteResult::Invalid;
            *static_cast<int64_t*>(reference->address) = value;
            return ReferenceWriteResult::Stored;
        }
        if (reference->typeName == "bool") {
            bool value = false;
            if (std::holds_alternative<bool>(newValue)) {
                value = std::get<bool>(newValue);
            } else if (std::holds_alternative<LexerType::Value>(newValue) &&
                       std::holds_alternative<bool>(std::get<LexerType::Value>(newValue).data)) {
                value = std::get<bool>(std::get<LexerType::Value>(newValue).data);
            } else {
                return ReferenceWriteResult::Invalid;
            }
            *static_cast<bool*>(reference->address) = value;
            return ReferenceWriteResult::Stored;
        }
        if (reference->typeName == "shared") {
            const auto shared = ZeusShared::getGlobalHeap().findByAddress(
                static_cast<const ZeusShared::SharedCell*>(reference->address),
                reference->generation);
            if (!shared) return ReferenceWriteResult::Invalid;
            assignSharedCellValue(shared, newValue);
            return ReferenceWriteResult::Stored;
        }
        return ReferenceWriteResult::Invalid;
    }

    void pushLoadedCellValue(Cell* cell) {
        auto value = loadCellValue(cell);
        // StackWithOwner releases Variant and NamedArg contents deeply. Match
        // that ownership contract here so nested objects/closures loaded from
        // a cell gain the corresponding references before the stack owns them.
        ZeusGC::retainVariantDeep(*this, value);
        stack.push_back(std::move(value));
    }

    void pushStackTransferred(StackItem& it) {
        if (stack.size() == stack.owner.size()) {
            stack.push_back(it.value);
            // ownerFlag will be recalculated by type; keep it consistent
        } else {
            stack.push_back(it.value);
        }
        // transfer: caller should not release after this
        it.owner = 0;
    }

    struct StackItemPairGuard {
        VM* vm;
        StackItem left;
        StackItem right;

        StackItemPairGuard(VM& owner, StackItem leftItem, StackItem rightItem)
            : vm(&owner), left(std::move(leftItem)), right(std::move(rightItem)) {}

        StackItemPairGuard(const StackItemPairGuard&) = delete;
        StackItemPairGuard& operator=(const StackItemPairGuard&) = delete;

        ~StackItemPairGuard() {
            if (vm) {
                vm->dropStackItem(left);
                vm->dropStackItem(right);
            }
        }

        void transferToStack() {
            if (!vm) return;
            vm->pushStackTransferred(left);
            vm->pushStackTransferred(right);
            vm = nullptr;
        }
    };

    struct FrameLoopOptions {
        size_t stopFrameDepth = 0;
        bool captureReturn = false;
        bool handleExceptions = false;
        bool checkExternalStop = false;
        bool updateSourceFile = false;
        bool validateJumps = false;
        bool traceExecution = false;
        const char* traceName = "[VM]";
    };

    struct FrameLoopState {
        ZeusType::UniversalVariant retVal = LexerType::Value(0);
        uint8_t retOwner = 0;
    };

    void releaseCurrentFrameEnvironment() {
        // A Zeus destructor replaces VM.local/upvalue while executing. Keep
        // iterators in independent storage so nested execution cannot invalidate
        // the environment being released.
        auto releasingLocal = std::move(local);
        auto releasingUpvalue = std::move(upvalue);
        local.clear();
        upvalue.clear();
        ZeusVMRoots::InstructionScope teardownRoots(this);
        {
            std::lock_guard<std::recursive_mutex> graph(ZeusGC::graphMutex());
            RootSnapshotBuilder roots(*this, false, false);
            roots.cells(releasingLocal);
            roots.cells(releasingUpvalue);
            ZeusVMRoots::Registry::pin(this, std::move(roots.snapshot));
        }
        for (auto* cell : releasingLocal) {
            if (cell) releaseCellAndContent(cell);
        }
        for (auto* cell : releasingUpvalue) {
            if (cell) releaseCellAndContent(cell);
        }
    }

    void captureReturnValue(FrameLoopState& state) {
        if (state.retOwner) {
            dropStackItem(StackItem{state.retVal, state.retOwner});
            state.retOwner = 0;
        }

        if (!stack.empty()) {
            StackItem tmp = popStackItem();
            state.retVal = tmp.value;
            state.retOwner = tmp.owner;
        } else {
            state.retVal = LexerType::Value(0);
            state.retOwner = 0;
        }
    }

    void discardFrameExceptionHandlers(size_t frameDepth) {
        while (!exceptionHandlers.empty() &&
               exceptionHandlers.back().callFrameDepth > frameDepth) {
            exceptionHandlers.pop_back();
        }
    }

    void unwindFrameEnvironments(size_t targetDepth) {
        while (callFrames.size() > targetDepth) {
            CallFrame frame = std::move(callFrames.back());
                pinVMFrameRoots(frame);
            callFrames.pop_back();
            if (frame.pendingObjectCloneId != 0) {
                pendingObjectClones.erase(frame.pendingObjectCloneId);
            }
            shrinkStackWithRelease(frame.stackBase);
            releaseCurrentFrameEnvironment();
            if (frame.constructorResult) {
                releaseObject(*frame.constructorResult);
                frame.constructorResult.reset();
            }
            local = std::move(frame.savedLocal);
            upvalue = std::move(frame.savedUpvalue);
            __line = frame.savedLine;
        }
        discardFrameExceptionHandlers(targetDepth);
    }

    bool handlePendingRuntimeException(size_t stopFrameDepth = 0) {
        if (vmstat != ZeusType::VMStatus::Error || exceptionHandlers.empty() ||
            exceptionHandlers.back().callFrameDepth <= stopFrameDepth) {
            return false;
        }

        ZEUS_DEBUG_PRINT("[VM] Runtime error with exception handler, attempting catch");

        // An explicit THROW already owns currentException; keep its identity.
        if (!hasUnhandledException) {
            std::string errorType = "RuntimeError";
            std::string errorMsg = "Unknown error";
            if (VMException::lastError.hasError) {
                errorType = VMException::lastError.errorType;
                errorMsg = VMException::lastError.message;
            }

            // Built-in exception classes use their canonical module name.
            std::string fullClassName = "zeus.exception." + errorType;
            int classId = findClassByName(fullClassName);
            if (classId < 0) {
                classId = findClassByName("zeus.exception.RuntimeError");
            }
            if (classId < 0) {
                classId = findClassByName("zeus.exception.Exception");
            }

            if (classId >= 0) {
                ZeusType::ObjectInstance obj;
                obj.classId = classId;
                obj.extensionValues["_message"] = errorMsg;
                obj.extensionValues["_type"] = errorType;
                obj.flags["_message"] = 0;
                obj.flags["_type"] = 0;

                ZeusType::ObjectHandle objHandle = allocObject(std::move(obj));
                currentException = objHandle;
            } else {
                currentException = errorType + ": " + errorMsg;
            }

        }

        hasUnhandledException = true;
        VMException::lastError.hasError = false;

        const ExceptionHandler handler = exceptionHandlers.back();
        const size_t catchPc = handler.catchPc;

        // 异常展开：块内的 operator[~with] 必须补跑，否则线程不会汇合、句柄
        // 不会关闭。按注册时记录的深度把超出的 with 块逐个退出；这一路不能
        // 再抛异常（已经在展开中），失败只记录不中断。
        if (withContextStack.size() > handler.withDepth) {
            const ZeusType::VMStatus savedStatus = vmstat;
            // executeClosureSync 需要一个可运行状态；这里只是借用它执行析构式
            // 调用，结束后立刻恢复到展开前的状态。
            vmstat = ZeusType::VMStatus::Running;
            while (withContextStack.size() > handler.withDepth) {
                WithContext trapped = withContextStack.back();
                withContextStack.pop_back();
                IRType::IRInstr exitSite;
                if (handler.code != nullptr && catchPc < handler.code->size()) {
                    exitSite = handler.code->at(catchPc);
                }
                (void)invokeWithExit(trapped, exitSite);
            }
            vmstat = savedStatus;
        }

        unwindFrameEnvironments(handler.callFrameDepth);
        shrinkStackWithRelease(handler.stackBase);
        while (local.size() > handler.localSize) {
            if (local.back()) releaseCellAndContent(local.back());
            local.pop_back();
        }

        if (!callFrames.empty()) {
            callFrames.back().pc = catchPc;
        }

        exceptionHandlers.pop_back();
        vmstat = ZeusType::VMStatus::Running;
        ZEUS_DEBUG_PRINT("[VM] Jumping to catch at pc=%zu", catchPc);
        return true;
    }

    bool writeBreakpointSnapshot(const IRType::IRInstr& ins, size_t pc) {
        const auto record = ZeusDebug::captureVMSnapshot(*this, ins, pc);
        std::string writtenPath;
        std::string error;
        if (!snapshotWriter.write(record, writtenPath, error)) {
            std::cerr << "[VM Snapshot] Failed at line " << ins.line << ": " << error << std::endl;
            return false;
        }
        std::cout << "[VM Snapshot] " << writtenPath << std::endl;
        return true;
    }

    void runFrameLoop(const FrameLoopOptions& options, FrameLoopState* state = nullptr) {
        syncVMRootDomain();
        const bool captureReturn = options.captureReturn && state != nullptr;

        while (callFrames.size() > options.stopFrameDepth) {
            ZeusVMRoots::InstructionScope rootInstruction(this);
            if (instructionsUntilRootSnapshot == 0) publishVMRoots();
            --instructionsUntilRootSnapshot;
            CallFrame& frame = callFrames.back();

            if (vmstat != ZeusType::VMStatus::Running) {
                if (options.handleExceptions && handlePendingRuntimeException(options.stopFrameDepth)) {
                    continue;
                }

                if (options.handleExceptions && options.stopFrameDepth == 0 && vmstat == ZeusType::VMStatus::Error && VMException::lastError.hasError) {
                    printf("%s\r\n", VMException::lastError.formattedMsg.c_str());
                    VMException::lastError.hasError = false;
                }

                if (options.handleExceptions) {
                    ZEUS_DEBUG_PRINT("[VM] Aborted, status: %d", (int)vmstat);
                }
                break;
            }

            if (frame.pc >= frame.code->size()) {
                ZEUS_DEBUG_PRINT("%s Frame done (depth=%zu)", options.traceName, callFrames.size());

                CallFrame finishedFrame = std::move(callFrames.back());
                pinVMFrameRoots(finishedFrame);
                callFrames.pop_back();
                discardFrameExceptionHandlers(callFrames.size());

                if (finishedFrame.pendingObjectCloneId != 0) {
                    shrinkStackWithRelease(finishedFrame.stackBase);
                    releaseCurrentFrameEnvironment();
                    local = std::move(finishedFrame.savedLocal);
                    upvalue = std::move(finishedFrame.savedUpvalue);
                    __line = finishedFrame.savedLine;
                    resumePendingObjectClone(
                        finishedFrame.pendingObjectCloneId,
                        StackItem{LexerType::Value(0), 0},
                        options.stopFrameDepth
                    );
                    continue;
                }

                if (callFrames.size() > options.stopFrameDepth) {
                    shrinkStackWithRelease(finishedFrame.stackBase);
                    releaseCurrentFrameEnvironment();

                    local = finishedFrame.savedLocal;
                    upvalue = finishedFrame.savedUpvalue;
                    __line = finishedFrame.savedLine;

                    // Constructor frames discard the implicit value and transfer
                    // their retained result object to the waiting caller.
                    if (finishedFrame.constructorResult) {
                        stack.push_back(*finishedFrame.constructorResult);
                        finishedFrame.constructorResult.reset();
                    } else {
                        stack.push_back(LexerType::Value(0));
                    }
                    callFrames.back().pc++;
                } else if (captureReturn) {
                    if (finishedFrame.constructorResult) {
                        stack.push_back(*finishedFrame.constructorResult);
                        finishedFrame.constructorResult.reset();
                    }
                    captureReturnValue(*state);
                    releaseCurrentFrameEnvironment();

                    local = finishedFrame.savedLocal;
                    upvalue = finishedFrame.savedUpvalue;
                    __line = finishedFrame.savedLine;
                } else if (finishedFrame.constructorResult) {
                    stack.push_back(*finishedFrame.constructorResult);
                    finishedFrame.constructorResult.reset();
                } else {
                    ZEUS_DEBUG_PRINT("[FRAME_DONE] initialFrame ended, local.size=%zu", local.size());
                }

                continue;
            }

            const auto& ins = (*frame.code)[frame.pc];

            if (ins.line >= 0) {
                __line = ins.line;
                frame.savedLine = ins.line;
            }
            if (options.updateSourceFile && ins.sourceFileId >= 0) {
                __sourceFileId = ins.sourceFileId;
            }

            if (options.checkExternalStop && externalStopFlag && externalStopFlag->load()) {
                ZEUS_DEBUG_PRINT("[VM] External stop signal received");
                vmstat = ZeusType::VMStatus::Stopped;
                break;
            }

            if (breakpointConfig.enabled()) {
                const bool newSourcePosition = ZeusDebug::isNewSourcePosition(frame, ins);
                if (newSourcePosition && breakpointConfig.contains(ins.sourceFileId, ins.line) &&
                    !writeBreakpointSnapshot(ins, frame.pc)) {
                    vmstat = ZeusType::VMStatus::Error;
                    break;
                }
            }

            if (options.traceExecution) {
                ZEUS_DEBUG_PRINT("[Executing] >>> %s (depth=%zu pc=%zu)",
                    ins.toString().c_str(), callFrames.size(), frame.pc);
            }

            if (ins.op == VMOperator::FUNC_BEGIN || ins.op == VMOperator::FUNC_END ||
                ins.op == VMOperator::CAPTURE_OUTSIDE_VAR) {
                frame.pc++;
                continue;
            }

            if (ins.op == VMOperator::RETURN) {
                if (!validateNativeScalarReturn(*frame.code, ins)) continue;
                if (!applyClassAssignmentCopy(ins)) continue;
                StackItem retItem;
                CallFrame returningFrame = std::move(callFrames.back());
                pinVMFrameRoots(returningFrame);
                if (returningFrame.constructorResult) {
                    if (!stack.empty()) {
                        dropStackItem(popStackItem());
                    }
                    retItem.value = *returningFrame.constructorResult;
                    retItem.owner = 1;
                    returningFrame.constructorResult.reset();
                    stack.push_back(retItem.value);
                    stack.setOwnerFlag(stack.size() - 1, retItem.owner);
                    retItem.owner = 0;
                    if (captureReturn) captureReturnValue(*state);
                    else retItem = popStackItem();
                } else if (returningFrame.pendingObjectCloneId != 0) {
                    if (!stack.empty()) retItem = popStackItem();
                } else if (captureReturn) {
                    captureReturnValue(*state);
                } else if (!stack.empty()) {
                    retItem = popStackItem();
                }

                int autoReleaseId = returningFrame.calleeAutoReleaseId;
                callFrames.pop_back();
                discardFrameExceptionHandlers(callFrames.size());

                if (captureReturn || callFrames.size() > options.stopFrameDepth) {
                    if (autoReleaseId >= 0) {
                        releaseClosure(autoReleaseId);
                    }
                }

                if (returningFrame.pendingObjectCloneId != 0) {
                    shrinkStackWithRelease(returningFrame.stackBase);
                    releaseCurrentFrameEnvironment();
                    local = std::move(returningFrame.savedLocal);
                    upvalue = std::move(returningFrame.savedUpvalue);
                    __line = returningFrame.savedLine;
                    resumePendingObjectClone(
                        returningFrame.pendingObjectCloneId,
                        retItem,
                        options.stopFrameDepth
                    );
                    continue;
                }

                if (callFrames.size() > options.stopFrameDepth) {
                    shrinkStackWithRelease(returningFrame.stackBase);
                    releaseCurrentFrameEnvironment();

                    local = returningFrame.savedLocal;
                    upvalue = returningFrame.savedUpvalue;
                    __line = returningFrame.savedLine;

                    if (captureReturn) {
                        stack.push_back(state->retVal);
                        if (state->retOwner) {
                            stack.setOwnerFlag(stack.size() - 1, state->retOwner);
                            state->retOwner = 0;
                        }
                    } else {
                        stack.push_back(retItem.value);
                        if (retItem.owner) {
                            stack.setOwnerFlag(stack.size() - 1, retItem.owner);
                            retItem.owner = 0;
                        }
                    }

                    callFrames.back().pc++;
                } else if (captureReturn) {
                    releaseCurrentFrameEnvironment();

                    local = returningFrame.savedLocal;
                    upvalue = returningFrame.savedUpvalue;
                    __line = returningFrame.savedLine;
                } else if (retItem.owner) {
                    dropStackItem(retItem);
                    retItem.owner = 0;
                }

                continue;
            }

            if (ins.op == VMOperator::JUMP) {
                if (options.validateJumps) {
                    if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0]) ||
                        !std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(ins.args[0]).data)) {
                        ZEUS_DEBUG_PRINT("[VM][JUMP] invalid arg format at pc=%zu abort chunk", frame.pc);
                        break;
                    }

                    int off = 0;
                    try {
                        off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                    } catch (const std::exception& e) {
                        ZEUS_DEBUG_PRINT("[VM][JTRUE] failed to decode offset: %s", e.what());
                        break;
                    }

                    long long target = static_cast<long long>(frame.pc) + 1 + off;
                    if (target < 0 || target >= static_cast<long long>(frame.code->size())) {
                        ZEUS_DEBUG_PRINT("[VM][JUMP] backward jump out of range, abort");
                        break;
                    }
                    frame.pc = static_cast<size_t>(target);
                } else {
                    int off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                    frame.pc += 1 + off;
                }
                continue;
            }

            if (ins.op == VMOperator::JUMP_IF_FALSE || ins.op == VMOperator::JUMP_IF_TRUE) {
                const bool jumpIfTrue = ins.op == VMOperator::JUMP_IF_TRUE;
                int off = 0;

                if (options.validateJumps) {
                    if (ins.args.empty() || !std::holds_alternative<LexerType::Value>(ins.args[0]) ||
                        !std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(ins.args[0]).data)) {
                        ZEUS_DEBUG_PRINT("[VM][%s] invalid arg format at pc=%zu", jumpIfTrue ? "JTRUE" : "JFALSE", frame.pc);
                        break;
                    }
                    off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                } else {
                    try {
                        off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                    } catch (const std::exception& e) {
                        ZEUS_DEBUG_PRINT("[VM][%s] failed to decode offset: %s", jumpIfTrue ? "JTRUE" : "JFALSE", e.what());
                        break;
                    }
                }

                bool take = false;
                if (!stack.empty()) {
                    StackItem condItem = popStackItem();
                    take = jumpIfTrue ? truthy(condItem.value) : !truthy(condItem.value);
                    dropStackItem(condItem);
                } else if (options.validateJumps && !jumpIfTrue) {
                    // 保留 runChunk() 原有的空栈 JUMP_IF_FALSE 行为。
                    take = true;
                }

                if (take) {
                    if (options.validateJumps) {
                        long long target = static_cast<long long>(frame.pc) + 1 + off;
                        if (target < 0 || target >= static_cast<long long>(frame.code->size())) {
                            ZEUS_DEBUG_PRINT("[VM][%s] jump out of range, abort",
                                jumpIfTrue ? "JTRUE" : "JFALSE");
                            break;
                        }
                        frame.pc = static_cast<size_t>(target);
                    } else {
                        frame.pc += 1 + off;
                    }
                } else {
                    frame.pc++;
                }
                continue;
            }

            if (ins.op == VMOperator::JUMP_IF_PARAM_PROVIDED) {
                if (ins.args.size() < 2 ||
                    !std::holds_alternative<LexerType::Value>(ins.args[0]) ||
                    !std::holds_alternative<LexerType::Value>(ins.args[1])) {
                    VMException::VMException exp(
                        "JUMP_IF_PARAM_PROVIDED: invalid parameter or offset",
                        __line,
                        ins
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int paramIndex = -1;
                int offset = 0;
                try {
                    paramIndex = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                    offset = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[1]));
                } catch (const std::exception&) {
                    VMException::VMException exp(
                        "JUMP_IF_PARAM_PROVIDED: parameter and offset must be integers",
                        __line,
                        ins
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (paramIndex < 0 || paramIndex >= (int)frame.paramProvided.size()) {
                    VMException::VMException exp(
                        "JUMP_IF_PARAM_PROVIDED: parameter index is outside the active call frame",
                        __line,
                        ins
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (frame.paramProvided[paramIndex]) {
                    const long long target = static_cast<long long>(frame.pc) + 1 + offset;
                    if (target < 0 || target >= static_cast<long long>(frame.code->size())) {
                        VMException::VMException exp(
                            "JUMP_IF_PARAM_PROVIDED: jump target is outside the function body",
                            __line,
                            ins
                        );
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    frame.pc = static_cast<size_t>(target);
                } else {
                    frame.pc++;
                }
                continue;
            }

            justPushedFrame = false;
            try {
                execute(ins);
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception& error) {
                // Let the opcode unwind before entering the VM catch path.
                if (vmstat == ZeusType::VMStatus::Running) {
                    VMException::VMException(error.what(), __line, ins)
                        ._throw(vmstat, currentChunk);
                }
                continue;
            }

            if (justPushedFrame) {
                justPushedFrame = false;
            } else if (!callFrames.empty()) {
                callFrames.back().pc++;
            }
        }
    }

    // 同步调用闭包：在当前上下文中执行闭包并等待返回
    // 仅用于必须同步拿到返回值的 C++ 侧流程（字段初始化器求值、析构打断循环引用）。
    // operator 分发不走这里：它在 execute() 内转成 CALL_CLOSURE 尾调用，
    // 压栈执行而不递归 runFrameLoop，否则每层分发都要吃掉一整个 VM 栈帧。
    // 参数：closureId - 闭包ID，argc - 参数数量（栈上已有参数）
    // 返回：执行成功返回true，失败返回false
    bool executeClosureSync(int closureId, int argc) {
        ZEUS_DEBUG_PRINT("[executeClosureSync] closureId=%d, argc=%d", closureId, argc);
        
        Closure* closPtr = getClosure(closureId);
        if (!closPtr) {
            return false;
        }
        
        Closure& clos = *closPtr;
        retainClosure(closureId);
        struct ClosureHold {
            VM* vm;
            int id;
            ~ClosureHold() { vm->releaseClosure(id); }
        } closureHold{this, closureId};
        const Prototype* protoPtr = getPrototype(clos.protoId);
        if (!protoPtr) {
            return false;
        }
        
        const Prototype& proto = *protoPtr;
        
        ZEUS_DEBUG_PRINT("[executeClosureSync] proto.paramCount=%d, hasBoundThis=%d", 
            proto.paramCount, clos.boundThis.has_value() ? 1 : 0);
        
        // 如果闭包有 boundThis，需要额外处理
        int actualArgc = argc;
        if (clos.boundThis.has_value()) {
            // 在栈中插入 this 作为第一个参数
            size_t calleeIndex = stack.size() - (argc + 1);
            ZEUS_DEBUG_PRINT("[executeClosureSync] inserting boundThis at index %zu", calleeIndex + 1);
            retainObject(*clos.boundThis); // stack owns a reference
            stack.insert(stack.begin() + (calleeIndex + 1), clos.boundThis.value());
            actualArgc++;
        }
        
        // 栈布局: [..., closure, (this?), arg0, arg1, ..., arg{n-1}]
        size_t calleeIndex = stack.size() - (actualArgc + 1);

        const IRType::IRInstr noInstruction{};
        for (int i = 0; i < actualArgc && i < proto.paramCount; ++i) {
            if (!validateFunctionArgument(
                    proto, i, stack[calleeIndex + 1 + i], noInstruction)) {
                shrinkStackWithRelease(calleeIndex);
                stack.push_back(LexerType::Value(0));
                return false;
            }
        }
        if (!proto.hasRuntimeDefaultPrologue) {
            for (int i = actualArgc; i < proto.paramCount; ++i) {
                if (i < static_cast<int>(proto.paramHasDefault.size()) &&
                    proto.paramHasDefault[i] &&
                    i < static_cast<int>(proto.paramDefaultValues.size()) &&
                    !validateFunctionArgument(
                        proto, i, proto.paramDefaultValues[i], noInstruction)) {
                    shrinkStackWithRelease(calleeIndex);
                    stack.push_back(LexerType::Value(0));
                    return false;
                }
            }
        }
        
        // 保存当前环境
        auto oldLocal = local;
        auto oldUpvalue = upvalue;
        int oldLine = __line;
        
        // 初始化新的 local 数组
        // 注意：当 boundThis 存在时，即使 paramCount=0，也需要为 this 分配空间
        ZeusVMDetail::CallSetupScope callSetup([&]() {
            releaseCurrentFrameEnvironment();
            local = std::move(oldLocal);
            upvalue = std::move(oldUpvalue);
            shrinkStackWithRelease(calleeIndex);
            __line = oldLine;
        });
        local.clear();
        upvalue.clear();
        int localSize = std::max(proto.paramCount, actualArgc);
        ZEUS_DEBUG_PRINT("[executeClosureSync] localSize=%d, actualArgc=%d", localSize, actualArgc);
        local.resize(localSize);
        for (int i = 0; i < localSize; ++i) {
            local[i] = newCell(LexerType::Value(0), 0, "CLOSURE_SYNC_local");
            if (i < static_cast<int>(proto.paramTypes.size()) &&
                (i >= static_cast<int>(proto.paramIsReference.size()) || !proto.paramIsReference[i])) {
                configureNativeScalarCell(local[i], proto.paramTypes[i]);
            }
        }
        
        // 设置 upvalue
        upvalue.resize(clos.upvalues.size());
        for (size_t ci = 0; ci < clos.upvalues.size(); ++ci) {
            if (clos.upvalues[ci]) {
                upvalue[ci] = clos.upvalues[ci];
                upvalue[ci]->retain();
            } else {
                upvalue[ci] = newCell(LexerType::Value(0), 0, "CLOSURE_SYNC_upvalue");
            }
        }
        
        // 填充参数（包括可能的 this）
        ZEUS_DEBUG_PRINT("[executeClosureSync] filling params: localSize=%d, actualArgc=%d", localSize, actualArgc);
        for (int i = 0; i < localSize && i < actualArgc; ++i) {
            assignCellValue(local[i], stack[calleeIndex + 1 + i]);
            ZEUS_DEBUG_PRINT("[executeClosureSync] local[%d] set", i);
        }
        if (!proto.hasRuntimeDefaultPrologue) {
            for (int i = actualArgc; i < proto.paramCount; ++i) {
                if (i < (int)proto.paramHasDefault.size() && proto.paramHasDefault[i] &&
                    i < (int)proto.paramDefaultValues.size()) {
                    assignCellValue(local[i], proto.paramDefaultValues[i]);
                }
            }
        }
        
        // 弹出参数和闭包（释放栈持有的引用）
        shrinkStackWithRelease(calleeIndex);
        
        // 同步执行函数体 - 使用统一的栈帧循环以支持嵌套调用
        FrameLoopState frameState;
        
        // 记录调用前的栈帧深度
        size_t initialFrameDepth = callFrames.size();
        
        // 创建初始栈帧
        CallFrame syncFrame;
        syncFrame.code = &proto.body;
        syncFrame.pc = 0;
        syncFrame.stackBase = stack.size();
        syncFrame.savedLine = __line;
        syncFrame.savedLocal = oldLocal;
        syncFrame.savedUpvalue = oldUpvalue;
        syncFrame.paramProvided.assign(proto.paramCount, false);
        syncFrame.referenceParameterSlots = proto.paramIsReference;
        for (int i = 0; i < actualArgc && i < proto.paramCount; ++i) {
            syncFrame.paramProvided[i] = true;
        }
        callFrames.push_back(syncFrame);
        callSetup.commit();
        FrameLoopOptions frameOptions;
        frameOptions.stopFrameDepth = initialFrameDepth;
        frameOptions.captureReturn = true;
        frameOptions.handleExceptions = true;
        frameOptions.traceName = "[executeClosureSync]";
        frameOptions.traceExecution = true;
        runFrameLoop(frameOptions, &frameState);

        ZeusType::UniversalVariant retVal = frameState.retVal;
        uint8_t retOwner = frameState.retOwner;
        
        if (vmstat != ZeusType::VMStatus::Running) {
            ZEUS_DEBUG_PRINT("[executeClosureSync] aborted closure=%d status=%d depth=%zu -> %zu",
                closureId, (int)vmstat, callFrames.size(), initialFrameDepth);
            unwindFrameEnvironments(initialFrameDepth);
            if (retOwner) dropStackItem(StackItem{retVal, retOwner});
            return false;
        }

        stack.push_back(retVal);
        if (retOwner) {
            stack.setOwnerFlag(stack.size() - 1, retOwner);
            retOwner = 0;
        }
        return true;
    }

    std::string sharedObjectTypeName(
        const ZeusType::SharedObjectHandle& handle
    ) {
        // Resolve through the generation-tagged source slot. A stale handle
        // must never inherit the type of a later value stored under the same name.
        auto cell = ZeusShared::getGlobalHeap().find(handle.name, handle.generation);
        if (!cell) return {};

        const ZeusType::UniversalVariant sharedValue = cell->read();
        const auto* variant = std::get_if<ZeusType::Variant>(&sharedValue);
        if (!variant) return {};
        const auto* object = std::get_if<ZeusType::Object>(variant);
        if (!object || !object->impl) return {};

        auto classNameIt = object->impl->data.find("__class_name__");
        if (classNameIt == object->impl->data.end()) {
            classNameIt = object->impl->data.find("__className__");
        }
        if (classNameIt == object->impl->data.end()) return {};
        const auto* classNameValue = std::get_if<LexerType::Value>(&classNameIt->second);
        if (!classNameValue) return {};
        const auto* className = std::get_if<std::string>(&classNameValue->data);
        return className ? *className : std::string();
    }

    std::string typeImplementationName(
        const ZeusType::UniversalVariant& value
    ) {
        std::string resolvedTypeName;
        if (std::holds_alternative<ZeusType::RawBytesRange>(value)) {
            resolvedTypeName = "&bytes";
        } else if (const auto* sharedObject =
                std::get_if<ZeusType::SharedObjectHandle>(&value)) {
            resolvedTypeName = sharedObjectTypeName(*sharedObject);
            if (resolvedTypeName.empty()) return "unknown";
        } else if (const auto* objectHandle =
                       std::get_if<ZeusType::ObjectHandle>(&value)) {
            const auto* object = getValidObject(*objectHandle);
            const auto* classInfo = object ? getClass(object->classId) : nullptr;
            if (!classInfo) return "unknown";
            resolvedTypeName = classInfo->name;
        } else if (const auto* classHandle =
                       std::get_if<ZeusType::LLVMClassHandle>(&value)) {
            if (!classHandle->state || classHandle->state->typeName.empty()) {
                return "unknown";
            }
            resolvedTypeName = classHandle->state->typeName;
        } else if (std::holds_alternative<ZeusType::Bytes>(value)) {
            resolvedTypeName = "bytes";
        } else if (std::holds_alternative<ZeusType::ClosureHandle>(value) ||
                   std::holds_alternative<ZeusType::NativeFuncHandle>(value) ||
                   std::holds_alternative<ZeusType::NativeClosureHandle>(value)) {
            resolvedTypeName = "function";
        } else if (std::holds_alternative<ZeusType::ClassHandle>(value) ||
                   std::holds_alternative<ZeusType::SharedClassHandle>(value)) {
            resolvedTypeName = "class";
        } else if (std::holds_alternative<ZeusType::NamespaceHandle>(value) ||
                   std::holds_alternative<ZeusType::ModuleProxy>(value)) {
            resolvedTypeName = "namespace";
        } else {
            // Dictionary literals use ObjectData in the VM. Method dispatch
            // follows the source/native dict type, not that storage wrapper.
            if (const auto* variant = std::get_if<ZeusType::Variant>(&value)) {
                if (std::holds_alternative<ZeusType::Object>(*variant)) return "dict";
            }
            const ZeusType::UniversalVariant typeValue = ZeusBuiltins::builtin_type(
                std::vector<ZeusType::UniversalVariant>{value});
            const auto* typeName = std::get_if<std::string>(&typeValue);
            if (!typeName) return {};
            resolvedTypeName = *typeName;
        }

        const auto* builtinType = ZeusBuiltins::findTypeImplementation(resolvedTypeName);
        return builtinType
            ? std::string(builtinType->canonicalName)
            : resolvedTypeName;
    }

    std::string runtimeValueTypeName(
        const ZeusType::UniversalVariant& value
    ) {
        if (std::holds_alternative<ZeusType::RawBytesRange>(value)) {
            return "&bytes";
        }
        if (const auto* reference = std::get_if<ZeusType::ReferenceHandle>(&value)) {
            return "&" + reference->typeName;
        }
        return ZeusRuntimeType::canonicalTypeName(typeImplementationName(value));
    }

    bool resolveReferenceArgument(
        const IRType::IRInstr& callInstruction,
        const Prototype& proto,
        const std::vector<Cell*>& callerLocals,
        const std::vector<Cell*>& callerUpvalues,
        int parameterIndex,
        int positionalSourceIndex,
        const std::string& parameterName,
        const ZeusType::UniversalVariant& value,
        ZeusType::UniversalVariant& referenceValue
    ) {
        if (const auto* existing =
                std::get_if<ZeusType::ReferenceHandle>(&value)) {
            if (existing->typeName == "cell" && existing->address) {
                referenceValue = *existing;
                return true;
            }
        }

        const IRType::CallArgumentSource* source = nullptr;
        for (const auto& argument : callInstruction.metadata.callArguments) {
            const bool matches = argument.namedParameter.empty()
                ? argument.positionalIndex == positionalSourceIndex
                : argument.namedParameter == parameterName;
            if (matches) {
                source = &argument;
                break;
            }
        }

        Cell* target = nullptr;
        if (source && source->isAssignable && !source->source.isConst) {
            const auto& symbol = source->source;
            if (symbol.kind == IRType::SymbolRefKind::Local &&
                symbol.index >= 0 &&
                symbol.index < static_cast<int>(callerLocals.size())) {
                target = callerLocals[symbol.index];
            } else if (symbol.kind == IRType::SymbolRefKind::Global &&
                       symbol.index >= 0 && symbol.index < static_cast<int>(global.size())) {
                target = global[symbol.index];
            } else if (symbol.kind == IRType::SymbolRefKind::Upvalue &&
                       symbol.index >= 0 &&
                       symbol.index < static_cast<int>(callerUpvalues.size())) {
                target = callerUpvalues[symbol.index];
            }
        }
        if (target && (target->flags & 0x1)) target = nullptr;

        if (!target) {
            ZEUS_DEBUG_PRINT(
                "[ReferenceArgument] unresolved source: metadata_kind=%d count=%zu found=%d assignable=%d kind=%d index=%d const=%d",
                static_cast<int>(callInstruction.metadata.kind),
                callInstruction.metadata.callArguments.size(),
                source ? 1 : 0,
                source && source->isAssignable ? 1 : 0,
                source ? static_cast<int>(source->source.kind) : -1,
                source ? source->source.index : -1,
                source && source->source.isConst ? 1 : 0
            );
            const std::string functionName = proto.functionName.empty()
                ? "<anonymous>"
                : proto.functionName;
            const std::string expected = parameterIndex >= 0 &&
                    parameterIndex < static_cast<int>(proto.paramTypes.size())
                ? "variable reference to " + ZeusRuntimeType::canonicalTypeName(
                    proto.paramTypes[parameterIndex])
                : "variable reference";
            const std::string actual = runtimeValueTypeName(value);
            const int receiverOffset = !proto.ownerType.empty() &&
                !proto.isStaticMethod ? 1 : 0;
            VMException::typeError_InvalidArgument(
                functionName,
                std::max(0, parameterIndex - receiverOffset),
                expected,
                actual,
                __line,
                callInstruction
            )._throw(vmstat, currentChunk);
            return false;
        }

        const auto targetValue = target->readValue();
        if (const auto* forwarded =
                std::get_if<ZeusType::ReferenceHandle>(&targetValue)) {
            if (forwarded->typeName == "cell" && forwarded->address) {
                referenceValue = *forwarded;
                return true;
            }
        }
        referenceValue = ZeusType::ReferenceHandle{target, "cell"};
        return true;
    }

    bool isValueCompatibleWithType(
        const ZeusType::UniversalVariant& value,
        const std::string& expectedType
    ) {
        if (expectedType.empty()) return true;
        // A nullable (`?`) expectation accepts nil; the `?` is erased
        // before the ordinary compatibility check runs.
        std::string expected = expectedType;
        const bool expectedNullable = !expected.empty() && expected.back() == '?';
        if (expectedNullable) {
            expected.pop_back();
            if (runtimeValueTypeName(value) == "nil") return true;
        }
        return ZeusRuntimeType::isCompatible(
            expected,
            runtimeValueTypeName(value)
        );
    }

    bool matchesGenericTypePattern(
        std::string expected,
        std::string actual,
        const std::vector<std::string>& typeParameters,
        std::unordered_map<std::string, std::string>& bindings,
        unsigned depth = 0
    ) const {
        if (depth > 128) return false;
        bool expectedReference = !expected.empty() && expected.front() == '&';
        bool actualReference = !actual.empty() && actual.front() == '&';
        if (expectedReference != actualReference) return false;
        if (expectedReference) {
            expected.erase(expected.begin());
            actual.erase(actual.begin());
        }

        if (ZeusBuiltins::findNativeScalarCanonical(expected)) {
            return ZeusRuntimeType::isCompatible(expected, actual);
        }
        expected = ZeusRuntimeType::canonicalTypeName(expected);
        actual = ZeusRuntimeType::canonicalTypeName(actual);
        if (std::find(typeParameters.begin(), typeParameters.end(), expected) !=
            typeParameters.end()) {
            const auto previous = bindings.find(expected);
            if (previous != bindings.end()) return previous->second == actual;
            bindings.emplace(expected, actual);
            return true;
        }

        std::string expectedBase;
        std::string actualBase;
        std::vector<std::string> expectedArguments;
        std::vector<std::string> actualArguments;
        const bool expectedGeneric = ZeusTypeInfer::decodeGenericSpecializationName(
            expected, expectedBase, expectedArguments);
        const bool actualGeneric = ZeusTypeInfer::decodeGenericSpecializationName(
            actual, actualBase, actualArguments);
        if (!expectedGeneric || !actualGeneric) {
            // Mirror the isCompatible rule: a plain `object` accepts
            // dictionaries. Both names are canonicalized above.
            if (expected == "object" && actual == "dict") return true;
            return expected == actual;
        }
        if (ZeusRuntimeType::canonicalTypeName(expectedBase) !=
                ZeusRuntimeType::canonicalTypeName(actualBase) ||
            expectedArguments.size() != actualArguments.size()) {
            return false;
        }
        for (size_t index = 0; index < expectedArguments.size(); ++index) {
            if (!matchesGenericTypePattern(
                    expectedArguments[index], actualArguments[index],
                    typeParameters, bindings, depth + 1)) {
                return false;
            }
        }
        return true;
    }

    bool isValueCompatibleWithGenericType(
        const ZeusType::UniversalVariant& value,
        const std::string& expectedType,
        const std::vector<std::string>& typeParameters
    ) {
        std::unordered_map<std::string, std::string> bindings;
        return matchesGenericTypePattern(
            expectedType, runtimeValueTypeName(value), typeParameters, bindings);
    }

    bool validateFunctionArgument(
        const Prototype& proto,
        int parameterIndex,
        const ZeusType::UniversalVariant& value,
        const IRType::IRInstr& instr = {}
    ) {
        const bool isTypeParameter = parameterIndex >= 0 &&
            parameterIndex < static_cast<int>(proto.paramTypes.size()) &&
            std::find(proto.typeParameters.begin(), proto.typeParameters.end(),
                proto.paramTypes[parameterIndex]) != proto.typeParameters.end();
        if (parameterIndex >= 0 &&
            parameterIndex < static_cast<int>(proto.paramIsReference.size()) &&
            proto.paramIsReference[parameterIndex]) {
            const auto* reference =
                std::get_if<ZeusType::ReferenceHandle>(&value);
            if (!reference || reference->typeName != "cell" || !reference->address) {
                const std::string functionName = proto.functionName.empty()
                    ? "<anonymous>"
                    : proto.functionName;
                const std::string expected = parameterIndex <
                        static_cast<int>(proto.paramTypes.size())
                    ? "variable reference to " + ZeusRuntimeType::canonicalTypeName(
                        proto.paramTypes[parameterIndex])
                    : "variable reference";
                VMException::typeError_InvalidArgument(
                    functionName,
                    std::max(0, parameterIndex),
                    expected,
                    runtimeValueTypeName(value),
                    __line,
                    instr
                )._throw(vmstat, currentChunk);
                return false;
            }
            if (parameterIndex >= static_cast<int>(proto.paramTypes.size()) ||
                proto.paramTypes[parameterIndex].empty() || isTypeParameter) {
                return true;
            }
            ZeusType::UniversalVariant referencedValue;
            try {
                const auto* target = static_cast<const Cell*>(reference->address);
                referencedValue = loadCellValue(target);
            } catch (const std::exception& error) {
                VMException::VMException exp(
                    std::string("Invalid reference argument: ") + error.what(),
                    __line,
                    instr
                );
                exp._throw(vmstat, currentChunk);
                return false;
            }
            if (isValueCompatibleWithType(
                    referencedValue, proto.paramTypes[parameterIndex])) {
                return true;
            }
            const std::string functionName = proto.functionName.empty()
                ? "<anonymous>"
                : proto.functionName;
            const int receiverOffset = !proto.ownerType.empty() &&
                !proto.isStaticMethod ? 1 : 0;
            VMException::typeError_InvalidArgument(
                functionName,
                std::max(0, parameterIndex - receiverOffset),
                ZeusRuntimeType::canonicalTypeName(proto.paramTypes[parameterIndex]),
                runtimeValueTypeName(referencedValue),
                __line,
                instr
            )._throw(vmstat, currentChunk);
            return false;
        }
        if (parameterIndex < 0 ||
            parameterIndex >= static_cast<int>(proto.paramTypes.size()) ||
            proto.paramTypes[parameterIndex].empty()) {
            return true;
        }
        // A nullable (`?`) parameter accepts nil; the `?` is erased before
        // the ordinary compatibility check runs.
        std::string expectedTypeName = proto.paramTypes[parameterIndex];
        if (!expectedTypeName.empty() && expectedTypeName.back() == '?' &&
            runtimeValueTypeName(value) == "nil") {
            return true;
        }
        if (!expectedTypeName.empty() && expectedTypeName.back() == '?') {
            expectedTypeName.pop_back();
        }
        if (isValueCompatibleWithGenericType(
                value, expectedTypeName, proto.typeParameters)) {
            return true;
        }

        const std::string functionName = proto.functionName.empty()
            ? "<anonymous>"
            : proto.functionName;
        const std::string expected = ZeusRuntimeType::canonicalTypeName(
            proto.paramTypes[parameterIndex]);
        const std::string actual = runtimeValueTypeName(value);
        const int receiverOffset = !proto.ownerType.empty() &&
            !proto.isStaticMethod ? 1 : 0;
        const int userArgumentIndex = std::max(
            0, parameterIndex - receiverOffset);
        VMException::typeError_InvalidArgument(
            functionName,
            userArgumentIndex,
            expected,
            actual,
            __line,
            instr
        )._throw(vmstat, currentChunk);
        return false;
    }

    bool tryDispatchTypeImplementationOperator(
        const std::string& operatorName,
        const ZeusType::UniversalVariant* const* arguments,
        size_t argumentCount,
        const IRType::IRInstr& instr,
        std::optional<IRType::IRInstr>& nextInstruction
    ) {
        if (instr.metadata.kind == IRType::IRMetadataKind::IntrinsicOperator ||
            arguments == nullptr || argumentCount == 0 || arguments[0] == nullptr ||
            std::holds_alternative<ZeusType::ObjectHandle>(*arguments[0]) ||
            std::holds_alternative<ZeusType::SharedObjectHandle>(*arguments[0])) {
            return false;
        }

        const std::string receiverType = typeImplementationName(*arguments[0]);
        const auto implementation = typeImplementations.find(receiverType);
        if (implementation == typeImplementations.end()) return false;
        const auto op = implementation->second.operatorMethods.find(operatorName);
        if (op == implementation->second.operatorMethods.end()) return false;

        if (argumentCount > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            getPrototype(op->second) == nullptr) {
            VMException::VMException exp(
                "TYPE_IMPL_BUILD registered an invalid operator prototype",
                __line, instr);
            exp._throw(vmstat, currentChunk);
            return true;
        }

        // Validate every argument before the closure reaches the stack: the
        // prepared call below owns the pushed closure and retained arguments,
        // and an error return never dispatches them.
        for (size_t i = 0; i < argumentCount; ++i) {
            if (arguments[i] == nullptr) {
                VMException::VMException exp(
                    "type implementation operator received a null argument",
                    __line, instr);
                exp._throw(vmstat, currentChunk);
                return true;
            }
        }

        Closure closure;
        closure.protoId = op->second;
        closure.autoRelease = true;
        const int closureId = allocClosure(std::move(closure));
        stack.push_back(ZeusType::ClosureHandle{closureId});
        for (size_t i = 0; i < argumentCount; ++i) {
            ZeusGC::retainVariantDeep(*this, *arguments[i]);
            stack.push_back(*arguments[i]);
        }

        // Only the call shape crosses over. Carrying the operator instruction
        // metadata would re-run applyClassAssignmentCopy against the last
        // pushed argument instead of the receiver.
        nextInstruction.emplace();
        nextInstruction->op = VMOperator::CALL_CLOSURE;
        nextInstruction->line = instr.line;
        nextInstruction->sourceFileId = instr.sourceFileId;
        nextInstruction->args = {LexerType::Value(static_cast<int>(argumentCount))};
        return true;
    }

    bool startPendingObjectClone(const NativeCallContext& nativeCall) {
        if (callFrames.empty() || !nativeCall.desc || nativeCall.argc != 1 ||
            nativeCall.desc->name != "__zeus_object_clone") {
            return false;
        }

        auto copyContext = std::make_unique<ZeusVMCopy::Context<VM>>(*this);
        ZeusType::ObjectHandle source;
        int prototypeId = -1;
        try {
            if (!copyContext->beginHook(
                    stack[nativeCall.calleeIndex + 1], source, prototypeId)) {
                return false;
            }
        } catch (const std::exception&) {
            // The ordinary native path reports copy validation errors through
            // its existing exception conversion.
            return false;
        }

        const Prototype* prototype = getPrototype(prototypeId);
        if (!prototype || prototype->paramCount != 1) return false;

        const IRType::IRInstr noInstruction{};
        if (!validateFunctionArgument(*prototype, 0, source, noInstruction)) {
            cleanupNativeCall(nativeCall, LexerType::Value(0));
            return true;
        }

        if (callFrames.size() >= static_cast<size_t>(MAX_CALL_DEPTH)) {
            VMException::VMException exp(
                VMException::ErrorCode::E1900_STACK_OVERFLOW,
                VMException::ErrorCategory::VM_RUNTIME,
                "Stack overflow: possible infinite recursion",
                __line, noInstruction
            );
            exp._throw(vmstat, currentChunk);
            cleanupNativeCall(nativeCall, LexerType::Value(0));
            return true;
        }

        std::vector<Cell*> hookLocal;
        try {
            hookLocal.resize(std::max(prototype->paramCount, 1), nullptr);
            for (auto& cell : hookLocal) {
                cell = newCell(LexerType::Value(0), 0, "OBJECT_COPY_HOOK_local");
            }
            assignCellValue(hookLocal[0], source);
        } catch (const std::exception&) {
            for (Cell* cell : hookLocal) {
                if (cell) releaseCellAndContent(cell);
            }
            return false;
        }

        size_t continuationId = nextPendingObjectCloneId++;
        if (continuationId == 0) continuationId = nextPendingObjectCloneId++;

        PendingObjectClone pending;
        pending.nativeCall = nativeCall;
        pending.copyContext = std::move(copyContext);

        CallFrame hookFrame;
        hookFrame.code = &prototype->body;
        hookFrame.pc = 0;
        hookFrame.stackBase = stack.size();
        hookFrame.savedLine = __line;
        hookFrame.paramProvided.assign(prototype->paramCount, false);
        hookFrame.paramProvided[0] = true;
        hookFrame.referenceParameterSlots = prototype->paramIsReference;
        hookFrame.pendingObjectCloneId = continuationId;

        hookFrame.savedLocal = std::move(local);
        hookFrame.savedUpvalue = std::move(upvalue);
        try {
            pendingObjectClones.emplace(continuationId, std::move(pending));
            callFrames.push_back(std::move(hookFrame));
        } catch (const std::exception&) {
            pendingObjectClones.erase(continuationId);
            local = std::move(hookFrame.savedLocal);
            upvalue = std::move(hookFrame.savedUpvalue);
            for (Cell* cell : hookLocal) {
                if (cell) releaseCellAndContent(cell);
            }
            return false;
        }

        local = std::move(hookLocal);
        justPushedFrame = true;
        return true;
    }

    void resumePendingObjectClone(
        size_t continuationId,
        const StackItem& hookResult,
        size_t stopFrameDepth
    ) {
        const auto pendingIt = pendingObjectClones.find(continuationId);
        if (pendingIt == pendingObjectClones.end()) {
            dropStackItem(hookResult);
            IRType::IRInstr errInstr{VMOperator::CALL_NATIVE, {}};
            errInstr.line = __line;
            errInstr.sourceFileId = __sourceFileId;
            VMException::VMException(
                "class copy hook continuation is missing", __line, errInstr
            )._throw(vmstat, currentChunk);
            return;
        }

        PendingObjectClone pending = std::move(pendingIt->second);
        pendingObjectClones.erase(pendingIt);
        try {
            auto result = pending.copyContext->finishHook(hookResult.value);
            dropStackItem(hookResult);
            cleanupNativeCall(pending.nativeCall, std::move(result));
        } catch (const std::exception& error) {
            dropStackItem(hookResult);
            IRType::IRInstr errInstr{VMOperator::CALL_NATIVE, {}};
            errInstr.line = __line;
            errInstr.sourceFileId = __sourceFileId;
            const std::string nativeName = pending.nativeCall.desc
                ? pending.nativeCall.desc->name
                : std::string("__zeus_object_clone");
            VMException::VMException(
                std::string("Native exception in ") + nativeName + ": " + error.what(),
                __line, errInstr
            )._throw(vmstat, currentChunk);
            cleanupNativeCall(pending.nativeCall, LexerType::Value(0));
        }

        if (!callFrames.empty() && callFrames.size() > stopFrameDepth) {
            callFrames.back().pc++;
        }
    }

    bool applyClassAssignmentCopy(const IRType::IRInstr& instr) {
        if (!instr.metadata.classAssignmentCopy || stack.empty() ||
            !std::holds_alternative<ZeusType::ObjectHandle>(stack.back())) return true;
        try {
            auto copied = ZeusVMCopy::copy(
                *this, stack.back(), instr.metadata.classCopySkipHook);
            auto original = popStackItem();
            stack.push_back(std::move(copied));
            dropStackItem(original);
            return true;
        } catch (const std::exception& error) {
            if (vmstat == ZeusType::VMStatus::Running) {
                VMException::VMException(
                    std::string("class assignment copy failed: ") + error.what(),
                    __line, instr)._throw(vmstat, currentChunk);
            }
            return false;
        }
    }

    // P1：按 const 引用接收指令，避免每次 dispatch 深拷贝
    // 整个 vector<IRArgs>（含 string/Value/Bytes）。实测前这是每条指令
    // 一次堆分配，是循环基准的最大头。
    // Opcode handlers must remain separate under LTCG: their temporary storage
    // must not accumulate in one native stack frame on every VM instruction.
    #include "Dispatch/ZeusVMAddress.inl"
    #include "Dispatch/ZeusVMConstruction.inl"
    #include "Dispatch/ZeusVMObjects.inl"
    #include "Dispatch/ZeusVMControl.inl"
    #include "Dispatch/ZeusVMOperators.inl"
    #include "Dispatch/ZeusVMNativeDouble.inl"
    #include "Dispatch/ZeusVMCollections.inl"
    #include "Dispatch/ZeusVMCalls.inl"
    #include "Dispatch/ZeusVMVariables.inl"
    #include "Dispatch/ZeusVMLifetime.inl"

    ZEUS_VM_NOINLINE void execute(const IRType::IRInstr& initialInstruction) {
        std::optional<IRType::IRInstr> currentInstruction;
        const IRType::IRInstr* instruction = &initialInstruction;
        while (true) {
            const auto& instr = *instruction;
            if (instr.line >= 0) __line = instr.line;
            if (instr.sourceFileId >= 0) __sourceFileId = instr.sourceFileId;
            if (vmstat != ZeusType::VMStatus::Running) return;
            if (!applyClassAssignmentCopy(instr)) return;

            // Handlers copy/retain deferred call arguments before their locals
            // are released. Only this dispatcher starts the prepared call.
            std::optional<IRType::IRInstr> nextInstruction;
            if (tryExecuteNativeDouble(instr)) return;
            switch (instr.op) {
                case VMOperator::LLVM_ADDR: execute_LLVM_ADDR(instr, nextInstruction); break;
                case VMOperator::LLVM_FADDR: execute_LLVM_FADDR(instr, nextInstruction); break;
                case VMOperator::LLVM_FSTORE: execute_LLVM_FSTORE(instr, nextInstruction); break;
                case VMOperator::TYPE_IMPL_BUILD: execute_TYPE_IMPL_BUILD(instr, nextInstruction); break;
                case VMOperator::CLASS_BUILD: execute_CLASS_BUILD(instr, nextInstruction); break;
                case VMOperator::CLASS_BUILD_SHARED: execute_CLASS_BUILD_SHARED(instr, nextInstruction); break;
                case VMOperator::OBJ_BUILD: execute_OBJ_BUILD(instr, nextInstruction); break;
                case VMOperator::OBJ_BUILD_SHARED: execute_OBJ_BUILD_SHARED(instr, nextInstruction); break;
                case VMOperator::OBJ_LOAD_ATTR:
                case VMOperator::OBJ_LOAD_SAFE_ATTR: execute_OBJ_LOAD_ATTR(instr, nextInstruction); break;
                case VMOperator::OBJ_STORE_ATTR: execute_OBJ_STORE_ATTR(instr, nextInstruction); break;
                case VMOperator::OBJ_REPLACE_STATE: execute_OBJ_REPLACE_STATE(instr, nextInstruction); break;
                case VMOperator::CLASS_STATIC_METHOD_CALL: execute_CLASS_STATIC_METHOD_CALL(instr, nextInstruction); break;
                case VMOperator::OBJ_CALL_METHOD:
                case VMOperator::OBJ_CALL_SAFE: execute_OBJ_CALL_METHOD(instr, nextInstruction); break;
                case VMOperator::PUSH: execute_PUSH(instr, nextInstruction); break;
                case VMOperator::PUSH_NAMED: execute_PUSH_NAMED(instr, nextInstruction); break;
                case VMOperator::ADD: execute_ADD(instr, nextInstruction); break;
                case VMOperator::LIST_BUILD: execute_LIST_BUILD(instr, nextInstruction); break;
                case VMOperator::DICT_BUILD: execute_DICT_BUILD(instr, nextInstruction); break;
                case VMOperator::DICT_KEYS_BUILD: execute_DICT_KEYS_BUILD(instr, nextInstruction); break;
                case VMOperator::DICT_LOAD_VALUE: execute_DICT_LOAD_VALUE(instr, nextInstruction); break;
                case VMOperator::DICT_STORE_VALUE: execute_DICT_STORE_VALUE(instr, nextInstruction); break;
                case VMOperator::LIST_LENGTH: execute_LIST_LENGTH(instr, nextInstruction); break;
                case VMOperator::LIST_LOAD_ELEM: execute_LIST_LOAD_ELEM(instr, nextInstruction); break;
                case VMOperator::LIST_STORE_ELEM: execute_LIST_STORE_ELEM(instr, nextInstruction); break;
                case VMOperator::COUNT_CHECK_NEG: execute_COUNT_CHECK_NEG(instr, nextInstruction); break;
                case VMOperator::COUNT_CHECK_STEP_ZERO: execute_COUNT_CHECK_STEP_ZERO(instr, nextInstruction); break;
                case VMOperator::SUBTRACT: execute_SUBTRACT(instr, nextInstruction); break;
                case VMOperator::MULTIPLY: execute_MULTIPLY(instr, nextInstruction); break;
                case VMOperator::DIVIDE: execute_DIVIDE(instr, nextInstruction); break;
                case VMOperator::MODULUS: execute_MODULUS(instr, nextInstruction); break;
                case VMOperator::FUNC_BEGIN: execute_FUNC_BEGIN(instr, nextInstruction); break;
                case VMOperator::IR_ANNOTATION: execute_IR_ANNOTATION(instr, nextInstruction); break;
                case VMOperator::LLVM_SIZEOF: execute_LLVM_SIZEOF(instr, nextInstruction); break;
                case VMOperator::FUNC_END: execute_FUNC_END(instr, nextInstruction); break;
                case VMOperator::POP: execute_POP(instr, nextInstruction); break;
                case VMOperator::SWAP: execute_SWAP(instr, nextInstruction); break;
                case VMOperator::CALL_FUNC: execute_CALL_FUNC(instr, nextInstruction); break;
                case VMOperator::MAKE_FUNCTION: execute_MAKE_FUNCTION(instr, nextInstruction); break;
                case VMOperator::CALL_CLOSURE: execute_CALL_CLOSURE(instr, nextInstruction); break;
                case VMOperator::LOAD_NATIVE: execute_LOAD_NATIVE(instr, nextInstruction); break;
                case VMOperator::CALL_NATIVE: execute_CALL_NATIVE(instr, nextInstruction); break;
                case VMOperator::STORE_VALUE_LOCAL: execute_STORE_VALUE_LOCAL(instr, nextInstruction); break;
                case VMOperator::STORE_VALUE_GLOBAL: execute_STORE_VALUE_GLOBAL(instr, nextInstruction); break;
                case VMOperator::ASSIGN_VALUE_GLOBAL: execute_ASSIGN_VALUE_GLOBAL(instr, nextInstruction); break;
                case VMOperator::ASSIGN_VALUE_LOCAL: execute_ASSIGN_VALUE_LOCAL(instr, nextInstruction); break;
                case VMOperator::ASSIGN_VALUE_UPVALUE: execute_ASSIGN_VALUE_UPVALUE(instr, nextInstruction); break;
                case VMOperator::STORE_VALUE_SHARED: execute_STORE_VALUE_SHARED(instr, nextInstruction); break;
                case VMOperator::LOAD_VALUE_SHARED: execute_LOAD_VALUE_SHARED(instr, nextInstruction); break;
                case VMOperator::ASSIGN_VALUE_SHARED: execute_ASSIGN_VALUE_SHARED(instr, nextInstruction); break;
                case VMOperator::SHARED_LOCK: execute_SHARED_LOCK(instr, nextInstruction); break;
                case VMOperator::SHARED_UNLOCK: execute_SHARED_UNLOCK(instr, nextInstruction); break;
                case VMOperator::LOAD_CELL_REFERENCE: execute_LOAD_CELL_REFERENCE(instr, nextInstruction); break;
                case VMOperator::LOAD_VALUE: execute_LOAD_VALUE(instr, nextInstruction); break;
                case VMOperator::JUMP: execute_JUMP(instr, nextInstruction); break;
                case VMOperator::JUMP_IF_TRUE: execute_JUMP_IF_TRUE(instr, nextInstruction); break;
                case VMOperator::JUMP_IF_FALSE: execute_JUMP_IF_FALSE(instr, nextInstruction); break;
                case VMOperator::JUMP_IF_PARAM_PROVIDED: execute_JUMP_IF_PARAM_PROVIDED(instr, nextInstruction); break;
                case VMOperator::EQUL: execute_EQUL(instr, nextInstruction); break;
                case VMOperator::NOT_EQUAL: execute_NOT_EQUAL(instr, nextInstruction); break;
                case VMOperator::GREATER_THAN: execute_GREATER_THAN(instr, nextInstruction); break;
                case VMOperator::LESS_THAN: execute_LESS_THAN(instr, nextInstruction); break;
                case VMOperator::GREATER_EQUAL: execute_GREATER_EQUAL(instr, nextInstruction); break;
                case VMOperator::LESS_EQUAL: execute_LESS_EQUAL(instr, nextInstruction); break;
                case VMOperator::LOGICAL_AND: execute_LOGICAL_AND(instr, nextInstruction); break;
                case VMOperator::LOGICAL_OR: execute_LOGICAL_OR(instr, nextInstruction); break;
                case VMOperator::LOGICAL_NOT: execute_LOGICAL_NOT(instr, nextInstruction); break;
                case VMOperator::IMPORT: execute_IMPORT(instr, nextInstruction); break;
                case VMOperator::DELETE_VAR: execute_DELETE_VAR(instr, nextInstruction); break;
                case VMOperator::TRY_BEGIN: execute_TRY_BEGIN(instr, nextInstruction); break;
                case VMOperator::TRY_END: execute_TRY_END(instr, nextInstruction); break;
                case VMOperator::CATCH_BEGIN: execute_CATCH_BEGIN(instr, nextInstruction); break;
                case VMOperator::CATCH_END: execute_CATCH_END(instr, nextInstruction); break;
                case VMOperator::THROW: execute_THROW(instr, nextInstruction); break;
                case VMOperator::WITH_BEGIN: execute_WITH_BEGIN(instr, nextInstruction); break;
                case VMOperator::WITH_END: execute_WITH_END(instr, nextInstruction); break;
                case VMOperator::HOTSPOT: execute_HOTSPOT(instr, nextInstruction); break;
            }
            if (!nextInstruction || vmstat != ZeusType::VMStatus::Running) return;
            currentInstruction = std::move(nextInstruction);
            instruction = &*currentInstruction;
        }
    }
    
    // 将 IR 指令块作为 chunk 运行（支持栈帧，避免C++递归）
    void runChunk(const std::vector<IRType::IRInstr>& bytecode) {
        // 创建初始栈帧
        CallFrame initialFrame;
        initialFrame.code = &bytecode;
        initialFrame.pc = 0;
        initialFrame.stackBase = stack.size();
        initialFrame.savedLine = __line;
        callFrames.push_back(initialFrame);

        FrameLoopOptions frameOptions;
        frameOptions.stopFrameDepth = 0;
        frameOptions.handleExceptions = true;
        frameOptions.checkExternalStop = true;
        frameOptions.updateSourceFile = true;
        frameOptions.validateJumps = true;
        frameOptions.traceExecution = true;
        frameOptions.traceName = "[VM]";
        runFrameLoop(frameOptions);

        // Cleanup: if we exit early (vmstat != Running) or otherwise leave frames, release any autoRelease closures
        while (!callFrames.empty()) {
            CallFrame& f = callFrames.back();
            if (f.pendingObjectCloneId != 0) {
                pendingObjectClones.erase(f.pendingObjectCloneId);
            }
            if (f.calleeAutoReleaseId >= 0) {
                releaseClosure(f.calleeAutoReleaseId);
            }
            if (f.constructorResult) {
                releaseObject(*f.constructorResult);
                f.constructorResult.reset();
            }
            callFrames.pop_back();
        }
        
        // 清理栈帧
        callFrames.clear();
        pendingObjectClones.clear();
    }
};

// ===== 线程执行函数实现 =====

namespace ZeusThread {

    // 线程入口函数：在新线程中创建 VM 并执行指定的函数
    inline void threadEntry(std::shared_ptr<ThreadInfo> info) {
        ZEUS_DEBUG_PRINT("[Thread] threadEntry: id=%d, protoId=%d", info->id, info->protoId);
        std::optional<VM> vmStorage;
        int preparedTargetClosureId = -1;

        try {
            info->state = ThreadState::Running;
            info->nativeId = std::this_thread::get_id();

            // 创建新的 VM 实例
            vmStorage.emplace();
            VM& vm = *vmStorage;
            // Configure a coherent shared domain before child execution while
            // the pending-peer lease protects the parent's published roots.
            std::unique_lock<std::recursive_mutex> rootSetup(ZeusGC::graphMutex());
            
            // 设置外部停止标志（用于 eliminate）
            vm.externalStopFlag = &info->shouldStop;

            // 注册函数原型（使用指针引用，不复制）
            if (info->sharedPrototypes) {
                // 使用共享的原型指针（零拷贝）
                vm.sharedPrototypes = static_cast<std::vector<VM::Prototype>*>(info->sharedPrototypes);
                ZEUS_DEBUG_PRINT("[Thread] Using shared prototypes (pointer): %zu", vm.sharedPrototypes->size());
            } else if (info->program) {
                std::string registrationError;
                if (!vm.registerPrototypes(*info->program, &registrationError)) {
                    throw std::runtime_error(registrationError);
                }
            }

            // 设置全局名称
            // 需要复制到 vm.globalNames，因为很多代码直接使用 globalNames
            if (info->sharedGlobalNames && !info->sharedGlobalNames->empty()) {
                vm.sharedGlobalNames = info->sharedGlobalNames;
                vm.globalNames = *info->sharedGlobalNames;  // 也复制一份到 globalNames
                ZEUS_DEBUG_PRINT("[Thread] Using shared globalNames (pointer): %zu, copied to local", vm.sharedGlobalNames->size());
            } else if (!info->globalNames.empty()) {
                vm.setGlobalNames(info->globalNames);
            }
            
            // 设置共享的类和对象系统
            if (info->sharedClasses && info->sharedObjects) {
                vm.sharedClasses = info->sharedClasses;
                vm.sharedObjects = info->sharedObjects;
                vm.sharedObjectsMutex = info->sharedObjectsMutex;
                vm.sharedFreeObjectSlots = info->sharedFreeObjectSlots;
                vm.syncVMRootDomain();
                if (info->sharedTypeImplementations) {
                    vm.typeImplementations = *static_cast<
                        std::unordered_map<std::string, ZeusType::ClassInfo>*>(
                            info->sharedTypeImplementations);
                }
                ZEUS_DEBUG_PRINT("[Thread] Using shared object system: classes=%zu, objects=%zu",
                    info->sharedClasses->size(), info->sharedObjects->size());
            }
            
            // 设置共享的全局变量（关键：使子线程能访问主线程的全局变量）
            if (info->sharedGlobal) {
                // 复制全局变量引用（Cell 指针）- 这是必要的，因为子线程可能添加新的全局变量
                vm.global = *static_cast<std::vector<Cell*>*>(info->sharedGlobal);
                // 记录继承的边界，析构时跳过这些槽位
                vm.inheritedGlobalSize = vm.global.size();
                ZEUS_DEBUG_PRINT("[Thread] Using shared global: %zu slots (inherited)", vm.global.size());
            }
            
            // 设置共享的闭包系统（使用指针引用，不复制）
            if (info->sharedClosures) {
                vm.sharedClosures = static_cast<std::vector<VM::Closure>*>(info->sharedClosures);
                vm.sharedFreeClosureSlots = info->sharedFreeClosureSlots;
                vm.sharedClosuresMutex = info->sharedClosuresMutex;
                
                // 关键：让本地 closures 预留共享闭包的槽位
                // 这样新分配的闭包 ID 不会与共享闭包冲突
                vm.inheritedClosuresSize = vm.sharedClosures->size();
                vm.closures.resize(vm.inheritedClosuresSize);
                
                ZEUS_DEBUG_PRINT("[Thread] Using shared closures (pointer): %zu, local reserved: %zu", 
                    vm.sharedClosures->size(), vm.closures.size());
            }
            
            // 设置共享的名称映射（使用指针引用，不复制）
            if (info->sharedNameToClosure) {
                vm.sharedNameToClosure = static_cast<std::unordered_map<std::string,int>*>(info->sharedNameToClosure);
            }
            
            if (info->sharedNameToClass) {
                vm.sharedNameToClass = static_cast<std::unordered_map<std::string,int>*>(info->sharedNameToClass);
            }

            // ThreadInfo may own arguments before they reach the operand
            // stack. Publish those roots before completing the hand-off.
            for (const auto& argument : info->args) vm.pinVMTemporaryRoot(argument);
            if (info->boundThis) vm.pinVMObjectRoot(*info->boundThis);

            if (info->targetClosureId >= 0) {
                std::vector<Cell*> captureCells;
                captureCells.reserve(info->targetUpvalues.size());
                for (void* capture : info->targetUpvalues) {
                    auto* cell = static_cast<Cell*>(capture);
                    if (!cell) {
                        throw std::runtime_error(
                            "thread target closure contains an uninitialized capture cell");
                    }
                    captureCells.push_back(cell);
                }

                VM::Closure targetClosure;
                targetClosure.protoId = info->protoId;
                targetClosure.boundThis = info->boundThis;
                targetClosure.upvalues = captureCells;

                bool retainedCaptures = false;
                bool retainedBoundThis = false;
                try {
                    {
                        std::lock_guard<std::recursive_mutex> graph(
                            ZeusGC::graphMutex());
                        for (Cell* capture : captureCells) capture->retain();
                        retainedCaptures = true;
                        if (targetClosure.boundThis) {
                            vm.retainObject(*targetClosure.boundThis);
                            retainedBoundThis = true;
                        }
                    }
                    preparedTargetClosureId = vm.allocClosure(std::move(targetClosure));
                    retainedCaptures = false;
                    retainedBoundThis = false;
                } catch (...) {
                    if (retainedCaptures) {
                        for (Cell* capture : captureCells) {
                            vm.releaseCellAndContent(capture);
                        }
                    }
                    if (retainedBoundThis && info->boundThis) {
                        vm.releaseObject(*info->boundThis);
                    }
                    throw;
                }

                // The child now owns its local closure and capture references.
                // It no longer reads from the parent's resizable closure table.
                info->releaseRetainedTargetClosure();
            }

            info->rootPeerLease.reset();
            rootSetup.unlock();

            // 如果 VM 仍在运行，执行目标函数
            const VM::Prototype* protoPtr = vm.getPrototype(info->protoId);
            if (vm.vmstat == ZeusType::VMStatus::Running && protoPtr) {

                ZEUS_DEBUG_PRINT("[Thread] Executing protoId %d, hasBoundThis=%d", info->protoId, info->boundThis.has_value());

                // 直接使用 prototypes[protoId].body 执行（不需要 chunkToProtoId）
                const auto& proto = *protoPtr;
                
                if (!proto.body.empty()) {
                    // 构造闭包并执行
                     int closureId = preparedTargetClosureId;
                     if (closureId < 0) {
                         VM::Closure clos;
                         clos.protoId = info->protoId;
                         // CALL_CLOSURE inserts the bound receiver when present.
                         if (info->boundThis.has_value()) {
                             vm.retainObject(*info->boundThis);
                         }
                         clos.boundThis = info->boundThis;
                         closureId = vm.allocClosure(std::move(clos));
                     }
                    
                    ZEUS_DEBUG_PRINT("[Thread] Created closure #%d for protoId %d", closureId, info->protoId);
                    
                    // 压入闭包句柄
                    vm.stack.push_back(ZeusType::ClosureHandle{closureId});
                    
                    // 注意：不需要手动压入 boundThis，CALL_CLOSURE 会根据 clos.boundThis 自动插入
                    
                    // 压入其他参数（不包括 this）
                    for (const auto& arg : info->args) {
                        vm.stack.push_back(arg);
                    }
                    
                    // 参数计数只包括 info->args，不包括 boundThis
                    // （CALL_CLOSURE 会自动检测 clos.boundThis 并增加参数）
                    int totalArgCount = (int)info->args.size();
                    
                    // 构造 CALL_CLOSURE 调用
                    std::vector<IRType::IRInstr> callSequence;
                    IRType::IRInstr callInstr;
                    callInstr.op = VMOperator::CALL_CLOSURE;
                    callInstr.args.push_back(LexerType::Value(totalArgCount));
                    callSequence.push_back(callInstr);
                    
                    ZEUS_DEBUG_PRINT("[Thread] Calling with argc=%d (boundThis handled by CALL_CLOSURE)", totalArgCount);
                    
                    vm.runChunk(callSequence);
                    ZEUS_DEBUG_PRINT("[Thread] runChunk completed");

                    // 线程内临时闭包只用于执行一次，执行后释放
                    vm.releaseClosure(closureId);
                    preparedTargetClosureId = -1;
                } else {
                    ZEUS_DEBUG_PRINT("[Thread] Empty prototype body!");
                    if (preparedTargetClosureId >= 0) {
                        vm.releaseClosure(preparedTargetClosureId);
                        preparedTargetClosureId = -1;
                    }
                }
            } else {
                ZEUS_DEBUG_PRINT("[Thread] Skipped execution: vmstat=%d protoId=%d protoPtr=%p",
                    (int)vm.vmstat, info->protoId, (void*)protoPtr);
            }

            if (preparedTargetClosureId >= 0) {
                vm.releaseClosure(preparedTargetClosureId);
                preparedTargetClosureId = -1;
            }

            // 线程结束时释放 info->args / boundThis 中的对象或闭包引用，避免线程记录长期持有
            for (auto& arg : info->args) {
                ZeusGC::releaseVariantIfObject(vm, arg);
                if (std::holds_alternative<ZeusType::ClosureHandle>(arg)) {
                    vm.releaseClosure(std::get<ZeusType::ClosureHandle>(arg).id);
                }
            }
            info->args.clear();
            if (info->boundThis.has_value()) {
                vm.releaseObject(*info->boundThis);
                info->boundThis.reset();
            }

            info->state = ThreadState::Finished;
            ZEUS_DEBUG_PRINT("[Thread] threadEntry finished: id=%d", info->id);

        } catch (const std::exception& e) {
            if (preparedTargetClosureId >= 0 && vmStorage) {
                try {
                    vmStorage->releaseClosure(preparedTargetClosureId);
                } catch (...) {
                    ZEUS_DEBUG_PRINT(
                        "[Thread] failed to release prepared closure #%d after error",
                        preparedTargetClosureId);
                }
            }
            info->releaseRetainedTargetClosure();
            info->rootPeerLease.reset();
            info->state = ThreadState::Error;
            info->errorMessage = e.what();
            ZEUS_DEBUG_PRINT("[Thread] threadEntry error: id=%d, msg=%s", info->id, e.what());
        } catch (...) {
            if (preparedTargetClosureId >= 0 && vmStorage) {
                try {
                    vmStorage->releaseClosure(preparedTargetClosureId);
                } catch (...) {
                    ZEUS_DEBUG_PRINT(
                        "[Thread] failed to release prepared closure #%d after error",
                        preparedTargetClosureId);
                }
            }
            info->releaseRetainedTargetClosure();
            info->rootPeerLease.reset();
            info->state = ThreadState::Error;
            info->errorMessage = "thread entry threw a non-standard exception";
            ZEUS_DEBUG_PRINT(
                "[Thread] threadEntry error: id=%d, non-standard exception", info->id);
        }
    }

    // 自动注册线程入口函数
    namespace {
        struct ThreadEntryRegistrar {
            ThreadEntryRegistrar() {
                setGlobalThreadEntry(threadEntry);
            }
        };
        static ThreadEntryRegistrar _threadEntryRegistrar;
    }

}  // namespace ZeusThread



