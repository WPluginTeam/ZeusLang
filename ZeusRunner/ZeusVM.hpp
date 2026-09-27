#pragma once

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
#include "ZeusVMOperator.hpp"
#include "../ZeusRuntimeLib/ZeusVMException.hpp"
#include "../ZeusRuntimeLib/ZeusSharedHeap.hpp"
#include "../ZeusIR/IRType.hpp"
#include "../ZeusRuntimeLib/ZeusCell.hpp"
#include "ZeusBuiltins.hpp"
#include "../ZeusRuntimeLib/ZeusVMUtils.hpp"
#include "ZeusHotspot.hpp"
#include "../ZeusRuntimeLib/ZeusGC.hpp"
#if 0
/* Computed-goto dispatch removed for MSVC builds. */
#endif
#ifdef ZEUS_DEBUG_FILE_CHANNEL
#undef ZEUS_DEBUG_FILE_CHANNEL
#endif
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_VM
#include "../ZeusDebug/Utils.hpp"

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
            data.erase(data.begin() + idx);
            owner.erase(owner.begin() + idx);
        }
        void pop_back() {
            data.pop_back();
            owner.pop_back();
        }
        uint8_t ownerFlag(size_t idx) const { return owner[idx]; }
        void setOwnerFlag(size_t idx, uint8_t f) { owner[idx] = f; }
        void set(size_t idx, const ZeusType::UniversalVariant& v) {
            data[idx] = v;
            owner[idx] = calcOwner(v);
        }
        void set(size_t idx, ZeusType::UniversalVariant&& v) {
            owner[idx] = calcOwner(v);
            data[idx] = std::move(v);
        }
    } stack;

    // 变量槽：global/local/upvalue 存储对应 Cell* 引用
    std::vector<Cell*> global;   // 全局变量槽
    std::vector<Cell*> local;    // 局部变量槽
    std::vector<Cell*> upvalue;  // 上值（闭包捕获）槽
    
    // 子线程继承的资源边界（析构时跳过这些资源的释放）
    size_t inheritedGlobalSize = 0;       // 继承的 global 槽数
    size_t inheritedClosuresSize = 0;     // 继承的 closures 数

    // 调用深度计数器：通过栈帧数量跟踪（不再受C++栈限制）
    static constexpr int MAX_CALL_DEPTH = 100000;  // 最大调用深度（栈帧模式支持深度递归）
    
    // 标记：execute()是否创建了新栈帧（CALL_CLOSURE会设置此标志）
    bool justPushedFrame = false;

    // 析构函数：清理所有分配的内存
    ~VM() {
        ZEUS_DEBUG_PRINT("[~VM] Destructor: inheritedGlobalSize=%zu, global=%zu, local=%zu, upvalue=%zu",
            inheritedGlobalSize, global.size(), local.size(), upvalue.size());

        // 重要：释放栈上仍然存活的引用（尤其是子线程 VM 退出时）
        // 否则线程执行结束但栈残留的 ObjectHandle/Variant/NamedArg 会永久多 retain 一次，导致对象/闭包泄露。
        shrinkStackWithRelease(0);
        stack.clear();
         
        // 释放局部变量（通常在函数返回时已清理）
        // 使用 releaseCellAndContent 确保 Cell 内的 ClosureHandle 也被释放
        for (auto* cell : local) {
            if (cell) releaseCellAndContent(cell);
        }
        local.clear();
        
        // 释放全局变量（跳过继承的槽位，只释放本 VM 新增的）
        for (size_t i = inheritedGlobalSize; i < global.size(); ++i) {
            if (global[i]) releaseCellAndContent(global[i]);
        }
        global.clear();
        
        // 释放上值
        for (auto* cell : upvalue) {
            if (cell) releaseCellAndContent(cell);
        }
        upvalue.clear();
        
        // 释放闭包中的 upvalues（跳过继承的闭包）
        for (size_t i = inheritedClosuresSize; i < closures.size(); ++i) {
            for (auto* cell : closures[i].upvalues) {
                if (cell) releaseCellAndContent(cell);
            }
            closures[i].upvalues.clear();
        }
        closures.clear();
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
        c->createdAtMs = __getCurrentTimeMs();
        c->createdBy = createdBy;
        HeapStats::recordCreate();  // 修复：cloneCell 也需要记录创建
        HeapStats::registerCell(c);
        
        // 追踪ObjectHandle Cell的克隆（调试通道）
        if (std::holds_alternative<ZeusType::ObjectHandle>(src->value)) {
            auto& oh = std::get<ZeusType::ObjectHandle>(src->value);
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
        ctx.desc = ZeusBuiltins::get(ctx.nativeId);

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
        std::vector<ZeusType::UniversalVariant> args;
        args.reserve(ctx.argc);

        for (int i = 0; i < ctx.argc; ++i) {
            args.push_back(stack[ctx.calleeIndex + 1 + i]);
        }

        ZeusType::UniversalVariant ret = LexerType::Value(0);

        try {
            // 特殊处理需要访问VM内部状态的内置函数
            if (ctx.desc->name == "__getglobal") {
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
                ret = global[index]->value;
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
                global[index]->value = args[1];
                ret = LexerType::Value(0); // return null
            }
            else if (ctx.desc->name == "__get_module") {
                // __get_module(modulePath) -> ModuleProxy
                // 创建一个模块代理对象，用于引用映射而非值复制
                // 例如: __get_module("ZeusBuiltins.cppfunctions")
                // 返回 ModuleProxy{"ZeusBuiltins.cppfunctions"}
                // 访问 proxy.io 时会自动映射到 "ZeusBuiltins.cppfunctions.io" 全局变量
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
                // 通过符号名从globalNames查找索引,然后返回global[index]->value
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
                
                ret = global[index]->value;
                
                // Debug: 输出 __loadglobal 返回的值类型
                ZEUS_DEBUG_PRINT("[__loadglobal] name='%s' index=%d -> %s", 
                    name.c_str(), index, ZeusVMUtils::valueToDebugString(ret).c_str());
            }
            else if (ctx.desc->name == "__dict_clone") {
                ret = ZeusBuiltins::builtin_dict_clone(args);
                ZeusGC::retainVariantDeep(*this, ret);
            }
            else if (ctx.desc->name == "__dict_delete") {
                if (args.size() != 2) {
                    throw std::runtime_error("__dict_delete expects 2 arguments");
                }
                const std::string key = ZeusBuiltins::getDictKeyString(args[1]);
                if (!ZeusBuiltins::getMutableDictData(args[0])) {
                    throw std::runtime_error("__dict_delete: first argument must be a dictionary");
                }
                if (!ZeusBuiltins::takeDictValue(args[0], key, ret)) {
                    ret = ZeusType::Variant(ZeusType::Null());
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
                    int objId = std::get<ZeusType::ObjectHandle>(args[0]).id;
                    auto* objPtr = getObject(objId);
                    if (!objPtr) {
                        throw std::runtime_error("hasKey: invalid object id");
                    }
                    const auto& obj = *objPtr;
                    exists = obj.values.find(keyStr) != obj.values.end();
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
                // 通过符号名从globalNames查找索引,然后设置global[index]->value
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
                global[index]->value = args[1];
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
                
                int objId = std::get<ZeusType::ObjectHandle>(objVar).id;
                auto* objPtr = getObject(objId);
                if (!objPtr) {
                    throw std::runtime_error("getattr: invalid object id");
                }
                
                auto& obj = *objPtr;
                auto it = obj.values.find(attrName);
                if (it != obj.values.end()) {
                    ret = it->second;
                } else {
                    throw std::runtime_error("getattr: attribute '" + attrName + "' not found");
                }
            }
            else if (ctx.desc->name == "__objget") {
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
                
                int objId = std::get<ZeusType::ObjectHandle>(objVar).id;
                auto* objPtr = getObject(objId);
                if (!objPtr) {
                    throw std::runtime_error("__objget: invalid object id");
                }
                
                auto& obj = *objPtr;
                auto fieldIt = obj.values.find(fieldName);
                if (fieldIt == obj.values.end()) {
                    throw std::runtime_error("__objget: field '" + fieldName + "' not found");
                }
                
                // field应该是Variant(Object)类型
                if (!std::holds_alternative<ZeusType::Variant>(fieldIt->second)) {
                    throw std::runtime_error("__objget: field '" + fieldName + "' is not a dictionary");
                }
                
                const auto& variant = std::get<ZeusType::Variant>(fieldIt->second);
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
                
                int objId = std::get<ZeusType::ObjectHandle>(objVar).id;
                auto* objPtr = getObject(objId);
                if (!objPtr) {
                    throw std::runtime_error("__objset: invalid object id");
                }
                
                auto& obj = *objPtr;
                auto fieldIt = obj.values.find(fieldName);
                if (fieldIt == obj.values.end()) {
                    throw std::runtime_error("__objset: field '" + fieldName + "' not found");
                }
                
                // field应该是Variant(Object)类型
                if (!std::holds_alternative<ZeusType::Variant>(fieldIt->second)) {
                    throw std::runtime_error("__objset: field '" + fieldName + "' is not a dictionary");
                }
                
                auto& variant = std::get<ZeusType::Variant>(fieldIt->second);
                if (!std::holds_alternative<ZeusType::Object>(variant)) {
                    throw std::runtime_error("__objset: field '" + fieldName + "' is not a dictionary object");
                }
                
                auto& dictObj = std::get<ZeusType::Object>(variant);
                if (!dictObj.impl) {
                    throw std::runtime_error("__objset: dictionary has null impl");
                }
                
                // 设置字典中的key
                dictObj.impl->data[keyName] = value;
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
                
                int objId = std::get<ZeusType::ObjectHandle>(objVar).id;
                auto* objPtr = getObject(objId);
                if (!objPtr) {
                    throw std::runtime_error("setattr: invalid object id");
                }
                
                auto& obj = *objPtr;
                
                // 直接设置值(obj.values 是 UniversalVariant 的 map,不是 Cell*)
                obj.values[attrName] = value;
                
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
                ret = ctx.desc->fn(args);
            }
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
        int paramCount = 0;
        std::vector<std::string> paramNames; // 参数名列表(用于命名参数匹配)
        bool hasVariadicArgs = false;   // 是否有*args
        bool hasVariadicKwargs = false; // 是否有**kwargs
        std::string variadicArgsName;   // *args的参数名
        std::string variadicKwargsName; // **kwargs的参数名
        std::vector<ZeusType::UniversalVariant> paramDefaults; // 参数默认值(空表示无默认值)
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


    // 调用栈帧：用于非递归函数调用
    struct CallFrame {
        const std::vector<IRType::IRInstr>* code;                     // 执行的代码块
        size_t                              pc;                       // 程序计数器
        std::vector<Cell*>                  savedLocal;               // 保存的局部变量
        std::vector<Cell*>                  savedUpvalue;             // 保存的upvalue
        size_t                              stackBase;                // 栈基址（用于返回时恢复）
        int                                 savedLine;                // 保存的行号
        int                                 calleeAutoReleaseId = -1; // 如果为函数调用传递的临时闭包, 返回时自动 release id
    };

    std::vector<Prototype> prototypes; // 为外部函数/闭包注册的原型列表
    size_t                 prototypesCount = 0;        // 原型数量（调试用）
    std::vector<int>       chunkToProtoId;   // chunkIndex -> protoId（若无则为 -1）
    std::vector<Closure>   closures;     // 已创建的闭包集合
    size_t                 closuresCount = 0;          // 闭包计数器（用于统计/调试）
    size_t                 freeClosureSlotsCount = 0;  // 空闲闭包槽计数（用于统计/调试）
    std::vector<CallFrame> callFrames; // 调用栈（支持深度递归）
    
    // ===== 异常处理栈 =====
    struct ExceptionHandler {
        size_t catchPc;                    // catch 块的起始 PC
        size_t callFrameDepth;             // 注册时的调用栈深度
        const std::vector<IRType::IRInstr>* code;  // 当前代码块
        size_t stackBase;                  // 注册时的栈基址（用于恢复）
        size_t localSize;                  // 注册时的局部变量数（用于恢复）
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
    std::deque<ZeusType::ObjectInstance> objects;  // 使用 deque 避免重分配时指针失效
    mutable std::shared_mutex objectsMutex;  // 读写锁，用于线程安全的对象访问
    std::vector<ZeusType::NamespaceData> namespaces;  // 包命名空间集合
    std::vector<ZeusType::SocketData> sockets;        // Socket 对象集合
    std::vector<int> freeSocketSlots;   // 已释放的 socket 槽位
    size_t freeSocketSlotsCount = 0;     // 空闲 socket 槽位计数（调试用）
    mutable std::mutex socketsMutex;    // Socket 操作互斥锁
    
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
        
        // 检查 generation 是否匹配（slot 复用后 generation 会递增）
        if (obj->generation != handle.generation) {
            ZEUS_DEBUG_PRINT("[GC] Stale object reference: handle.id=%d handle.gen=%d obj.gen=%d", 
                             handle.id, handle.generation, obj->generation);
            return nullptr;
        }
        
        // 检查对象是否已标记删除
        if (obj->markedForDeletion || obj->classId < 0) {
            ZEUS_DEBUG_PRINT("[GC] Accessing deleted object: id=%d classId=%d", handle.id, obj->classId);
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
                    const auto field = object->values.find("value");
                    if (field != object->values.end() &&
                        primitiveString(field->second, result)) {
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
        obj.refCount = 1;
        obj.markedForDeletion = false;
        
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
                return ZeusType::ObjectHandle{slot, newGen};
            }
            
            int objId = (int)sharedObjects->size();
            obj.generation = 0;
            sharedObjects->push_back(std::move(obj));
            ZEUS_DEBUG_PRINT("[GC] Allocated shared object #%d", objId);
            return ZeusType::ObjectHandle{objId, 0};
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
            return ZeusType::ObjectHandle{slot, newGen};
        }
        
        // 没有空闲槽位，分配新槽位
        int objId = (int)objects.size();
        obj.generation = 0;
        objects.push_back(std::move(obj));
        return ZeusType::ObjectHandle{objId, 0};
    }
    
    // 释放 Cell 及其内容（如果内容是 ClosureHandle 或 ObjectHandle，也会释放）
    // 用于函数返回时清理 local 变量
    void releaseCellAndContent(Cell* cell) {
        if (!cell) return;

        // 仅当 Cell 即将被销毁（refCount==1）时，才释放其内容的引用。
        // 否则该 Cell 仍被其他持有者共享（例如多个 closure upvalue 槽），提前释放内容会造成悬垂引用。
        if (cell->refCount == 1) {
            ZeusGC::releaseVariantDeep(*this, cell->value);
        }

        // 然后释放 Cell 本身（refCount--，到0时 delete）
        cell->release();
    }

    // 增加对象引用计数
    void retainObject(int objId) {
        auto* obj = getMutableObject(objId);
        
        if (obj && !obj->markedForDeletion) {
            obj->retain();
            ZEUS_DEBUG_PRINT("[GC] Object #%d retain -> refCount=%d", objId, obj->refCount);
            if (obj->classId == 20 || obj->classId == 17 || obj->classId == 18) {
                ZEUS_DEBUG_PRINT("[GC][TRACE] classId=%d retain -> refCount=%d", obj->classId, obj->refCount);
            }
        }
    }

    // 释放对象中嵌套持有的其他对象和资源
    void releaseNestedObjects(ZeusType::ObjectInstance& obj) {
        for (auto& kv : obj.values) {
            ZeusGC::releaseVariantDeep(*this, kv.second);
            // 注意: SocketHandle 的实际资源清理应在 operator[~] 中进行
            // 这里只记录日志，不做强制清理（避免关闭未关闭的连接造成问题）
        }
    }
    
    // 强制打断对象的闭包循环引用（递归处理嵌套对象）
    // 在 DELETE_VAR 删除对象时调用，用于打断 using &this 产生的循环引用：
    // Object -> ClosureHandle -> Closure.upvalues -> Cell -> ObjectHandle -> Object
    // 也处理嵌套对象中的闭包（如 this.thread.target）
    //
    // 重要：这里不能在遍历 obj->values 过程中直接 releaseObject / releaseCellAndContent，
    // 因为这可能导致当前正在遍历的对象被提前清空（迭代器失效 -> 崩溃）。
    // 因此采用“先断边、后延迟释放”的策略：在递归遍历期间只清除 closure 的引用关系，
    // 把需要释放的 Cell/对象收集起来，待遍历完成后再统一释放。
    struct PendingCycleBreakRelease {
        std::unordered_set<int> processedClosures;
        std::vector<Cell*> upvalueCellsToRelease; // one entry per upvalue slot (respect retain multiplicity)
        std::vector<int> boundThisObjToRelease;
    };

    void forceBreakClosureCycles(int objId, std::set<int>& visited, PendingCycleBreakRelease& pending) {
        if (visited.count(objId)) return;
        visited.insert(objId);

        auto* obj = getMutableObject(objId);
        if (!obj || obj->markedForDeletion) return;

        ZEUS_DEBUG_PRINT("[GC] forceBreakClosureCycles: breaking cycles for object #%d", objId);

        for (auto& kv : obj->values) {
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
                    pending.boundThisObjToRelease.push_back(clo->boundThis->id);
                    clo->boundThis.reset();
                }
            } else if (std::holds_alternative<ZeusType::ObjectHandle>(kv.second)) {
                int nestedObjId = std::get<ZeusType::ObjectHandle>(kv.second).id;
                forceBreakClosureCycles(nestedObjId, visited, pending);
            }
        }
    }
    
    // 便捷重载：从单个对象开始打断循环
    void forceBreakClosureCycles(int objId) {
        std::set<int> visited;
        PendingCycleBreakRelease pending;
        forceBreakClosureCycles(objId, visited, pending);

        // Now it is safe to release, after all traversals are done.
        for (Cell* cell : pending.upvalueCellsToRelease) {
            if (cell) releaseCellAndContent(cell);
        }
        for (int oid : pending.boundThisObjToRelease) {
            releaseObject(oid);
        }
    }
    
    // 减少对象引用计数，必要时释放
    void releaseObject(int objId) {
        auto* obj = getMutableObject(objId);
        
        if (!obj || obj->markedForDeletion) return;

        // 永久保留 Namespace 对象，避免全局命名空间被误释放
        if (obj->classId >= 0 && obj->classId < (int)classes.size()) {
            const auto& ci = classes[obj->classId];
            if (ci.name == "Namespace") {
                ZEUS_DEBUG_PRINT("[GC] Skip release for Namespace object #%d (pinned)", objId);
                return;
            }
        }
        
        if (obj->release()) {
            ZEUS_DEBUG_PRINT("[GC] Object #%d released (refCount=0), freeing slot", objId);
            if (obj->classId == 20 || obj->classId == 17 || obj->classId == 18) {
                ZEUS_DEBUG_PRINT("[GC][TRACE] classId=%d release to zero", obj->classId);
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
    int allocClosure(Closure&& clo) {
        std::unique_lock<std::shared_mutex> lock(closuresMutex);
        clo.refCount = 1;
        
        if (!freeClosureSlots.empty()) {
            int slot = freeClosureSlots.back();
            freeClosureSlots.pop_back();
            if (freeClosureSlotsCount > 0) freeClosureSlotsCount--;
            closures[slot] = std::move(clo);
            ZEUS_DEBUG_PRINT("[GC] Reused closure slot #%d", slot);
            return slot;
        }

        int cloId = (int)closures.size();
        closures.push_back(std::move(clo));
        closuresCount++;
        ZEUS_DEBUG_PRINT("[GC] allocClosure: new closure id=%d protoId=%d", cloId, closures[cloId].protoId);
        return cloId;
    }    // 调试: 输出当前闭包列表的详情（用于内置的 __closure_details 调用）
    std::string dumpClosureDetails() const {
        std::ostringstream oss;
        oss << "CLOSURE DETAILS: total=" << closures.size() << ", count=" << closuresCount << "\n";
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
                if (obj && obj->classId >= 0 && obj->classId < (int)classes.size()) {
                    cname = classes[obj->classId].name;
                }
                oss << " boundThis=" << oid << ":" << cname;
            }
            oss << "\n";
        }
        return oss.str();
    }

    // ========== Mark/Sweep GC (补充) ==========
    // 现有引用计数在一些路径下会留下“无根 refCount=1”的对象（例如栈/Cell 所有权不对称时）。
    // 这里提供一个保守的 mark-sweep，用于在安全点（RETURN/DELETE_VAR）回收真正不可达的对象/闭包。
    void collectGarbageSweep() {
        // Mark vectors sized to current storage.
        std::vector<uint8_t> markedObj(objects.size(), 0);
        std::vector<uint8_t> markedClo(closures.size(), 0);
        std::unordered_set<const void*> markedNativeClo;
        std::vector<std::shared_ptr<void>> nativeClosureRoots;

        auto markObject = [&](int objId, auto&& markObjectRef, auto&& markVariantRef) -> void {
            if (objId < 0 || objId >= (int)objects.size()) return;
            auto& obj = objects[(size_t)objId];
            if (obj.classId < 0 || obj.markedForDeletion) return;
            if (markedObj[(size_t)objId]) return;
            markedObj[(size_t)objId] = 1;
            for (const auto& kv : obj.values) {
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
                markVariantRef(cell->value, markVariantRef, markObjectRef);
            }
            // mark boundThis
            if (clo->boundThis.has_value()) {
                markObjectRef(clo->boundThis->id, markObjectRef, markVariantRef);
            }
        };

        auto markVariant = [&](const ZeusType::UniversalVariant& v, auto&& markVariantRef, auto&& markObjectRef) -> void {
            if (std::holds_alternative<ZeusType::ObjectHandle>(v)) {
                int oid = std::get<ZeusType::ObjectHandle>(v).id;
                markObjectRef(oid, markObjectRef, markVariantRef);
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
                    for (const auto& it : inner.impl->data) {
                        markVariantRef(it, markVariantRef, markObjectRef);
                    }
                } else if constexpr (std::is_same_v<T, ZeusType::Object>) {
                    if (!inner.impl) return;
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

        // Roots: global/local/upvalue Cells
        for (auto* cell : global) {
            if (cell) markVariant(cell->value, markVariant, markObject);
        }
        for (auto* cell : local) {
            if (cell) markVariant(cell->value, markVariant, markObject);
        }
        for (auto* cell : upvalue) {
            if (cell) markVariant(cell->value, markVariant, markObject);
        }

        ZeusGC::collect(nativeClosureRoots);

        // Sweep closures (skip shared/inherited slots)
        for (int cid = (int)inheritedClosuresSize; cid < (int)closures.size(); ++cid) {
            Closure* clo = getClosure(cid);
            if (!clo || clo->isFreed() || clo->protoId < 0) continue;
            if (markedClo[(size_t)cid]) continue;

            // Free unreachable closure: release upvalues + boundThis then recycle slot.
            for (auto* cell : clo->upvalues) {
                if (cell) releaseCellAndContent(cell);
            }
            clo->upvalues.clear();
            if (clo->boundThis.has_value()) {
                releaseObject(clo->boundThis->id);
            }
            clo->boundThis.reset();
            clo->protoId = -1;
            clo->refCount = 0;
            freeClosureSlots.push_back(cid);
            freeClosureSlotsCount++;
        }

        // Sweep objects (skip pinned Namespace; treat shared objects via isSharedObject)
        for (int oid = 0; oid < (int)objects.size(); ++oid) {
            auto& obj = objects[(size_t)oid];
            if (obj.classId < 0 || obj.markedForDeletion) continue;
            if (markedObj[(size_t)oid]) continue;

            // 永久保留 Namespace 对象，避免全局命名空间被误释放
            if (obj.classId >= 0 && obj.classId < (int)classes.size()) {
                const auto& ci = classes[obj.classId];
                if (ci.name == "Namespace") continue;
            }

            // Unreachable object: clear and recycle slot.
            obj.clear();
            if (isSharedObject(oid)) {
                if (sharedFreeObjectSlots && sharedObjectsMutex) {
                    std::unique_lock<std::shared_mutex> lock(*sharedObjectsMutex);
                    sharedFreeObjectSlots->push_back(oid);
                }
            } else {
                freeObjectSlots.push_back(oid);
                freeObjectSlotsCount++;
            }
        }
    }
    
    // 增加闭包引用计数（线程安全）
    void retainClosure(int cloId) {
        std::unique_lock<std::shared_mutex> lock(closuresMutex);
        Closure* clo = getClosure(cloId);
        if (clo && !clo->isFreed()) {
            clo->retain();
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
        std::unique_lock<std::shared_mutex> lock(closuresMutex);
        Closure* clo = getClosure(cloId);
        if (!clo || clo->isFreed()) {
            ZEUS_DEBUG_PRINT("[GC] releaseClosure #%d skipped: clo=%p, isFreed=%d", cloId, (void*)clo, clo ? (int)clo->isFreed() : -1);
            return;
        }
        
        ZEUS_DEBUG_PRINT("[GC] releaseClosure #%d: refCount=%d before release", cloId, clo->refCount);
        
        bool isShared = isSharedClosure(cloId);

        if (clo->release()) {
            // 共享闭包（来自主线程/共享环境）不释放槽位，避免在子线程中释放导致 protoId 失效
            if (isShared) {
                clo->refCount = 1; // pin
                ZEUS_DEBUG_PRINT("[GC] Closure #%d is shared; skipping free and pinning (refCount=1)", cloId);
                return;
            }

             ZEUS_DEBUG_PRINT("[GC] Closure #%d released (refCount=0), freeing upvalues and slot", cloId);
             
             // 释放 upvalues
             for (auto* cell : clo->upvalues) {
                if (cell) releaseCellAndContent(cell);
             }
             clo->upvalues.clear();
             clo->protoId = -1;
            if (clo->boundThis.has_value()) {
                releaseObject(clo->boundThis->id);
            }
            clo->boundThis.reset();
            
            // 将槽位加入正确的空闲列表
            ZEUS_DEBUG_PRINT("[GC] Closure #%d isShared=%d, closures.size=%zu, sharedClosures=%p", 
                cloId, (int)isShared, closures.size(), (void*)sharedClosures);
            
            if (isShared) {
                // 共享闭包：加入共享的空闲列表（需要额外加锁）
                // 注意：这里已经持有 closuresMutex，需要避免死锁
                if (sharedFreeClosureSlots && sharedClosuresMutex && sharedClosuresMutex != &closuresMutex) {
                    std::unique_lock<std::shared_mutex> sharedLock(*sharedClosuresMutex);
                    sharedFreeClosureSlots->push_back(cloId);
                    ZEUS_DEBUG_PRINT("[GC] Added shared closure slot #%d to free list", cloId);
                } else if (sharedFreeClosureSlots) {
                    // 同一个 mutex，已经持有锁
                    sharedFreeClosureSlots->push_back(cloId);
                    ZEUS_DEBUG_PRINT("[GC] Added shared closure slot #%d to free list (same mutex)", cloId);
                } else {
                    ZEUS_DEBUG_PRINT("[GC] WARNING: Cannot add shared closure #%d to free list (ptrs null)", cloId);
                }
            } else if (cloId >= 0 && cloId < (int)closures.size()) {
                // 本地闭包
                freeClosureSlots.push_back(cloId);
                freeClosureSlotsCount++;
                ZEUS_DEBUG_PRINT("[GC] Added local closure slot #%d to free list (size=%zu)", cloId, freeClosureSlots.size());
            } else {
                ZEUS_DEBUG_PRINT("[GC] WARNING: Closure #%d not added to any free list", cloId);
            }
        } else {
            ZEUS_DEBUG_PRINT("[GC] Closure #%d release -> refCount=%d", cloId, clo->refCount);
        }
    }

    // 对象垃圾回收：标记删除（兼容旧代码）
    void markObjectForDeletion(int objId) {
        if (objId >= 0 && objId < (int)objects.size()) {
            auto& obj = objects[objId];
            
            if (!obj.markedForDeletion) {
                ZEUS_DEBUG_PRINT("[GC] Mark object #%d (class=%d) for deletion", objId, obj.classId);
                obj.clear();
                freeObjectSlots.push_back(objId);
            }
        }
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
                line += ZeusVMUtils::valueToDebugString(cell->value);
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
    void registerPrototypes(const std::vector<std::vector<IRType::IRInstr>>& bytecodes) {

        ZEUS_DEBUG_PRINT("[registerPrototypes] Total bytecodes chunks: %zu", bytecodes.size());
        
        chunkToProtoId.assign(bytecodes.size(), -1);

        int protoCounter = 0;
        for (size_t i = 0; i < bytecodes.size(); ++i) {
            const auto& bc = bytecodes[i];
            bool isFuncBegin = (!bc.empty() && bc.front().op == VMOperator::FUNC_BEGIN);
            ZEUS_DEBUG_PRINT("[registerPrototypes] Chunk #%zu: %zu instructions, FUNC_BEGIN=%d", 
                i, bc.size(), isFuncBegin ? 1 : 0);
            if (isFuncBegin) {
                Prototype proto;
                proto.body = bc;

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
                            
                            // 初始化 paramDefaults 数组
                            proto.paramDefaults.resize(proto.paramCount);
                            
                            // 读取每个默认值 (索引 + 值)
                            size_t argIdx = 7;
                            for (int i = 0; i < defaultCount && argIdx + 1 < bc.front().args.size(); ++i) {
                                if (std::holds_alternative<LexerType::Value>(bc.front().args[argIdx])) {
                                    int paramIdx = VMHelpers::getIntFromValue(std::get<LexerType::Value>(bc.front().args[argIdx]));
                                    if (paramIdx >= 0 && paramIdx < proto.paramCount) {
                                        auto defVal = std::get<LexerType::Value>(bc.front().args[argIdx + 1]);
                                        
                                        // 检查是否是 __NIL__ 标记，转换为 ZeusType::Null
                                        if (std::holds_alternative<std::string>(defVal.data) && 
                                            std::get<std::string>(defVal.data) == "__NIL__") {
                                            proto.paramDefaults[paramIdx] = ZeusType::Variant(ZeusType::Null());
                                        } else {
                                            proto.paramDefaults[paramIdx] = defVal;
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
            }
        }
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

        auto& oldVal = cell->value;
        ZeusGC::releaseVariantDeep(*this, oldVal);
        ZeusGC::retainVariantDeep(*this, newVal);

        cell->value = newVal;
    }

    enum class ReferenceWriteResult { NotReference, Stored, ConstTarget, Invalid };

    ZeusType::UniversalVariant loadCellValue(const Cell* cell) const {
        if (!cell) return LexerType::Value(std::string("__NIL__"));
        const ZeusType::UniversalVariant* current = &cell->value;
        for (int depth = 0; depth < 64; ++depth) {
            const auto* reference = std::get_if<ZeusType::ReferenceHandle>(current);
            if (!reference) return *current;
            if (!reference->address) {
                throw std::runtime_error("cannot load through a null reference");
            }
            if (reference->typeName == "cell") {
                const auto* target = static_cast<const Cell*>(reference->address);
                if (!target) throw std::runtime_error("reference points to a missing VM cell");
                current = &target->value;
                continue;
            }
            if (reference->typeName == "int") {
                return ZeusType::BigInteger(*static_cast<const int64_t*>(reference->address));
            }
            if (reference->typeName == "bool") {
                return *static_cast<const bool*>(reference->address);
            }
            // Preserve opaque reference types until their VM representation is registered.
            return *current;
        }
        throw std::runtime_error("cyclic reference chain while loading VM cell");
    }

    ReferenceWriteResult writeThroughReference(
        Cell* binding,
        const ZeusType::UniversalVariant& newValue
    ) {
        if (!binding) return ReferenceWriteResult::Invalid;
        auto* reference = std::get_if<ZeusType::ReferenceHandle>(&binding->value);
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
        for (auto* cell : local) {
            if (cell) releaseCellAndContent(cell);
        }

        for (auto* cell : upvalue) {
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
            callFrames.pop_back();
            shrinkStackWithRelease(frame.stackBase);
            releaseCurrentFrameEnvironment();
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

            // 类名带有完整命名空间前缀：ZeusBuiltins.exception.XXX
            std::string fullClassName = "ZeusBuiltins.exception." + errorType;
            int classId = findClassByName(fullClassName);
            if (classId < 0) {
                classId = findClassByName("ZeusBuiltins.exception.RuntimeError");
            }
            if (classId < 0) {
                classId = findClassByName("ZeusBuiltins.exception.Exception");
            }

            if (classId >= 0) {
                ZeusType::ObjectInstance obj;
                obj.classId = classId;
                obj.values["_message"] = errorMsg;
                obj.values["_type"] = errorType;
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

    // 统一执行 runChunk() 与 executeClosureSync() 的栈帧循环。
    // 两者只在停止深度、返回值捕获、异常处理和调试检查上有差异。
    void runFrameLoop(const FrameLoopOptions& options, FrameLoopState* state = nullptr) {
        const bool captureReturn = options.captureReturn && state != nullptr;

        while (callFrames.size() > options.stopFrameDepth) {
            CallFrame& frame = callFrames.back();

            if (vmstat != ZeusType::VMStatus::Running) {
                if (options.handleExceptions && handlePendingRuntimeException(options.stopFrameDepth)) {
                    continue;
                }

                if (options.handleExceptions && options.stopFrameDepth == 0 && vmstat == ZeusType::VMStatus::Error && VMException::lastError.hasError) {
                    printf("%s\n", VMException::lastError.formattedMsg.c_str());
                    VMException::lastError.hasError = false;
                }

                if (options.handleExceptions) {
                    ZEUS_DEBUG_PRINT("[VM] Aborted, status: %d", (int)vmstat);
                }
                break;
            }

            if (frame.pc >= frame.code->size()) {
                ZEUS_DEBUG_PRINT("%s Frame done (depth=%zu)", options.traceName, callFrames.size());

                CallFrame finishedFrame = callFrames.back();
                callFrames.pop_back();
                discardFrameExceptionHandlers(callFrames.size());

                if (callFrames.size() > options.stopFrameDepth) {
                    shrinkStackWithRelease(finishedFrame.stackBase);
                    releaseCurrentFrameEnvironment();

                    local = finishedFrame.savedLocal;
                    upvalue = finishedFrame.savedUpvalue;
                    __line = finishedFrame.savedLine;

                    // 隐式返回 0，并跳过调用者的 CALL_CLOSURE。
                    stack.push_back(LexerType::Value(0));
                    callFrames.back().pc++;
                } else if (captureReturn) {
                    captureReturnValue(*state);
                    releaseCurrentFrameEnvironment();

                    local = finishedFrame.savedLocal;
                    upvalue = finishedFrame.savedUpvalue;
                    __line = finishedFrame.savedLine;
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
                StackItem retItem;
                if (captureReturn) {
                    captureReturnValue(*state);
                } else if (!stack.empty()) {
                    retItem = popStackItem();
                }

                CallFrame returningFrame = callFrames.back();
                int autoReleaseId = returningFrame.calleeAutoReleaseId;
                callFrames.pop_back();
                discardFrameExceptionHandlers(callFrames.size());

                if (captureReturn || callFrames.size() > options.stopFrameDepth) {
                    if (autoReleaseId >= 0) {
                        releaseClosure(autoReleaseId);
                    }
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

            justPushedFrame = false;
            execute(ins);

            if (justPushedFrame) {
                justPushedFrame = false;
            } else if (!callFrames.empty()) {
                callFrames.back().pc++;
            }
        }
    }

    // 同步调用闭包：在当前上下文中执行闭包并等待返回
    // 用于 operator[.] 等需要同步获取返回值的场景
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
            retainObject(clos.boundThis->id); // stack owns a reference
            stack.insert(stack.begin() + (calleeIndex + 1), clos.boundThis.value());
            actualArgc++;
        }
        
        // 栈布局: [..., closure, (this?), arg0, arg1, ..., arg{n-1}]
        size_t calleeIndex = stack.size() - (actualArgc + 1);
        
        // 保存当前环境
        auto oldLocal = local;
        auto oldUpvalue = upvalue;
        int oldLine = __line;
        
        // 初始化新的 local 数组
        // 注意：当 boundThis 存在时，即使 paramCount=0，也需要为 this 分配空间
        local.clear();
        int localSize = std::max(proto.paramCount, actualArgc);
        ZEUS_DEBUG_PRINT("[executeClosureSync] localSize=%d, actualArgc=%d", localSize, actualArgc);
        local.resize(localSize);
        for (int i = 0; i < localSize; ++i) {
            local[i] = newCell(LexerType::Value(0), 0, "CLOSURE_SYNC_local");
        }
        
        // 设置 upvalue
        std::vector<Cell*> callUpvalues;
        callUpvalues.resize(clos.upvalues.size());
        for (size_t ci = 0; ci < clos.upvalues.size(); ++ci) {
            if (clos.upvalues[ci]) {
                callUpvalues[ci] = clos.upvalues[ci];
                callUpvalues[ci]->retain();
            } else {
                callUpvalues[ci] = newCell(LexerType::Value(0), 0, "CLOSURE_SYNC_upvalue");
            }
        }
        upvalue = callUpvalues;
        
        // 填充参数（包括可能的 this）
        ZEUS_DEBUG_PRINT("[executeClosureSync] filling params: localSize=%d, actualArgc=%d", localSize, actualArgc);
        for (int i = 0; i < localSize && i < actualArgc; ++i) {
            assignCellValue(local[i], stack[calleeIndex + 1 + i]);
            ZEUS_DEBUG_PRINT("[executeClosureSync] local[%d] set", i);
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
        callFrames.push_back(syncFrame);
        FrameLoopOptions frameOptions;
        frameOptions.stopFrameDepth = initialFrameDepth;
        frameOptions.captureReturn = true;
        frameOptions.handleExceptions = true;
        frameOptions.traceName = "[executeClosureSync]";
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

    // P1：按 const 引用接收指令，避免每次 dispatch 深拷贝
    // 整个 vector<IRArgs>（含 string/Value/Bytes）。实测前这是每条指令
    // 一次堆分配，是循环基准的最大头。
    void execute(const IRType::IRInstr& instr) {
        // 从指令读取行号和源文件ID（如果有的话）
        if (instr.line >= 0) {
            __line = instr.line;
        }
        if (instr.sourceFileId >= 0) {
            __sourceFileId = instr.sourceFileId;
        }

        if (vmstat != ZeusType::VMStatus::Running) {
            return;
        }

        /* Computed-goto removed: use switch-based dispatch only */
        if (vmstat == ZeusType::VMStatus::Error) {
            return;
        }
        
        switch (instr.op) {
            case VMOperator::CLASS_BUILD: {
                // 说明：栈布局 [ className, usingThisDefault, staticMethods(dict), instanceMethods(dict), operatorMethods(dict), fields(dict) ]
                if (stack.size() < 6) {
                    VMException::VMException exp("CLASS_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto fieldsU   = stack.back();
                stack.pop_back();

                auto operatorMethU = stack.back();
                stack.pop_back();

                auto instMethU = stack.back();
                stack.pop_back();

                auto staticMethU = stack.back();
                stack.pop_back();

                auto usingThisU = stack.back();
                stack.pop_back();

                auto nameU     = stack.back();
                stack.pop_back();

                std::string className = "";
                std::string usingThisAlias = "";

                if (std::holds_alternative<std::string>(nameU)) {
                    className = std::get<std::string>(nameU);
                } else if (std::holds_alternative<LexerType::Value>(nameU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(nameU).data)) {
                    className = std::get<std::string>(std::get<LexerType::Value>(nameU).data);
                } else {
                    VMException::VMException exp("CLASS_BUILD className must be string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // usingThisU is expected to be a string (alias) or empty string pushed as Value("")
                if (std::holds_alternative<std::string>(usingThisU)) {
                    usingThisAlias = std::get<std::string>(usingThisU);
                } else if (std::holds_alternative<LexerType::Value>(usingThisU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(usingThisU).data)) {
                    usingThisAlias = std::get<std::string>(std::get<LexerType::Value>(usingThisU).data);
                }

                ZeusType::ClassInfo info;
                info.name = className;
                info.usingThisAlias = usingThisAlias;

                // fields/staticMethods/instanceMethods/operatorMethods are provided as Variant (Object) values.
                // Parse instanceMethods/staticMethods/operatorMethods dicts if present and populate info maps.
                if (std::holds_alternative<ZeusType::Variant>(staticMethU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(staticMethU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto &kv : obj.impl->data) {
                                // keys are method names, values expected to be integer protoId
                                int pid = -1;
                                const ZeusType::UniversalVariant &val = kv.second;
                                if (std::holds_alternative<LexerType::Value>(val)) {
                                    const auto &lv = std::get<LexerType::Value>(val);
                                    if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) pid = VMHelpers::getIntFromValue(lv);
                                }
                                if (pid >= 0) info.staticMethods[kv.first] = pid;
                            }
                        }
                    }
                }

                if (std::holds_alternative<ZeusType::Variant>(instMethU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(instMethU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto &kv : obj.impl->data) {
                                int pid = -1;
                                const ZeusType::UniversalVariant &val = kv.second;
                                if (std::holds_alternative<LexerType::Value>(val)) {
                                    const auto &lv = std::get<LexerType::Value>(val);
                                    if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) pid = VMHelpers::getIntFromValue(lv);
                                }
                                if (pid >= 0) info.instanceMethods[kv.first] = pid;
                            }
                        }
                    }
                }

                // operatorMethods: parse operator symbols -> protoId
                if (std::holds_alternative<ZeusType::Variant>(operatorMethU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(operatorMethU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto &kv : obj.impl->data) {
                                int pid = -1;
                                const ZeusType::UniversalVariant &val = kv.second;
                                if (std::holds_alternative<LexerType::Value>(val)) {
                                    const auto &lv = std::get<LexerType::Value>(val);
                                    if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) pid = VMHelpers::getIntFromValue(lv);
                                }
                                if (pid >= 0) info.operatorMethods[kv.first] = pid;
                            }
                        }
                    }
                }

                // fields: if provided, evaluate default values will have been pushed as Values/Variants
                if (std::holds_alternative<ZeusType::Variant>(fieldsU)) {
                    const ZeusType::Variant &v = std::get<ZeusType::Variant>(fieldsU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto &kv : obj.impl->data) {
                                // store default values as-is
                                ZeusType::FieldDef fdef;
                                fdef.defaultValue = kv.second;
                                fdef.isConst = false; // const info not encoded in current DICT_BUILD
                                info.fields[kv.first] = fdef;
                            }
                        }
                    }
                }

                int classId = (int)classes.size();
                classes.push_back(std::move(info));

                stack.push_back(ZeusType::ClassHandle{classId});
                printStack();
                break;
            }

            case VMOperator::CLASS_BUILD_SHARED: {
                // 说明：栈布局 [ className, usingThisDefault, staticMethods(dict), instanceMethods(dict), operatorMethods(dict), fields(dict) ]
                // 与 CLASS_BUILD 类似，但类定义存储到共享堆上
                if (stack.size() < 6) {
                    VMException::VMException exp("CLASS_BUILD_SHARED stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto fieldsU       = stack.back(); stack.pop_back();
                auto operatorMethU = stack.back(); stack.pop_back();
                auto instMethU     = stack.back(); stack.pop_back();
                auto staticMethU   = stack.back(); stack.pop_back();
                auto usingThisU    = stack.back(); stack.pop_back();
                auto nameU         = stack.back(); stack.pop_back();

                std::string className = "";
                std::string usingThisAlias = "";

                if (std::holds_alternative<std::string>(nameU)) {
                    className = std::get<std::string>(nameU);
                } else if (std::holds_alternative<LexerType::Value>(nameU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(nameU).data)) {
                    className = std::get<std::string>(std::get<LexerType::Value>(nameU).data);
                } else {
                    VMException::VMException exp("CLASS_BUILD_SHARED className must be string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (std::holds_alternative<std::string>(usingThisU)) {
                    usingThisAlias = std::get<std::string>(usingThisU);
                } else if (std::holds_alternative<LexerType::Value>(usingThisU) && std::holds_alternative<std::string>(std::get<LexerType::Value>(usingThisU).data)) {
                    usingThisAlias = std::get<std::string>(std::get<LexerType::Value>(usingThisU).data);
                }

                ZeusType::ClassInfo info;
                info.name = className;
                info.usingThisAlias = usingThisAlias;

                // 解析方法映射 (与 CLASS_BUILD 相同)
                auto parseMethodDict = [](const ZeusType::UniversalVariant& dictU, std::unordered_map<std::string, int>& outMap) {
                    if (std::holds_alternative<ZeusType::Variant>(dictU)) {
                        const ZeusType::Variant& v = std::get<ZeusType::Variant>(dictU);
                        if (std::holds_alternative<ZeusType::Object>(v)) {
                            const ZeusType::Object& obj = std::get<ZeusType::Object>(v);
                            if (obj.impl) {
                                for (const auto& kv : obj.impl->data) {
                                    int pid = -1;
                                    const ZeusType::UniversalVariant& val = kv.second;
                                    if (std::holds_alternative<LexerType::Value>(val)) {
                                        const auto& lv = std::get<LexerType::Value>(val);
                                        if (std::holds_alternative<ZeusType::BigInteger>(lv.data)) {
                                            pid = VMHelpers::getIntFromValue(lv);
                                        }
                                    }
                                    if (pid >= 0) outMap[kv.first] = pid;
                                }
                            }
                        }
                    }
                };

                parseMethodDict(staticMethU, info.staticMethods);
                parseMethodDict(instMethU, info.instanceMethods);
                parseMethodDict(operatorMethU, info.operatorMethods);

                // 解析字段
                if (std::holds_alternative<ZeusType::Variant>(fieldsU)) {
                    const ZeusType::Variant& v = std::get<ZeusType::Variant>(fieldsU);
                    if (std::holds_alternative<ZeusType::Object>(v)) {
                        const ZeusType::Object& obj = std::get<ZeusType::Object>(v);
                        if (obj.impl) {
                            for (const auto& kv : obj.impl->data) {
                                ZeusType::FieldDef fdef;
                                fdef.defaultValue = kv.second;
                                fdef.isConst = false;
                                info.fields[kv.first] = fdef;
                            }
                        }
                    }
                }

                // 同时存储到本地 classes 数组（用于本 VM 访问）
                int classId = (int)classes.size();
                classes.push_back(info);

                // 将 ClassInfo 序列化为 Object 存储到共享堆
                // 为了跨 VM 访问，我们创建一个包含类信息的 ObjectData
                auto classObjData = std::make_shared<ZeusType::ObjectData>();
                classObjData->data["__class_id__"] = LexerType::Value(classId);
                classObjData->data["__class_name__"] = LexerType::Value(className);
                classObjData->data["__using_this__"] = LexerType::Value(usingThisAlias);
                classObjData->data["__is_shared_class__"] = LexerType::Value(true);

                // 存储方法信息
                for (const auto& kv : info.staticMethods) {
                    classObjData->data["__static_" + kv.first] = LexerType::Value(kv.second);
                }
                for (const auto& kv : info.instanceMethods) {
                    classObjData->data["__inst_" + kv.first] = LexerType::Value(kv.second);
                }
                for (const auto& kv : info.operatorMethods) {
                    classObjData->data["__op_" + kv.first] = LexerType::Value(kv.second);
                }
                for (const auto& kv : info.fields) {
                    classObjData->data["__field_" + kv.first] = kv.second.defaultValue;
                }

                ZeusType::Object sharedClassObj;
                sharedClassObj.impl = classObjData;

                // 存储到共享堆
                ZeusShared::getGlobalHeap().getOrCreate(
                    "__class__" + className,
                    ZeusType::Variant(sharedClassObj)
                );

                // 返回 SharedClassHandle
                stack.push_back(ZeusType::SharedClassHandle{className});
                printStack();
                break;
            }

            case VMOperator::OBJ_BUILD: {
                // 读取参数个数
                int argc = 0;
                if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }
                
                if (stack.size() < (size_t)(argc + 1)) {
                    VMException::VMException exp("OBJ_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 保存构造函数参数（从栈中弹出，并转移“栈所有权”到本地临时列表）
                // 重要：不能直接 stack.pop_back()，否则 Object/Closure 的引用计数不会对称释放，导致泄露或悬空引用。
                std::vector<StackItem> ctorArgItems;
                ctorArgItems.reserve(argc);
                struct CtorArgGuard {
                    VM* vm;
                    std::vector<StackItem>* items;
                    ~CtorArgGuard() {
                        if (!vm || !items) return;
                        for (auto& it : *items) {
                            if (it.owner) vm->dropStackItem(it);
                        }
                    }
                } ctorArgGuard{this, &ctorArgItems};

                for (int i = 0; i < argc; ++i) {
                    ctorArgItems.insert(ctorArgItems.begin(), popStackItem());
                }

                StackItem clsItem = popStackItem();
                auto clsU = clsItem.value;
                dropStackItem(clsItem);
                
                // DEBUG: 输出 clsU 的类型
                ZEUS_DEBUG_PRINT("[OBJ_BUILD] clsU type: ClassHandle=%d, SharedClassHandle=%d, Value=%d", 
                    std::holds_alternative<ZeusType::ClassHandle>(clsU),
                    std::holds_alternative<ZeusType::SharedClassHandle>(clsU),
                    std::holds_alternative<LexerType::Value>(clsU));
                if (std::holds_alternative<LexerType::Value>(clsU)) {
                    ZEUS_DEBUG_PRINT("[OBJ_BUILD] clsU value: %s", ZeusVMUtils::valueToDebugString(clsU).c_str());
                }

                int classId = -1;
                
                // 支持多种形式：ClassHandle、SharedClassHandle 或类名字符串
                if (std::holds_alternative<ZeusType::ClassHandle>(clsU)) {
                    classId = std::get<ZeusType::ClassHandle>(clsU).id;
                } else if (std::holds_alternative<ZeusType::SharedClassHandle>(clsU)) {
                    // SharedClassHandle：从共享堆获取类信息
                    std::string className = std::get<ZeusType::SharedClassHandle>(clsU).name;
                    
                    // 先在本地 classes 中查找
                    for (size_t i = 0; i < classes.size(); ++i) {
                        if (classes[i].name == className) {
                            classId = (int)i;
                            break;
                        }
                    }
                    
                    // 如果本地没有，尝试从共享堆恢复类定义
                    if (classId == -1) {
                        auto cellPtr = ZeusShared::getGlobalHeap().find("__class__" + className);
                        
                        if (cellPtr) {
                            ZeusType::UniversalVariant& uniVar = cellPtr->value;
                            
                            if (std::holds_alternative<ZeusType::Variant>(uniVar)) {
                                ZeusType::Variant& variant = std::get<ZeusType::Variant>(uniVar);
                                
                                if (std::holds_alternative<ZeusType::Object>(variant)) {
                                    ZeusType::Object& objWrapper = std::get<ZeusType::Object>(variant);
                                    
                                    if (objWrapper.impl) {
                                        // 从共享堆恢复 ClassInfo
                                        ZeusType::ClassInfo info;
                                        info.name = className;
                                        
                                        for (const auto& kv : objWrapper.impl->data) {
                                            const std::string& key = kv.first;
                                            const ZeusType::UniversalVariant& val = kv.second;
                                            
                                            if (key == "__using_this__") {
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    const LexerType::Value& lv = std::get<LexerType::Value>(val);
                                                    if (std::holds_alternative<std::string>(lv.data)) {
                                                        info.usingThisAlias = std::get<std::string>(lv.data);
                                                    }
                                                }
                                            } else if (key.rfind("__static_", 0) == 0) {
                                                std::string methName = key.substr(9);
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    info.staticMethods[methName] = VMHelpers::getIntFromValue(std::get<LexerType::Value>(val));
                                                }
                                            } else if (key.rfind("__inst_", 0) == 0) {
                                                std::string methName = key.substr(7);
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    info.instanceMethods[methName] = VMHelpers::getIntFromValue(std::get<LexerType::Value>(val));
                                                }
                                            } else if (key.rfind("__op_", 0) == 0) {
                                                std::string opName = key.substr(5);
                                                if (std::holds_alternative<LexerType::Value>(val)) {
                                                    info.operatorMethods[opName] = VMHelpers::getIntFromValue(std::get<LexerType::Value>(val));
                                                }
                                            } else if (key.rfind("__field_", 0) == 0) {
                                                std::string fieldName = key.substr(8);
                                                ZeusType::FieldDef fdef;
                                                fdef.defaultValue = val;
                                                fdef.isConst = false;
                                                info.fields[fieldName] = fdef;
                                            }
                                        }
                                        
                                        classId = (int)classes.size();
                                        classes.push_back(info);
                                    }
                                }
                            }
                        }
                    }
                    
                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD SharedClassHandle not found in heap: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (std::holds_alternative<std::string>(clsU)) {
                    // 延迟解析：在 nameToClass 映射中查找类名
                    std::string className = std::get<std::string>(clsU);
                    
                    // 优先在 nameToClass 映射中查找
                    auto it = nameToClass.find(className);
                    if (it != nameToClass.end()) {
                        classId = it->second;
                        vmMessage("[late-resolve] OBJ_BUILD resolved class '" + className + "' -> #" + std::to_string(classId));
                    } else {
                        // 回退：在全局变量槽中查找（兼容性）
                        for (size_t i = 0; i < global.size(); ++i) {
                            if (global[i] && std::holds_alternative<ZeusType::ClassHandle>(global[i]->value)) {
                                int cid = std::get<ZeusType::ClassHandle>(global[i]->value).id;
                                auto* classPtr = getClass(cid);
                                if (classPtr && classPtr->name == className) {
                                    classId = cid;
                                    break;
                                }
                            }
                        }
                    }
                    
                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD class not found: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (std::holds_alternative<LexerType::Value>(clsU)) {
                    // 处理 LOAD_VALUE 发出的 Value 类型
                    const auto& val = std::get<LexerType::Value>(clsU);
                    if (std::holds_alternative<std::string>(val.data)) {
                        std::string className = std::get<std::string>(val.data);
                        
                        // 优先在 nameToClass 映射中查找
                        auto it = nameToClass.find(className);
                        if (it != nameToClass.end()) {
                            classId = it->second;
                            vmMessage("[late-resolve] OBJ_BUILD resolved class '" + className + "' -> #" + std::to_string(classId));
                        } else {
                            // 回退：在全局变量槽中查找（兼容性）
                            for (size_t i = 0; i < global.size(); ++i) {
                                if (global[i] && std::holds_alternative<ZeusType::ClassHandle>(global[i]->value)) {
                                    int cid = std::get<ZeusType::ClassHandle>(global[i]->value).id;
                                    auto* classPtr = getClass(cid);
                                    if (classPtr && classPtr->name == className) {
                                        classId = cid;
                                        break;
                                    }
                                }
                            }
                        }
                        
                        if (classId == -1) {
                            VMException::VMException exp("OBJ_BUILD class not found: " + className, __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                    } else {
                        VMException::VMException exp("OBJ_BUILD needs ClassHandle or className string/Value", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else {
                    VMException::VMException exp("OBJ_BUILD needs ClassHandle or className string/Value", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    VMException::VMException exp("OBJ_BUILD invalid class id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::ObjectInstance obj;
                obj.classId = classId;

                // initialize object fields from class info
                const auto& ci = *classPtr;
                for (const auto& kv : ci.fields) {
                    obj.values[kv.first] = kv.second.defaultValue;
                    // The object now holds a reference to the default value, so it
                    // has to be retained. Without this the instance owns a
                    // reference it never took, and the first OBJ_STORE_ATTR on the
                    // field releases the value out from under every other instance
                    // that was initialised from the same default.
                    ZeusGC::retainVariantDeep(*this, obj.values[kv.first]);
                    obj.flags[kv.first]  = kv.second.isConst ? 0x1 : 0x0;
                }

                ZeusType::ObjectHandle objHandle = allocObject(std::move(obj));
                int objId = objHandle.id;
                // 保护构造过程中对象不被提前释放（调用结束后会对称释放一次）
                retainObject(objId);

                // 检查是否有 init 构造函数方法
                ZEUS_DEBUG_PRINT("[VM] OBJ_BUILD: Looking for init in instanceMethods (size=%zu)", ci.instanceMethods.size());
                for (const auto& kv : ci.instanceMethods) {
                    ZEUS_DEBUG_PRINT("[VM]   - method '%s' -> protoId=%d", kv.first.c_str(), kv.second);
                }
                
                auto initIt = ci.instanceMethods.find("init");
                if (initIt != ci.instanceMethods.end()) {
                    // 调用 init 方法
                    int protoId = initIt->second;
                    const Prototype* protoPtr = getPrototype(protoId);
                    ZEUS_DEBUG_PRINT("[VM] Found init: protoId=%d, protoPtr=%p", protoId, (void*)protoPtr);
                    if (protoPtr) {
                        const Prototype& proto = *protoPtr;
                        ZEUS_DEBUG_PRINT("[VM] Calling init constructor with %d args", argc);
                        // 创建临时闭包
                        Closure clo;
                        clo.protoId = protoId;
                        clo.autoRelease = true;
                        int cloId = allocClosure(std::move(clo));
                        const size_t constructorFrameDepth = callFrames.size();
                        const bool savedJustPushedFrame = justPushedFrame;

                         stack.push_back(ZeusType::ClosureHandle{cloId});
                         retainObject(objId); // stack will later release this slot in shrinkStackWithRelease
                         stack.push_back(objHandle);  // 使用完整的 ObjectHandle（包含 generation）
                         for (auto& argIt : ctorArgItems) {
                             stack.push_back(argIt.value);
                             // transfer ownership from ctorArgItems -> stack
                             argIt.owner = 0;
                         }
                        
                        // 保存oldLocal（但在OBJ_BUILD中，local应该是当前frame的）
                        auto oldLocal = local;
                        auto oldUpvalue = upvalue;
                        
                        // 设置init的参数（使用与CALL_CLOSURE相同的逻辑）
                        int calleeIndex = stack.size() - (argc + 2);  // argc+1(this) + 1(closure)
                        
                        // 初始化local数组
                        int regularParamCount = proto.paramCount;
                        local.clear();
                        local.resize(regularParamCount);
                        upvalue.clear();
                        for (int i = 0; i < regularParamCount; ++i) {
                            local[i] = newCell(LexerType::Value(0), 0, "CONSTRUCTOR_local");
                        }
                        
                        // 扩展 local 以容纳 *args 和 **kwargs
                        if (proto.hasVariadicArgs || proto.hasVariadicKwargs) {
                            int extraSlots = 0;
                            if (proto.hasVariadicArgs) extraSlots++;
                            if (proto.hasVariadicKwargs) extraSlots++;
                            
                            int newLocalSize = regularParamCount + extraSlots;
                            local.resize(newLocalSize);
                            for (int idx = regularParamCount; idx < newLocalSize; ++idx) {
                                local[idx] = newCell(LexerType::Value(0), 0, "CONSTRUCTOR_varargs");
                            }
                        }
                        
                        // 分离位置参数和命名参数
                        std::map<std::string, ZeusType::UniversalVariant> namedArgs;
                        std::vector<ZeusType::UniversalVariant> positionalArgs;
                        
                        // this是第一个参数
                        positionalArgs.push_back(stack[calleeIndex + 1]);
                        
                        // 处理构造函数的其他参数
                        for (int i = 0; i < argc; ++i) {
                            auto& arg = stack[calleeIndex + 2 + i];  // +2因为有closure和this
                            if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(arg)) {
                                auto namedArgPtr = std::get<std::shared_ptr<ZeusType::NamedArg>>(arg);
                                namedArgs[namedArgPtr->name] = namedArgPtr->value;
                            } else {
                                positionalArgs.push_back(arg);
                            }
                        }
                        
                        int positionalCount = (int)positionalArgs.size();
                        
                        // 填充常规参数
                        std::set<std::string> consumedNamedArgs;
                        
                        for (int i = 0; i < regularParamCount; ++i) {
                            bool filled = false;
                            
                            // 优先使用位置参数
                            if (i < positionalCount) {
                                assignCellValue(local[i], positionalArgs[i]);
                                filled = true;
                            }
                            
                            // 尝试从命名参数匹配
                            if (!filled && i < (int)proto.paramNames.size()) {
                                const std::string& paramName = proto.paramNames[i];
                                auto it = namedArgs.find(paramName);
                                if (it != namedArgs.end()) {
                                    assignCellValue(local[i], it->second);
                                    consumedNamedArgs.insert(paramName);
                                    filled = true;
                                }
                            }
                            
                            // 使用默认值
                            if (!filled && i < (int)proto.paramDefaults.size()) {
                                const auto& defVal = proto.paramDefaults[i];
                                if (std::holds_alternative<LexerType::Value>(defVal)) {
                                    assignCellValue(local[i], std::get<LexerType::Value>(defVal));
                                    filled = true;
                                } else if (ZeusVMUtils::isNull(defVal)) {
                                    assignCellValue(local[i], ZeusType::Variant(ZeusType::Null()));
                                    filled = true;
                                }
                            }
                            
                            // 最后用0初始化
                            if (!filled) {
                                assignCellValue(local[i], LexerType::Value(0));
                            }
                        }
                        
                        // 构建 *args 数组
                        if (proto.hasVariadicArgs) {
                            std::vector<ZeusType::UniversalVariant> argsVec;
                            for (int i = regularParamCount; i < positionalCount; ++i) {
                                argsVec.push_back(positionalArgs[i]);
                            }
                            
                            auto arrayDataPtr = std::make_shared<ZeusType::ArrayData>();
                            arrayDataPtr->data = argsVec;
                            ZeusType::Array argsArray;
                            argsArray.impl = arrayDataPtr;
                            
                            ZeusType::Variant variantArgsArray = argsArray;
                            assignCellValue(local[regularParamCount], variantArgsArray);
                        }
                        
                        // 构建 **kwargs 字典
                        if (proto.hasVariadicKwargs) {
                            auto objDataPtr = std::make_shared<ZeusType::ObjectData>();
                            
                            for (const auto& kv : namedArgs) {
                                if (consumedNamedArgs.find(kv.first) == consumedNamedArgs.end()) {
                                    objDataPtr->data[kv.first] = kv.second;
                                    objDataPtr->order.push_back(kv.first);
                                }
                            }
                            
                            ZeusType::Object kwargsDict;
                            kwargsDict.impl = objDataPtr;
                            
                            ZeusType::Variant variantKwargs = kwargsDict;
                            int kwargsSlot = regularParamCount;
                            if (proto.hasVariadicArgs) kwargsSlot++;
                            assignCellValue(local[kwargsSlot], variantKwargs);
                        }
                        
                        // Use the shared frame loop while retaining constructor argument binding.
                        CallFrame constructorFrame;
                        constructorFrame.code = &proto.body;
                        constructorFrame.pc = 0;
                        constructorFrame.savedLocal = oldLocal;
                        constructorFrame.savedUpvalue = oldUpvalue;
                        constructorFrame.stackBase = calleeIndex;
                        constructorFrame.savedLine = __line;
                        callFrames.push_back(std::move(constructorFrame));
                        FrameLoopOptions constructorOptions;
                        constructorOptions.stopFrameDepth = constructorFrameDepth;
                        constructorOptions.captureReturn = true;
                        constructorOptions.handleExceptions = true;
                        constructorOptions.traceName = "[constructor]";
                        FrameLoopState constructorState;
                        runFrameLoop(constructorOptions, &constructorState);
                        if (vmstat != ZeusType::VMStatus::Running) {
                            unwindFrameEnvironments(constructorFrameDepth);
                        }
                        if (constructorState.retOwner) {
                            dropStackItem(StackItem{constructorState.retVal, constructorState.retOwner});
                        }
                        justPushedFrame = savedJustPushedFrame;
                        // The stack owns the temporary constructor closure and arguments.
                        shrinkStackWithRelease(calleeIndex);
                    }
                }

                // 释放构造保护 retain（创建时 +1）
                releaseObject(objId);
                if (vmstat != ZeusType::VMStatus::Running) {
                    releaseObject(objId); // no result is transferred to the caller
                    break;
                }
                stack.push_back(objHandle);  // 使用完整的 ObjectHandle（包含 generation）
                printStack();
                break;
            }

            case VMOperator::OBJ_BUILD_SHARED: {
                // OBJ_BUILD_SHARED "objName" argc
                // 创建共享对象，存储到共享堆上
                // 栈布局: [classHandle/className, arg1, arg2, ...]
                
                std::string objName = "";
                int argc = 0;
                
                if (instr.args.size() >= 1 && std::holds_alternative<std::string>(instr.args[0])) {
                    objName = std::get<std::string>(instr.args[0]);
                }
                if (instr.args.size() >= 2 && std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));
                }

                if (objName.empty()) {
                    VMException::VMException exp("OBJ_BUILD_SHARED requires object name", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.size() < (size_t)(argc + 1)) {
                    VMException::VMException exp("OBJ_BUILD_SHARED stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 保存构造函数参数
                std::vector<ZeusType::UniversalVariant> ctorArgs;
                for (int i = 0; i < argc; ++i) {
                    ctorArgs.insert(ctorArgs.begin(), stack.back());
                    stack.pop_back();
                }

                // 获取类
                auto clsU = stack.back();
                stack.pop_back();

                int classId = -1;

                if (std::holds_alternative<ZeusType::ClassHandle>(clsU)) {
                    classId = std::get<ZeusType::ClassHandle>(clsU).id;
                } else if (std::holds_alternative<ZeusType::SharedClassHandle>(clsU)) {
                    // 共享类：需要从共享堆获取类信息
                    std::string className = std::get<ZeusType::SharedClassHandle>(clsU).name;
                    
                    // 查找本地 classes 数组中是否已有该类
                    for (size_t i = 0; i < classes.size(); ++i) {
                        if (classes[i].name == className) {
                            classId = (int)i;
                            break;
                        }
                    }
                    
                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD_SHARED: shared class not found locally: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (std::holds_alternative<std::string>(clsU)) {
                    std::string className = std::get<std::string>(clsU);
                    for (size_t i = 0; i < classes.size(); ++i) {
                        if (classes[i].name == className) {
                            classId = (int)i;
                            break;
                        }
                    }
                    if (classId == -1) {
                        VMException::VMException exp("OBJ_BUILD_SHARED class not found: " + className, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else {
                    VMException::VMException exp("OBJ_BUILD_SHARED needs ClassHandle or className", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    VMException::VMException exp("OBJ_BUILD_SHARED invalid class id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 创建对象实例（同时存储到本地和共享堆）
                ZeusType::ObjectInstance obj;
                obj.classId = classId;

                const auto& ci = *classPtr;
                for (const auto& kv : ci.fields) {
                    obj.values[kv.first] = kv.second.defaultValue;
                    // Same ownership rule as OBJ_BUILD: the instance owns its
                    // field values, so the shared default has to be retained.
                    ZeusGC::retainVariantDeep(*this, obj.values[kv.first]);
                    obj.flags[kv.first]  = kv.second.isConst ? 0x1 : 0x0;
                }

                ZeusType::ObjectHandle objHandle = allocObject(std::move(obj));
                int objId = objHandle.id;

                // 将对象序列化为 ObjectData 存储到共享堆
                auto sharedObjData = std::make_shared<ZeusType::ObjectData>();
                sharedObjData->data["__obj_id__"] = LexerType::Value(objId);
                sharedObjData->data["__class_id__"] = LexerType::Value(classId);
                sharedObjData->data["__class_name__"] = LexerType::Value(ci.name);
                
                // 注意：这里需要重新获取对象引用，因为 allocObject 可能复用槽位
                auto* objRef = getObject(objId);
                if (objRef) {
                    for (const auto& kv : objRef->values) {
                        sharedObjData->data[kv.first] = kv.second;
                    }
                }

                ZeusType::Object sharedObj;
                sharedObj.impl = sharedObjData;

                ZeusShared::getGlobalHeap().getOrCreate(
                    objName,
                    ZeusType::Variant(sharedObj)
                );

                // 调用 init 构造函数（如果存在）
                auto initIt = ci.instanceMethods.find("init");
                if (initIt != ci.instanceMethods.end() && getPrototype(initIt->second)) {
                    // 简化处理：不调用 init，让用户自己调用
                    // 或者之后扩展支持
                }

                // 返回 SharedObjectHandle
                stack.push_back(ZeusType::SharedObjectHandle{objName});
                printStack();
                break;
            }

            case VMOperator::OBJ_LOAD_ATTR: {
                if (instr.args.empty() || !std::holds_alternative<std::string>(instr.args[0])) {
                    VMException::VMException exp("OBJ_LOAD_ATTR missing attrName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("OBJ_LOAD_ATTR stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string attr = std::get<std::string>(instr.args[0]);

                StackItem objItem = popStackItem();
                auto objU = objItem.value;
                struct DropObj {
                    VM* vm;
                    StackItem* it;
                    ~DropObj() { if (vm && it) vm->dropStackItem(*it); }
                } dropObj{this, &objItem};

                // 特殊处理：如果是ModuleProxy，将属性访问映射到全局变量
                if (std::holds_alternative<ZeusType::ModuleProxy>(objU)) {
                    const auto& proxy = std::get<ZeusType::ModuleProxy>(objU);
                    std::string fullName = proxy.modulePath + "." + attr;
                    
                    // 从globalNames查找索引
                    int index = -1;
                    for (size_t i = 0; i < globalNames.size(); ++i) {
                        if (globalNames[i] == fullName) {
                            index = (int)i;
                            break;
                        }
                    }
                    
                    if (index < 0) {
                        VMException::VMException exp("Module '" + proxy.modulePath + "' has no attribute '" + attr + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    if ((size_t)index >= global.size()) {
                        VMException::VMException exp("Module attribute index out of range: " + fullName, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    if (!global[index]) {
                        VMException::VMException exp("Module attribute not initialized: " + fullName, __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // 压入全局变量的值（为栈建立所有权引用）
                    ZeusGC::retainVariantDeep(*this, global[index]->value);
                    stack.push_back(global[index]->value);
                    break;
                }

                // 处理 SharedObjectHandle：从共享堆读取属性
                if (std::holds_alternative<ZeusType::SharedObjectHandle>(objU)) {
                    std::string sharedName = std::get<ZeusType::SharedObjectHandle>(objU).name;
                    
                    auto cell = ZeusShared::getGlobalHeap().find(sharedName);
                    if (!cell) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: undefined shared object '" + sharedName + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    ZeusType::UniversalVariant sharedVal = cell->read();
                    
                    // 检查是否是 Variant 中的 Object
                    if (!std::holds_alternative<ZeusType::Variant>(sharedVal)) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& variantVal = std::get<ZeusType::Variant>(sharedVal);
                    if (!std::holds_alternative<ZeusType::Object>(variantVal)) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& obj = std::get<ZeusType::Object>(variantVal);
                    if (!obj.impl) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared object has null impl", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto attrIt = obj.impl->data.find(attr);
                    if (attrIt == obj.impl->data.end()) {
                        VMException::VMException exp("OBJ_LOAD_ATTR: shared object has no attribute '" + attr + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    ZeusGC::retainVariantDeep(*this, attrIt->second);
                    stack.push_back(attrIt->second);
                    ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: shared object '%s', attr='%s'", sharedName.c_str(), attr.c_str());
                    break;
                }

                if (!std::holds_alternative<ZeusType::ObjectHandle>(objU)) {
                    // 检查是否是ClassHandle（试图访问类的静态方法-MODIFIED）
                    std::string actualType = "unknown";
                    if (std::holds_alternative<ZeusType::ClassHandle>(objU)) {
                        int cid = std::get<ZeusType::ClassHandle>(objU).id;
                        auto* classPtr = getClass(cid);
                        if (!classPtr) {
                            VMException::VMException exp("OBJ_LOAD_ATTR invalid class id", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        std::string className = classPtr->name;
                        
                        // 尝试加载静态方法
                        auto itStatic = classPtr->staticMethods.find(attr);
                        if (itStatic != classPtr->staticMethods.end()) {
                            // 找到静态方法，创建闭包并压栈
                            int protoId = itStatic->second;
                            if (!getPrototype(protoId)) {
                                VMException::VMException exp("OBJ_LOAD_ATTR invalid static method protoId", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                            Closure clo;
                            clo.protoId = protoId;
                            // 如果下一个指令是 CALL_CLOSURE 则视为临时调用并自动释放
                            // We execute this closure synchronously; mark as autoRelease
                            clo.autoRelease = true;
                            int cloId = allocClosure(std::move(clo));
                            stack.push_back(ZeusType::ClosureHandle{cloId});
                            break;
                        }
                        
                        // 没找到静态方法，报错
                        auto exp = VMException::VMException(
                            VMException::ErrorCode::E1600_ATTRIBUTE_NOT_FOUND,
                            VMException::ErrorCategory::ATTRIBUTE_ERROR,
                            "Cannot find static member '" + attr + "' on class '" + className + "'",
                            __line, instr
                        );
                        exp.setDetails("Class '" + className + "' does not have a static method or static variable named '" + attr + "'");
                        exp.setSuggestion(
                            "Check if the member name is correct, or if it should be an instance member: 'let obj = new " + 
                            className + "(...); obj." + attr + "'"
                        );
                        exp._throw(vmstat, currentChunk);
                        break;
                    } else if (std::holds_alternative<LexerType::Value>(objU)) {
                        actualType = "primitive value";
                    } else if (std::holds_alternative<ZeusType::ClosureHandle>(objU)) {
                        actualType = "function";
                    }
                    
                    auto exp = VMException::typeError_ExpectedObject(
                        "Attribute access", actualType, __line, instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 获取 ObjectHandle 并验证 generation
                ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(objU);
                auto* objPtr = getValidObject(objHandle);
                
                if (!objPtr) {
                    // 对象已被释放或 slot 已被复用
                    // 提供更详细的错误信息
                    std::string detailMsg = "OBJ_LOAD_ATTR: object has been freed or reused (stale reference)";
                    auto* rawObj = getObject(objHandle.id);
                    if (!rawObj) {
                        detailMsg += " [object not found: id=" + std::to_string(objHandle.id) + "]";
                    } else if (rawObj->generation != objHandle.generation) {
                        detailMsg += " [generation mismatch: handle.gen=" + std::to_string(objHandle.generation) 
                                   + " obj.gen=" + std::to_string(rawObj->generation) + "]";
                    } else if (rawObj->markedForDeletion) {
                        detailMsg += " [object marked for deletion]";
                    } else if (rawObj->classId < 0) {
                        detailMsg += " [object cleared, classId=" + std::to_string(rawObj->classId) + "]";
                    }
                    VMException::VMException exp(detailMsg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto& obj = *objPtr;
                
                // 首先检查是否有 operator[.] 重载
                int classId = obj.classId;
                auto* classPtr = getClass(classId);
                
                ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: obj.classId=%d, attr='%s', classCount=%zu", 
                    classId, attr.c_str(), getClassCount());
                
                if (classPtr) {
                    const auto& ci = *classPtr;
                    ZEUS_DEBUG_PRINT("[VM]   class='%s', operatorMethods.size()=%zu", 
                        ci.name.c_str(), ci.operatorMethods.size());
                    
                    for (const auto& kv : ci.operatorMethods) {
                        ZEUS_DEBUG_PRINT("[VM]     operator '%s' -> protoId=%d", kv.first.c_str(), kv.second);
                    }
                    
                    auto opIt = ci.operatorMethods.find("[.]");
                    
                    if (opIt != ci.operatorMethods.end()) {
                        // 有 operator[.] 重载,调用它
                        int protoId = opIt->second;
                        ZEUS_DEBUG_PRINT("[VM] Found operator[.]! protoId=%d, getPrototype=%p)", 
                            protoId, (void*)getPrototype(protoId));
                        
                        if (!getPrototype(protoId)) {
                            ZEUS_DEBUG_PRINT("[VM] ERROR: Invalid protoId!");
                            VMException::VMException exp("OBJ_LOAD_ATTR: invalid protoId for operator[.]", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        
                        // 创建 Closure 包装 protoId
                        Closure clo;
                        clo.protoId = protoId;
                        clo.autoRelease = true; // operator[.] is an immediate call wrapper
                        int cloId = allocClosure(std::move(clo));
                        
                        ZEUS_DEBUG_PRINT("[VM] Created closure #%d for protoId=%d", cloId, protoId);
                        
                        // 压入: closure, this(对象), 属性名
                        stack.push_back(ZeusType::ClosureHandle{cloId});
                        stack.push_back(objU);  // this
                        stack.push_back(LexerType::Value(attr));  // 属性名作为参数
                        
                        ZEUS_DEBUG_PRINT("[VM] Stack prepared, calling executeClosureSync with 2 args...");
                        
                        // 使用同步调用 operator[.]
                        if (!executeClosureSync(cloId, 2)) {
                            if (vmstat != ZeusType::VMStatus::Running) break;
                            VMException::VMException exp("OBJ_LOAD_ATTR: operator[.] sync call failed", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        
                        ZEUS_DEBUG_PRINT("[VM] executeClosureSync returned, stack.size=%zu", stack.size());
                        
                        // 方法调用完成,结果在栈顶,继续执行
                        break;
                    } else {
                        ZEUS_DEBUG_PRINT("[VM]   operator[.] NOT found!");
                    }
                }
                
                // 没有operator[.]重载,使用默认行为
                auto it = obj.values.find(attr);
                if (it == obj.values.end()) {
                    // 如果不在values中，尝试查找instanceMethods
                    if (classPtr) {
                        const auto& ci = *classPtr;
                        auto methodIt = ci.instanceMethods.find(attr);
                        if (methodIt != ci.instanceMethods.end()) {
                            // 找到实例方法，创建一个绑定了this的闭包
                            int protoId = methodIt->second;
                            ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: found instance method '%s', protoId=%d", attr.c_str(), protoId);
                            
                            if (!getPrototype(protoId)) {
                                VMException::VMException exp("OBJ_LOAD_ATTR: invalid protoId for instance method '" + attr + "'", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                            
                            // 创建闭包并绑定this（使用完整的 ObjectHandle 包含 generation）
                            Closure clo;
                            clo.protoId = protoId;
                            retainObject(objHandle.id); // closure will own & later release boundThis
                            clo.boundThis = objHandle;
                            int cloId = allocClosure(std::move(clo));
                            stack.push_back(ZeusType::ClosureHandle{cloId});
                            ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: created bound closure #%d for method '%s'", cloId, attr.c_str());
                            break;
                        }
                    }
                    
                    // 打印所有可用的属性名
                    std::string availableAttrs;
                    for (const auto& kv : obj.values) {
                        if (!availableAttrs.empty()) availableAttrs += ", ";
                        availableAttrs += kv.first;
                    }
                    ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: object has fields: [%s]", availableAttrs.c_str());
                    
                    VMException::VMException exp("OBJ_LOAD_ATTR missing field '" + attr + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZEUS_DEBUG_PRINT("[VM] OBJ_LOAD_ATTR: found field '%s', pushing to stack", attr.c_str());
                auto& fieldVal = it->second;
                ZeusGC::retainVariantDeep(*this, fieldVal);
                stack.push_back(fieldVal);
                printStack();
                break;
            }

            case VMOperator::OBJ_STORE_ATTR: {
                if (instr.args.empty() || !std::holds_alternative<std::string>(instr.args[0])) {
                    VMException::VMException exp("OBJ_STORE_ATTR missing attrName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.size() < 2) {
                    VMException::VMException exp("OBJ_STORE_ATTR stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string attr = std::get<std::string>(instr.args[0]);

                StackItem valItem = popStackItem();
                StackItem objItem = popStackItem();
                auto val = valItem.value;
                auto objU = objItem.value;
                struct DropInputs {
                    VM* vm;
                    StackItem* a;
                    StackItem* b;
                    ~DropInputs() {
                        if (vm && a) vm->dropStackItem(*a);
                        if (vm && b) vm->dropStackItem(*b);
                    }
                } dropInputs{this, &valItem, &objItem};

                // 处理 SharedObjectHandle：写入共享堆中的对象属性
                if (std::holds_alternative<ZeusType::SharedObjectHandle>(objU)) {
                    std::string sharedName = std::get<ZeusType::SharedObjectHandle>(objU).name;
                    
                    auto cell = ZeusShared::getGlobalHeap().find(sharedName);
                    if (!cell) {
                        VMException::VMException exp("OBJ_STORE_ATTR: undefined shared object '" + sharedName + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // 加锁修改共享对象
                    cell->lockWrite();
                    
                    // 检查是否是 Variant 中的 Object
                    if (!std::holds_alternative<ZeusType::Variant>(cell->value)) {
                        cell->unlockWrite();
                        VMException::VMException exp("OBJ_STORE_ATTR: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& variantVal = std::get<ZeusType::Variant>(cell->value);
                    if (!std::holds_alternative<ZeusType::Object>(variantVal)) {
                        cell->unlockWrite();
                        VMException::VMException exp("OBJ_STORE_ATTR: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& obj = std::get<ZeusType::Object>(variantVal);
                    if (!obj.impl) {
                        cell->unlockWrite();
                        VMException::VMException exp("OBJ_STORE_ATTR: shared object has null impl", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // 更新属性值
                    if (obj.impl->data.find(attr) == obj.impl->data.end()) {
                        obj.impl->order.push_back(attr);
                    }
                    obj.impl->data[attr] = val;
                    
                    cell->unlockWrite();
                    
                    ZEUS_DEBUG_PRINT("[VM] OBJ_STORE_ATTR: shared object '%s', attr='%s'", sharedName.c_str(), attr.c_str());
                    break;
                }

                if (!std::holds_alternative<ZeusType::ObjectHandle>(objU)) {
                    VMException::VMException exp("OBJ_STORE_ATTR needs ObjectHandle", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 获取 ObjectHandle 并验证 generation
                ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(objU);
                auto* objPtr = getValidObject(objHandle);
                
                if (!objPtr) {
                    // 对象已被释放或 slot 已被复用
                    // 提供更详细的错误信息
                    std::string detailMsg = "OBJ_STORE_ATTR: object has been freed or reused (stale reference)";
                    auto* rawObj = getObject(objHandle.id);
                    if (!rawObj) {
                        detailMsg += " [object not found: id=" + std::to_string(objHandle.id) + "]";
                    } else if (rawObj->generation != objHandle.generation) {
                        detailMsg += " [generation mismatch: handle.gen=" + std::to_string(objHandle.generation) 
                                   + " obj.gen=" + std::to_string(rawObj->generation) + "]";
                    } else if (rawObj->markedForDeletion) {
                        detailMsg += " [object marked for deletion]";
                    } else if (rawObj->classId < 0) {
                        detailMsg += " [object cleared, classId=" + std::to_string(rawObj->classId) + "]";
                    }
                    VMException::VMException exp(detailMsg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto& obj = *objPtr;
                
                // 首先检查是否有 operator[.=] 重载
                int classId = obj.classId;
                auto* classPtr = getClass(classId);
                if (classPtr) {
                    const auto& ci = *classPtr;
                    auto opIt = ci.operatorMethods.find("[.=]");
                    if (opIt != ci.operatorMethods.end()) {
                        // 有 operator[.=] 重载,调用它
                        int protoId = opIt->second;
                        ZEUS_DEBUG_PRINT("[VM] Calling operator[.=] for attr '%s' (protoId=%d)", attr.c_str(), protoId);
                        
                        // 创建 Closure 包装 protoId
                        Closure clo;
                        clo.protoId = protoId;
                        int cloId = allocClosure(std::move(clo));
                        
                        // 压入: closure, this(对象), 属性名, 值
                        stack.push_back(ZeusType::ClosureHandle{cloId});
                        ZeusGC::retainVariantDeep(*this, objU);
                        stack.push_back(objU);  // this
                        stack.push_back(attr);  // 属性名
                        ZeusGC::retainVariantDeep(*this, val);
                        stack.push_back(val);   // 值
                        
                        // 调用 CALL_CLOSURE,3个参数(this + 属性名 + 值)
                        // Dispatch through a real frame: executeClosureSync pushes a
                        // CallFrame and runs it to completion, so the callee gets its own
                        // local slots. A recursive execute() would share this frame's local
                        // buffer and leave the callee's slots behind in it.
                        if (!executeClosureSync(cloId, 3)) {
                            if (vmstat != ZeusType::VMStatus::Running) break;
                            VMException::VMException exp(
                                "[execute] nested dispatch failed", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        
                        // 方法调用完成,继续执行
                        break;
                    }
                }
                
                // 没有operator[.=]重载,使用默认行为
                auto fit = obj.flags.find(attr);
                if (fit != obj.flags.end() && (fit->second & 0x1)) {
                    VMException::VMException exp("OBJ_STORE_ATTR write to const field '" + attr + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 如果旧值是对象或闭包，减少引用计数
                auto oldIt = obj.values.find(attr);
                if (oldIt != obj.values.end()) {
                    ZeusGC::releaseVariantDeep(*this, oldIt->second);
                }

                // 新值进入对象字段：对象持有一份引用
                ZeusGC::retainVariantDeep(*this, val);

                obj.values[attr] = val;
                printStack();
                break;
            }

            case VMOperator::CLASS_STATIC_METHOD_CALL: {
                if (instr.args.size() < 2 || !std::holds_alternative<std::string>(instr.args[0]) || !std::holds_alternative<LexerType::Value>(instr.args[1]) || !std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(instr.args[1]).data)) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL bad args", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string method = std::get<std::string>(instr.args[0]);
                int argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));

                if (stack.size() < (size_t)argc + 1) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL stack not enough", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                size_t calleeIndex = stack.size() - (argc + 1);
                auto& callee = stack[calleeIndex];
                if (!std::holds_alternative<ZeusType::ClassHandle>(callee)) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL callee not class handle", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int classId = std::get<ZeusType::ClassHandle>(callee).id;
                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL invalid class id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto it = classPtr->staticMethods.find(method);
                if (it == classPtr->staticMethods.end()) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL unknown static method '" + method + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int protoId = it->second;
                if (!getPrototype(protoId)) {
                    VMException::VMException exp("CLASS_STATIC_METHOD_CALL invalid protoId", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 将类的静态方法包装成一个临时 closure 并压入栈
                // 注意：该 closure 没有捕获 upvalue（纯静态方法）
                Closure clo;
                clo.protoId = protoId;
                clo.autoRelease = true;

                int cloId = allocClosure(std::move(clo));

                stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});

                // 直接用 CALL_CLOSURE 指令调用新创建的 closure（相当于 execute(CALL_CLOSURE ...)）
                // Dispatch through a real frame so the callee gets its own local
                // slots instead of sharing this frame's local buffer.
                if (!executeClosureSync(cloId, argc)) {
                    if (vmstat != ZeusType::VMStatus::Running) break;
                    VMException::VMException exp(
                        "[execute] nested dispatch failed", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                break;
            }


            case VMOperator::OBJ_CALL_METHOD: {
                // args: [ methodName (string), argc (Value int) ]
                if (instr.args.size() < 2 || !std::holds_alternative<std::string>(instr.args[0]) || !std::holds_alternative<LexerType::Value>(instr.args[1]) || !std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(instr.args[1]).data)) {
                    VMException::VMException exp("OBJ_CALL_METHOD bad args", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string method = std::get<std::string>(instr.args[0]);
                int argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));

                if (stack.size() < (size_t)argc + 1) {
                    VMException::VMException exp("OBJ_CALL_METHOD stack not enough", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // callee is at position stack.size() - (argc + 1)
                size_t calleeIndex = stack.size() - (argc + 1);
                auto& callee = stack[calleeIndex];

                // Support static-style call on ClassHandle as well: ClassName.method(...)
                if (std::holds_alternative<ZeusType::ClassHandle>(callee)) {
                    int classId = std::get<ZeusType::ClassHandle>(callee).id;
                    auto* classPtr = getClass(classId);
                    if (!classPtr) {
                        VMException::VMException exp("OBJ_CALL_METHOD invalid class id", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto itStatic = classPtr->staticMethods.find(method);
                    if (itStatic == classPtr->staticMethods.end()) {
                        VMException::VMException exp("OBJ_CALL_METHOD unknown static method '" + method + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    int protoId = itStatic->second;
                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("OBJ_CALL_METHOD invalid protoId", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                     // Create closure for proto and replace callee with closure handle. Static methods do NOT receive implicit this.
                     Closure clo;
                     clo.protoId = protoId;
                     clo.autoRelease = true;
                     int cloId = allocClosure(std::move(clo));
                     stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});
 
                     // Reuse CALL_CLOSURE with same argc
                     // Dispatch through a real frame so the callee gets its own local
                     // slots instead of sharing this frame's local buffer.
                     if (!executeClosureSync(cloId, argc)) {
                         if (vmstat != ZeusType::VMStatus::Running) break;
                         VMException::VMException exp(
                             "[execute] nested dispatch failed", __line, instr);
                         exp._throw(vmstat, currentChunk);
                         break;
                     }
                     break;
                 }

                // 处理 SharedObjectHandle：从共享堆读取对象并调用方法
                if (std::holds_alternative<ZeusType::SharedObjectHandle>(callee)) {
                    std::string sharedName = std::get<ZeusType::SharedObjectHandle>(callee).name;
                    
                    auto cell = ZeusShared::getGlobalHeap().find(sharedName);
                    if (!cell) {
                        VMException::VMException exp("SharedObjectHandle: undefined shared object '" + sharedName + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // 读取共享对象数据
                    ZeusType::UniversalVariant sharedVal = cell->read();
                    
                    // 检查是否是 Variant 中的 Object
                    if (!std::holds_alternative<ZeusType::Variant>(sharedVal)) {
                        VMException::VMException exp("SharedObjectHandle: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& variantVal = std::get<ZeusType::Variant>(sharedVal);
                    if (!std::holds_alternative<ZeusType::Object>(variantVal)) {
                        VMException::VMException exp("SharedObjectHandle: shared variable is not an object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& obj = std::get<ZeusType::Object>(variantVal);
                    if (!obj.impl) {
                        VMException::VMException exp("SharedObjectHandle: shared object has null impl", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // 获取 instanceMethods（存储为 Variant 中的 Object）
                    auto methodsIt = obj.impl->data.find("__instanceMethods__");
                    if (methodsIt == obj.impl->data.end()) {
                        VMException::VMException exp("SharedObjectHandle: object has no instanceMethods", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // instanceMethods 是 Variant 包装的 Object
                    if (!std::holds_alternative<ZeusType::Variant>(methodsIt->second)) {
                        VMException::VMException exp("SharedObjectHandle: instanceMethods is not a Variant", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& methodsVariant = std::get<ZeusType::Variant>(methodsIt->second);
                    if (!std::holds_alternative<ZeusType::Object>(methodsVariant)) {
                        VMException::VMException exp("SharedObjectHandle: instanceMethods is not an Object", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& methodsObj = std::get<ZeusType::Object>(methodsVariant);
                    if (!methodsObj.impl) {
                        VMException::VMException exp("SharedObjectHandle: instanceMethods has null impl", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto protoIt = methodsObj.impl->data.find(method);
                    if (protoIt == methodsObj.impl->data.end()) {
                        VMException::VMException exp("SharedObjectHandle: unknown method '" + method + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // 获取 protoId
                    int protoId = -1;
                    if (std::holds_alternative<LexerType::Value>(protoIt->second)) {
                        protoId = VMHelpers::getIntFromValue(std::get<LexerType::Value>(protoIt->second));
                    }
                    
                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("SharedObjectHandle: invalid protoId for method '" + method + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    // 创建闭包
                    Closure clo;
                    clo.protoId = protoId;
                    clo.autoRelease = true;
                    int cloId = allocClosure(std::move(clo));
                    
                    // 替换 callee 为闭包，实例方法需要 this 作为第一个参数
                    // 当前栈布局: [..., SharedObjectHandle(at calleeIndex), arg0, arg1, ...]
                    // 需要变成: [..., ClosureHandle, SharedObjectHandle(作为this), arg0, arg1, ...]
                    
                    // 把 SharedObjectHandle 作为 this 参数插入
                    stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});
                    
                    // 在参数列表开头插入 this (SharedObjectHandle)
                    // 需要将 this 插入到 args 之前
                    // 栈变为: [closure, this, arg0, arg1, ...]
                    stack.insert(stack.begin() + calleeIndex + 1, ZeusType::SharedObjectHandle{sharedName});
                    
                    // 调用闭包，argc + 1 因为添加了 this
                    // Dispatch through a real frame so the callee gets its own local
                    // slots instead of sharing this frame's local buffer.
                    if (!executeClosureSync(cloId, argc + 1)) {
                        if (vmstat != ZeusType::VMStatus::Running) break;
                        VMException::VMException exp(
                            "[execute] nested dispatch failed", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    break;
                }

                if (!std::holds_alternative<ZeusType::ObjectHandle>(callee)) {
                    std::string msg = "Cannot call method '" + method + "' on a non-object value";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 获取 ObjectHandle 并验证 generation
                ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(callee);
                auto* objPtr = getValidObject(objHandle);
                
                if (!objPtr) {
                    // 对象已被释放或 slot 已被复用
                    std::string msg = "OBJ_CALL_METHOD: object has been freed or reused (stale reference) when calling method '" + method + "'";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int classId = objPtr->classId;
                auto* classPtr = getClass(classId);
                if (!classPtr) {
                    std::string msg = "Object has invalid class when calling method '" + method + "'";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto it = classPtr->instanceMethods.find(method);
                
                if (it == classPtr->instanceMethods.end()) {
                    // 方法不存在于 instanceMethods 中
                    // 首先检查对象的实例变量中是否有该名称的可调用值
                    auto valIt = objPtr->values.find(method);
                    if (valIt != objPtr->values.end()) {
                        // 找到了实例变量，检查它是否是可调用的
                        const auto& fieldValue = valIt->second;
                        bool isCallable = std::holds_alternative<ZeusType::ClosureHandle>(fieldValue) ||
                                         std::holds_alternative<ZeusType::NativeFuncHandle>(fieldValue);
                        
                        if (isCallable) {
                            // 这是一个存储在实例变量中的函数，调用它
                            // 当前栈布局: [..., object(在calleeIndex), arg0, arg1, ..., argN-1]
                            // 需要变成: [..., function, arg0, arg1, ..., argN-1]
                            
                            // 替换 callee 位置的 object 为函数
                            stack[calleeIndex] = fieldValue;
                            
                            // 根据函数类型使用不同的调用指令
                            if (std::holds_alternative<ZeusType::ClosureHandle>(fieldValue)) {
                                // 使用 CALL_CLOSURE 调用闭包
                                IRType::IRInstr callIns{ VMOperator::CALL_CLOSURE, { LexerType::Value(argc) } };
                                callIns.line = instr.line;  // 继承原指令的行号
                                execute(callIns);
                            } else {
                                // 使用 CALL_NATIVE 调用原生函数
                                IRType::IRInstr callIns{ VMOperator::CALL_NATIVE, { LexerType::Value(argc) } };
                                callIns.line = instr.line;  // 继承原指令的行号
                                execute(callIns);
                            }
                            break;
                        }
                    }
                    
                    // 检查是否有 operator[.] 重载
                    auto opIt = classPtr->operatorMethods.find("[.]");
                    
                    if (opIt != classPtr->operatorMethods.end()) {
                        // 通过 operator[.] 获取成员再调用
                        // 当前栈布局: [..., object(在calleeIndex), arg0, arg1, ..., argN-1]
                        int opProtoId = opIt->second;
                        
                        vmMessage("Found operator[.]! protoId=" + std::to_string(opProtoId) + ", getPrototype=" + (getPrototype(opProtoId) ? "valid" : "null") + ")");
                        
                        if (getPrototype(opProtoId)) {
                            // 步骤1: 保存原始参数到临时数组
                            std::vector<ZeusType::UniversalVariant> savedArgs;
                            savedArgs.reserve(argc);
                            
                            for (int i = 0; i < argc; ++i) {
                                savedArgs.push_back(stack.back());
                                stack.pop_back();
                            }
                            
                            // 现在栈顶是object,将其弹出
                            stack.pop_back();
                            
                            // 步骤2: 同步调用 operator[.](methodName)
                            // 压入调用所需的栈: [closure, this(object), methodName]
                            Closure opClo;
                            opClo.protoId = opProtoId;
                            int opCloId = allocClosure(std::move(opClo));
                            
                            stack.push_back(ZeusType::ClosureHandle{opCloId});
                            stack.push_back(objHandle);  // 使用完整的 ObjectHandle
                            stack.push_back(LexerType::Value(method));
                            
                            vmMessage("Calling operator[.] SYNC with method='" + method + "'");
                            
                            // 使用同步调用，等待 operator[.] 返回
                            if (!executeClosureSync(opCloId, 2)) {
                                if (vmstat != ZeusType::VMStatus::Running) break;
                                VMException::VMException exp("operator[.] sync call failed", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                            // temporary opClo was used only for this sync call, release it now
                            releaseClosure(opCloId);
                            
                            // 步骤3: operator[.] 返回值在栈顶,这应该是要调用的函数
                            if (stack.empty()) {
                                VMException::VMException exp("operator[.] returned nothing", __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                            
                            auto retrievedMember = stack.back();
                            stack.pop_back();
                            
                            vmMessage("operator[.] returned: " + ZeusVMUtils::valueToDebugString(retrievedMember));
                            
                            // 步骤4: 设置调用目标函数的栈布局
                            // 需要: [function, arg0, arg1, ..., argN-1]
                            stack.push_back(retrievedMember);
                            
                            // 恢复参数(注意savedArgs是反序的)
                            for (int i = (int)savedArgs.size() - 1; i >= 0; --i) {
                                stack.push_back(savedArgs[i]);
                            }
                            
                            // 步骤5: 调用函数（使用栈帧机制）
                            IRType::IRInstr callFuncInstr{VMOperator::CALL_CLOSURE, {LexerType::Value(argc)}};
                            execute(callFuncInstr);
                            break;
                        }
                    }
                    
                    // 没有 operator[.] 或调用失败,报错
                    std::string className = classPtr->name;
                    std::string msg = "Object of type '" + className + "' has no method '" + method + "'";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int protoId = it->second;
                if (!getPrototype(protoId)) {
                    VMException::VMException exp("OBJ_CALL_METHOD invalid protoId", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // We will call method with implicit 'this' via Closure::boundThis.
                // This avoids inserting an extra arg manually and keeps ownership symmetric:
                // - closure owns boundThis (retained)
                // - stack drops its callee object reference when overwritten
                ZeusType::UniversalVariant oldCallee = callee;
                uint8_t oldOwner = stack.ownerFlag(calleeIndex);

                Closure clo;
                clo.protoId = protoId;
                clo.autoRelease = true;
                retainObject(objHandle.id); // closure will own & later release boundThis
                clo.boundThis = objHandle;

                int cloId = allocClosure(std::move(clo));

                // Replace callee (object handle) with closure handle
                stack.set(calleeIndex, ZeusType::ClosureHandle{cloId});

                // Drop the overwritten callee slot (stack-owned object ref)
                dropStackItem(StackItem{oldCallee, oldOwner});

                // Reuse CALL_CLOSURE with original argc; CALL_CLOSURE will inject boundThis.
                // Dispatch through a real frame so the callee gets its own local
                // slots instead of sharing this frame's local buffer.
                if (!executeClosureSync(cloId, argc)) {
                    if (vmstat != ZeusType::VMStatus::Running) break;
                    VMException::VMException exp(
                        "[execute] nested dispatch failed", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                break;
            }


            case VMOperator::PUSH: {
                    // IR 中用 Value("__NIL__") 表示 Zeus 的 Null
                    if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                        const auto &val = std::get<LexerType::Value>(instr.args[0]);
                        if (std::holds_alternative<std::string>(val.data) && std::get<std::string>(val.data) == "__NIL__") {
                            stack.push_back(ZeusType::Variant(ZeusType::Null()));
                        } else {
                            stack.push_back(val);
                        }
                    } else if (!instr.args.empty() && std::holds_alternative<ZeusType::Bytes>(instr.args[0])) {
                        // Bytes 字面量：包装到 Variant 后推入栈
                        ZeusType::Variant bytesVar = std::get<ZeusType::Bytes>(instr.args[0]);
                        stack.push_back(bytesVar);
                    } else {
                        // 直接将原始 Value 推入栈
                        stack.push_back(std::get<LexerType::Value>(instr.args[0]));
                    }
                printStack();
                break;
            }
            
            case VMOperator::PUSH_NAMED: {
                // PUSH_NAMED: 将栈顶的值包装成 NamedArg(命名参数)
                // args[0] = 参数名(string)
                if (stack.empty()) {
                    VMException::VMException exp("PUSH_NAMED: stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                std::string paramName;
                if (!instr.args.empty() && std::holds_alternative<std::string>(instr.args[0])) {
                    paramName = std::get<std::string>(instr.args[0]);
                } else {
                    VMException::VMException exp("PUSH_NAMED: missing parameter name", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                // 从栈顶取值
                auto value = stack.back();
                stack.pop_back();
                
                // 创建 NamedArg 包装
                auto namedArg = std::make_shared<ZeusType::NamedArg>();
                namedArg->name = paramName;
                namedArg->value = value;
                
                // 将包装后的命名参数推回栈(占用一个栈位置)
                stack.push_back(namedArg);
                
                ZEUS_DEBUG_PRINT("[VM] PUSH_NAMED: wrapped '%s'", paramName.c_str());
                printStack();
                break;
            }


            case VMOperator::ADD: {
                // 1. 类型/数量检查
                
                if (stack.size() < 2) {
                    VMException::VMException exp("ADD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右操作数和左操作数（右操作数在栈顶）
                // P1：move 替代拷贝（string/Array 等大 value 省一次深拷贝+引用计数）
                ZeusType::UniversalVariant right = std::move(stack.back());
                stack.pop_back();

                ZeusType::UniversalVariant left  = std::move(stack.back());
                stack.pop_back();

                // P0 fast path: int + int（覆盖 x=x+1 与 for 计数器自增）
                // 跳过 operator重载 / Bytes / convertToValue / getStringType 全套慢路径
                {
                    int64_t li = 0, ri = 0;

                    if (ZeusVMUtils::tryGetInt64(left, li) && ZeusVMUtils::tryGetInt64(right, ri)) {
                        bool overflow = false;

                        if (ri > 0) overflow = (li > INT64_MAX - ri);
                        else if (ri < 0) overflow = (li < INT64_MIN - ri);

                        if (!overflow) {
                            stack.push_back(LexerType::Value(li + ri));
                            break;
                        }
                        // 溢出回退慢路径（大整数加法，极少发生）
                    }
                }

                // 2.5 检查左操作数是否为对象，且是否有operator[+]重载
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[+]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                // 找到operator[+]重载，调用它
                                int protoId = opIt->second;
                                ZEUS_DEBUG_PRINT("[VM] Calling operator[+] overload (protoId=%d) for object %d", protoId, objId);
                                
                                // 创建临时闭包
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                
                                // 栈布局: [closure, this(left), arg(right)]
                                // 对于 func operator[+] (other) [&this]:
                                //   - local 0 = this (来自捕获列表,但作为参数传递)
                                //   - local 1 = other (显式参数)
                                stack.push_back(ZeusType::ClosureHandle{cloId}); // callee
                                stack.push_back(left);  // this -> local 0
                                stack.push_back(right); // other -> local 1
                                
                                // 调用: argc=2 (this + other)
                                // Dispatch through a real frame so the callee gets its own local
                                // slots instead of sharing this frame's local buffer.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                // 结果已在栈顶，直接break
                                vmMessage(">>> Operator[+] overload called.");
                                break;
                            }
                        }
                    }
                }

                // 3. 特殊处理: Bytes + Bytes
                // Bytes可能以两种形式存在：直接Bytes或Variant包装的Bytes
                const ZeusType::Bytes* leftBytes = nullptr;
                const ZeusType::Bytes* rightBytes = nullptr;
                
                // 尝试直接获取Bytes
                if (std::holds_alternative<ZeusType::Bytes>(left)) {
                    leftBytes = &std::get<ZeusType::Bytes>(left);
                } else if (std::holds_alternative<ZeusType::Variant>(left)) {
                    const auto& leftVar = std::get<ZeusType::Variant>(left);
                    if (std::holds_alternative<ZeusType::Bytes>(leftVar)) {
                        leftBytes = &std::get<ZeusType::Bytes>(leftVar);
                    }
                }
                
                if (std::holds_alternative<ZeusType::Bytes>(right)) {
                    rightBytes = &std::get<ZeusType::Bytes>(right);
                } else if (std::holds_alternative<ZeusType::Variant>(right)) {
                    const auto& rightVar = std::get<ZeusType::Variant>(right);
                    if (std::holds_alternative<ZeusType::Bytes>(rightVar)) {
                        rightBytes = &std::get<ZeusType::Bytes>(rightVar);
                    }
                }
                
                if (leftBytes && rightBytes) {
                    // 合并两个bytes
                    ZeusType::Bytes result(leftBytes->data);
                    result.data.insert(result.data.end(), rightBytes->data.begin(), rightBytes->data.end());
                    
                    stack.push_back(result);
                    vmMessage(">>> Exec add (bytes).");
                    printStack();
                    break;
                }

                // 4. 没有重载，使用默认实现：转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);

                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("ADD operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 根据类型判断是数字加法还是字符串拼接
                bool numPair = ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv);
                bool strPair = (lv.getStringType() == "string" && rv.getStringType() == "string");
                
                if (!numPair && !strPair) {
                    VMException::VMException exp("ADD expects numeric+numeric or string+string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 推入计算结果
                stack.push_back(lv + rv);
                vmMessage(">>> Exec add.");
                printStack();
                break;
            }

            case VMOperator::LIST_BUILD: {
                // LIST_BUILD 指令的第一个参数 args[0] 为元素数量（int Value）
                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("LIST_BUILD missing elem count", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                int n = 0;
                const auto &val = std::get<LexerType::Value>(instr.args[0]);
                if (std::holds_alternative<ZeusType::BigInteger>(val.data)) n = VMHelpers::getIntFromValue(val);
                // 检查栈中是否有足够的元素
                if (n > 0 && stack.size() < (size_t)n) {
                    VMException::VMException exp("LIST_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                ZeusType::UniversalVariant arr = ZeusVMUtils::makeArray(n, stack.data);
                stack.push_back(arr);
                printStack();
                break;
            }

            case VMOperator::DICT_BUILD: {
                // DICT_BUILD 的第一个参数 args[0] 为键值对数量（pairCount，int）
                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("DICT_BUILD missing pair count", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                int pairCount = 0;
                const auto &pcVal = std::get<LexerType::Value>(instr.args[0]);
                if (std::holds_alternative<ZeusType::BigInteger>(pcVal.data)) pairCount = VMHelpers::getIntFromValue(pcVal);
                if (pairCount < 0) {
                    VMException::VMException exp("DICT_BUILD negative pair count", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                size_t need = (size_t)pairCount * 2;
                if (stack.size() < need) {
                    VMException::VMException exp("DICT_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 更新调试计数（初始键值对数）
                debugDictInitialPairs += (size_t)pairCount;
                // 从栈中弹出 2N 个元素，顺序为: valN, keyN, ..., val1, key1
                std::vector<ZeusType::UniversalVariant> tmp;
                tmp.reserve(need);
                for (size_t i = 0; i < need; ++i) {
                    tmp.push_back(stack.back());
                    stack.pop_back();
                }
                // tmp: [valN, keyN, ..., val1, key1]
                auto objData = std::make_shared<ZeusType::ObjectData>();
                for (int i = 0; i < pairCount; ++i) {
                    // tmp 中的排列: [valN, keyN, ..., val1, key1]
                    // 计算第 i 对应的 key 和 value 在 tmp 中的位置
                    // key_i 在 tmp 的索引为 need - 1 - (i * 2)
                    // value_i 在 tmp 的索引为 need - 2 - (i * 2)
                    auto &keyVar = tmp[need - 1 - (i * 2)];
                    auto &valVar = tmp[need - 2 - (i * 2)];

                    const std::string keyStr = dictionaryKeyString(keyVar);
                    auto it = (*objData).data.find(keyStr);
                    if (it == (*objData).data.end()) {
                        (*objData).order.push_back(keyStr); // 记录键的插入顺序
                        (*objData).data[keyStr] = valVar;
                    } else {
                        it->second = valVar; // 覆盖已有键的值
                    }
                }
                ZeusType::Object objWrap; objWrap.impl = objData;
                ZeusType::UniversalVariant uv = objWrap;
                stack.push_back(uv);
                printStack();
                break;
            }

            case VMOperator::DICT_KEYS_BUILD: {
                if (stack.empty()) {
                    VMException::VMException exp("DICT_KEYS_BUILD stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                StackItem topItem = popStackItem();
                ZeusType::UniversalVariant top = topItem.value;
                // expect a dictionary/object
                ZeusType::Array arr; arr.impl = std::make_shared<ZeusType::ArrayData>();
                if (std::holds_alternative<ZeusType::Variant>(top)) {
                    const ZeusType::Variant &var = std::get<ZeusType::Variant>(top);
                    if (std::holds_alternative<ZeusType::Object>(var)) {
                        const ZeusType::Object &obj = std::get<ZeusType::Object>(var);
                        if (obj.impl) {
                            for (auto const &k : obj.impl->order) {
                                arr.impl->data.push_back(LexerType::Value(k));
                            }
                        }
                    }
                }
                dropStackItem(topItem);
                ZeusType::Variant v = arr;
                stack.push_back(ZeusType::UniversalVariant(v));
                printStack();
                break;
            }

            case VMOperator::DICT_LOAD_VALUE: {
                if (stack.size() < 2) {
                    VMException::VMException exp("DICT_LOAD_VALUE stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                StackItem keyItem = popStackItem();
                StackItem dictItem = popStackItem();
                ZeusType::UniversalVariant keyVar = keyItem.value;
                ZeusType::UniversalVariant dictVar = dictItem.value;

                struct DropInputs {
                    VM* vm;
                    StackItem* a;
                    StackItem* b;
                    ~DropInputs() {
                        if (vm && a) vm->dropStackItem(*a);
                        if (vm && b) vm->dropStackItem(*b);
                    }
                } dropInputs{this, &keyItem, &dictItem};
                 
                // default missing value -> nil (ZeusType::Null)
                ZeusType::UniversalVariant valueOut = ZeusType::Null();

                // 处理下标/键访问（数组或对象）
                if (std::holds_alternative<ZeusType::Variant>(dictVar)) {
                    const ZeusType::Variant &dv = std::get<ZeusType::Variant>(dictVar);
                    if (std::holds_alternative<ZeusType::Array>(dv)) {
                        // 数组访问：使用整数索引
                        int idx = 0;
                        bool idxIsInt = false;
                        if (std::holds_alternative<LexerType::Value>(keyVar)) {
                            const auto &iv = std::get<LexerType::Value>(keyVar);
                            if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) {
                                idx = VMHelpers::getIntFromValue(iv);
                                idxIsInt = true;
                            }
                        }
                        if (!idxIsInt) {
                            VMException::VMException exp("TypeError: array index must be integer", __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        
                        // 进行边界检查
                        const ZeusType::Array &arr = std::get<ZeusType::Array>(dv);
                        if (!arr.impl || idx < 0 || (size_t)idx >= arr.impl->data.size()) {
                            std::string msg = "Array index out of bounds: index=" + std::to_string(idx) + 
                                            ", size=" + std::to_string(arr.impl ? arr.impl->data.size() : 0);
                            VMException::VMException exp(msg, __line, instr);
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                        
                        valueOut = ZeusVMUtils::arrayRead(dictVar, idx);
                    }
                    else if (std::holds_alternative<ZeusType::Object>(dv)) {
                        // 对象访问：使用字符串键
                        const std::string keyStr = dictionaryKeyString(keyVar);
                            const ZeusType::Object &obj = std::get<ZeusType::Object>(dv);
                            if (obj.impl) {
                                auto it = obj.impl->data.find(keyStr);
                                if (it != obj.impl->data.end()) {
                                    valueOut = it->second;
                                } else {
                                    // Missing key -> raise an error instead of returning nil
                                    std::string msg = "KeyError: missing key '" + keyStr + "'";
                                    VMException::VMException exp(msg, __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                            } else {
                                // Empty object (no impl) -> missing key
                                std::string msg = "KeyError: missing key '" + keyStr + "'";
                                VMException::VMException exp(msg, __line, instr);
                                exp._throw(vmstat, currentChunk);
                                break;
                            }
                    }
                } else if (std::holds_alternative<ZeusType::ObjectHandle>(dictVar)) {
                    // 类实例：尝试调用 operator[[]]
                    ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(dictVar);
                    auto* objPtr = getValidObject(objHandle);
                    if (!objPtr) {
                        VMException::VMException exp("DICT_LOAD_VALUE: object has been freed or reused (stale reference)", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    int classId = objPtr->classId;
                    auto* classPtr = getClass(classId);
                    if (!classPtr) {
                        VMException::VMException exp("DICT_LOAD_VALUE invalid class id", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& classInfo = *classPtr;
                    auto opIt = classInfo.operatorMethods.find("[[]]");
                    if (opIt == classInfo.operatorMethods.end()) {
                        VMException::VMException exp("TypeError: class '" + classInfo.name + "' does not support operator[[]]", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    int protoId = opIt->second;
                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("DICT_LOAD_VALUE invalid operator[[]] protoId", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                     // 调用 operator[[]](key)
                     // 栈布局: [ closure, this, key ]
                     Closure clo;
                     clo.protoId = protoId;
                     clo.autoRelease = true;
                     int cloId = allocClosure(std::move(clo));
                    
                    stack.push_back(ZeusType::ClosureHandle{cloId});  // callee
                    stack.push_back(objHandle);                       // this (完整 ObjectHandle)
                    stack.push_back(keyVar);                          // key argument
                    
                     // 调用 CALL_CLOSURE (argc=2: this + key)
                     // Dispatch through a real frame: executeClosureSync pushes a
                     // CallFrame and runs it to completion, so the callee gets its own
                     // local slots. A recursive execute() would share this frame's local
                     // buffer and leave the callee's slots behind in it.
                     if (!executeClosureSync(cloId, 2)) {
                         if (vmstat != ZeusType::VMStatus::Running) break;
                         VMException::VMException exp(
                             "[execute] nested dispatch failed", __line, instr);
                         exp._throw(vmstat, currentChunk);
                         break;
                     }
                     break;  // 结果已在栈顶
                 } else {
                    // dictVar 不是 Variant 也不是 ObjectHandle，生成更友好的类型错误信息
                    std::string msg = "TypeError: attempted subscript on non-container";
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                ZeusGC::retainVariantDeep(*this, valueOut);
                stack.push_back(valueOut);
                printStack();
                break;
            }

            case VMOperator::DICT_STORE_VALUE: {
                if (stack.size() < 3) {
                    VMException::VMException exp("DICT_STORE_VALUE stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                debugDictStoreCount++;
                ZeusType::UniversalVariant newVal = stack.back(); stack.pop_back();
                ZeusType::UniversalVariant keyVar = stack.back(); stack.pop_back();
                ZeusType::UniversalVariant dictVar = stack.back(); stack.pop_back();
                
                if (std::holds_alternative<ZeusType::Variant>(dictVar)) {
                    ZeusType::Variant &dv = std::get<ZeusType::Variant>(dictVar);
                    
                    if (std::holds_alternative<ZeusType::Array>(dv)) {
                        // 数组写入：使用整数索引
                        int idx = 0;
                        if (std::holds_alternative<LexerType::Value>(keyVar)) {
                            const auto &iv = std::get<LexerType::Value>(keyVar);
                            if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) idx = VMHelpers::getIntFromValue(iv);
                        }
                        ZeusVMUtils::arrayWrite(dictVar, idx, newVal);
                    }
                    else if (std::holds_alternative<ZeusType::Object>(dv)) {
                        // 对象写入：使用字符串键
                        const std::string keyStr = dictionaryKeyString(keyVar);
                        ZeusType::Object &obj = std::get<ZeusType::Object>(dv);
                        if (obj.impl) {
                            auto it2 = obj.impl->data.find(keyStr);
                            ZeusGC::retainVariantDeep(*this, newVal);
                            if (it2 == obj.impl->data.end()) {
                                obj.impl->order.push_back(keyStr);
                                obj.impl->data[keyStr] = newVal;
                            } else {
                                ZeusGC::releaseVariantDeep(*this, it2->second);
                                it2->second = newVal;
                            }
                        }
                    }
                } else if (std::holds_alternative<ZeusType::ObjectHandle>(dictVar)) {
                    // 类实例：尝试调用 operator[[]=]
                    ZeusType::ObjectHandle objHandle = std::get<ZeusType::ObjectHandle>(dictVar);
                    auto* objPtr = getValidObject(objHandle);
                    if (!objPtr) {
                        VMException::VMException exp("DICT_STORE_VALUE: object has been freed or reused (stale reference)", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    int classId = objPtr->classId;
                    auto* classPtr = getClass(classId);
                    if (!classPtr) {
                        VMException::VMException exp("DICT_STORE_VALUE invalid class id", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    auto& classInfo = *classPtr;
                    auto opIt = classInfo.operatorMethods.find("[[]=]");
                    if (opIt == classInfo.operatorMethods.end()) {
                        VMException::VMException exp("TypeError: class '" + classInfo.name + "' does not support operator[[]=]", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                    int protoId = opIt->second;
                    if (!getPrototype(protoId)) {
                        VMException::VMException exp("DICT_STORE_VALUE invalid operator[[]=] protoId", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                    
                     // 调用 operator[[]=](key, value)
                     // 栈布局: [ closure, this, key, value ]
                     Closure clo;
                     clo.protoId = protoId;
                     clo.autoRelease = true;
                     int cloId = allocClosure(std::move(clo));
                    
                    stack.push_back(ZeusType::ClosureHandle{cloId});  // callee
                    stack.push_back(objHandle);                       // this (完整 ObjectHandle)
                    stack.push_back(keyVar);                          // key argument
                    stack.push_back(newVal);                          // value argument
                    
                     // 调用 CALL_CLOSURE (argc=3: this + key + value)
                     // Dispatch through a real frame: executeClosureSync pushes a
                     // CallFrame and runs it to completion, so the callee gets its own
                     // local slots. A recursive execute() would share this frame's local
                     // buffer and leave the callee's slots behind in it.
                     if (!executeClosureSync(cloId, 3)) {
                         if (vmstat != ZeusType::VMStatus::Running) break;
                         VMException::VMException exp(
                             "[execute] nested dispatch failed", __line, instr);
                         exp._throw(vmstat, currentChunk);
                         break;
                     }
                     
                     // operator[[]=] 通常返回 nil，但我们保持与数组/字典一致，将新值压回栈
                     stack.pop_back();  // 移除 operator[[]=] 的返回值
                 }
                // 恢复栈状态：将新值压回栈（与 LIST_STORE_ELEM 保持一致的行为）
                stack.push_back(newVal);
                printStack();
                break;
            }

            case VMOperator::LIST_LENGTH: {
                if (stack.empty()) {
                    VMException::VMException exp("LIST_LENGTH stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant top = stack.back();
                stack.pop_back();

                int lengthValue = (int)ZeusVMUtils::arrayLength(top);
                stack.push_back(LexerType::Value(lengthValue));
                printStack();
                break;
            }

            case VMOperator::LIST_LOAD_ELEM: {
                if (stack.size() < 2) {
                    VMException::VMException exp("LIST_LOAD_ELEM stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant indexVar = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant listVar = stack.back();
                stack.pop_back();

                int idx = 0;
                if (std::holds_alternative<LexerType::Value>(indexVar)) {
                    const auto &iv = std::get<LexerType::Value>(indexVar);
                    if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) idx = VMHelpers::getIntFromValue(iv);
                }
                ZeusType::UniversalVariant elem = ZeusVMUtils::arrayRead(listVar, idx);
                stack.push_back(elem);
                printStack();
                break;
            }

            case VMOperator::LIST_STORE_ELEM: {
                if (stack.size() < 3) {
                    VMException::VMException exp("LIST_STORE_ELEM stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant valueVar = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant indexVar = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant listVar = stack.back();
                stack.pop_back();

                int idx = 0;
                if (std::holds_alternative<LexerType::Value>(indexVar)) {
                    const auto &iv = std::get<LexerType::Value>(indexVar);
                    if (std::holds_alternative<ZeusType::BigInteger>(iv.data)) idx = VMHelpers::getIntFromValue(iv);
                }
                ZeusVMUtils::arrayWrite(listVar, idx, valueVar);
                stack.push_back(valueVar);
                printStack();
                break;
            }

            case VMOperator::COUNT_CHECK_NEG: {
                if (stack.empty()) {
                    VMException::VMException exp("COUNT_CHECK_NEG stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant top = stack.back();
                stack.pop_back();

                bool isNeg = false;

                if (std::holds_alternative<LexerType::Value>(top)) {
                    const auto &val = std::get<LexerType::Value>(top);
                    if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                        const ZeusType::BigInteger& n = std::get<ZeusType::BigInteger>(val.data);
                        if (n < ZeusType::BigInteger(0)) {
                            isNeg = true;
                        }
                        stack.push_back(LexerType::Value(n));
                    } else {
                        stack.push_back(top);
                    }
                } else {
                    stack.push_back(top);
                }

                if (isNeg) {
                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1904_INVALID_OPERATION,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "For loop end value cannot be negative",
                        __line, instr
                    );
                    exp.setDetails("For loop range must have a non-negative end value");
                    exp.setSuggestion("Check the loop range: for (start..end..step) requires end >= 0");
                    exp._throw(vmstat, currentChunk);
                }

                printStack();
                break;
            }

            case VMOperator::COUNT_CHECK_STEP_ZERO: {
                if (stack.empty()) {
                    VMException::VMException exp("COUNT_CHECK_STEP_ZERO stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant top = stack.back();
                stack.pop_back();

                bool isZero = false;

                if (std::holds_alternative<LexerType::Value>(top)) {
                    const auto &val = std::get<LexerType::Value>(top);
                    if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                        const ZeusType::BigInteger& n = std::get<ZeusType::BigInteger>(val.data);
                        if (n == ZeusType::BigInteger(0)) {
                            isZero = true;
                        }
                        stack.push_back(LexerType::Value(n));
                    } else {
                        stack.push_back(top);
                    }
                } else {
                    stack.push_back(top);
                }

                if (isZero) {
                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1904_INVALID_OPERATION,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "For loop step cannot be zero",
                        __line, instr
                    );
                    exp.setDetails("A zero step would cause an infinite loop");
                    exp.setSuggestion("Use a non-zero step value in for (start..end..step)");
                    exp._throw(vmstat, currentChunk);
                }
                
                printStack();
                break;
            }


            case VMOperator::SUBTRACT: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("SUBTRACT stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                // 3. 检查左操作数是否为对象（支持operator[-]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[-]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                ZEUS_DEBUG_PRINT("[VM] Calling operator- overload (protoId=%d) for object %d", protoId, objId);
                                
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator- overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("SUBTRACT operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 验证操作数为数值类型
                if (!(ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv))) {
                    VMException::VMException exp("SUBTRACT operands must be numeric", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 执行减法并推入结果
                stack.push_back(lv - rv);

                vmMessage(">>> Exec subtract.");
                printStack();
                break;
            }



            case VMOperator::MULTIPLY: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("MULTIPLY stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                // 3. 检查左操作数是否为对象（支持operator[*]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[*]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                ZEUS_DEBUG_PRINT("[VM] Calling operator* overload (protoId=%d) for object %d", protoId, objId);
                                
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator* overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("MULTIPLY operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 验证操作数为数值类型
                if (!(ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv))) {
                    VMException::VMException exp("MULTIPLY operands must be numeric", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 执行乘法并推入结果
                stack.push_back(lv * rv);

                vmMessage(">>> Exec multiply.");
                printStack();
                break;
            }



            case VMOperator::DIVIDE: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("DIVIDE stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                // 3. 检查左操作数是否为对象（支持operator[/]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[/]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator/ overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("DIVIDE operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 验证操作数为数值类型
                if (!(ZeusVMUtils::isNumericValue(lv) && ZeusVMUtils::isNumericValue(rv))) {
                    VMException::VMException exp("DIVIDE operands must be numeric", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 6. 检查除数是否为零（避免除零错误）
                ZeusType::BigDecimal rnum(0);
                if (ZeusVMUtils::extractNumber(right, rnum) && rnum == ZeusType::BigDecimal(0)) {
                    VMException::VMException exp("DIVIDE by zero", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 7. 执行除法并推入结果
                stack.push_back(lv / rv);

                vmMessage(">>> Exec divide.");
                printStack();
                break;
            }


            case VMOperator::MODULUS: {
                // 1. 参数和栈长度检查
                if (stack.size() < 2) {
                    VMException::VMException exp("MODULUS stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右、左操作数（右操作数在栈顶）
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left = stack.back();
                stack.pop_back();

                // 3. 检查左操作数是否为对象（支持operator[%]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[%]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator% overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 转换为内部 Value 表示
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!ZeusVMUtils::convertToValue(left, lv) || !ZeusVMUtils::convertToValue(right, rv)) {
                    VMException::VMException exp("MODULUS operands not convertible", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 5. 确认为整数类型（int % int）
                if (!(lv.getStringType() == "int" && rv.getStringType() == "int")) {
                    VMException::VMException exp("MODULUS expects int % int", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                const ZeusType::BigInteger& li = std::get<ZeusType::BigInteger>(lv.data);
                const ZeusType::BigInteger& ri = std::get<ZeusType::BigInteger>(rv.data);

                if (ri == ZeusType::BigInteger(0)) {
                    VMException::VMException exp("MODULUS by zero", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::BigInteger res = li % ri;
                stack.push_back(LexerType::Value(res));
                vmMessage(">>> Exec modulus.");
                printStack();
                break;
            }



            case VMOperator::FUNC_BEGIN: {
                // FUNC_BEGIN 占位（无实际操作）
                break;
            }


            case VMOperator::IR_ANNOTATION: {
                // 语义标注只供 LLVM 等后端读取，动态 VM 不产生任何栈副作用。
                break;
            }



            case VMOperator::FUNC_END: {
                
                break;
            }

            case VMOperator::POP: {
                
                if (!stack.empty()) {
                    dropStackBack();
                }

                printStack();
                break;
            }

            case VMOperator::SWAP: {
                // 交换栈顶两个元素
                if (stack.size() < 2) {
                    VMException::VMException exp("SWAP requires at least 2 elements on stack", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                auto top = stack.back();
                stack.pop_back();
                auto second = stack.back();
                stack.pop_back();
                
                stack.push_back(top);    // 原栈顶到第二位
                stack.push_back(second); // 原第二位到栈顶
                
                printStack();
                break;
            }



            case VMOperator::CALL_FUNC: {

                auto const& funcName = std::get<std::string>(instr.args[0]);
                auto const& argc = std::get<LexerType::Value>(instr.args[1]);

                // printf(">>> Call function: %s %d\n", funcName.c_str(), std::get<int>(argc.data));

                // now its function is replaced by CALL_CLOSURE
                break;
            }



            case VMOperator::MAKE_FUNCTION: {
                // 参数：chunkIndex (Value int)。需要通过 chunkToProtoId 映射到实际的 protoId
                auto const& pidVal = std::get<LexerType::Value>(instr.args[0]);
                int chunkIndex = VMHelpers::getIntFromValue(pidVal);

                int protoId = -1;
                if (chunkIndex >= 0 && (size_t)chunkIndex < chunkToProtoId.size()) {
                    protoId = chunkToProtoId[chunkIndex];
                }

                const Prototype* protoPtr = getPrototype(protoId);
                if (!protoPtr) {
                    ZEUS_DEBUG_PRINT("[VM] MAKE_FUNCTION invalid chunkIndex: %d", chunkIndex);
                    break;
                }

                const Prototype& proto = *protoPtr;

                Closure clos;
                clos.protoId = protoId;
                clos.upvalues.resize(proto.captures.size());

                // 捕获模式 modeFlag: 0 = 复制 (copy), 1 = 引用 (ref)
                for (size_t i = 0; i < proto.captures.size(); ++i) {
                    const auto& cap = proto.captures[i];
                    int src = cap.srcIndex;

                    bool isLocal = (cap.fromKind == "local");
                    if (isLocal) {
                        // 这里处理对 local 变量的捕获：
                        // - 如果 local 数组太短则扩展
                        // - 如果 local[src] 为空则创建一个 Cell 占位
                        // - 解释器在 MAKE_FUNCTION/STORE_VALUE_LOCAL 时会把值分配到 Cell
                        if (src >= 0 && (size_t)src >= local.size()) {
                            local.resize(src + 1, nullptr);
                        }
                        if (src < 0) {
                            VMException::VMException exp("Capture local negative index: " + std::to_string(src), __line, instr);
                            exp._throw(vmstat, currentChunk);
                            continue;
                        }
                        if (!local[src]) {
                            local[src] = newCell(LexerType::Value(0), 0, "CAPTURE_placeholder"); // 占位 Cell（nil/0）
                        }
                        // 现在 local[src] 已可安全使用

                        if (cap.modeFlag == 1) {
                            // ref-mode: store pointer and retain
                            clos.upvalues[cap.dstIndex] = local[src];
                            clos.upvalues[cap.dstIndex]->retain();
                        } else {
                            // copy-mode: snapshot the value now (template), do not point to local
                            clos.upvalues[cap.dstIndex] = cloneCell(local[src], "CAPTURE_copy");
                        }
                    } else {
                        bool inRange = (src >= 0 && (size_t)src < upvalue.size() && upvalue[src]);
                        
                        if (!inRange) {
                            VMException::VMException exp("Capture upvalue out of range: " + std::to_string(src), __line, instr);
                            exp._throw(vmstat, currentChunk);
                            continue;
                        }

                        if (cap.modeFlag == 1) {
                            // ref-mode: share outer upvalue pointer
                            clos.upvalues[cap.dstIndex] = upvalue[src];
                            clos.upvalues[cap.dstIndex]->retain();
                        } else {
                            // copy-mode: snapshot outer upvalue now
                            clos.upvalues[cap.dstIndex] = cloneCell(upvalue[src]);
                        }
                    }
                }

                int handle = allocClosure(std::move(clos));

                    // Debug: dump closure template upvalues (debug channel)
                    {
                        ZEUS_DEBUG_PRINT("[MAKE_FUNCTION] proto=%d clos=%d upvalues=", protoId, handle);
                        for (size_t ui = 0; ui < closures[handle].upvalues.size(); ++ui) {
                            Cell* c = closures[handle].upvalues[ui];
                            ZEUS_DEBUG_PRINT("  slot=%zu ptr=%p", ui, (void*)c);
                        }
                    }

                stack.push_back(ZeusType::ClosureHandle{ handle });
                printStack();
                break;
            }


            case VMOperator::CALL_CLOSURE: {
                // ========== 函数调用入口 ==========
                const int argc = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));

                // 错误处理 helper：在出错时从栈中弹出 callee + argc（可选）并返回占位值
                auto fail = [&](const std::string& msg, bool includeCallee) {
                    VMException::VMException exp(msg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                    int pops = argc + (includeCallee ? 1 : 0);
                    if (pops > 0 && stack.size() >= (size_t)pops) {
                        shrinkStackWithRelease(stack.size() - (size_t)pops);
                    } else {
                        shrinkStackWithRelease(0);
                    }
                    stack.push_back(LexerType::Value(0));
                };

                // 期望栈布局: [..., callee, arg0, arg1, ..., arg{n-1}]
                if (stack.size() < static_cast<size_t>(argc) + 1) {
                    fail("CALL_CLOSURE stack not enough for new layout", false);
                    break;
                }

                const size_t calleeIndex   = stack.size() - (argc + 1);
                const size_t callFrameBase = calleeIndex;

                vmMessage(
                    std::string("[CALL_CLOSURE_ENTER] stackSize=") + std::to_string(stack.size()) +
                    " argc=" + std::to_string(argc) +
                    " calleeIndex=" + std::to_string(calleeIndex)
                );

                ZeusType::UniversalVariant fnVar = stack[calleeIndex];
                
                // DEBUG: 输出 fnVar 的类型
                ZEUS_DEBUG_PRINT("[CALL_CLOSURE] fnVar index=%d, holds_alternative<ClosureHandle>=%d, holds_alternative<Value>=%d",
                    (int)calleeIndex,
                    std::holds_alternative<ZeusType::ClosureHandle>(fnVar),
                    std::holds_alternative<LexerType::Value>(fnVar));

                // late-resolve 尝试：当 callee 是字符串（函数名）时，尝试将其解析为 closure handle
                auto tryLateResolve = [&](ZeusType::UniversalVariant& v) {
                    // 如果已经是 ClosureHandle，直接返回
                    if (std::holds_alternative<ZeusType::ClosureHandle>(v)) {
                        return;
                    }
                    // 仅处理 Value<string> 类型
                    if (!std::holds_alternative<LexerType::Value>(v)) {
                        return;
                    }

                    const auto& vv = std::get<LexerType::Value>(v);
                    if (!std::holds_alternative<std::string>(vv.data)) {
                        return;
                    }

                    // 严格模式下不允许 late resolve
                    if (!enableLateResolve) {
                        VMException::VMException exp("Strict mode: unresolved function name before call", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        return;
                    }

                    const std::string& name = std::get<std::string>(vv.data);

                    // 1) 优先查找 nameToClosure 映射
                    auto itc = nameToClosure.find(name);
                    if (itc != nameToClosure.end()) {
                        int h = itc->second;
                        if (h >= 0 && h < (int)closures.size()) {
                            stack[calleeIndex] = ZeusType::ClosureHandle{ h };
                            v = stack[calleeIndex];
                            return;
                        }
                    }

                    // 2) 回退：遍历 globalNames，对应 global 插槽中查找 closure
                    if (!globalNames.empty()) {
                        for (int gi = 0; gi < (int)globalNames.size(); ++gi) {
                            if (globalNames[gi] == name && gi < (int)global.size() && global[gi]) {
                                auto& gval = global[gi]->value;
                                if (std::holds_alternative<ZeusType::ClosureHandle>(gval)) {
                                    int cid = std::get<ZeusType::ClosureHandle>(gval).id;
                                    if (cid >= 0 && cid < (int)closures.size()) {
                                        nameToClosure[name]   = cid;
                                        stack[calleeIndex]    = ZeusType::ClosureHandle{ cid };
                                        v                     = stack[calleeIndex];
                                    }
                                }
                                break; // 找到匹配后跳出循环
                            }
                        }
                    }
                };

                tryLateResolve(fnVar);

                // 调用前的类型检查：确保 callee 已解析为 closure handle
                if (!std::holds_alternative<ZeusType::ClosureHandle>(fnVar)) {
                    std::string actualType = ZeusVMUtils::valueToDebugString(fnVar);
                    std::string errorMsg = "CALL_CLOSURE: Attempted to call non-function value. ";
                    errorMsg += "Expected a function but got: " + actualType + ". ";
                    errorMsg += "This usually means the variable is nil or not properly initialized as a function.";
                    fail(errorMsg, true);
                    break;
                }

                const int handle = std::get<ZeusType::ClosureHandle>(fnVar).id;
                Closure* closPtr = getClosure(handle);
                if (!closPtr) {
                    fail("CALL_CLOSURE invalid handle", true);
                    break;
                }

                Closure&    clos  = *closPtr;
                const Prototype* protoPtr = getPrototype(clos.protoId);
                if (!protoPtr) {
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] invalid protoId=%d, closureId=%d, closures.size=%zu, sharedClosures=%p, sharedPrototypes=%p",
                        clos.protoId, handle, closures.size(), (void*)sharedClosures, (void*)sharedPrototypes);
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] closure refCount=%d, isShared=%d, upvalues=%zu, boundThis=%d",
                        clos.refCount, (int)isSharedClosure(handle), clos.upvalues.size(), clos.boundThis.has_value() ? 1 : 0);
                    if (sharedClosures) {
                        ZEUS_DEBUG_PRINT("[CALL_CLOSURE] sharedClosures->size=%zu", sharedClosures->size());
                    }
                    if (sharedPrototypes) {
                        ZEUS_DEBUG_PRINT("[CALL_CLOSURE] sharedPrototypes->size=%zu", sharedPrototypes->size());
                    }
                    fail("CALL_CLOSURE invalid protoId", true);
                    break;
                }
                const Prototype&  proto = *protoPtr;

                // 保存调用前的 local 和 upvalue 环境
                bool capturesReady = true;
                for (Cell* captureCell : clos.upvalues) {
                    if (!captureCell) {
                        fail("CALL_CLOSURE: closure has an uninitialized capture cell", true);
                        capturesReady = false;
                        break;
                    }
                }
                if (!capturesReady) break;

                auto oldLocal   = local;
                auto oldUpvalue = upvalue;

                // 清空并初始化新的 local，用于被调用函数（大小为 proto.paramCount），并为每个参数创建占位 Cell
                local.clear();
                local.resize(proto.paramCount);
                for (int i = 0; i < proto.paramCount; ++i) {
                    local[i] = newCell(LexerType::Value(0), 0, "CALL_CLOSURE_local");
                }
                // upvalue 指向 closure 的 upvalues（Cell 指针集合）
                // The closure owns the capture cells. Reference captures share
                // the outer cell; copy captures share the closure's snapshot cell.
                // Both modes use that same cell for each invocation, so a copied
                // capture can keep private mutable state across calls.
                std::vector<Cell*> callUpvalues = clos.upvalues;
                for (auto* cell : callUpvalues) cell->retain();
                upvalue = callUpvalues;
                    // Debug: dump callUpvalues (debug channel)
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] calling proto=%d clos=%d callUpvals=", clos.protoId, handle);
                    for (size_t ui = 0; ui < callUpvalues.size(); ++ui) {
                        ZEUS_DEBUG_PRINT("  slot=%zu ptr=%p", ui, (void*)callUpvalues[ui]);
                    }

                // 如果闭包有绑定的this (来自 OBJ_LOAD_ATTR 获取实例方法), 将其作为第一个参数插入
                // 当前栈: [callee, arg0, arg1, ...]
                // 需要变为: [callee, this, arg0, arg1, ...]
                int actualArgc = argc;
                if (clos.boundThis.has_value()) {
                    ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Closure has boundThis, inserting as first arg");
                    // 在 calleeIndex + 1 位置插入 boundThis
                    retainObject(clos.boundThis->id); // stack owns a reference
                    stack.insert(stack.begin() + (calleeIndex + 1), clos.boundThis.value());
                    actualArgc++;
                }
                    

                // ===== 参数处理：支持默认参数、可变参数和命名参数 =====
                int regularParamCount = proto.paramCount;
                int minRequiredArgs = proto.minRequiredParams >= 0 ? proto.minRequiredParams : regularParamCount;
                
                // 扩展 local 以容纳常规参数 + *args + **kwargs
                if (proto.hasVariadicArgs || proto.hasVariadicKwargs) {
                    int extraSlots = 0;
                    if (proto.hasVariadicArgs) extraSlots++;
                    if (proto.hasVariadicKwargs) extraSlots++;
                    
                    int newLocalSize = regularParamCount + extraSlots;
                    local.resize(newLocalSize);
                    for (int idx = regularParamCount; idx < newLocalSize; ++idx) {
                        local[idx] = newCell(LexerType::Value(0), 0, "CALL_varargs");
                    }
                }
                
                // 扫描栈上的参数,分离位置参数和命名参数
                std::map<std::string, ZeusType::UniversalVariant> namedArgs;
                std::vector<ZeusType::UniversalVariant> positionalArgs;
                
                for (int i = 0; i < actualArgc; ++i) {
                    auto& arg = stack[calleeIndex + 1 + i];
                    if (std::holds_alternative<std::shared_ptr<ZeusType::NamedArg>>(arg)) {
                        // 命名参数
                        auto namedArgPtr = std::get<std::shared_ptr<ZeusType::NamedArg>>(arg);
                        namedArgs[namedArgPtr->name] = namedArgPtr->value;
                        ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Named arg: %s", namedArgPtr->name.c_str());
                    } else {
                        // 位置参数
                        positionalArgs.push_back(arg);
                    }
                }
                
                int positionalCount = (int)positionalArgs.size();
                ZEUS_DEBUG_PRINT("[CALL_CLOSURE] positional=%d, named=%d", positionalCount, (int)namedArgs.size());
                
                // 填充常规参数
                std::set<std::string> consumedNamedArgs; // 记录已使用的命名参数
                
                for (int i = 0; i < regularParamCount; ++i) {
                    bool filled = false;
                    
                    // 优先使用位置参数
                    if (i < positionalCount) {
                        assignCellValue(local[i], positionalArgs[i]);
                        filled = true;
                    }
                    
                    // 尝试从命名参数中按名字匹配
                    if (!filled && i < (int)proto.paramNames.size()) {
                        const std::string& paramName = proto.paramNames[i];
                        auto it = namedArgs.find(paramName);
                        if (it != namedArgs.end()) {
                            assignCellValue(local[i], it->second);
                            consumedNamedArgs.insert(paramName);
                            filled = true;
                            ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Filled param[%d] '%s' from named arg", i, paramName.c_str());
                        }
                    }
                    
                    // 使用默认值
                    if (!filled) {
                        if (i < (int)proto.paramDefaults.size()) {
                            const auto& defVal = proto.paramDefaults[i];
                            if (std::holds_alternative<LexerType::Value>(defVal)) {
                                assignCellValue(local[i], std::get<LexerType::Value>(defVal));
                                filled = true;
                            } else if (ZeusVMUtils::isNull(defVal)) {
                                assignCellValue(local[i], ZeusType::Variant(ZeusType::Null()));
                                filled = true;
                            }
                        }
                    }
                    
                    // 最后用0初始化
                    if (!filled) {
                        assignCellValue(local[i], LexerType::Value(0));
                    }
                }
                
                // 检查必需参数
                if (positionalCount < minRequiredArgs && namedArgs.empty()) {
                    VMException::VMException exp(
                        "CALL_CLOSURE: missing required arguments. Expected at least " + 
                        std::to_string(minRequiredArgs) + " arguments, but got " +
                        std::to_string(positionalCount),
                        __line,
                        instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                // 检查是否传递了过多的参数（仅当没有 *args 时）
                if (!proto.hasVariadicArgs && positionalCount > regularParamCount) {
                    VMException::VMException exp(
                        "CALL_CLOSURE: too many arguments. Expected at most " + 
                        std::to_string(regularParamCount) + " arguments, but got " +
                        std::to_string(positionalCount),
                        __line,
                        instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                // 检查未被消费的命名参数（仅当没有 **kwargs 时）
                if (!proto.hasVariadicKwargs) {
                    for (const auto& kv : namedArgs) {
                        if (consumedNamedArgs.find(kv.first) == consumedNamedArgs.end()) {
                            VMException::VMException exp(
                                "CALL_CLOSURE: unexpected named argument '" + kv.first + "'",
                                __line,
                                instr
                            );
                            exp._throw(vmstat, currentChunk);
                            break;
                        }
                    }
                }
                
                // 构建 *args 数组(收集多余的位置参数)
                if (proto.hasVariadicArgs) {
                    std::vector<ZeusType::UniversalVariant> argsVec;
                    for (int i = regularParamCount; i < positionalCount; ++i) {
                        argsVec.push_back(positionalArgs[i]);
                    }
                    
                    auto arrayDataPtr = std::make_shared<ZeusType::ArrayData>();
                    arrayDataPtr->data = argsVec;
                    ZeusType::Array argsArray;
                    argsArray.impl = arrayDataPtr;
                    
                    ZeusType::Variant variantArgsArray = argsArray;
                    assignCellValue(local[regularParamCount], variantArgsArray);
                }
                
                // 构建 **kwargs 字典(收集未被消费的命名参数)
                if (proto.hasVariadicKwargs) {
                    auto objDataPtr = std::make_shared<ZeusType::ObjectData>();
                    
                    // 只加入未被消费的命名参数
                    for (const auto& kv : namedArgs) {
                        if (consumedNamedArgs.find(kv.first) == consumedNamedArgs.end()) {
                            objDataPtr->data[kv.first] = kv.second;
                            objDataPtr->order.push_back(kv.first);
                            ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Extra named arg '%s' -> kwargs", kv.first.c_str());
                        }
                    }
                    
                    ZeusType::Object kwargsDict;
                    kwargsDict.impl = objDataPtr;
                    
                    ZeusType::Variant variantKwargs = kwargsDict;
                    int kwargsSlot = regularParamCount;
                    if (proto.hasVariadicArgs) kwargsSlot++;
                    assignCellValue(local[kwargsSlot], variantKwargs);
                }

                // ========== 新的栈帧实现：不再使用while循环 ==========
                // 检查调用深度
                if (callFrames.size() >= (size_t)MAX_CALL_DEPTH) {
                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1900_STACK_OVERFLOW,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "Stack overflow: possible infinite recursion",
                        __line, instr
                    );
                    exp.setDetails(
                        std::string("Call depth exceeded ") + std::to_string(MAX_CALL_DEPTH) + " levels\n" +
                        "Current call depth: " + std::to_string(callFrames.size())
                    );
                    exp.setSuggestion(
                        std::string("Check for infinite recursion, especially in:\n") +
                        "  • operator[.] overloads accessing 'this.member' (use getattr instead)\n" +
                        "  • Recursive function calls without proper base case\n" +
                        "  • Circular references in object construction"
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                // 创建新栈帧
                CallFrame newFrame;
                newFrame.code = &proto.body;
                newFrame.pc = 0;
                newFrame.savedLocal = oldLocal;
                newFrame.savedUpvalue = oldUpvalue;
                newFrame.stackBase = callFrameBase;
                newFrame.savedLine = __line;
                
                 // callee closure 的生命周期由栈收缩（RETURN -> shrinkStackWithRelease(stackBase)）统一处理。
                 // 这里不再基于 refCount 推断“临时闭包”，避免与栈/容器 retain 语义冲突。
                 newFrame.calleeAutoReleaseId = -1;
                 callFrames.push_back(newFrame);
                 justPushedFrame = true;  // 标记已创建栈帧
                
                ZEUS_DEBUG_PRINT("[CALL_CLOSURE] Pushed frame (depth=%zu), proto body size=%zu", 
                    callFrames.size(), proto.body.size());
                
                // 不需要while循环了，runChunk会处理新栈帧的执行
                break;
                
                // ========== 以下是旧代码，已被栈帧机制替代 ==========
                #if 0
                while (pc < proto.body.size()) {
                    const auto& ins = proto.body[pc];

                    // ---- 跳过 FUNC_BEGIN / CAPTURE_OUTSIDE_VAR 指令 ----
                    if (ins.op == VMOperator::FUNC_BEGIN || ins.op == VMOperator::CAPTURE_OUTSIDE_VAR) {
                        pc++;
                        continue;
                    }

                    if (ins.op == VMOperator::FUNC_END) {
                        pc++;
                        continue;
                    }

                    // ---- RETURN ----
                    if (ins.op == VMOperator::RETURN) {
                        if (!stack.empty()) {
                            retVal = stack.back();
                            stack.pop_back();
                            hasRet = true;
                        }
                        break;
                    }

                    // ---- 跳转指令处理 ----
                    if (ins.op == VMOperator::JUMP) {
                        int off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                        pc += 1 + off;
                        continue;
                    }

                    if (ins.op == VMOperator::JUMP_IF_FALSE) {
                        int off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                        
                        if (!stack.empty()) {
                            auto cond = stack.back();
                            stack.pop_back();
                            if (!truthy(cond)) {
                                pc += 1 + off;
                                continue;
                            }
                        }
                        pc++;
                        continue;
                    }

                    if (ins.op == VMOperator::JUMP_IF_TRUE) {
                        int off = VMHelpers::getIntFromValue(std::get<LexerType::Value>(ins.args[0]));
                        
                        if (!stack.empty()) {
                            auto cond = stack.back();
                            stack.pop_back();

                            if (truthy(cond)) {
                                pc += 1 + off;
                                continue;
                            }
                        }
                        pc++;
                        continue;
                    }

                    // ---- 执行普通指令 ----
                    execute(ins);
                    pc++;
                }
                #endif
                // ========== 旧的清理代码已移到RETURN处理（runChunk） ==========
                #if 0
                // 恢复调用前的 local/upvalue 状态，并对需要闭合的 local 做闭合处理
                {
                    // 收集当前 local 中的 Cell 指针
                    std::unordered_set<Cell*> localSet;
                    localSet.reserve(local.size());

                    for (Cell* c : local)
                        if (c) localSet.insert(c);

                    // 遍历所有 closures 的 upvalues，若某个 upvalue 指向当前 local 中的 Cell，则需要将其替换为闭包闭合的 Cell
                    std::unordered_map<Cell*, Cell*> closedMap; // oldLocalPtr -> closedCell
                    
                    for (auto& c : closures) {
                        for (auto& uv : c.upvalues) {
                            if (uv && localSet.count(uv)) {
                                Cell* oldPtr = uv;
                                Cell* closed = nullptr;

                                auto it = closedMap.find(oldPtr);
                                if (it == closedMap.end()) {
                                    closed = cloneCell(oldPtr);
                                    closedMap.emplace(oldPtr, closed);
                                } else {
                                    closed = it->second;
                                }
                                // closed 是闭合后的 Cell 指针，增加引用计数
                                closed->retain();
                                // 释放原 upvalue 的引用
                                uv->release();
                                // 用闭合后的 Cell 替换 upvalue
                                uv = closed;
                            }
                        }
                    }

                    // 清理当前函数的 local：释放并置空所有占位 Cell
                    for (auto& cptr : local) {
                        if (cptr) { cptr->release(); cptr = nullptr; }
                    }
                    // release any temporary per-call upvalues we created earlier
                    for (size_t ci = 0; ci < /*callUpvalues size*/ (size_t)0; ++ci) {
                        // no-op placeholder: actual callUpvalues is out of scope here, so we cleanup below
                    }
                }

                // 在缩小栈回到 callFrameBase 时进行校验
                if (stack.size() < callFrameBase + argc + 1) {
                    vmMessage("[CALL_CLOSURE_WARN] unexpected stack shrink before frame cleanup: size=" + std::to_string(stack.size()) +
                        " expected>=" + std::to_string(callFrameBase + argc + 1));
                }
                shrinkStackWithRelease(callFrameBase);

                local = std::move(oldLocal);
                upvalue = std::move(oldUpvalue);

                // 将返回值压回栈（如果有返回值则压入返回值，否则压入 nil）
                stack.push_back(hasRet ? retVal : ZeusType::UniversalVariant(LexerType::Value(0)));
                vmMessage("[CALL_CLOSURE_EXIT] base=" + std::to_string(callFrameBase) + " finalStackSize=" + std::to_string(stack.size()) +
                    " retType=" + (hasRet?retVal.index()==0?"val":"complex":"void"));
                printStack();
                
                // 递归深度恢复
                callDepth--;
                #endif
                break;
            }



            case VMOperator::LOAD_NATIVE: {
                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("LOAD_NATIVE missing id", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                int nid = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));

                // 基本检查
                if (!ZeusBuiltins::get(nid)) {
                    VMException::VMException exp("LOAD_NATIVE invalid id: " + std::to_string(nid), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                stack.push_back(ZeusType::NativeFuncHandle{ nid });
                printStack();
                break;
            }

            case VMOperator::CALL_NATIVE: {
                NativeCallContext ctx;
                if (!prepareNativeCall(instr, ctx)) { break; }
                 if (!validateNativeArity(ctx, instr)) {
                    for (int i = 0; i < ctx.argc + 1; ++i) {
                        if (!stack.empty()) dropStackBack();
                    }
                    stack.push_back(LexerType::Value(0));
                    printStack();
                    break;
                 }
                auto ret = invokeNative(ctx);
                cleanupNativeCall(ctx, ret);
                printStack();
                break;
            }


            case VMOperator::STORE_VALUE_LOCAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                const int __varIndexInt = VMHelpers::getIntFromValue(__varIndex);

                auto const& __mutOrConst = std::get<std::string>(instr.args[1]);

                if (local.size() <= (size_t)__varIndexInt) {
                    local.resize(__varIndexInt + 1, nullptr);
                }

                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;
                
                // local Cell 将持有新值的一份引用
                ZeusGC::retainVariantDeep(*this, newVal);

                if (!local[__varIndexInt]) {
                    local[__varIndexInt] = newCell(newVal, (__mutOrConst == "const") ? 1 : 0, "STORE_LOCAL");
                } else {
                    if (local[__varIndexInt]->flags & 0x1) {
                        VMException::VMException exp("Attempt to modify a const local variable at index " + std::to_string(__varIndexInt), __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& oldVal = local[__varIndexInt]->value;
                    ZeusGC::releaseVariantDeep(*this, oldVal);

                    local[__varIndexInt]->value = newVal;

                    if (__mutOrConst == "const") {
                        local[__varIndexInt]->flags |= 0x1;
                    } else {
                        local[__varIndexInt]->flags &= ~0x1;
                    }
                }

                // 丢弃栈持有的那一份引用（local cell 已 retain 一份）
                dropStackItem(newItem);

                printLocals();
                printStack();
                break;
            }


            case VMOperator::STORE_VALUE_GLOBAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                const int __varIndexInt = VMHelpers::getIntFromValue(__varIndex);
                auto const& __mutOrConst = std::get<std::string>(instr.args[1]);
                
                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: index=%d, mutOrConst='%s', stack.size=%zu", 
                    __varIndexInt, __mutOrConst.c_str(), stack.size());
                
                // 可选 inline 名称（来自 .zir），用于在 globalNames 中记录符号名称以便后续查找
                std::string inlineName;
                if (instr.args.size() > 2 && std::holds_alternative<std::string>(instr.args[2])) {
                    inlineName = std::get<std::string>(instr.args[2]);
                    if (__varIndexInt >= 0) {
                        if (__varIndexInt >= (int)globalNames.size()) globalNames.resize(__varIndexInt+1);
                        if (globalNames[__varIndexInt].empty()) globalNames[__varIndexInt] = inlineName;
                    }
                }
                
                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Getting stack.back()...");
                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value; // 将栈顶值视为要存储的新值（可能包含 closure）
                
                // global Cell 将持有新值的一份引用
                ZeusGC::retainVariantDeep(*this, newVal);
                
                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Resizing global if needed (current size=%zu)...", global.size());
                if (global.size() <= __varIndexInt) {
                    global.resize(__varIndexInt + 1, nullptr);
                }
                
                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Setting global[%d]...", __varIndexInt);
                if (!global[__varIndexInt]) {
                    global[__varIndexInt] = newCell(newVal, (__mutOrConst == "const") ? 1 : 0, "STORE_GLOBAL");
                    ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Created new cell with flags=%d", global[__varIndexInt]->flags);
                } else {
                    ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Cell exists, flags=%d, mutOrConst=%s", 
                        global[__varIndexInt]->flags, __mutOrConst.c_str());

                    if (global[__varIndexInt]->flags & 0x1) {
                        VMException::VMException exp("Attempt to modify a const global variable at index " + std::to_string(__varIndexInt), __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    auto& oldVal = global[__varIndexInt]->value;
                    ZeusGC::releaseVariantDeep(*this, oldVal);

                    global[__varIndexInt]->value = newVal;

                    if (__mutOrConst == "const") {
                        global[__varIndexInt]->flags |= 0x1;
                    } else {
                        global[__varIndexInt]->flags &= ~0x1;
                    }
                }

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Popping stack...");
                // 释放栈临时持有的对象引用（global cell 已 retain 一份）
                dropStackItem(newItem);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: tryRegisterGlobalClosure...");
                // 如果新值是 closure，则尝试在 nameToClosure/register 中注册
                tryRegisterGlobalClosure(__varIndexInt, newVal);

                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: printGlobals...");
                printGlobals();
                
                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: printStack...");
                printStack();
                
                ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_GLOBAL: Done!");
                // TODO: future: 如果在注册 closure 时需要自动记录名称（nameToClosure），
                // 需要考虑名字冲突、作用域以及 late-resolve 的一致性。目前通过
                // tryRegisterGlobalClosure 做部分处理，后续可在这里统一改进。
                break;
            }

            case VMOperator::ASSIGN_VALUE_GLOBAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                int idx = VMHelpers::getIntFromValue(__varIndex);

                // 处理 inline 名称参数（index, mut/const, name?）
                if (instr.args.size() > 2 && std::holds_alternative<std::string>(instr.args[2])) {
                    if (idx >= 0) {
                        if (idx >= (int)globalNames.size()) {
                            globalNames.resize(idx + 1);
                        }
                        if (globalNames[idx].empty()) {
                            globalNames[idx] = std::get<std::string>(instr.args[2]);
                        }
                    }
                }

                if (idx < 0 || idx >= (int)global.size() || !global[idx]) {
                    VMException::VMException exp("ASSIGN global slot invalid: " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (global[idx]->flags & 0x1) {
                    std::string varName = (idx >= 0 && idx < (int)globalNames.size()) 
                        ? globalNames[idx] : ("g[" + std::to_string(idx) + "]");
                    
                    auto exp = VMException::VMException(
                        VMException::ErrorCode::E1301_CONST_ASSIGNMENT,
                        VMException::ErrorCategory::VM_RUNTIME,
                        "Cannot assign to constant '" + varName + "'",
                        __line, instr
                    );
                    exp.setDetails("The variable '" + varName + "' was declared as 'const' and cannot be modified");
                    exp.setSuggestion("If you need to modify this variable, declare it with 'let' instead of 'const'");
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN global stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;

                const auto referenceWrite = writeThroughReference(global[idx], newVal);
                if (referenceWrite != ReferenceWriteResult::NotReference) {
                    if (referenceWrite != ReferenceWriteResult::Stored) {
                        VMException::VMException exp(
                            referenceWrite == ReferenceWriteResult::ConstTarget
                                ? "Cannot assign through a reference to a const cell"
                                : "Cannot assign this value through the reference",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                    }
                    dropStackItem(newItem);
                    printGlobals();
                    printStack();
                    break;
                }

                auto& oldVal = global[idx]->value;
                ZeusGC::releaseVariantDeep(*this, oldVal);
                ZeusGC::retainVariantDeep(*this, newVal);

                global[idx]->value = newVal;

                // TODO: 如果可能，应在 nameToClosure 中记录名称（当新值为 closure 时），以便 later-resolve
                tryRegisterGlobalClosure(idx, newVal);
                // 释放栈临时持有的对象引用（slot 已 retain）
                dropStackItem(newItem);

                printGlobals();
                printStack();
                // TODO: 需要将名称注册到 nameToClosure（当新值为 closure 时），以便 later-resolve
                break;
            }

            case VMOperator::ASSIGN_VALUE_LOCAL: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                int idx = VMHelpers::getIntFromValue(__varIndex);

                if (idx < 0 || idx >= (int)local.size() || !local[idx]) {
                    VMException::VMException exp("ASSIGN local slot invalid: " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (local[idx]->flags & 0x1) {
                    VMException::VMException exp("Assign to const local index " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN local stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;

                const auto referenceWrite = writeThroughReference(local[idx], newVal);
                if (referenceWrite != ReferenceWriteResult::NotReference) {
                    if (referenceWrite != ReferenceWriteResult::Stored) {
                        VMException::VMException exp(
                            referenceWrite == ReferenceWriteResult::ConstTarget
                                ? "Cannot assign through a reference to a const cell"
                                : "Cannot assign this value through the reference",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                    }
                    dropStackItem(newItem);
                    printLocals();
                    printStack();
                    break;
                }

                auto& oldVal = local[idx]->value;
                ZeusGC::releaseVariantDeep(*this, oldVal);
                ZeusGC::retainVariantDeep(*this, newVal);

                local[idx]->value = newVal;
                // 释放栈临时持有的对象引用（slot 已 retain）
                dropStackItem(newItem);
                printLocals();
                printStack();
                break;
            }

            case VMOperator::ASSIGN_VALUE_UPVALUE: {
                auto const& __varIndex = std::get<LexerType::Value>(instr.args[0]);
                int idx = VMHelpers::getIntFromValue(__varIndex);

                if (idx < 0 || idx >= (int)upvalue.size() || !upvalue[idx]) {
                    VMException::VMException exp("ASSIGN upvalue slot invalid: " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (upvalue[idx]->flags & 0x1) {
                    VMException::VMException exp("Assign to const upvalue index " + std::to_string(idx), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN upvalue stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                StackItem newItem = popStackItem();
                ZeusType::UniversalVariant newVal = newItem.value;

                const auto referenceWrite = writeThroughReference(upvalue[idx], newVal);
                if (referenceWrite != ReferenceWriteResult::NotReference) {
                    if (referenceWrite != ReferenceWriteResult::Stored) {
                        VMException::VMException exp(
                            referenceWrite == ReferenceWriteResult::ConstTarget
                                ? "Cannot assign through a reference to a const cell"
                                : "Cannot assign this value through the reference",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                    }
                    dropStackItem(newItem);
                    printVarStack(2);
                    printStack();
                    break;
                }

                auto& oldVal = upvalue[idx]->value;
                ZeusGC::releaseVariantDeep(*this, oldVal);
                ZeusGC::retainVariantDeep(*this, newVal);

                upvalue[idx]->value = newVal;
                // 释放栈临时持有的对象引用（slot 已 retain）
                dropStackItem(newItem);
                printVarStack(2);
                printStack();
                break;
            }

            // ===== 共享堆操作 =====

            case VMOperator::STORE_VALUE_SHARED: {
                // STORE_VALUE_SHARED "varName" "mut/const"
                if (instr.args.size() < 2) {
                    VMException::VMException exp("STORE_VALUE_SHARED missing arguments", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                std::string mutOrConst = std::get<std::string>(instr.args[1]);

                if (stack.empty()) {
                    VMException::VMException exp("STORE_VALUE_SHARED stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant newVal = stack.back();
                stack.pop_back();

                // 特殊处理 ObjectHandle：将对象序列化存储到共享堆
                if (std::holds_alternative<ZeusType::ObjectHandle>(newVal)) {
                    int oid = std::get<ZeusType::ObjectHandle>(newVal).id;
                    
                    auto* objPtr = getObject(oid);
                    if (objPtr) {
                        auto& obj = *objPtr;
                        
                        // 构造一个 Object 存储到共享堆
                        // 使用 ZeusType::Object 包装 ObjectData
                        ZeusType::Object sharedObj;
                        sharedObj.impl = std::make_shared<ZeusType::ObjectData>();
                        auto& sharedObjData = *sharedObj.impl;
                        
                        // 存储类信息
                        auto* classPtr = getClass(obj.classId);
                        if (classPtr) {
                            sharedObjData.data["__className__"] = LexerType::Value(classPtr->name);
                            
                            // 存储 instanceMethods（protoId 映射）- 作为嵌套 Object
                            ZeusType::Object methodsObj;
                            methodsObj.impl = std::make_shared<ZeusType::ObjectData>();
                            for (const auto& kv : classPtr->instanceMethods) {
                                methodsObj.impl->data[kv.first] = LexerType::Value((long long)kv.second);
                                methodsObj.impl->order.push_back(kv.first);
                            }
                            // 将 Object 包装为 Variant 存储
                            sharedObjData.data["__instanceMethods__"] = ZeusType::Variant(methodsObj);
                            
                            // 存储别名
                            sharedObjData.data["__alias__"] = LexerType::Value(classPtr->usingThisAlias);
                        }
                        sharedObjData.order.push_back("__className__");
                        sharedObjData.order.push_back("__instanceMethods__");
                        sharedObjData.order.push_back("__alias__");
                        
                        // 复制所有字段值
                        for (const auto& kv : obj.values) {
                            sharedObjData.data[kv.first] = kv.second;
                            sharedObjData.order.push_back(kv.first);
                        }
                        
                        // 存储到共享堆（作为 Variant 包装的 Object）
                        ZeusType::Variant variantObj = sharedObj;
                        auto cell = ZeusShared::getGlobalHeap().getOrCreate(varName, variantObj);
                        cell->write(variantObj);
                        
                        ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_SHARED: serialized ObjectHandle to shared heap, name='%s'", 
                            varName.c_str());
                    }
                } else {
                    // 其他类型直接存储
                    auto cell = ZeusShared::getGlobalHeap().getOrCreate(varName, newVal);
                    cell->write(newVal);
                    
                    ZEUS_DEBUG_PRINT("[VM] STORE_VALUE_SHARED: name='%s', mutOrConst='%s'", 
                        varName.c_str(), mutOrConst.c_str());
                }

                printStack();
                break;
            }

            case VMOperator::LOAD_VALUE_SHARED: {
                // LOAD_VALUE_SHARED "varName"
                if (instr.args.empty()) {
                    VMException::VMException exp("LOAD_VALUE_SHARED missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);

                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("LOAD_VALUE_SHARED: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 读取值（内部自动加读锁）
                ZeusType::UniversalVariant val = cell->read();
                
                // 检查是否是序列化的对象（Variant 中的 Object 带 __className__）
                if (std::holds_alternative<ZeusType::Variant>(val)) {
                    auto& variantVal = std::get<ZeusType::Variant>(val);
                    
                    if (std::holds_alternative<ZeusType::Object>(variantVal)) {
                        auto& obj = std::get<ZeusType::Object>(variantVal);
                        
                        if (obj.impl && obj.impl->data.find("__className__") != obj.impl->data.end()) {
                            // 这是一个共享对象，返回 SharedObjectHandle
                            stack.push_back(ZeusType::SharedObjectHandle{varName});
                            
                            ZEUS_DEBUG_PRINT("[VM] LOAD_VALUE_SHARED: name='%s' (shared object)", varName.c_str());
                            printStack();
                            break;
                        }
                    }
                }
                
                stack.push_back(val);

                ZEUS_DEBUG_PRINT("[VM] LOAD_VALUE_SHARED: name='%s'", varName.c_str());

                printStack();
                break;
            }

            case VMOperator::ASSIGN_VALUE_SHARED: {
                // ASSIGN_VALUE_SHARED "varName"
                if (instr.args.empty()) {
                    VMException::VMException exp("ASSIGN_VALUE_SHARED missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);

                if (stack.empty()) {
                    VMException::VMException exp("ASSIGN_VALUE_SHARED stack empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("ASSIGN_VALUE_SHARED: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                ZeusType::UniversalVariant newVal = stack.back();
                stack.pop_back();

                // 写入值（内部自动加写锁）
                cell->write(newVal);

                ZEUS_DEBUG_PRINT("[VM] ASSIGN_VALUE_SHARED: name='%s'", varName.c_str());

                printStack();
                break;
            }

            case VMOperator::SHARED_LOCK: {
                // SHARED_LOCK "varName" - 获取共享变量的写锁
                if (instr.args.empty()) {
                    VMException::VMException exp("SHARED_LOCK missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("SHARED_LOCK: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                cell->lockWrite();

                ZEUS_DEBUG_PRINT("[VM] SHARED_LOCK: name='%s' locked", varName.c_str());
                break;
            }

            case VMOperator::SHARED_UNLOCK: {
                // SHARED_UNLOCK "varName" - 释放共享变量的写锁
                if (instr.args.empty()) {
                    VMException::VMException exp("SHARED_UNLOCK missing varName", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                auto cell = ZeusShared::getGlobalHeap().find(varName);

                if (!cell) {
                    VMException::VMException exp("SHARED_UNLOCK: undefined shared variable '" + varName + "'", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                cell->unlockWrite();

                ZEUS_DEBUG_PRINT("[VM] SHARED_UNLOCK: name='%s' unlocked", varName.c_str());
                break;
            }


            case VMOperator::LOAD_CELL_REFERENCE: {
                if (instr.args.size() < 2 || !std::holds_alternative<std::string>(instr.args[0]) ||
                    !std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    VMException::VMException exp("LOAD_CELL_REFERENCE requires a scope and index", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                const auto& scope = std::get<std::string>(instr.args[0]);
                const int index = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));
                if (scope != "global") {
                    VMException::VMException exp(
                        "reference return requires stable global storage; local and upvalue references cannot escape",
                        __line, instr
                    );
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                if (index < 0 || index >= static_cast<int>(global.size()) || !global[index]) {
                    VMException::VMException exp("LOAD_CELL_REFERENCE global slot is invalid", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                stack.push_back(ZeusType::ReferenceHandle{global[index], "cell"});
                printStack();
                break;
            }

            case VMOperator::LOAD_VALUE: {
                // Support multiple IR forms:
                // 1) [ string name ] -> resolve & load variable (late-resolve / global name lookup)
                // 2) [ LexerType::Value literal ] -> push literal value (for late-resolve class names etc.)
                // 3) [ string scope, LexerType::Value index, (optional string name) ] -> load cell
                
                // Form (2): single LexerType::Value argument - push as literal
                if (instr.args.size() == 1 && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    stack.push_back(std::get<LexerType::Value>(instr.args[0]));
                    printStack();
                    break;
                }
                
                // Form (1): single string argument - resolve variable name
                if (instr.args.size() == 1 && std::holds_alternative<std::string>(instr.args[0])) {
                    // form (1) - resolve variable name to its value (top-level/global expected)
                    std::string name = std::get<std::string>(instr.args[0]);
                    bool found = false;

                    if (!globalNames.empty()) {
                        // First try: exact match
                        for (int gi = 0; gi < (int)globalNames.size(); ++gi) {
                            if (globalNames[gi] == name) {
                                if (gi < (int)global.size() && global[gi]) {
                                    pushLoadedCellValue(global[gi]);
                                    found = true;
                                }
                                break;
                            }
                        }
                        
                        // Second try: match with package prefix (for module-internal references)
                        // e.g., "Bytes" can match "ZeusBuiltins.socket.Bytes"
                        if (!found) {
                            for (int gi = 0; gi < (int)globalNames.size(); ++gi) {
                                const std::string& fullName = globalNames[gi];

                                // Check if fullName ends with ".name"
                                size_t dotPos = fullName.rfind('.');

                                if (dotPos != std::string::npos) {
                                    std::string suffix = fullName.substr(dotPos + 1);

                                    if (suffix == name) {
                                        if (gi < (int)global.size() && global[gi]) {
                                            pushLoadedCellValue(global[gi]);
                                            found = true;
                                            vmMessage(std::string("[LOAD_VALUE] Resolved '") + name + "' to '" + fullName + "'");
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (!found) {
                        VMException::VMException exp(std::string("NameError: undefined variable '") + name + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }

                    printStack();
                    break;
                }

                // Form (3): scope + index format
                auto const& kind = std::get<std::string>(instr.args[0]);
                auto const& indexVal = std::get<LexerType::Value>(instr.args[1]);
                int index = VMHelpers::getIntFromValue(indexVal);

                if (kind == "local") {
                    if (index >= 0 && index < (int)local.size() && local[index]) {
                        pushLoadedCellValue(local[index]);
                    } else {
                        std::string varname = "<unknown>";
                        // 尝试从args[2]获取变量名
                        if (instr.args.size() >= 3 && std::holds_alternative<std::string>(instr.args[2])) {
                            varname = std::get<std::string>(instr.args[2]);
                        }
                        // local变量没有globalNames映射，保持<unknown>
                        VMException::VMException exp(
                            std::string("NameError: undefined variable '") + varname + "'",
                            __line, instr
                        );
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (kind == "global") {
                    if (index >= 0 && index < (int)global.size() && global[index]) {
                        auto& loadedVal = global[index]->value;
                        // DEBUG: 检查 global[46] 的值
                        if (index == 46) {
                            std::string valType = "unknown";
                            if (std::holds_alternative<ZeusType::ClosureHandle>(loadedVal)) {
                                valType = "ClosureHandle(" + std::to_string(std::get<ZeusType::ClosureHandle>(loadedVal).id) + ")";
                            } else if (std::holds_alternative<LexerType::Value>(loadedVal)) {
                                valType = "Value: " + ZeusVMUtils::valueToDebugString(loadedVal);
                            } else {
                                valType = "OtherType";
                            }
                            ZEUS_DEBUG_PRINT("[LOAD_VALUE global 46] Cell=%p, refCount=%d, value=%s", 
                                (void*)global[index], global[index]->refCount, valType.c_str());
                        }
                        pushLoadedCellValue(global[index]);
                    } else {
                        std::string varname = "<unknown>";
                        // 尝试从args[2]获取变量名
                        if (instr.args.size() >= 3 && std::holds_alternative<std::string>(instr.args[2])) {
                            varname = std::get<std::string>(instr.args[2]);
                        } 
                        // 如果args[2]不存在，尝试从globalNames查找
                        else if (index >= 0 && index < (int)globalNames.size()) {
                            varname = globalNames[index];
                        }
                        VMException::VMException exp(std::string("NameError: undefined variable '") + varname + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else if (kind == "upvalue") {
                    if (index >= 0 && index < (int)upvalue.size() && upvalue[index]) {
                        pushLoadedCellValue(upvalue[index]);
                    } else {
                        std::string varname = "<unknown>";
                        // 尝试从args[2]获取变量名
                        if (instr.args.size() >= 3 && std::holds_alternative<std::string>(instr.args[2])) {
                            varname = std::get<std::string>(instr.args[2]);
                        }
                        // upvalue变量没有globalNames映射，保持<unknown>
                        VMException::VMException exp(std::string("NameError: undefined variable '") + varname + "'", __line, instr);
                        exp._throw(vmstat, currentChunk);
                        break;
                    }
                } else {
                    // 直接把 kind（字面量或标识）压入栈
                    stack.push_back(kind);
                }

                printStack();
                break;
            }

            case VMOperator::JUMP: {
                // 单个 execute 中不处理 PC，PC 的整体控制由 runChunk 负责
                break;
            }
            case VMOperator::JUMP_IF_TRUE: {
                break;
            }
            case VMOperator::JUMP_IF_FALSE: {
                break;
            }

            case VMOperator::EQUL: {
                // 1. 检查栈中是否有足够的元素
                if (stack.size() < 2) {
                    VMException::VMException exp("EQUL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右操作数和左操作数
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                // 2.5 处理 nil 比较: nil == nil 为 true，nil == 非nil 为 false
                bool leftIsNull = ZeusVMUtils::isNull(left);
                bool rightIsNull = ZeusVMUtils::isNull(right);
                
                if (leftIsNull || rightIsNull) {
                    bool result = (leftIsNull && rightIsNull);
                    stack.push_back(LexerType::Value(result));
                    printStack();
                    break;
                }

                // 3. 检查左操作数是否为对象（支持operator[==]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[==]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator== overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 处理闭包和原生函数句柄的相等性比较
                if (std::holds_alternative<ZeusType::ClosureHandle>(left) &&
                    std::holds_alternative<ZeusType::ClosureHandle>(right)) {
                    bool r = std::get<ZeusType::ClosureHandle>(left).id ==
                              std::get<ZeusType::ClosureHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }
                if (std::holds_alternative<ZeusType::NativeFuncHandle>(left) &&
                    std::holds_alternative<ZeusType::NativeFuncHandle>(right)) {
                    bool r = std::get<ZeusType::NativeFuncHandle>(left).id ==
                              std::get<ZeusType::NativeFuncHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }

                // 5. 将任意值转换为内部可比较的 LexerType::Value（如果可能）
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!(ZeusVMUtils::convertToValue(left, lv) && ZeusVMUtils::convertToValue(right, rv))) {
                    // 无法转换为可比较的值或类型不一致 -> 视为不相等
                    stack.push_back(LexerType::Value(false));
                    printStack();
                    break;
                }

                // 6. 根据类型选择比较策略（数值/字符串/布尔等）
                std::string lt = lv.getStringType();
                std::string rt = rv.getStringType();

                auto isNum = [](const std::string &s) {
                    return s == "int" || s == "float";
                };

                bool result = false;
                if (isNum(lt) && isNum(rt)) {
                    ZeusType::BigDecimal lf = (lt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(lv.data))
                                              : std::get<ZeusType::BigDecimal>(lv.data);
                    ZeusType::BigDecimal rf = (rt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(rv.data))
                                              : std::get<ZeusType::BigDecimal>(rv.data);
                    result = (lf == rf);
                } else if (lt == rt) {
                    if (lt == "string")       result = std::get<std::string>(lv.data) == std::get<std::string>(rv.data);
                    else if (lt == "bool")    result = std::get<bool>(lv.data)        == std::get<bool>(rv.data);
                    else if (lt == "int")     result = std::get<ZeusType::BigInteger>(lv.data) == std::get<ZeusType::BigInteger>(rv.data);
                    else if (lt == "float")   result = std::get<ZeusType::BigDecimal>(lv.data)       == std::get<ZeusType::BigDecimal>(rv.data);
                }

                // 7. 推入比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }

            case VMOperator::NOT_EQUAL: {
                // 1. 前置检查：栈元素数量是否足够
                if (stack.size() < 2) {
                    VMException::VMException exp("NOT_EQUAL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右操作数和左操作数
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                // 2.5 处理 nil 比较: nil != nil 为 false，nil != 非nil 为 true
                bool leftIsNull = ZeusVMUtils::isNull(left);
                bool rightIsNull = ZeusVMUtils::isNull(right);
                
                if (leftIsNull || rightIsNull) {
                    bool result = !(leftIsNull && rightIsNull);
                    stack.push_back(LexerType::Value(result));
                    printStack();
                    break;
                }

                // 3. 检查左操作数是否为对象（支持operator[!=]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[!=]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator!= overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 处理闭包/原生函数句柄的特殊不等比较
                if (std::holds_alternative<ZeusType::ClosureHandle>(left) &&
                    std::holds_alternative<ZeusType::ClosureHandle>(right)) {
                    bool r = std::get<ZeusType::ClosureHandle>(left).id !=
                              std::get<ZeusType::ClosureHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }
                if (std::holds_alternative<ZeusType::NativeFuncHandle>(left) &&
                    std::holds_alternative<ZeusType::NativeFuncHandle>(right)) {
                    bool r = std::get<ZeusType::NativeFuncHandle>(left).id !=
                              std::get<ZeusType::NativeFuncHandle>(right).id;
                    stack.push_back(LexerType::Value(r));
                    printStack();
                    break;
                }

                // 5. 尝试将值转换为内部可比较形式
                LexerType::Value lv(0);
                LexerType::Value rv(0);
                if (!(ZeusVMUtils::convertToValue(left, lv) && ZeusVMUtils::convertToValue(right, rv))) {
                    // 无法转换为可比较值 -> 视为不相等
                    stack.push_back(LexerType::Value(true));
                    printStack();
                    break;
                }

                // 6. 根据类型决定比较逻辑
                std::string lt = lv.getStringType();
                std::string rt = rv.getStringType();
                auto isNum = [](const std::string &s) { return s == "int" || s == "float"; };

                bool result = true; // 默认认为不相等
                if (isNum(lt) && isNum(rt)) {
                    ZeusType::BigDecimal lf = (lt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(lv.data))
                                              : std::get<ZeusType::BigDecimal>(lv.data);
                    ZeusType::BigDecimal rf = (rt == "int") ? ZeusType::BigDecimal(std::get<ZeusType::BigInteger>(rv.data))
                                              : std::get<ZeusType::BigDecimal>(rv.data);
                    result = (lf != rf);
                } else if (lt == rt) {
                    if (lt == "string")       result = std::get<std::string>(lv.data) != std::get<std::string>(rv.data);
                    else if (lt == "bool")    result = std::get<bool>(lv.data)        != std::get<bool>(rv.data);
                    else if (lt == "int")     result = std::get<ZeusType::BigInteger>(lv.data) != std::get<ZeusType::BigInteger>(rv.data);
                    else if (lt == "float")   result = std::get<ZeusType::BigDecimal>(lv.data)       != std::get<ZeusType::BigDecimal>(rv.data);
                    else                       result = true;
                } else {
                    result = true; // 不同类型 -> 视为不相等
                }

                // 7. 推入不等比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }

            case VMOperator::GREATER_THAN: {
                // 1. 前置检查：栈元素数量是否足够
                if (stack.size() < 2) {
                    VMException::VMException exp("GREATER_THAN stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右操作数和左操作数
                ZeusType::UniversalVariant right = stack.back();
                stack.pop_back();

                ZeusType::UniversalVariant left  = stack.back();
                stack.pop_back();

                // 3. 检查左操作数是否为对象（支持operator[>]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[>]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator> overload called.");
                                break;
                            }
                        }
                    }
                }

                // 4. 尝试提取数值（用于数值比较）
                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okLeft  = ZeusVMUtils::extractNumber(left,  a);
                bool okRight = ZeusVMUtils::extractNumber(right, b);

                // 5. 如果两侧均为数值，则进行数值比较
                bool result = false;
                if (okLeft && okRight) {
                    result = (a > b);
                }

                // 6. 推入比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }

            case VMOperator::LESS_THAN: {
                // 1. 检查栈是否至少有两个值
                if (stack.size() < 2) {
                    VMException::VMException exp("LESS_THAN stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }

                // 2. 弹出右操作数（P1：move 替代拷贝）
                ZeusType::UniversalVariant right = std::move(stack.back());
                stack.pop_back();

                // 3. 弹出左操作数
                ZeusType::UniversalVariant left  = std::move(stack.back());
                stack.pop_back();

                // P0 fast path: int < int（覆盖 for 循环条件）
                // 跳过 BigDecimal(0) 构造 + extractNumber + BigDecimal 比较全套
                {
                    int64_t li = 0, ri = 0;

                    if (ZeusVMUtils::tryGetInt64(left, li) && ZeusVMUtils::tryGetInt64(right, ri)) {
                        stack.push_back(LexerType::Value(li < ri));
                        printStack();
                        break;
                    }
                }

                // 4. 检查左操作数是否为对象（支持operator[<]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[<]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator< overload called.");
                                break;
                            }
                        }
                    }
                }

                // 5. 尝试将两侧转换为数值并比较
                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okLeft  = ZeusVMUtils::extractNumber(left,  a);
                bool okRight = ZeusVMUtils::extractNumber(right, b);

                bool result = false;
                if (okLeft && okRight) {
                    result = (a < b);
                }

                // 6. 推入比较结果
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }

            case VMOperator::GREATER_EQUAL: {
                if (stack.size() < 2) {
                    VMException::VMException exp("GREATER_EQUAL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                auto right = stack.back();
                stack.pop_back();
                auto left  = stack.back();
                stack.pop_back();

                // 检查左操作数是否为对象（支持operator[>=]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[>=]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator>= overload called.");
                                break;
                            }
                        }
                    }
                }

                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okL = ZeusVMUtils::extractNumber(left,  a);
                bool okR = ZeusVMUtils::extractNumber(right, b);

                bool res = false;
                if (okL && okR) {
                    res = (a >= b);
                }
                stack.push_back(LexerType::Value(res));
                printStack();
                break;
            }

            case VMOperator::LESS_EQUAL: {
                if (stack.size() < 2) {
                    VMException::VMException exp("LESS_EQUAL stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                auto right = stack.back();
                stack.pop_back();
                auto left  = stack.back();
                stack.pop_back();

                // 检查左操作数是否为对象（支持operator[<=]重载）
                if (std::holds_alternative<ZeusType::ObjectHandle>(left)) {
                    int objId = std::get<ZeusType::ObjectHandle>(left).id;
                    auto* objPtr = getObject(objId);
                    if (objPtr) {
                        int classId = objPtr->classId;
                        auto* classPtr = getClass(classId);
                        if (classPtr) {
                            auto& classInfo = *classPtr;
                            auto opIt = classInfo.operatorMethods.find("[<=]");
                            if (opIt != classInfo.operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                stack.push_back(left);
                                stack.push_back(right);
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its own
                                // local slots. A recursive execute() would share this frame's local
                                // buffer and leave the callee's slots behind in it.
                                if (!executeClosureSync(cloId, 2)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "[execute] nested dispatch failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                vmMessage(">>> Operator<= overload called.");
                                break;
                            }
                        }
                    }
                }

                ZeusType::BigDecimal a(0);
                ZeusType::BigDecimal b(0);
                bool okL = ZeusVMUtils::extractNumber(left,  a);
                bool okR = ZeusVMUtils::extractNumber(right, b);

                bool res = false;
                if (okL && okR) {
                    res = (a <= b);
                }
                stack.push_back(LexerType::Value(res));
                printStack();
                break;
            }

            case VMOperator::LOGICAL_AND: {
                // 逻辑与运算: left and right
                if (stack.size() < 2) {
                    VMException::VMException exp("LOGICAL_AND stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                auto right = stack.back();
                stack.pop_back();
                auto left = stack.back();
                stack.pop_back();
                
                // 将左右操作数转换为布尔值
                bool leftBool = ZeusVMUtils::toBool(left);
                bool rightBool = ZeusVMUtils::toBool(right);
                
                bool result = leftBool && rightBool;
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }

            case VMOperator::LOGICAL_OR: {
                // 逻辑或运算: left or right
                if (stack.size() < 2) {
                    VMException::VMException exp("LOGICAL_OR stack underflow", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                auto right = stack.back();
                stack.pop_back();
                auto left = stack.back();
                stack.pop_back();
                
                // 将左右操作数转换为布尔值
                bool leftBool = ZeusVMUtils::toBool(left);
                bool rightBool = ZeusVMUtils::toBool(right);
                
                bool result = leftBool || rightBool;
                stack.push_back(LexerType::Value(result));
                printStack();
                break;
            }


            case VMOperator::IMPORT: {
                // IMPORT: 导入模块
                // 参数: [packageName(Value string)]
                
                if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    VMException::VMException exp("IMPORT requires package name", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                const auto& val = std::get<LexerType::Value>(instr.args[0]);
                
                if (!std::holds_alternative<std::string>(val.data)) {
                    VMException::VMException exp("IMPORT package name must be string", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                std::string packageName = std::get<std::string>(val.data);
                
                vmMessage("[IMPORT] Loading package: " + packageName);
                
                // TODO: 实现实际的包加载逻辑
                // 1. 使用IRImport::loadPackage获取包文件路径
                // 2. 编译包文件为IR
                // 3. 执行包的IR代码
                // 4. 注册包的导出符号到命名空间
                
                vmMessage("[IMPORT] Package loaded (placeholder): " + packageName);
                
                printStack();
                break;
            }


            case VMOperator::DELETE_VAR: {
                // DELETE_VAR: 删除变量，释放其 Cell 对象
                // 参数: [string varName, string kind, Value index]
                // kind: "local", "upvalue", "global"
                // 如果对象有 operator[~]，会在释放前调用它
                
                if (instr.args.empty() || !std::holds_alternative<std::string>(instr.args[0])) {
                    vmMessage("[DELETE_VAR] Error: Missing variable name");
                    break;
                }

                std::string varName = std::get<std::string>(instr.args[0]);
                std::string kind = "global";
                int varIndex = -1;
                
                // 解析类型和索引参数
                if (instr.args.size() >= 2 && std::holds_alternative<std::string>(instr.args[1])) {
                    kind = std::get<std::string>(instr.args[1]);
                }
                
                if (instr.args.size() >= 3 && std::holds_alternative<LexerType::Value>(instr.args[2])) {
                    varIndex = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[2]));
                }
                
                ZEUS_DEBUG_PRINT("[DELETE_VAR] Deleting %s variable: %s (index=%d)", kind.c_str(), varName.c_str(), varIndex);
                
                // 调用 operator[~] 的辅助 lambda
                auto callDestructor = [&](Cell* cell) {
                    
                    if (!cell) return;
                    
                    auto& cellVal = cell->value;
                    
                    if (!std::holds_alternative<ZeusType::ObjectHandle>(cellVal)) return;
                    
                    ZeusType::ObjectHandle objH = std::get<ZeusType::ObjectHandle>(cellVal);
                    
                    // 验证 generation，确保对象没有被复用
                    auto* objPtr = getValidObject(objH);
                    
                    if (!objPtr) {
                        ZEUS_DEBUG_PRINT("[DELETE_VAR] Skipping destructor: object freed or reused");
                        return;
                    }
                    
                    int classId = objPtr->classId;
                    auto* classPtr = getClass(classId);
                    
                    if (!classPtr) return;
                    
                    auto opIt = classPtr->operatorMethods.find("[~]");
                    
                    if (opIt == classPtr->operatorMethods.end()) return;
                    
                    int protoId = opIt->second;
                    ZEUS_DEBUG_PRINT("[DELETE_VAR] Calling operator[~] for object, protoId=%d", protoId);
                    
                    const Prototype* protoPtr = getPrototype(protoId);
                    if (!protoPtr) return;
                    
                    const auto& proto = *protoPtr;
                    
                    // 创建闭包并调用 operator[~]
                    // 关键：设置 boundThis 和 upvalues，使得 operator[~] 中可以访问 this
                    Closure clo;
                    clo.protoId = protoId;
                    retainObject(objH.id); // closure will own & later release boundThis
                    clo.boundThis = objH;
                    
                    // 用于跟踪需要释放的临时 Cell
                    Cell* thisCell = nullptr;
                    
                    // 为 using &this 设置 upvalues
                    // 根据 prototype 的 captures 信息来设置
                    if (!proto.captures.empty()) {
                        // 找到最大的 dstIndex 来确定 upvalues 大小
                        int maxDst = 0;
                        for (const auto& cap : proto.captures) {
                            if (cap.dstIndex > maxDst) maxDst = cap.dstIndex;
                        }
                        clo.upvalues.resize(maxDst + 1, nullptr);
                        
                        // 创建包含 this 的 Cell（初始 refCount=1）
                        thisCell = newCell(objH, 0, "DELETE_VAR_destructor");
                        
                        // 将 this 放入 upvalue 槽位
                        for (const auto& cap : proto.captures) {
                            if (cap.dstIndex >= 0 && (size_t)cap.dstIndex < clo.upvalues.size()) {
                                clo.upvalues[cap.dstIndex] = thisCell;
                                thisCell->retain();  // 每个槽位增加引用
                            }
                        }
                    }
                    
                    int cloId = allocClosure(std::move(clo));
                    
                    // 压入闭包句柄
                    stack.push_back(ZeusType::ClosureHandle{cloId});
                    
                    // 额外持有一次 this，确保 operator[~] 执行期间对象不会被提前释放
                    // （尤其是当外层变量 Cell 的对象引用计数语义不完全对称时）
                    struct ThisHoldGuard {
                        VM* vm;
                        int objId;
                        ~ThisHoldGuard() {
                            if (vm && objId >= 0) vm->releaseObject(objId);
                        }
                    } thisHold{this, objH.id};
                    retainObject(objH.id);

                    // 同步调用 operator[~]，传入 0 个参数
                    executeClosureSync(cloId, 0);
                    
                    // 释放临时创建的 thisCell（释放初始的 refCount=1）
                    // upvalue 槽位的引用会在 executeClosureSync 内部清理
                    if (thisCell) {
                        thisCell->release();
                    }
                    
                    // 释放临时闭包（使用 releaseClosure 正确处理引用计数和空闲槽位）
                    releaseClosure(cloId);
                };
                
                // 释放 Cell 中持有的对象或闭包的辅助 lambda
                auto releaseRefInCell = [&](Cell* cell) {
                    if (!cell) return;
                    
                    auto& cellVal = cell->value;
                    
                    if (std::holds_alternative<ZeusType::ObjectHandle>(cellVal)) {
                        int objId = std::get<ZeusType::ObjectHandle>(cellVal).id;
                        releaseObject(objId);
                    } else if (std::holds_alternative<ZeusType::ClosureHandle>(cellVal)) {
                        int cloId = std::get<ZeusType::ClosureHandle>(cellVal).id;
                        releaseClosure(cloId);
                        ZEUS_DEBUG_PRINT("[DELETE_VAR] Released closure #%d from cell", cloId);
                    }
                };
                
                bool found = false;
                
                // 提取对象 ID 并打断闭包循环的辅助 lambda
                auto breakCyclesIfObject = [&](Cell* cell) {
                    if (!cell) return;

                    auto& cellVal = cell->value;

                    if (std::holds_alternative<ZeusType::ObjectHandle>(cellVal)) {
                        int objId = std::get<ZeusType::ObjectHandle>(cellVal).id;
                        forceBreakClosureCycles(objId);
                    }
                };
                
                if (kind == "local" && varIndex >= 0 && varIndex < (int)local.size()) {
                    // 删除局部变量
                    Cell* cell = local[varIndex];
                    
                    if (cell) {
                        callDestructor(cell);
                        if (vmstat != ZeusType::VMStatus::Running) break;
                        breakCyclesIfObject(cell);
                        releaseRefInCell(cell);
                        cell->release();
                        local[varIndex] = nullptr;
                        found = true;
                    }
                } else if (kind == "upvalue" && varIndex >= 0 && varIndex < (int)upvalue.size()) {
                    // 删除 upvalue
                    Cell* cell = upvalue[varIndex];
                    
                    if (cell) {
                        callDestructor(cell);
                        if (vmstat != ZeusType::VMStatus::Running) break;
                        breakCyclesIfObject(cell);
                        releaseRefInCell(cell);
                        cell->release();
                        upvalue[varIndex] = nullptr;
                        found = true;
                    }
                } else {
                    // 删除全局变量（按名称查找）
                    for (size_t i = 0; i < global.size(); ++i) {
                        if (i < globalNames.size() && globalNames[i] == varName) {
                            if (global[i]) {
                                callDestructor(global[i]);
                                if (vmstat != ZeusType::VMStatus::Running) break;
                                breakCyclesIfObject(global[i]);
                                releaseRefInCell(global[i]);
                                global[i]->release();
                                global[i] = nullptr;
                                found = true;
                            }
                            break;
                        }
                    }
                }

                if (vmstat != ZeusType::VMStatus::Running) break;

                if (!found) {
                    vmMessage("[DELETE_VAR] Warning: Variable not found or already null: " + varName);
                }

                // Safe point: sweep truly unreachable objects/closures.
                collectGarbageSweep();

                printStack();
                break;
            }

            // ===== 异常处理操作码 =====
            case VMOperator::TRY_BEGIN: {
                // TRY_BEGIN catchOffset: 注册异常处理器
                // catchOffset 是从当前位置到 catch 块的偏移量
                if (instr.args.empty()) {
                    VMException::VMException exp("TRY_BEGIN missing catchOffset", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                int64_t catchOffset = 0;
                if (std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    catchOffset = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }
                
                // 获取当前 PC（需要从调用栈帧获取）
                size_t currentPc = 0;
                const std::vector<IRType::IRInstr>* currentCode = nullptr;
                if (!callFrames.empty()) {
                    currentPc = callFrames.back().pc;
                    currentCode = callFrames.back().code;
                }
                
                // 注册异常处理器
                ExceptionHandler handler;
                handler.catchPc = currentPc + catchOffset;  // catch 块的起始位置
                handler.callFrameDepth = callFrames.size();
                handler.code = currentCode;
                handler.stackBase = stack.size();
                handler.localSize = local.size();
                exceptionHandlers.push_back(handler);
                
                ZEUS_DEBUG_PRINT("[VM] TRY_BEGIN: registered handler, catchPc=%zu, depth=%zu", 
                    handler.catchPc, handler.callFrameDepth);
                break;
            }
            
            case VMOperator::TRY_END: {
                // TRY_END: try 块正常完成，移除异常处理器
                if (!exceptionHandlers.empty()) {
                    exceptionHandlers.pop_back();
                    ZEUS_DEBUG_PRINT("[VM] TRY_END: removed handler, remaining=%zu", exceptionHandlers.size());
                }
                break;
            }
            
            case VMOperator::CATCH_BEGIN: {
                // CATCH_BEGIN "varName" slotIndex: catch 块开始
                // 如果有变量名和槽索引，将当前异常存储到该槽位
                std::string varName;
                int slotIndex = -1;
                
                if (!instr.args.empty() && std::holds_alternative<std::string>(instr.args[0])) {
                    varName = std::get<std::string>(instr.args[0]);
                }
                if (instr.args.size() > 1 && std::holds_alternative<LexerType::Value>(instr.args[1])) {
                    slotIndex = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[1]));
                }
                
                if (!varName.empty() && slotIndex >= 0 && hasUnhandledException) {
                    // 确保 local 足够大
                    while (local.size() <= (size_t)slotIndex) {
                        local.push_back(nullptr);
                    }
                    
                    // 如果该槽已有值，先释放
                    if (local[slotIndex]) {
                        releaseCellAndContent(local[slotIndex]);
                    }
                    
                    // 在指定槽位存储异常
                    Cell* excCell = newCell(currentException, 0, "catch_var");
                    local[slotIndex] = excCell;
                    ZEUS_DEBUG_PRINT("[VM] CATCH_BEGIN: stored exception to '%s', local[%d]", 
                        varName.c_str(), slotIndex);
                }
                
                hasUnhandledException = false;
                break;
            }
            
            case VMOperator::CATCH_END: {
                // CATCH_END: catch 块结束
                ZEUS_DEBUG_PRINT("[VM] CATCH_END");
                break;
            }
            
            case VMOperator::THROW: {
                // THROW: 抛出异常（栈顶是异常对象）
                if (stack.empty()) {
                    VMException::VMException exp("THROW: stack is empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                currentException = stack.back();
                stack.pop_back();
                hasUnhandledException = true;
                
                ZEUS_DEBUG_PRINT("[VM] THROW: exception raised");
                
                vmstat = ZeusType::VMStatus::Error;
                if (exceptionHandlers.empty()) {
                    std::string errorMsg = "Unhandled exception";
                    if (std::holds_alternative<ZeusType::ObjectHandle>(currentException)) {
                        auto& oh = std::get<ZeusType::ObjectHandle>(currentException);
                        auto* obj = getObject(oh.id);
                        if (obj) {
                            // 尝试获取 _message 字段
                            auto it = obj->values.find("_message");
                            if (it != obj->values.end() && std::holds_alternative<std::string>(it->second)) {
                                errorMsg = std::get<std::string>(it->second);
                            }
                            // 尝试获取 _type 字段
                            auto typeIt = obj->values.find("_type");
                            if (typeIt != obj->values.end() && std::holds_alternative<std::string>(typeIt->second)) {
                                errorMsg = std::get<std::string>(typeIt->second) + ": " + errorMsg;
                            }
                        }
                    } else if (std::holds_alternative<std::string>(currentException)) {
                        errorMsg = std::get<std::string>(currentException);
                    } else if (std::holds_alternative<LexerType::Value>(currentException)) {
                        auto& val = std::get<LexerType::Value>(currentException);
                        if (std::holds_alternative<std::string>(val.data)) {
                            errorMsg = std::get<std::string>(val.data);
                        }
                    }
                    
                    VMException::VMException exp("Uncaught exception: " + errorMsg, __line, instr);
                    exp._throw(vmstat, currentChunk);
                }
                break;
            }
            
            case VMOperator::WITH_BEGIN: {
                // WITH_BEGIN slotIdx: 开始 with 块（栈顶是 with 对象）
                // slotIdx: capture 变量槽索引，-1 表示无 capture
                if (stack.empty()) {
                    VMException::VMException exp("WITH_BEGIN: stack is empty", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                auto withObj = stack.back();
                stack.pop_back();
                
                if (!std::holds_alternative<ZeusType::ObjectHandle>(withObj)) {
                    VMException::VMException exp("WITH_BEGIN: expected object, got " + 
                        ZeusVMUtils::valueToDebugString(withObj), __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                auto& oh = std::get<ZeusType::ObjectHandle>(withObj);
                auto* obj = getObject(oh.id);
                if (!obj) {
                    VMException::VMException exp("WITH_BEGIN: object not found", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                // 检查是否有 operator[with] 方法
                auto* classPtr = getClass(obj->classId);
                if (!classPtr) {
                    VMException::VMException exp("WITH_BEGIN: class not found", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                auto opIt = classPtr->operatorMethods.find("[with]");
                if (opIt == classPtr->operatorMethods.end()) {
                    VMException::VMException exp("WITH_BEGIN: object does not have operator[with] method", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                // 如果有 capture 槽，将对象引用存入该槽
                int captureSlot = -1;
                if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0])) {
                    captureSlot = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }
                
                if (captureSlot >= 0) {
                    // 将对象引用存入 capture 槽
                    if (local.size() <= static_cast<size_t>(captureSlot)) {
                        local.resize(captureSlot + 1, nullptr);
                    }
                    if (!local[captureSlot]) {
                        local[captureSlot] = newCell(withObj, 0, "WITH_CAPTURE");
                    } else {
                        local[captureSlot]->value = withObj;
                    }
                    ZEUS_DEBUG_PRINT("[VM] WITH_BEGIN: stored object to capture slot %d", captureSlot);
                }
                
                // 调用 operator[with]()
                //
                // 用 executeClosureSync 而不是递归 execute()：它会压一个真正的栈帧
                // 并把帧跑完，局部变量随帧保存/还原。递归 execute() 会共用同一份
                // local 缓冲区，被调用帧的槽位会污染调用者。
                int protoId = opIt->second;
                Closure clo;
                clo.protoId = protoId;
                int cloId = allocClosure(std::move(clo));
                stack.push_back(ZeusType::ClosureHandle{cloId});
                // The nested call machinery releases every argument slot it pops,
                // so the stack has to own a reference for the receiver it pushes.
                ZeusGC::retainVariantDeep(*this, withObj);
                stack.push_back(withObj);  // this 参数
                // Dispatch through a real frame: executeClosureSync pushes a
                // CallFrame and runs it to completion, so the callee gets its own
                // local slots. A recursive execute() would share this frame's local
                // buffer and leave the callee's slots behind in it.
                if (!executeClosureSync(cloId, 1)) {
                    if (vmstat != ZeusType::VMStatus::Running) break;
                    VMException::VMException exp("WITH_BEGIN: operator[with] call failed", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                // 弹出 operator[with]() 的返回值（不需要使用）
                if (!stack.empty()) {
                    stack.pop_back();
                }
                
                // 检查是否有 operator[~with] 方法
                bool hasExitMethod = (classPtr->operatorMethods.find("[~with]") != classPtr->operatorMethods.end());
                
                // 保存 with 上下文
                WithContext ctx;
                ctx.objHandle = oh;
                ctx.hasExitMethod = hasExitMethod;
                withContextStack.push_back(ctx);
                
                ZEUS_DEBUG_PRINT("[VM] WITH_BEGIN: entered with block, hasExitMethod=%d", hasExitMethod);
                break;
            }
            
            case VMOperator::WITH_END: {
                // WITH_END: 结束 with 块
                if (withContextStack.empty()) {
                    VMException::VMException exp("WITH_END: no active with block", __line, instr);
                    exp._throw(vmstat, currentChunk);
                    break;
                }
                
                WithContext ctx = withContextStack.back();
                withContextStack.pop_back();
                
                // 如果有 operator[~with] 方法，调用它
                if (ctx.hasExitMethod) {
                    auto* obj = getObject(ctx.objHandle.id);
                    if (obj) {
                        auto* classPtr = getClass(obj->classId);
                        if (classPtr) {
                            auto opIt = classPtr->operatorMethods.find("[~with]");
                            if (opIt != classPtr->operatorMethods.end()) {
                                int protoId = opIt->second;
                                Closure clo;
                                clo.protoId = protoId;
                                int cloId = allocClosure(std::move(clo));
                                stack.push_back(ZeusType::ClosureHandle{cloId});
                                // Same ownership rule as WITH_BEGIN: the nested call
                                // releases the receiver slot it pops.
                                ZeusGC::retainVariantDeep(
                                    *this, ZeusType::ObjectHandle{ctx.objHandle.id, ctx.objHandle.generation});
                                stack.push_back(ZeusType::ObjectHandle{ctx.objHandle.id, ctx.objHandle.generation});
                                // Dispatch through a real frame: executeClosureSync pushes a
                                // CallFrame and runs it to completion, so the callee gets its
                                // own local slots instead of sharing this frame's buffer.
                                if (!executeClosureSync(cloId, 1)) {
                                    if (vmstat != ZeusType::VMStatus::Running) break;
                                    VMException::VMException exp(
                                        "WITH_END: operator[~with] call failed", __line, instr);
                                    exp._throw(vmstat, currentChunk);
                                    break;
                                }
                                
                                // 弹出 operator[~with]() 的返回值（不需要使用）
                                if (!stack.empty()) {
                                    stack.pop_back();
                                }
                            }
                        }
                    }
                }
                
                ZEUS_DEBUG_PRINT("[VM] WITH_END: exited with block");
                break;
            }

            
            case VMOperator::HOTSPOT: {
                // HOTSPOT: 热点触发指令，根据第一个参数决定要执行的热热点 ID
                // 支持两种参数布局：
                //  - HotspotCompiler 生成的参数序列: [id(Value int), pushValue(Value), idx(Value int), mutOrConst(string), name?]
                //  - 返回/写入形式: [idx(Value int), mutOrConst(string), name?]（用于读/写操作）
                int hotId = -1;
                if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0]) &&
                    std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(instr.args[0]).data)) {
                    hotId = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
                }

                // 构造 HotspotContext（封装 VM 状态以供热点处理器使用）
                HotspotContext hctx{
                    /*stack*/   this->stack.data,
                    /*global*/  this->global,
                    /*local*/   this->local,
                    /*upvalue*/ this->upvalue,
                    /*vmstat*/  this->vmstat,
                    /*__line*/  this->__line,
                    /*globalNames*/ this->globalNames,
                    /*nameToClosure*/ this->nameToClosure,
                    /*enableLateResolve*/ this->enableLateResolve,
                    /*newCellCb*/ [this](const ZeusType::UniversalVariant& v, uint8_t flags, const char* createdBy){ return this->newCell(v, flags, createdBy); },
                    /*tryRegisterGlobalClosureCb*/ [this](int slot, const ZeusType::UniversalVariant& v){ this->tryRegisterGlobalClosure(slot, v); },
                    /*printGlobalsCb*/ [this](){ this->printGlobals(); },
                    /*printStackCb*/   [this](){ this->printStack(); }
                };
                // 处理已知的 hotspot id（hotId==0 为示例处理器）
                if (hotId == 0) {
                    // instr 可能直接携带 hotspot1 的内部参数布局，调用 hotspot1 处理
                    hotspot1(instr, hctx);
                } else {
                    // 未知的 hotspot id：仅打印调试信息并跳过
                    ZEUS_DEBUG_PRINT("[VM][HOTSPOT] unknown id=%d, noop", hotId);
                }
                break;
            }
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
            if (f.calleeAutoReleaseId >= 0) {
                releaseClosure(f.calleeAutoReleaseId);
            }
            callFrames.pop_back();
        }
        
        // 清理栈帧
        callFrames.clear();
    }
};

// ===== 线程执行函数实现 =====

namespace ZeusThread {

    // 线程入口函数：在新线程中创建 VM 并执行指定的函数
    inline void threadEntry(std::shared_ptr<ThreadInfo> info) {
        ZEUS_DEBUG_PRINT("[Thread] threadEntry: id=%d, protoId=%d", info->id, info->protoId);

        try {
            info->state = ThreadState::Running;
            info->nativeId = std::this_thread::get_id();

            // 创建新的 VM 实例
            VM vm;
            
            // 设置外部停止标志（用于 eliminate）
            vm.externalStopFlag = &info->shouldStop;

            // 注册函数原型（使用指针引用，不复制）
            if (info->sharedPrototypes) {
                // 使用共享的原型指针（零拷贝）
                vm.sharedPrototypes = static_cast<std::vector<VM::Prototype>*>(info->sharedPrototypes);
                ZEUS_DEBUG_PRINT("[Thread] Using shared prototypes (pointer): %zu", vm.sharedPrototypes->size());
            } else if (info->program) {
                vm.registerPrototypes(*info->program);
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

            // 如果 VM 仍在运行，执行目标函数
            const VM::Prototype* protoPtr = vm.getPrototype(info->protoId);
            if (vm.vmstat == ZeusType::VMStatus::Running && protoPtr) {

                ZEUS_DEBUG_PRINT("[Thread] Executing protoId %d, hasBoundThis=%d", info->protoId, info->boundThis.has_value());

                // 直接使用 prototypes[protoId].body 执行（不需要 chunkToProtoId）
                const auto& proto = *protoPtr;
                
                if (!proto.body.empty()) {
                    // 构造闭包并执行
                     int closureId = (int)vm.closures.size();
                     VM::Closure clos;
                     clos.protoId = info->protoId;
                     // 设置 boundThis，CALL_CLOSURE 会自动处理 this 参数的插入
                     if (info->boundThis.has_value()) {
                         vm.retainObject(info->boundThis->id); // closure will own & later release boundThis
                     }
                     clos.boundThis = info->boundThis;
                     vm.closures.push_back(clos);
                    
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
                } else {
                    ZEUS_DEBUG_PRINT("[Thread] Empty prototype body!");
                }
            } else {
                ZEUS_DEBUG_PRINT("[Thread] Skipped execution: vmstat=%d protoId=%d protoPtr=%p",
                    (int)vm.vmstat, info->protoId, (void*)protoPtr);
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
                vm.releaseObject(info->boundThis->id);
                info->boundThis.reset();
            }

            info->state = ThreadState::Finished;
            ZEUS_DEBUG_PRINT("[Thread] threadEntry finished: id=%d", info->id);

        } catch (const std::exception& e) {
            info->state = ThreadState::Error;
            info->errorMessage = e.what();
            ZEUS_DEBUG_PRINT("[Thread] threadEntry error: id=%d, msg=%s", info->id, e.what());
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



