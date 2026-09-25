// 轻量 IR 指令构造工具
// 规则：
//  1. 每个逻辑块前有空行。
//  2. 不压缩多语句到同一行。
//  3. 仅提供常用、重复出现的指令封装，避免到处手写模板代码。
//  4. 不含任何全局可变状态。

#ifndef IR_UTILS_HPP
#define IR_UTILS_HPP

#include <vector>
#include <string>
#include "IRType.hpp"
#include "../ZeusRunner/ZeusVMOperator.hpp"
#include "../ZeusParser/LexerType.hpp"

namespace IRUtils {

	inline IRType::IRInstr makePushInt(int v) {
		IRType::IRInstr ins;
		ins.op = VMOperator::PUSH;
		ins.args.push_back(LexerType::Value(v));
		return ins;
	}

	inline IRType::IRInstr makePushBool(bool v) {
		IRType::IRInstr ins;
		ins.op = VMOperator::PUSH;
		ins.args.push_back(LexerType::Value(v));
		return ins;
	}

	inline IRType::IRInstr makePushString(const std::string& s) {
		IRType::IRInstr ins;
		ins.op = VMOperator::PUSH;
		ins.args.push_back(LexerType::Value(s));
		return ins;
	}

	inline IRType::IRInstr makeLoadLocal(int slot) {
		IRType::IRInstr ins;
		ins.op = VMOperator::LOAD_VALUE;
		ins.args.push_back(std::string("local"));
		ins.args.push_back(LexerType::Value(slot));
		return ins;
	}

	inline IRType::IRInstr makeStoreLocal(int slot, bool isConst) {
		IRType::IRInstr ins;
		ins.op = VMOperator::STORE_VALUE_LOCAL;
		ins.args.push_back(LexerType::Value(slot));
		ins.args.push_back(std::string(isConst ? "const" : "mutable"));
		return ins;
	}

	inline IRType::IRInstr makeAssignLocal(int slot) {
		IRType::IRInstr ins;
		ins.op = VMOperator::ASSIGN_VALUE_LOCAL;
		ins.args.push_back(LexerType::Value(slot));
		return ins;
	}

	inline IRType::IRInstr makeAssignGlobal(int slot) {
		IRType::IRInstr ins;
		ins.op = VMOperator::ASSIGN_VALUE_GLOBAL;
		ins.args.push_back(LexerType::Value(slot));
		return ins;
	}

	inline IRType::IRInstr makeStoreGlobal(int slot, bool isConst, const std::string* nameOpt = nullptr) {
		IRType::IRInstr ins;
		ins.op = VMOperator::STORE_VALUE_GLOBAL;
		ins.args.push_back(LexerType::Value(slot));
		ins.args.push_back(std::string(isConst ? "const" : "mutable"));
		if (nameOpt) {
			ins.args.push_back(*nameOpt);
		}
		return ins;
	}

	inline IRType::IRInstr makeJumpIfFalsePlaceholder() {
		IRType::IRInstr ins;
		ins.op = VMOperator::JUMP_IF_FALSE;
		ins.args.push_back(LexerType::Value(0));
		return ins;
	}

	inline IRType::IRInstr makeJump(int offset) {
		IRType::IRInstr ins;
		ins.op = VMOperator::JUMP;
		ins.args.push_back(LexerType::Value(offset));
		return ins;
	}

	// 创建 break 占位符指令（使用 LOOP_BREAK）
	// offset 将在循环 IR 生成完成后回填
	inline IRType::IRInstr makeLoopBreakPlaceholder() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LOOP_BREAK;
		ins.args.push_back(LexerType::Value(0));  // 占位符
		return ins;
	}

	inline IRType::IRInstr makeListBuild(int count) {
		IRType::IRInstr ins;
		ins.op = VMOperator::LIST_BUILD;
		ins.args.push_back(LexerType::Value(count));
		return ins;
	}

	inline IRType::IRInstr makeListLoadElem() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LIST_LOAD_ELEM;
		return ins;
	}

	inline IRType::IRInstr makeListStoreElem() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LIST_STORE_ELEM;
		return ins;
	}

	inline IRType::IRInstr makeDictBuild(int pairCount) {
		IRType::IRInstr ins;
		ins.op = VMOperator::DICT_BUILD;
		ins.args.push_back(LexerType::Value(pairCount));
		return ins;
	}

	inline IRType::IRInstr makeDictKeysBuild() {
		IRType::IRInstr ins;
		ins.op = VMOperator::DICT_KEYS_BUILD;
		return ins;
	}

	inline IRType::IRInstr makeDictLoadValue() {
		IRType::IRInstr ins;
		ins.op = VMOperator::DICT_LOAD_VALUE;
		return ins;
	}

	inline IRType::IRInstr makeDictStoreValue() {
		IRType::IRInstr ins;
		ins.op = VMOperator::DICT_STORE_VALUE;
		return ins;
	}

	inline IRType::IRInstr makeCountCheckNeg() {
		IRType::IRInstr ins;
		ins.op = VMOperator::COUNT_CHECK_NEG;
		return ins;
	}

	inline IRType::IRInstr makeCountCheckStepZero() {
		IRType::IRInstr ins;
		ins.op = VMOperator::COUNT_CHECK_STEP_ZERO;
		return ins;
	}

	inline IRType::IRInstr makeAdd() {
		IRType::IRInstr ins;
		ins.op = VMOperator::ADD;
		return ins;
	}

	inline IRType::IRInstr makeLessThan() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LESS_THAN;
		return ins;
	}

	inline IRType::IRInstr makeReturn() {
		IRType::IRInstr ins;
		ins.op = VMOperator::RETURN;
		return ins;
	}

	inline IRType::IRInstr makePop() {
		IRType::IRInstr ins;
		ins.op = VMOperator::POP;
		return ins;
	}

	// ========== Binary Operators ==========

	inline IRType::IRInstr makeAddOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::ADD;
		return ins;
	}

	inline IRType::IRInstr makeSubOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::SUBTRACT;
		return ins;
	}

	inline IRType::IRInstr makeMulOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::MULTIPLY;
		return ins;
	}

	inline IRType::IRInstr makeDivOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::DIVIDE;
		return ins;
	}

	inline IRType::IRInstr makeModOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::MODULUS;
		return ins;
	}

	inline IRType::IRInstr makeEqOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::EQUL;
		return ins;
	}

	inline IRType::IRInstr makeNotEqOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::NOT_EQUAL;
		return ins;
	}

	inline IRType::IRInstr makeGtOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::GREATER_THAN;
		return ins;
	}

	inline IRType::IRInstr makeLtOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LESS_THAN;
		return ins;
	}

	inline IRType::IRInstr makeGeOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::GREATER_EQUAL;
		return ins;
	}

	inline IRType::IRInstr makeLeOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LESS_EQUAL;
		return ins;
	}

	inline IRType::IRInstr makeAndOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LOGICAL_AND;
		return ins;
	}

	inline IRType::IRInstr makeOrOp() {
		IRType::IRInstr ins;
		ins.op = VMOperator::LOGICAL_OR;
		return ins;
	}

	inline IRType::IRInstr makeBinaryOpBySymbol(const std::string& op) {
		if (op == "+") return makeAddOp();
		if (op == "-") return makeSubOp();
		if (op == "*") return makeMulOp();
		if (op == "/") return makeDivOp();
		if (op == "%") return makeModOp();
		if (op == "==") return makeEqOp();
		if (op == "!=") return makeNotEqOp();
		if (op == ">") return makeGtOp();
		if (op == "<") return makeLtOp();
		if (op == ">=") return makeGeOp();
		if (op == "<=") return makeLeOp();
		if (op == "and") return makeAndOp();
		if (op == "or") return makeOrOp();
		return IRType::IRInstr{}; // 未知操作返回默认(op 未设置,调用方可忽略)
	}

} // namespace IRUtils

#endif // IR_UTILS_HPP
