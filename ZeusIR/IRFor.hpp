#ifndef IRFor_hpp
#define IRFor_hpp

#include <vector>
#include "IRType.hpp"
#include "IRDebugUtils.hpp"
#include "IRPriorityFold.hpp"
#include "IRScope.hpp"
#include "IRException.hpp"
#include "IRUtils.hpp" // 公共 IR 构造工具
#include "../ZeusParser/TreeParser.hpp"
#include "../ZeusParser/LexerType.hpp"
#include <functional>
#include <sstream>

// For 循环 IR 构建骨架：
// 说明：
//   1. 仅提供最小函数声明与风格占位。
//   2. 不生成实际指令，后续步骤补齐。
//   3. 所有函数保持浅层逻辑，无深嵌套；后续添加时请继续遵守格式约束。
//   4. 注释前一行留空行，避免被视为代码块分隔的替代。

namespace IRFor {

    // 回填循环体中的 LOOP_BREAK 占位符
    // loopEndOffset: 循环结束位置相对于循环 IR 开始的偏移量
    inline void patchBreakPlaceholders(std::vector<IRType::IRInstr>& ir, size_t bodyStartIndex, size_t loopEndIndex) {
        for (size_t i = bodyStartIndex; i < ir.size(); ++i) {
            if (ir[i].op == VMOperator::LOOP_BREAK) {
                // 计算跳转偏移: 从当前位置跳转到循环结束
                // VM 中 JUMP 执行: pc = pc + 1 + offset
                // 我们希望跳到 loopEndIndex，所以: loopEndIndex = i + 1 + offset
                // 即: offset = loopEndIndex - i - 1
                int offset = static_cast<int>(loopEndIndex) - static_cast<int>(i) - 1;
                ir[i].op = VMOperator::JUMP;  // 将 LOOP_BREAK 转换为 JUMP
                ir[i].args[0] = LexerType::Value(offset);
            }
        }
    }

    struct ForBuildInput {
        const TreeParser::CodeBlock& block;
        std::vector<IRType::IRInstr> bodyIR;
        std::function<std::vector<IRType::IRInstr>(const std::vector<LexerType::Token>&)> emitExpr; // 回调：表达式 -> IR
    };

    inline std::vector<IRType::IRInstr> buildListForIR(const ForBuildInput& in) {
        std::vector<IRType::IRInstr> out;

        // 基本语义：
        //   listExpr 只求值一次 -> 存隐藏 list 槽 + 长度槽 + index 槽 + 绑定变量槽
        //   模式：for [ &? name ] : expr { body }
        //   当前仅实现 copy 绑定（&/const& 后续扩展）。

        if (in.block.forKind != TreeParser::CodeBlock::ForKind::LIST || !in.emitExpr) {
            return out;
        }

        if (in.block.forBindings.empty()) {
            return out;
        }

        const auto &binding = in.block.forBindings[0];

        int listSlot = in.block.loopHidden.listSlot;
        int lenSlot  = in.block.loopHidden.lenSlot;
        int idxSlot  = in.block.loopHidden.idxSlot;
        int elemSlot = -1;
        if (!binding.isRef) {
            // 复制模式：用户变量槽已由 parseCodeBlock declareLocal -> 需解析一次获取索引
            auto sym = IRScope::resolve(binding.name);
            elemSlot = sym.index;
        }

        // 1. 求值 list 表达式
    auto listIR = in.emitExpr(in.block.listExprTokens);
    out.insert(out.end(), listIR.begin(), listIR.end());

        // 2. STORE list
        {
            out.push_back(IRUtils::makeStoreLocal(listSlot, false));
        }

        // 3. 取长度 -> len 槽
        {
            out.push_back(IRUtils::makeLoadLocal(listSlot));
            IRType::IRInstr len; len.op = VMOperator::LIST_LENGTH; out.push_back(len);
            out.push_back(IRUtils::makeStoreLocal(lenSlot, false));
        }

        // 4. index = 0
        {
            out.push_back(IRUtils::makePushInt(0));
            out.push_back(IRUtils::makeStoreLocal(idxSlot, false));
        }

        // 5. 进入循环：记录起始位置
        size_t loopStartIndex = out.size();

        // 5.1 载入 index 与 len 比较
        {
            out.push_back(IRUtils::makeLoadLocal(idxSlot));
            out.push_back(IRUtils::makeLoadLocal(lenSlot));
            out.push_back(IRUtils::makeLessThan());
            out.push_back(IRUtils::makeJumpIfFalsePlaceholder());
        }

        size_t jfalseIndex = out.size() - 1; // 最后插入的指令

        // 5.2 加载元素 -> 绑定槽 (copy 模式)。引用模式下跳过，由引用解析动态加载。
        if (!binding.isRef) {
            out.push_back(IRUtils::makeLoadLocal(listSlot));
            out.push_back(IRUtils::makeLoadLocal(idxSlot));
            out.push_back(IRUtils::makeListLoadElem());
            out.push_back(IRUtils::makeStoreLocal(elemSlot, binding.isConst));
            // register constness for the user local variable
            IRScope::markLocalConst(binding.name, binding.isConst);
        }

        // 记录 body 开始位置（用于 break 回填）
        size_t bodyStartIndex = out.size();

        // 5.3 插入 body IR
        out.insert(out.end(), in.bodyIR.begin(), in.bodyIR.end());

        // 5.4 index++
        {
            out.push_back(IRUtils::makeLoadLocal(idxSlot));
            out.push_back(IRUtils::makePushInt(1));
            out.push_back(IRUtils::makeAdd());
            out.push_back(IRUtils::makeStoreLocal(idxSlot, false));
        }

        // 5.5 跳回循环开头
        // 说明：回跳 offset 需要满足：pc + 1 + off == loopStartIndex
        // 此时 out.size() 为即将插入 JUMP 的索引 N
        // 期望 off = loopStartIndex - (N + 1)
        // 之前实现遗漏 +1 导致第二次迭代从条件块第二条指令开始，跳过 idx 加载，造成一次性循环
        {
            int backOffset = static_cast<int>(loopStartIndex) - static_cast<int>(out.size() + 1);
            out.push_back(IRUtils::makeJump(backOffset));
        }

        // 循环结束位置（break 跳转目标）
        size_t loopEndIndex = out.size();

        // 5.6 回填跳出偏移
        int skip = static_cast<int>(out.size() - (jfalseIndex + 1));
        out[jfalseIndex].args[0] = LexerType::Value(skip);

        // 回填 body 中的 LOOP_BREAK 占位符
        patchBreakPlaceholders(out, bodyStartIndex, loopEndIndex);

        return out;
    }

    inline std::vector<IRType::IRInstr> buildDictForIR(const ForBuildInput& in) {
        std::vector<IRType::IRInstr> out;

        if (in.block.forKind != TreeParser::CodeBlock::ForKind::DICT || !in.emitExpr) {
            return out;
        }

        if (in.block.forBindings.size() != 2) {
            return out; // 语法错误保护
        }

        const auto &keyBind = in.block.forBindings[0];
        const auto &valBind = in.block.forBindings[1];

        // 分配隐藏槽（已在 block.loopHidden 中预留 listSlot, lenSlot, idxSlot) 复用:
        //   listSlot -> dictSlot
        //   lenSlot  -> lenSlot (键数量)
        //   idxSlot  -> idx
        // 额外需要：keys 数组槽、当前 key 临时槽（供引用 value 使用）
        int dictSlot = in.block.loopHidden.listSlot;
        int lenSlot  = in.block.loopHidden.lenSlot;
        int idxSlot  = in.block.loopHidden.idxSlot;
    int keysSlot = in.block.loopHidden.dictKeysSlot;
    int curKeySlot = in.block.loopHidden.dictCurKeySlot;

        // 用户变量槽（非引用模式）
        int userKeySlot = -1;
        int userValSlot = -1;

        if (!keyBind.isRef) {
            auto s = IRScope::resolve(keyBind.name);
            userKeySlot = s.index;
        }
        if (!valBind.isRef) {
            auto s = IRScope::resolve(valBind.name);
            userValSlot = s.index;
        }

        // 1. 求值 dictExpr
        auto dictIR = in.emitExpr(in.block.dictExprTokens);
        out.insert(out.end(), dictIR.begin(), dictIR.end());

        // 2. 存 dict
        out.push_back(IRUtils::makeStoreLocal(dictSlot, false));

        // 3. 构建 keys 数组（字典在栈顶需先加载）
        out.push_back(IRUtils::makeLoadLocal(dictSlot));
        out.push_back(IRUtils::makeDictKeysBuild());
        out.push_back(IRUtils::makeStoreLocal(keysSlot, false));

        // 4. len = keys.length
        out.push_back(IRUtils::makeLoadLocal(keysSlot));
        IRType::IRInstr lenop; lenop.op = VMOperator::LIST_LENGTH; out.push_back(lenop);
        out.push_back(IRUtils::makeStoreLocal(lenSlot, false));

        // 5. idx = 0
        out.push_back(IRUtils::makePushInt(0));
        out.push_back(IRUtils::makeStoreLocal(idxSlot, false));

        size_t loopStartIndex = out.size();

        // 6. 条件 idx < len
        out.push_back(IRUtils::makeLoadLocal(idxSlot));
        out.push_back(IRUtils::makeLoadLocal(lenSlot));
        out.push_back(IRUtils::makeLessThan());
        out.push_back(IRUtils::makeJumpIfFalsePlaceholder());
        size_t jfalseIndex = out.size() - 1;

        // 7. 取当前 key -> curKeySlot
        out.push_back(IRUtils::makeLoadLocal(keysSlot));
        out.push_back(IRUtils::makeLoadLocal(idxSlot));
    out.push_back(IRUtils::makeListLoadElem());
    // curKeySlot 需可重复写入每轮循环，不能标记为 const
    out.push_back(IRUtils::makeStoreLocal(curKeySlot, false));

        // 8. key 绑定：永远 const （即使写了 & 也视为 const copy）。
        if (!keyBind.isRef) {
            out.push_back(IRUtils::makeLoadLocal(curKeySlot));
            // copy 模式 key 也需要每轮覆盖，因此不标记 const；若要禁止用户赋值，需未来注册只读引用而非局部。
            out.push_back(IRUtils::makeStoreLocal(userKeySlot, false));
        }

        // 9. value 绑定
        //    如果是引用 -> 注册引用；否则复制当前 value
        if (!valBind.isRef) {
            // 加载 value： dict + key -> DICT_LOAD_VALUE
            out.push_back(IRUtils::makeLoadLocal(dictSlot));
            out.push_back(IRUtils::makeLoadLocal(curKeySlot));
            out.push_back(IRUtils::makeDictLoadValue());
            out.push_back(IRUtils::makeStoreLocal(userValSlot, valBind.isConst));
            IRScope::markLocalConst(valBind.name, valBind.isConst);
        }

        // 记录 body 开始位置（用于 break 回填）
        size_t bodyStartIndex = out.size();

        // 10. body
        out.insert(out.end(), in.bodyIR.begin(), in.bodyIR.end());

        // 11. idx++
        out.push_back(IRUtils::makeLoadLocal(idxSlot));
        out.push_back(IRUtils::makePushInt(1));
        out.push_back(IRUtils::makeAdd());
        out.push_back(IRUtils::makeStoreLocal(idxSlot, false));

        // 12. 回跳
        {
            int backOffset = static_cast<int>(loopStartIndex) - static_cast<int>(out.size() + 1);
            out.push_back(IRUtils::makeJump(backOffset));
        }

        // 循环结束位置（break 跳转目标）
        size_t loopEndIndex = out.size();

        int skip = static_cast<int>(out.size() - (jfalseIndex + 1));
        out[jfalseIndex].args[0] = LexerType::Value(skip);

        // 回填 body 中的 LOOP_BREAK 占位符
        patchBreakPlaceholders(out, bodyStartIndex, loopEndIndex);

        return out;
    }

    inline std::vector<IRType::IRInstr> buildCountForIR(const ForBuildInput& in) {
        std::vector<IRType::IRInstr> out;

        if (!(in.block.forKind == TreeParser::CodeBlock::ForKind::COUNT || in.block.forKind == TreeParser::CodeBlock::ForKind::COUNT_SIMPLE) || !in.emitExpr) {
            return out;
        }
        bool hasBinding = !in.block.forBindings.empty();
        TreeParser::CodeBlock::ForBinding binding; // 默认空
        if (hasBinding) binding = in.block.forBindings[0];

        auto addrStr = [&]() -> std::string {
            std::ostringstream oss;
            oss << (const void*)&in.block;
            return oss.str();
        }();

        int idxSlot  = in.block.loopHidden.idxSlot;
        int endSlot  = in.block.loopHidden.endSlot;
        int stepSlot = in.block.loopHidden.stepSlot;
        auto firstTokenLine = [](const std::vector<TreeParser::Token>& tokens) {
            if (tokens.empty()) return -1;
            return std::visit(
                [](const auto& token) { return token.line; },
                tokens.front()
            );
        };

        int userIdxSlot = -1;
        if (hasBinding && !binding.isRef) {
            auto sym = IRScope::resolve(binding.name);
            userIdxSlot = sym.index;
        }

        // start (COUNT_SIMPLE: 隐式 0)
        if (in.block.forKind == TreeParser::CodeBlock::ForKind::COUNT_SIMPLE) {
            out.push_back(IRUtils::makePushInt(0));
        } else {
            auto startIR = in.emitExpr(in.block.countStartTokens);
            out.insert(out.end(), startIR.begin(), startIR.end());
        }
        {
            out.push_back(IRUtils::makeStoreLocal(idxSlot, false));
        }

        // end
        auto endIR = in.emitExpr(in.block.countEndTokens);
        out.insert(out.end(), endIR.begin(), endIR.end());
        {
            out.push_back(IRUtils::makeStoreLocal(endSlot, false));
        }

        // COUNT_CHECK_NEG (对 end 做非负检查)
        {
            out.push_back(IRUtils::makeLoadLocal(endSlot));
            auto check = IRUtils::makeCountCheckNeg();
            check.line = firstTokenLine(in.block.countEndTokens);
            out.push_back(std::move(check));
        }

        // step (默认 1)
        if (!in.block.countStepTokens.empty()) {
            auto stepIR = in.emitExpr(in.block.countStepTokens);
            out.insert(out.end(), stepIR.begin(), stepIR.end());
        } else {
            out.push_back(IRUtils::makePushInt(1));
        }
        {
            out.push_back(IRUtils::makeStoreLocal(stepSlot, false));
        }

        // COUNT_CHECK_STEP_ZERO (检查步长不能为0)
        {
            out.push_back(IRUtils::makeLoadLocal(stepSlot));
            auto check = IRUtils::makeCountCheckStepZero();
            check.line = in.block.countStepTokens.empty()
                ? firstTokenLine(in.block.countEndTokens)
                : firstTokenLine(in.block.countStepTokens);
            out.push_back(std::move(check));
        }

        size_t loopStartIndex = out.size();

        // loop condition: depends on step sign and inclusive flag
        // We'll implement two cases:
        //  - If step is a compile-time literal (pushed earlier), use a static comparison ( < or <= / > or >= )
        //  - Otherwise emit a runtime branch: if step > 0 then use idx < end (or <= when inclusive), else if step < 0 use idx > end (or >= when inclusive), else runtime error

        // Helper: emit cmp idx ? end and a placeholder jump
        auto emitCmpAndJFalse = [&](bool greaterThanComparison, bool inclusive)->size_t {
            if (!greaterThanComparison) {
                // idx < end  or idx <= end
                out.push_back(IRUtils::makeLoadLocal(idxSlot));
                out.push_back(IRUtils::makeLoadLocal(endSlot));
                out.push_back(inclusive ? IRUtils::makeLeOp() : IRUtils::makeLessThan());
                out.push_back(IRUtils::makeJumpIfFalsePlaceholder());
            } else {
                // idx > end or idx >= end
                out.push_back(IRUtils::makeLoadLocal(idxSlot));
                out.push_back(IRUtils::makeLoadLocal(endSlot));
                out.push_back(inclusive ? IRUtils::makeGeOp() : IRUtils::makeGtOp());
                out.push_back(IRUtils::makeJumpIfFalsePlaceholder());
            }
            return out.size() - 1;
        };

        std::vector<size_t> exitJumpIndices;

        // If step was pushed as a literal and is a Value BigInteger, we can inspect it here.
        // The emitted code earlier stored step into stepSlot; however we don't have the literal value here easily.
        // Instead, try a simple heuristic: if countStepTokens was a single literal token, use compile-time path.
        // Also: if countStepTokens is empty then the parser/emit defaulted step to 1 -> treat as static positive step.
        bool usedStaticStepPath = false;
        if (in.block.countStepTokens.empty()) {
            // default step == 1 (positive)
            exitJumpIndices.push_back(
                emitCmpAndJFalse(false, in.block.countEndInclusive)
            );
            usedStaticStepPath = true;
        }

        if (!usedStaticStepPath && !in.block.countStepTokens.empty() && in.block.countStepTokens.size() == 1 && std::holds_alternative<LexerType::Value>(in.block.countStepTokens[0])) {
            // Extract literal sign
            const auto &tk = in.block.countStepTokens[0];
            const auto &val = std::get<LexerType::Value>(tk);
            if (std::holds_alternative<ZeusType::BigInteger>(val.data)) {
                ZeusType::BigInteger v = std::get<ZeusType::BigInteger>(val.data);
                if (v == ZeusType::BigInteger(0)) {
                    // COUNT_CHECK_STEP_ZERO reports the runtime error before
                    // entering this condition. Keep unreachable IR well-formed.
                    out.push_back(IRUtils::makePushBool(false));
                    out.push_back(IRUtils::makeJumpIfFalsePlaceholder());
                    exitJumpIndices.push_back(out.size() - 1);
                    usedStaticStepPath = true;
                } else if (v > ZeusType::BigInteger(0)) {
                    // positive step
                    exitJumpIndices.push_back(
                        emitCmpAndJFalse(false, in.block.countEndInclusive)
                    );
                    usedStaticStepPath = true;
                } else {
                    // negative step
                    exitJumpIndices.push_back(
                        emitCmpAndJFalse(true, in.block.countEndInclusive)
                    );
                    usedStaticStepPath = true;
                }
            }
        }

        if (!usedStaticStepPath) {
            // Route positive and negative steps to separate bound checks.
            out.push_back(IRUtils::makeLoadLocal(stepSlot));
            out.push_back(IRUtils::makePushInt(0));
            out.push_back(IRUtils::makeGtOp());
            out.push_back(IRUtils::makeJumpIfFalsePlaceholder());
            const size_t jumpToNegativeCheck = out.size() - 1;

            exitJumpIndices.push_back(
                emitCmpAndJFalse(false, in.block.countEndInclusive)
            );
            out.push_back(IRUtils::makeJump(0));
            const size_t positiveJumpToBody = out.size() - 1;

            const size_t negativeCheckStart = out.size();
            const int negativeCheckOffset = static_cast<int>(
                negativeCheckStart - (jumpToNegativeCheck + 1)
            );
            out[jumpToNegativeCheck].args[0] = LexerType::Value(negativeCheckOffset);

            out.push_back(IRUtils::makeLoadLocal(stepSlot));
            out.push_back(IRUtils::makePushInt(0));
            out.push_back(IRUtils::makeLtOp());
            out.push_back(IRUtils::makeJumpIfFalsePlaceholder());
            exitJumpIndices.push_back(out.size() - 1);

            exitJumpIndices.push_back(
                emitCmpAndJFalse(true, in.block.countEndInclusive)
            );

            const size_t bodyStart = out.size();
            const int positiveBodyOffset = static_cast<int>(
                bodyStart - (positiveJumpToBody + 1)
            );
            out[positiveJumpToBody].args[0] = LexerType::Value(positiveBodyOffset);
        }


        // body 之前：若是复制模式，需要把 idx 当前值拷贝到用户变量槽
        if (hasBinding && !binding.isRef) {
            out.push_back(IRUtils::makeLoadLocal(idxSlot));
            out.push_back(IRUtils::makeStoreLocal(userIdxSlot, binding.isConst));
            IRScope::markLocalConst(binding.name, binding.isConst);
        }

        // 记录 body 开始位置（用于 break 回填）
        size_t bodyStartIndex = out.size();

        out.insert(out.end(), in.bodyIR.begin(), in.bodyIR.end());

        // idx = idx + step
        {
            out.push_back(IRUtils::makeLoadLocal(idxSlot));
            out.push_back(IRUtils::makeLoadLocal(stepSlot));
            out.push_back(IRUtils::makeAdd());
            out.push_back(IRUtils::makeStoreLocal(idxSlot, false));
        }

        // back jump
        // 计算逻辑同上，确保回到条件首指令（idx 加载），而不是少一条导致比较使用错误操作数
        {
            int backOffset = static_cast<int>(loopStartIndex) - static_cast<int>(out.size() + 1);
            out.push_back(IRUtils::makeJump(backOffset));
        }

        // 循环结束位置（break 跳转目标）
        size_t loopEndIndex = out.size();

    for (const size_t exitJumpIndex : exitJumpIndices) {
        const int skip = static_cast<int>(out.size() - (exitJumpIndex + 1));
        out[exitJumpIndex].args[0] = LexerType::Value(skip);
    }

        // 回填 body 中的 LOOP_BREAK 占位符
        patchBreakPlaceholders(out, bodyStartIndex, loopEndIndex);

        return out;
    }

    inline std::vector<IRType::IRInstr> buildCondForIR(const ForBuildInput& in) {
        std::vector<IRType::IRInstr> out;

        if (in.block.forKind != TreeParser::CodeBlock::ForKind::COND || !in.emitExpr) {
            return out;
        }

        size_t loopStartIndex = out.size();

        // 条件表达式
        auto condIR = in.emitExpr(in.block.condExprTokens);
        out.insert(out.end(), condIR.begin(), condIR.end());

        IRType::IRInstr jfalse;
        jfalse.op = VMOperator::JUMP_IF_FALSE;
        jfalse.args.push_back(LexerType::Value(0));
        out.push_back(std::move(jfalse));
        size_t jfalseIndex = out.size() - 1;

        // 记录 body 开始位置（用于 break 回填）
        size_t bodyStartIndex = out.size();

        // body
        out.insert(out.end(), in.bodyIR.begin(), in.bodyIR.end());

        // 回跳到循环开始（loopStartIndex）
        // VM中JUMP执行: pc = pc + 1 + off
        // 我们希望跳到loopStartIndex，所以: loopStartIndex = pc + 1 + off
        // 即: off = loopStartIndex - pc - 1
        // 当前pc是JUMP指令的位置，即out.size()
        {
            int backOffset = static_cast<int>(loopStartIndex) - static_cast<int>(out.size()) - 1;
            IRType::IRInstr jmp;
            jmp.op = VMOperator::JUMP;
            jmp.args.push_back(LexerType::Value(backOffset));
            out.push_back(std::move(jmp));
        }

        // 循环结束位置（break 跳转目标）
        size_t loopEndIndex = out.size();

        int skip = static_cast<int>(out.size() - (jfalseIndex + 1));
        out[jfalseIndex].args[0] = LexerType::Value(skip);

        // 回填 body 中的 LOOP_BREAK 占位符
        patchBreakPlaceholders(out, bodyStartIndex, loopEndIndex);

        return out;
    }
}

#endif
