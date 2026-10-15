#pragma once

#include <vector>
#include <string>
#include <functional>
#include <unordered_map>

#include "../ZeusIR/IRHotspot.hpp"
#include "../ZeusRuntimeLib/ZeusCell.hpp"
#include "../ZeusRuntimeLib/ZeusVariant.hpp"
#include "../ZeusParser/LexerType.hpp"
#include "../ZeusRuntimeLib/ZeusVMException.hpp"
#include "../ZeusDebug/Utils.hpp"

using namespace IRHotspotList;

// 运行期热点执行所需上下文：
// - 承载 VM 的关键状态（栈、变量环境、调试信息）
// - 以及需要的辅助回调（newCell/登记闭包/调试打印）
struct HotspotContext {
    // 基础运行期状态
    std::vector<ZeusType::UniversalVariant>& stack;
    std::vector<Cell*>& global;
    std::vector<Cell*>& local;
    std::vector<Cell*>& upvalue;

    ZeusType::VMStatus& vmstat;
    int& __line;

    // 名称与闭包登记所需信息
    std::vector<std::string>& globalNames;
    std::unordered_map<std::string,int>& nameToClosure;
    bool& enableLateResolve;

    // 辅助回调（由 VM 提供实现）
    std::function<Cell*(const ZeusType::UniversalVariant&, uint8_t, const char*)> newCellCb;
    std::function<void(Cell*, const ZeusType::UniversalVariant&)> assignCellValueCb;
    std::function<void(int /*slot*/, const ZeusType::UniversalVariant&)> tryRegisterGlobalClosureCb;
    std::function<void()> printGlobalsCb;
    std::function<void()> printStackCb;
};

std::vector<std::function<void(const IRType::IRInstr&, HotspotContext&)>> hotspotHandlers;


inline void hotspot1(const IRType::IRInstr& instr, HotspotContext& ctx) {
    // 期望参数：idx(int as Value), mutOrConst(string), [optional name]
    if (instr.args.size() < 2) {
        VMException::VMException exp("HOTSPOT(h1) missing args", ctx.__line, instr);
        exp._throw(ctx.vmstat);
        return;
    }

    size_t base = 0; // args 基准：0=旧形态; 1=新形态(含 id)

    int slot = -1;
    std::string mutOrConst;
    ZeusType::UniversalVariant newVal; // 将要写入的值

    // 新形态：第 0 个参数为热点ID(int)
    if (!instr.args.empty() && std::holds_alternative<LexerType::Value>(instr.args[0]) &&
        std::holds_alternative<ZeusType::BigInteger>(std::get<LexerType::Value>(instr.args[0]).data)) {
        base = 1;
    }

    // 如果是新形态，args[1] 是被优化掉的 PUSH 的值
    bool tookValueFromArgs = false;
    if (base == 1) {
        if (instr.args.size() < 4) {
            VMException::VMException exp("HOTSPOT(h1) bad compiled args", ctx.__line, instr);
            exp._throw(ctx.vmstat);
            return;
        }
        // args[1] = pushValue
        const auto& pv = instr.args[1];
        if (std::holds_alternative<IRType::NullLiteral>(pv)) {
            newVal = ZeusType::Variant(ZeusType::Null());
            tookValueFromArgs = true;
        } else if (std::holds_alternative<LexerType::Value>(pv)) {
            const auto &val = std::get<LexerType::Value>(pv);
            newVal = val;
            tookValueFromArgs = true;
        } else if (std::holds_alternative<ZeusType::Bytes>(pv)) {
            const auto &bytes = std::get<ZeusType::Bytes>(pv);
            newVal = ZeusType::Variant(bytes);
            tookValueFromArgs = true;
        } else {
            // 非 Value/Bytes 的内联值暂未支持
            VMException::VMException exp("HOTSPOT(h1) pushValue must be NullLiteral, Value, or Bytes", ctx.__line, instr);
            exp._throw(ctx.vmstat);
            return;
        }

        // 读取 slot & mut/const
        const auto& idxVal = std::get<LexerType::Value>(instr.args[2]);
        slot = ZeusType::bigIntegerToInt(std::get<ZeusType::BigInteger>(idxVal.data));
        mutOrConst = std::get<std::string>(instr.args[3]);
    } else {
        // 旧形态：值在栈顶
        const auto& idxVal = std::get<LexerType::Value>(instr.args[0]);
        slot = ZeusType::bigIntegerToInt(std::get<ZeusType::BigInteger>(idxVal.data));
        mutOrConst = std::get<std::string>(instr.args[1]);
    }

    // 可选 inline 变量名（用于 late resolve 名称填充）
    if (instr.args.size() > base + 2 && std::holds_alternative<std::string>(instr.args[base + 2])) {
        const std::string& inlineName = std::get<std::string>(instr.args[base + 2]);
        if (slot >= 0) {
            if (slot >= (int)ctx.globalNames.size()) ctx.globalNames.resize(slot + 1);
            if (ctx.globalNames[slot].empty()) ctx.globalNames[slot] = inlineName;
        }
    }

    if (!tookValueFromArgs && ctx.stack.empty()) {
        VMException::VMException exp("HOTSPOT(h1) stack underflow", ctx.__line, instr);
        exp._throw(ctx.vmstat);
        return;
    }
    if (!tookValueFromArgs) {
        newVal = ctx.stack.back();
    }

    if (slot >= (int)ctx.global.size()) {
        ctx.global.resize(slot + 1, nullptr);
    }

    if (!ctx.global[slot]) {
        if (ctx.newCellCb) {
            ctx.global[slot] = ctx.newCellCb(newVal, (mutOrConst == "const") ? 1 : 0, "HOTSPOT_STORE_GLOBAL");
        } else {
            ctx.global[slot] = new Cell{ newVal, (uint8_t)((mutOrConst == "const") ? 1 : 0), __nextCellId() };
            ctx.global[slot]->createdAtMs = __getCurrentTimeMs();
            ctx.global[slot]->createdBy = "HOTSPOT_STORE_GLOBAL";
            HeapStats::recordCreate();  // 修复：手动创建 Cell 也需要记录
            HeapStats::registerCell(ctx.global[slot]);  // 注册到活跃Cell列表
        }
    } else {
        if (ctx.global[slot]->flags & 0x1) {
            VMException::VMException exp("Attempt to modify a const global variable at index " + std::to_string(slot), ctx.__line, instr);
            exp._throw(ctx.vmstat);
            return;
        }
        ctx.assignCellValueCb(ctx.global[slot], newVal);
        const bool wantConst = (mutOrConst == "const");
        const bool isConstNow = (ctx.global[slot]->flags & 0x1) != 0;
        if (wantConst != isConstNow) {
            if (wantConst) ctx.global[slot]->flags |= 0x1;
            else           ctx.global[slot]->flags &= ~0x1;
        }
    }

    // 弹出写入值（仅当来自栈）
    if (!tookValueFromArgs) {
        ctx.stack.pop_back();
    }

    // 若是闭包并开启宽松模式，则登记名称 -> 闭包句柄
    if (ctx.tryRegisterGlobalClosureCb) {
        ctx.tryRegisterGlobalClosureCb(slot, newVal);
    } else if (ctx.enableLateResolve && std::holds_alternative<ZeusType::ClosureHandle>(newVal)) {
        int cid = std::get<ZeusType::ClosureHandle>(newVal).id;
        if (slot >= 0 && slot < (int)ctx.globalNames.size()) {
            const std::string& nm = ctx.globalNames[slot];
            if (!nm.empty()) ctx.nameToClosure[nm] = cid;
        }
    }

    if (ZeusDebug::enabled()) {
        if (ctx.printGlobalsCb) ctx.printGlobalsCb();
        if (ctx.printStackCb)   ctx.printStackCb();
    }
}




void registerDefaultHotspotHandlers() {
    hotspotHandlers.push_back(hotspot1);
}
