
#ifndef IRScope_hpp
#define IRScope_hpp

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../ZeusBuiltins/BuiltinRegistry.hpp"

namespace IRScope {

    // 单个作用域：记录本作用域内标识符到局部槽位的映射
    enum Kind {
        UNDEFINED,
        LOCAL,
        GLOBAL,
        UPVALUE,
        SHARED,           // 共享堆变量
        LIST_ELEM_REF,
        DICT_KEY_REF,
        DICT_VAL_REF,
        COUNT_INDEX_REF
    };

    struct RefSymbolMeta {
        Kind kind;
        int  baseSlot;      // 关联的底层容器或索引槽（列表引用: list 槽 / 计数引用: 索引槽）
        int  indexSlot;     // 列表元素引用的索引槽；字典 key/value 引用对应的迭代器内部索引槽（预留）
        bool isConst;
    };

    struct Scope {
        std::unordered_map<std::string, int> nameToLocal;
        std::unordered_map<std::string, bool> nameToRawBytesRange;
        std::unordered_map<std::string, RefSymbolMeta> refSymbols;
        int functionDepth = 0;
    };

    // 使用 C++17 inline 变量，避免头文件多重定义问题
    inline std::vector<Scope> scopeStack;          // 作用域栈
    inline int localCount = 0;                     // 非函数上下文的简易局部计数（可选）
    inline std::unordered_map<std::string, int> globals; // 全局：名字 -> 全局槽位
    inline std::unordered_map<std::string, bool> globalRawBytesRange;
    inline std::unordered_set<std::string> sharedVars;   // 共享变量名集合（进程级共享堆）
    inline std::unordered_map<std::string, bool> sharedRawBytesRange;
    inline std::unordered_set<std::string> functionNames; // 当前编译单元可直接识别的函数名

    inline std::unordered_map<std::string, int> importedGlobalSlots;
    inline std::unordered_map<std::string, std::string> importedFunctionTargets;
    inline std::unordered_set<std::string> importedFunctionNames;
    inline std::unordered_map<std::string, int> nativeFunctionHandles;

    inline void clearImportedSymbols() {
        importedGlobalSlots.clear();
        importedFunctionTargets.clear();
        importedFunctionNames.clear();
    }

    inline bool isFunctionName(const std::string& name) {
        return functionNames.find(name) != functionNames.end();
    }

    inline bool isStaticFunctionName(const std::string& name) {
        return isFunctionName(name) || importedFunctionNames.find(name) != importedFunctionNames.end();
    }

    // ---- 闭包/函数环境 ----



    // 控制是否允许隐式捕获（默认允许）。在严格模式下应禁止自动在内层函数引用外层局部时生成捕获链。
    inline bool allowImplicitCapture = true;
    
    // using original 模式状态
    struct OriginalTypeMode {
        bool useOriginal = false;
        bool useOriginalList = false;      // 是否使用原生list(不自动包装为Array对象)
        bool useOriginalBytes = false;     // Legacy mode flag; bytes literals are native by default.
        bool useOriginalString = false;    // 是否使用原生string(不自动包装为String对象)
    };

    // Legacy per-type flags are kept for package-loader state save/restore.
    inline bool useOriginalList = false;
    inline bool useOriginalBytes = false;
    inline bool useOriginalString = false;
    
    // 全局using original模式栈，用于支持嵌套和范围限制
    inline std::vector<OriginalTypeMode> originalModeStack = {OriginalTypeMode()}; // 初始化为默认状态

    // `using LLVM` enables the LLVM-only expression intrinsics while IR is
    // generated for the enclosed source block.
    inline std::vector<bool> llvmModeStack = { false };

    inline bool isUsingLLVM() {
        return !llvmModeStack.empty() && llvmModeStack.back();
    }

    inline void pushLLVMMode() {
        llvmModeStack.push_back(true);
    }

    inline void popLLVMMode() {
        if (llvmModeStack.size() > 1) llvmModeStack.pop_back();
    }

    struct LLVMModeGuard {
        LLVMModeGuard() { pushLLVMMode(); }
        LLVMModeGuard(const LLVMModeGuard&) = delete;
        LLVMModeGuard& operator=(const LLVMModeGuard&) = delete;
        ~LLVMModeGuard() { popLLVMMode(); }
    };
    
    // 便捷方法：获取当前的using original模式
    inline OriginalTypeMode getCurrentOriginalMode() {
        if (!originalModeStack.empty()) {
            return originalModeStack.back();
        }
        return OriginalTypeMode();
    }

    inline bool isUsingOriginal() {
        if (originalModeStack.empty()) return false;
        const auto& mode = originalModeStack.back();
        return mode.useOriginal || mode.useOriginalList ||
               mode.useOriginalBytes || mode.useOriginalString;
    }

    inline bool isUsingOriginal(ZeusBuiltins::BuiltinLiteralKind kind) {
        const OriginalTypeMode mode = originalModeStack.empty()
            ? OriginalTypeMode()
            : originalModeStack.back();
        if (mode.useOriginal) return true;
        switch (kind) {
            case ZeusBuiltins::BuiltinLiteralKind::Array:
                return mode.useOriginalList || useOriginalList;
            case ZeusBuiltins::BuiltinLiteralKind::Bytes:
                return mode.useOriginalBytes || useOriginalBytes;
            case ZeusBuiltins::BuiltinLiteralKind::String:
                return mode.useOriginalString || useOriginalString;
        }
        return false;
    }

    inline void enableOriginalMode() {
        if (originalModeStack.empty()) originalModeStack.push_back(OriginalTypeMode());
        auto& mode = originalModeStack.back();
        mode.useOriginal = true;
        mode.useOriginalList = true;
        mode.useOriginalBytes = true;
        mode.useOriginalString = true;
        IRScope::useOriginalList = true;
        IRScope::useOriginalBytes = true;
        IRScope::useOriginalString = true;
    }
    
    // 便捷方法：推入新的using original模式
    inline void pushOriginalMode(bool useList, bool useBytes, bool useString) {
        OriginalTypeMode mode;
        mode.useOriginal = useList && useBytes && useString;
        mode.useOriginalList = useList;
        mode.useOriginalBytes = useBytes;
        mode.useOriginalString = useString;
        originalModeStack.push_back(mode);
    }
    
    // 便捷方法：弹出using original模式
    inline void popOriginalMode() {
        if (originalModeStack.size() > 1) {  // 保留至少一个默认状态
            originalModeStack.pop_back();
        }
    }
    
    // 包编译上下文：用于解决包内调用内置函数的命名冲突
    inline bool isCompilingPackage = false;  // 是否正在编译包
    inline std::string currentPackageName = "";  // 当前包名（如 "zeus.cppfunctions"）

    inline std::string functionTargetName(const std::string& name) {
        if (isFunctionName(name) && isCompilingPackage && !currentPackageName.empty()) {
            return currentPackageName + "." + name;
        }
        auto imported = importedFunctionTargets.find(name);
        return imported == importedFunctionTargets.end() ? name : imported->second;
    }

    struct Capture {
        Kind srcKind;     // 捕获来源：LOCAL 或 UPVALUE
        int  srcIndex;    // 来源槽位
        int  dstIndex;    // 在当前函数中的 upvalue 槽位
        std::string name; // 变量名（可选：调试用）
    };

    struct FuncEnv {
        std::unordered_map<std::string, int> locals;   // 名字 -> 本函数的局部槽位
        std::unordered_map<std::string, int> upvalues; // 名字 -> 本函数的 upvalue 槽位
        std::vector<Capture> captures;                 // 捕获记录（顺序即为目标 upvalue 槽位）
        std::unordered_set<std::string> referencedOuters; // 在严格模式下记录引用了外层变量但未显式捕获的名字
        std::unordered_set<std::string> constLocals;    // 标记为 const 的本地变量名集合（用于解析阶段报错）
        std::string name;                              // 本函数的名字（用于报错定位）
        int localCount = 0;                            // 本函数局部计数
    };

    inline std::vector<FuncEnv> funcStack;            // 函数环境栈（外层在前、内层在后）

    struct ReceiverContext {
        std::string ownerType;
        std::string alias;
    };
    inline std::vector<ReceiverContext> receiverContextStack;

    inline void pushReceiverContext(std::string ownerType, std::string alias) {
        receiverContextStack.push_back(
            ReceiverContext{std::move(ownerType), std::move(alias)});
    }

    inline void popReceiverContext() {
        if (!receiverContextStack.empty()) receiverContextStack.pop_back();
    }

    inline const ReceiverContext* currentReceiverContext() {
        return receiverContextStack.empty() ? nullptr : &receiverContextStack.back();
    }

    class ScopedReceiverContext {
    public:
        ScopedReceiverContext(std::string ownerType, std::string alias) {
            pushReceiverContext(std::move(ownerType), std::move(alias));
        }
        ~ScopedReceiverContext() { popReceiverContext(); }

        ScopedReceiverContext(const ScopedReceiverContext&) = delete;
        ScopedReceiverContext& operator=(const ScopedReceiverContext&) = delete;
    };

    // Capture-all state (used to force-capture unresolved outer refs for [&] / [] )
    inline int captureAllDepth = 0;
    inline void pushCaptureAll(bool enabled) { if (enabled) ++captureAllDepth; }
    inline void popCaptureAll(bool enabled) { if (enabled && captureAllDepth > 0) --captureAllDepth; }
    inline bool isCaptureAllActive() { return captureAllDepth > 0; }

    // 进入/离开作用域
    inline void pushScope() {
        scopeStack.emplace_back();
        scopeStack.back().functionDepth = static_cast<int>(funcStack.size());
    }

    inline void popScope () {
        if (!scopeStack.empty()) scopeStack.pop_back();
    }

    // 进入/离开函数环境
    inline void beginFunction() {
        funcStack.push_back(FuncEnv{});
        // Reference bindings such as `for (...) [const &index]` belong to the
        // active function and must be resolvable at its function depth.
        pushScope();
    }

    inline FuncEnv endFunction() {
        FuncEnv env = funcStack.back();
        popScope();
        funcStack.pop_back();
        return env;
    }

    // 在当前作用域声明一个局部变量，返回其槽位索引
    inline int declareLocal (const std::string& name) {
        if (scopeStack.empty()) {
            pushScope();
        }
        Scope& currentScope = scopeStack.back();

        // 函数内：使用函数环境的局部分配
        if (!funcStack.empty()) {
            FuncEnv& fe = funcStack.back();
            auto itScope = currentScope.nameToLocal.find(name);
            if (itScope != currentScope.nameToLocal.end()) return itScope->second;

            auto itLocal = fe.locals.find(name);
            if (itLocal != fe.locals.end()) return itLocal->second;

            int slot = fe.localCount++;
            fe.locals[name] = slot;
            currentScope.nameToLocal[name] = slot;
            return slot;
        }

        // 非函数上下文：使用简易局部计数
        auto it = currentScope.nameToLocal.find(name);
        if (it != currentScope.nameToLocal.end()) return it->second;
        int slot = localCount++;
        currentScope.nameToLocal[name] = slot;
        return slot;
    }

    // 内部槽：不登记名称，不可被用户解析；仅占用一个局部索引供内部结构（for 隐藏状态等）使用。
    // 须保证：
    //   1. 不写入 nameToLocal / locals 映射。
    //   2. 与 declareLocal 共享同一个计数空间，确保槽位连续且 VM 访问一致。
    inline int allocateInternalLocal() {
        if (scopeStack.empty()) {
            pushScope();
        }

        if (!funcStack.empty()) {
            FuncEnv &fe = funcStack.back();
            int slot = fe.localCount++;
            return slot;
        }

        int slot = localCount++;
        return slot;
    }

    // 声明或获取一个全局变量槽位
    inline int declareGlobal (const std::string& name) {
        auto it = globals.find(name);
        if (it != globals.end()) return it->second;
        int gid = static_cast<int>(globals.size());
        globals[name] = gid;
        return gid;
    }

    // 声明一个共享变量（进程级共享堆）
    inline void declareShared(const std::string& name) {
        sharedVars.insert(name);
    }

    // 检查是否为共享变量
    inline bool isSharedVar(const std::string& name) {
        return sharedVars.find(name) != sharedVars.end();
    }

    struct SymRef {
        Kind kind;
        int index;
        bool isConst = false;
        int aux = -1; // 对引用：aux 可作为 indexSlot
    };

    // 从最近作用域开始解析标识符；未命中则查全局；都没有则返回 UNDEFINED
    inline SymRef resolve(const std::string& name) {
        auto findActiveRef = [&]() -> const RefSymbolMeta* {
            const int activeDepth = static_cast<int>(funcStack.size());
            for (int si = static_cast<int>(scopeStack.size()) - 1; si >= 0; --si) {
                const Scope& scope = scopeStack[si];
                if (scope.functionDepth != activeDepth) continue;
                auto it = scope.refSymbols.find(name);
                if (it != scope.refSymbols.end()) return &it->second;
            }
            return nullptr;
        };
        auto makeRef = [](const RefSymbolMeta& meta) {
            SymRef r; r.kind = meta.kind; r.index = meta.baseSlot; r.isConst = meta.isConst; r.aux = meta.indexSlot; return r;
        };
        // 若在函数内：先解析词法局部和捕获，再查全局符号。
        if (!funcStack.empty()) {
            // 1) 当前函数的局部
            FuncEnv& cur = funcStack.back();
            auto itL = cur.locals.find(name);
            if (itL != cur.locals.end()) {
                SymRef r; r.kind = Kind::LOCAL; r.index = itL->second; r.isConst = (cur.constLocals.find(name) != cur.constLocals.end()); return r;
            }

            // 2) 当前函数已有 upvalue
            auto itU = cur.upvalues.find(name);
            if (itU != cur.upvalues.end()) return SymRef{ Kind::UPVALUE, itU->second };

            if (const auto* ref = findActiveRef()) return makeRef(*ref);

            // 如果禁止隐式捕获，则不要向外层查找并建立捕获链，直接跳到全局/外层作用域查找
            if (!allowImplicitCapture) {
                // 直接跳过捕获链构建，继续到全局解析
                goto _resolve_globals;
            }

            // 3) 向外函数链查找并“逐层”建立捕获链，避免跨层直接引用失效局部
            // 思路：找到最外层 firstHit 层（可能是某层的 local 或 upvalue），然后从 firstHit+1 一层层向内
            // ensure 该 name 在每一层都有 upvalue，最终在当前函数形成 capture。

            int curLevel = static_cast<int>(funcStack.size()) - 1; // 当前函数层索引
            int firstHitLevel = -1;        // 最外层命中层
            Kind hitKind = Kind::UNDEFINED; // 命中类别（LOCAL / UPVALUE）
            int hitIndex = -1;             // 在命中层的槽位（local slot 或 upvalue slot）

            // 先搜索命中层
            for (int fi = curLevel - 1; fi >= 0; --fi) {
                FuncEnv& outer = funcStack[fi];
                auto itOuterLocal = outer.locals.find(name);
                if (itOuterLocal != outer.locals.end()) {
                    firstHitLevel = fi;
                    hitKind = Kind::LOCAL;
                    hitIndex = itOuterLocal->second;
                    break;
                }
                auto itOuterUv = outer.upvalues.find(name);
                if (itOuterUv != outer.upvalues.end()) {
                    firstHitLevel = fi;
                    hitKind = Kind::UPVALUE;
                    hitIndex = itOuterUv->second;
                    break;
                }
            }

            if (firstHitLevel != -1) {
                // 从 firstHitLevel+1 到 curLevel-1 逐层补链（这些中间层之前没用过 name，但里层需要它）
                Kind srcKind = hitKind; // 当前来源种类
                int srcIndex = hitIndex; // 当前来源槽位
                for (int lev = firstHitLevel + 1; lev < curLevel; ++lev) {
                    FuncEnv& midEnv = funcStack[lev];
                    // 如果隐式捕获被禁止，不要修改中间环境的 upvalues/captures，但记录引用以便日后报错
                    if (!allowImplicitCapture) {
                        midEnv.referencedOuters.insert(name);
                        // 将来源视为 upvalue 向内传播，但不在中间层创建实际 upvalue
                        srcKind = Kind::UPVALUE;
                        srcIndex = -1; // unknown in strict mode
                        continue;
                    }
                    // 若该层已经有 upvalue 了就跳过
                    if (midEnv.upvalues.find(name) == midEnv.upvalues.end()) {
                        int dst = static_cast<int>(midEnv.upvalues.size());
                        midEnv.upvalues[name] = dst;
                        midEnv.captures.push_back(Capture{ srcKind, srcIndex, dst, name });
                        // 对下一层来说，来源就变成了 upvalue
                        srcKind = Kind::UPVALUE;
                        srcIndex = dst;
                    } else {
                        // 已存在则更新来源为该 upvalue，以便继续向内
                        srcKind = Kind::UPVALUE;
                        srcIndex = midEnv.upvalues[name];
                    }
                }

                // 最终在当前函数建立 capture
                if (!allowImplicitCapture && srcIndex == -1) {
                    // 严格模式：记录当前函数引用了外层变量但未显式捕获
                    cur.referencedOuters.insert(name);
                    return SymRef{ Kind::UNDEFINED, -1 };
                } else {
                    int dst = static_cast<int>(cur.upvalues.size());
                    cur.upvalues[name] = dst;
                    cur.captures.push_back(Capture{ srcKind, srcIndex, dst, name });
                    return SymRef{ Kind::UPVALUE, dst };
                }
            }
        } else {
            // 顶层代码块中的循环绑定等词法局部必须遮蔽同名全局变量。
            for (int si = static_cast<int>(scopeStack.size()) - 1; si >= 0; --si) {
                auto it = scopeStack[si].nameToLocal.find(name);
                if (it != scopeStack[si].nameToLocal.end()) {
                    return SymRef{ Kind::LOCAL, it->second };
                }
            }

            if (const auto* ref = findActiveRef()) return makeRef(*ref);
        }
        
        _resolve_globals:
        // shared 与普通全局变量同属非词法局部符号。
        if (sharedVars.find(name) != sharedVars.end()) {
            return SymRef{ Kind::SHARED, 0 };  // index 无意义，按名称访问
        }

        // 4) 全局
        auto imported = importedGlobalSlots.find(name);
        if (imported != importedGlobalSlots.end()) return SymRef{ Kind::GLOBAL, imported->second };

        auto g = globals.find(name);
        if (g != globals.end()) return SymRef{ Kind::GLOBAL, g->second };

        // 引用绑定使用独立符号表；先让显式局部、共享变量和全局变量完成解析。
        return SymRef{ Kind::UNDEFINED, -1 };
    }

    inline void setRawBytesRangeBinding(const std::string& name, bool isRawBytesRange) {
        const SymRef symbol = resolve(name);
        if (symbol.kind == Kind::LOCAL) {
            for (auto scope = scopeStack.rbegin(); scope != scopeStack.rend(); ++scope) {
                const auto local = scope->nameToLocal.find(name);
                if (local != scope->nameToLocal.end() && local->second == symbol.index) {
                    scope->nameToRawBytesRange[name] = isRawBytesRange;
                    return;
                }
            }
            return;
        }
        if (symbol.kind == Kind::GLOBAL) {
            globalRawBytesRange[name] = isRawBytesRange;
        } else if (symbol.kind == Kind::SHARED) {
            sharedRawBytesRange[name] = isRawBytesRange;
        }
    }

    inline bool isRawBytesRangeBinding(const std::string& name) {
        const SymRef symbol = resolve(name);
        if (symbol.kind == Kind::GLOBAL) {
            const auto found = globalRawBytesRange.find(name);
            return found != globalRawBytesRange.end() && found->second;
        }
        if (symbol.kind == Kind::SHARED) {
            const auto found = sharedRawBytesRange.find(name);
            return found != sharedRawBytesRange.end() && found->second;
        }
        if (symbol.kind == Kind::LOCAL || symbol.kind == Kind::UPVALUE) {
            for (auto scope = scopeStack.rbegin(); scope != scopeStack.rend(); ++scope) {
                const auto local = scope->nameToLocal.find(name);
                if (local == scope->nameToLocal.end()) continue;
                const auto rawRange = scope->nameToRawBytesRange.find(name);
                return rawRange != scope->nameToRawBytesRange.end() && rawRange->second;
            }
        }
        return false;
    }

    // 标记/取消标记当前函数的某个本地变量为 const
    inline void markLocalConst(const std::string& name, bool isConst) {
        if (funcStack.empty()) return;
        FuncEnv& cur = funcStack.back();
        if (isConst) cur.constLocals.insert(name);
        else cur.constLocals.erase(name);
    }

    // 强制在当前函数中为给定名称建立捕获（若可从外层函数环境找到），
    // 不考虑全局/普通作用域，仅针对函数闭包链。
    inline SymRef forceCapture(const std::string& name) {
        if (funcStack.empty()) {
            return SymRef{ Kind::UNDEFINED, -1 };
        }

        FuncEnv& cur = funcStack.back();
        // 已有局部/上值直接返回
        auto itL = cur.locals.find(name);
        if (itL != cur.locals.end()) return SymRef{ Kind::LOCAL, itL->second };
        auto itU = cur.upvalues.find(name);
        if (itU != cur.upvalues.end()) return SymRef{ Kind::UPVALUE, itU->second };

        int curLevel = static_cast<int>(funcStack.size()) - 1;
        int firstHitLevel = -1; Kind hitKind = Kind::UNDEFINED; int hitIndex = -1;
        for (int fi = curLevel - 1; fi >= 0; --fi) {
            FuncEnv& outer = funcStack[fi];
            auto itOuterLocal = outer.locals.find(name);
            if (itOuterLocal != outer.locals.end()) { firstHitLevel = fi; hitKind = Kind::LOCAL; hitIndex = itOuterLocal->second; break; }
            auto itOuterUv = outer.upvalues.find(name);
            if (itOuterUv != outer.upvalues.end()) { firstHitLevel = fi; hitKind = Kind::UPVALUE; hitIndex = itOuterUv->second; break; }
        }
        if (firstHitLevel == -1) {
            return SymRef{ Kind::UNDEFINED, -1 };
        }
        Kind srcKind = hitKind; int srcIndex = hitIndex;
        for (int lev = firstHitLevel + 1; lev < curLevel; ++lev) {
            FuncEnv& midEnv = funcStack[lev];
            if (midEnv.upvalues.find(name) == midEnv.upvalues.end()) {
                int dst = static_cast<int>(midEnv.upvalues.size());
                midEnv.upvalues[name] = dst;
                midEnv.captures.push_back(Capture{ srcKind, srcIndex, dst, name });
                srcKind = Kind::UPVALUE; srcIndex = dst;
            } else {
                srcKind = Kind::UPVALUE; srcIndex = midEnv.upvalues[name];
            }
        }
        int dst = static_cast<int>(cur.upvalues.size());
        cur.upvalues[name] = dst;
        cur.captures.push_back(Capture{ srcKind, srcIndex, dst, name });
        return SymRef{ Kind::UPVALUE, dst };
    }

    inline void registerListElemRef(const std::string& name, int listSlot, int indexSlot, bool isConst) {
        RefSymbolMeta m{ Kind::LIST_ELEM_REF, listSlot, indexSlot, isConst };
        if (scopeStack.empty()) pushScope();
        scopeStack.back().refSymbols[name] = m;
    }

    inline void registerCountIndexRef(const std::string& name, int indexSlot, bool isConst) {
        RefSymbolMeta m{ Kind::COUNT_INDEX_REF, indexSlot, -1, isConst };
        if (scopeStack.empty()) pushScope();
        scopeStack.back().refSymbols[name] = m;
    }

    inline void registerDictKeyRef(const std::string& name, int keySlot) {
        RefSymbolMeta m{ Kind::DICT_KEY_REF, keySlot, -1, true }; // key 永远视为 const
        if (scopeStack.empty()) pushScope();
        scopeStack.back().refSymbols[name] = m;
    }

    inline void registerDictValueRef(const std::string& name, int dictSlot, int keyTempSlot, bool isConst) {
        RefSymbolMeta m{ Kind::DICT_VAL_REF, dictSlot, keyTempSlot, isConst };
        if (scopeStack.empty()) pushScope();
        scopeStack.back().refSymbols[name] = m;
    }

    
}

#endif
