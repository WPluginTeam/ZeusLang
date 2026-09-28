
#ifndef IRGenerator_hpp
#define IRGenerator_hpp

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "../ZeusParser/TreeType.hpp"
#include "../ZeusParser/TreeParser.hpp" 
#include "../ZeusParser/FunctionTypeSyntax.hpp"

#include "../ZeusRunner/ZeusVMOperator.hpp"

#include "IRType.hpp"
#include "IRPriorityFold.hpp"
#include "IRException.hpp"
#include "IRScope.hpp"
#include "IRDebugUtils.hpp"
#include "IRImport.hpp"
#include "../ZeusRunner/ZeusBuiltins.hpp"
#include "../ZeusBuiltins/BuiltinRegistry.hpp"
#include "../ZeusBuiltins/NativeTypeABI.hpp"

#include "IRFor.hpp"
#include "IRUtils.hpp"
#include "../ZeusTyping/TypeModel.hpp"
#ifdef ZEUS_DEBUG_FILE_CHANNEL
#undef ZEUS_DEBUG_FILE_CHANNEL
#endif
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_IR
#include "../ZeusDebug/Utils.hpp"
// ===== 抽取：表达式 -> IR 指令（递归下降版，支持括号/函数调用/二元运算）=====
namespace IRGen {

    class IRGenerator; // 前置声明，供解析辅助函数使用

    // 表达式解析上下文：只携带状态与严格模式所需信息，避免传整个 IRGenerator
    struct ExprParseCtx {
        IRType::IRCompilerStatus& status;
        bool strictDecl;
        const std::unordered_set<std::string>& futureFuncDefs;
        bool useOriginal = false;  // 如果为true,不包装[]为Array对象,直接使用原生list
    };

    // 在本命名空间内前向声明 emitExprIR，便于在定义之前的辅助解析里调用
    std::vector<IRType::IRInstr> emitExprIR(
        IRType::IRCompilerStatus& status,
        bool strictDecl,
        const std::unordered_set<std::string>& futureFuncDefs,
        const std::vector<LexerType::Token>& inTokens);

    // --- 基础工具 ---
    static bool _isValue(const LexerType::Token& tk) {
        return std::holds_alternative<LexerType::Value>(tk);
    }

    static bool _isBytesLiteral(const LexerType::Token& tk) {
        return std::holds_alternative<LexerType::BytesLiteral>(tk);
    }

    static bool _isKeyword(const LexerType::Token& tk) {
        return std::holds_alternative<LexerType::Keyword>(tk);
    }

    static bool _isOperator(const LexerType::Token& tk) {
        return std::holds_alternative<LexerType::Operator>(tk);
    }

    static std::string _opSym(const LexerType::Token& tk) {
        return std::get<LexerType::Operator>(tk).symbol;
    }

    static std::string _nativeConstructionType(const std::string& name) {
        const auto* abi = ZeusBuiltins::findNativeTypeABI(name);
        return abi == nullptr ? std::string() : std::string(abi->canonicalName);
    }

    static int _getTokenLine(const LexerType::Token& tk) {
        if (std::holds_alternative<LexerType::Value>(tk)) {
            return std::get<LexerType::Value>(tk).line;
        } else if (std::holds_alternative<LexerType::BytesLiteral>(tk)) {
            return std::get<LexerType::BytesLiteral>(tk).line;
        } else if (std::holds_alternative<LexerType::Keyword>(tk)) {
            return std::get<LexerType::Keyword>(tk).line;
        } else if (std::holds_alternative<LexerType::Operator>(tk)) {
            return std::get<LexerType::Operator>(tk).line;
        }
        return -1;
    }

    static IRType::SymbolRef _makeSymbolRef(
        const std::string& name,
        const IRScope::SymRef& symbol
    ) {
        IRType::SymbolRef result;
        result.name = name;
        result.index = symbol.index;
        result.aux = symbol.aux;
        result.isConst = symbol.isConst;

        if (symbol.kind == IRScope::Kind::UNDEFINED && IRScope::isFunctionName(name)) {
            result.kind = IRType::SymbolRefKind::Function;
            return result;
        }

        switch (symbol.kind) {
            case IRScope::Kind::LOCAL:
                result.kind = IRType::SymbolRefKind::Local;
                break;
            case IRScope::Kind::GLOBAL:
                result.kind = IRType::SymbolRefKind::Global;
                break;
            case IRScope::Kind::UPVALUE:
                result.kind = IRType::SymbolRefKind::Upvalue;
                break;
            case IRScope::Kind::SHARED:
                result.kind = IRType::SymbolRefKind::Shared;
                break;
            case IRScope::Kind::LIST_ELEM_REF:
                result.kind = IRType::SymbolRefKind::ListElement;
                break;
            case IRScope::Kind::DICT_KEY_REF:
                result.kind = IRType::SymbolRefKind::DictKey;
                break;
            case IRScope::Kind::DICT_VAL_REF:
                result.kind = IRType::SymbolRefKind::DictValue;
                break;
            case IRScope::Kind::COUNT_INDEX_REF:
                result.kind = IRType::SymbolRefKind::CountIndex;
                break;
            default:
                result.kind = IRType::SymbolRefKind::Unknown;
                break;
        }

        return result;
    }

    static std::string _canonicalIRTypeName(const std::string& sourceName) {
        const ZeusTypeInfer::TypeRef type = ZeusTypeInfer::parseTypeRef(sourceName);
        return type.isKnown() ? type.toIRTypeName() : sourceName;
    }

    static std::string _operatorFunctionName(
        const TreeParser::CodeBlock& codeBlock
    ) {
        if (codeBlock.ownerClassName.empty()) return codeBlock.funcName;

        if (codeBlock.isOperatorOverload && !codeBlock.overloadedOps.empty()) {
            return codeBlock.ownerClassName + "::operator" +
                   codeBlock.overloadedOps.front();
        }

        return codeBlock.ownerClassName + "::" + codeBlock.funcName;
    }

    static std::string _operatorSymbolName(const std::string& value) {
        if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
            return value.substr(1, value.size() - 2);
        }
        return value;
    }

    static void _emitValueOrKeyword(ExprParseCtx& ctx, const LexerType::Token& tk, std::vector<IRType::IRInstr>& out) {
        int tokenLine = _getTokenLine(tk);

        if (std::holds_alternative<LexerType::Value>(tk)) {
            IRType::IRInstr ins;
            ins.op = VMOperator::PUSH;
            ins.line = tokenLine;
            ins.args.push_back(std::get<LexerType::Value>(tk));
            out.push_back(ins);
            return;
        }

        if (std::holds_alternative<LexerType::BytesLiteral>(tk)) {
            // bytes 字面量：将字节数据转换为 ZeusType::Bytes 并推入
            const auto& bytesLit = std::get<LexerType::BytesLiteral>(tk);
            ZeusType::Bytes bytes(bytesLit.data);
            
            IRType::IRInstr ins;
            ins.op = VMOperator::PUSH;
            ins.line = tokenLine;
            ins.args.push_back(bytes);
            out.push_back(ins);
            return;
        }

        if (std::holds_alternative<LexerType::Keyword>(tk)) {
            const std::string name = std::get<LexerType::Keyword>(tk).name;
            int tokenLine = _getTokenLine(tk);

            if (name == "true" || name == "false") {
                IRType::IRInstr ins;
                ins.op = VMOperator::PUSH;
                ins.line = tokenLine;
                ins.args.push_back(LexerType::Value(name == "true"));
                out.push_back(ins);
                return;
            }

            // nil 关键字 -> 推入 Zeus Null 值
            if (name == "nil") {
                IRType::IRInstr ins;
                ins.op = VMOperator::PUSH;
                ins.line = tokenLine;
                // Use a typed IR literal so no ordinary string can collide with nil.
                ins.args.push_back(IRType::NullLiteral{});
                out.push_back(ins);
                return;
            }

            IRScope::SymRef s = IRScope::resolve(name);
            if (s.kind == IRScope::Kind::UNDEFINED && IRScope::isCaptureAllActive()) {
                s = IRScope::forceCapture(name);
            }

            // 引用符号特殊处理：LIST_ELEM_REF / DICT_VAL_REF 需要动态取值；COUNT_INDEX_REF 当作局部槽；DICT_KEY_REF 直接 key 槽。
            if (s.kind == IRScope::Kind::LIST_ELEM_REF) {
                // list 槽在 s.index, 索引槽在 s.aux

                IRType::IRInstr loadList;
                loadList.op = VMOperator::LOAD_VALUE;
                loadList.line = tokenLine;
                loadList.args.push_back(std::string("local"));
                loadList.args.push_back(LexerType::Value((int)s.index));
                out.push_back(loadList);

                IRType::IRInstr loadIdx;
                loadIdx.op = VMOperator::LOAD_VALUE;
                loadIdx.line = tokenLine;
                loadIdx.args.push_back(std::string("local"));
                loadIdx.args.push_back(LexerType::Value((int)s.aux));
                out.push_back(loadIdx);

                IRType::IRInstr loadElem;
                loadElem.op = VMOperator::LIST_LOAD_ELEM;
                loadElem.line = tokenLine;
                out.push_back(loadElem);
                return;
            }

            if (s.kind == IRScope::Kind::DICT_KEY_REF) {
                IRType::IRInstr loadKey;
                loadKey.op = VMOperator::LOAD_VALUE;
                loadKey.line = tokenLine;
                loadKey.args.push_back(std::string("local"));
                loadKey.args.push_back(LexerType::Value((int)s.index)); // baseSlot 保存 key 槽
                out.push_back(loadKey);
                return;
            }

            if (s.kind == IRScope::Kind::DICT_VAL_REF) {
                // dict 槽在 s.index, key 槽在 s.aux
                IRType::IRInstr loadDict; loadDict.op = VMOperator::LOAD_VALUE; loadDict.line = tokenLine; loadDict.args.push_back(std::string("local")); loadDict.args.push_back(LexerType::Value((int)s.index)); out.push_back(loadDict);
                IRType::IRInstr loadCurKey; loadCurKey.op = VMOperator::LOAD_VALUE; loadCurKey.line = tokenLine; loadCurKey.args.push_back(std::string("local")); loadCurKey.args.push_back(LexerType::Value((int)s.aux)); out.push_back(loadCurKey);
                IRType::IRInstr dictLoad; dictLoad.op = VMOperator::DICT_LOAD_VALUE; dictLoad.line = tokenLine; out.push_back(dictLoad);
                return;
            }

            if (s.kind == IRScope::Kind::COUNT_INDEX_REF) {
                IRType::IRInstr loadIdx;
                loadIdx.op = VMOperator::LOAD_VALUE;
                loadIdx.line = tokenLine;
                loadIdx.args.push_back(std::string("local"));
                loadIdx.args.push_back(LexerType::Value((int)s.index));
                out.push_back(loadIdx);
                return;
            }

            IRType::IRInstr ins;
            ins.op = VMOperator::LOAD_VALUE;
            ins.line = tokenLine;

            if (s.kind == IRScope::Kind::SHARED) {
                // 共享变量：使用专用加载指令
                ins.op = VMOperator::LOAD_VALUE_SHARED;
                ins.args.push_back(name);
            }
            else if (s.kind == IRScope::Kind::LOCAL) {
                ins.args.push_back(std::string("local"));
                ins.args.push_back(LexerType::Value((int)s.index));
            }
            else if (s.kind == IRScope::Kind::GLOBAL) {
                ins.args.push_back(std::string("global"));
                ins.args.push_back(LexerType::Value((int)s.index));
            }
            else if (s.kind == IRScope::Kind::UPVALUE) {
                ins.args.push_back(std::string("upvalue"));
                ins.args.push_back(LexerType::Value((int)s.index));
            }
            else {
                // 未定义的变量
                // 允许 late-resolve（用于函数名、包内类名等）
                ins.args.push_back(name);
            }

            ins.metadata = IRType::IRMetadata::symbolRef(_makeSymbolRef(name, s));

            out.push_back(ins);
            return;
        }
    }

    // 二元操作发射已抽取到 IRUtils::makeBinaryOpBySymbol

    static void _emitCall(const std::string& name, int argc, std::vector<IRType::IRInstr>& out) {
        // 新调用约定：栈布局 callee, arg0, arg1, ..., argN-1 -> CALL_CLOSURE 弹出并推入返回值

        IRType::IRInstr call;
        call.op = VMOperator::CALL_CLOSURE;
        call.args.push_back(LexerType::Value(argc));
        out.push_back(call);
    }

    // 纯语义标注：动态 VM 忽略，LLVM 后端可据此恢复函数签名。
    // args:
    //   [0] "function_signature"
    //   [1] 函数名
    //   [2] 返回类型名（空表示 LLVM 走 ZeusValue out ABI）
    //   [3] 捕获数量
    //   [4] 普通参数数量
    //   [5..] 参数类型名（空表示该参数使用 ZeusValue* ABI）
    static IRType::IRInstr makeFunctionSignatureAnnotation(
        const TreeParser::CodeBlock& codeBlock,
        const std::string& inferredReturnType = std::string()
    ) {
        IRType::IRInstr annotation;
        annotation.op = VMOperator::IR_ANNOTATION;
        annotation.args.push_back(std::string("function_signature"));
        std::string signatureName = _operatorFunctionName(codeBlock);
        if (IRScope::isCompilingPackage && !IRScope::currentPackageName.empty() &&
            !codeBlock.isClassMethod && !signatureName.empty()) {
            signatureName = IRScope::currentPackageName + "." + signatureName;
        }
        annotation.args.push_back(signatureName);
        std::string declaredReturnType = codeBlock.returnTypeName;
        if (declaredReturnType.empty()) {
            if (!inferredReturnType.empty()) {
                declaredReturnType = inferredReturnType;
            } else if (!TreeParser::hasExplicitReturnStatement(codeBlock.nodes)) {
                declaredReturnType = "nil";
            }
        }
        const std::string returnTypeName = _canonicalIRTypeName(declaredReturnType);
        annotation.args.push_back(returnTypeName);
        annotation.args.push_back(LexerType::Value(0));
        annotation.args.push_back(LexerType::Value((int)codeBlock.paramNames.size()));

        std::vector<std::string> parameterTypes;
        parameterTypes.reserve(codeBlock.paramNames.size());
        for (size_t i = 0; i < codeBlock.paramNames.size(); ++i) {
            if (i < codeBlock.paramTypeNames.size()) {
                const std::string typeName = _canonicalIRTypeName(codeBlock.paramTypeNames[i]);
                const std::string resolvedType =
                    (i == 0 && codeBlock.isClassMethod &&
                     !codeBlock.isStaticMethod && !codeBlock.isCompileTimeStatic &&
                     typeName.empty())
                        ? codeBlock.ownerClassName
                        : typeName;
                annotation.args.push_back(resolvedType);
                parameterTypes.push_back(resolvedType);
            } else {
                const std::string resolvedType =
                    (i == 0 && codeBlock.isClassMethod &&
                     !codeBlock.isStaticMethod && !codeBlock.isCompileTimeStatic)
                        ? codeBlock.ownerClassName
                        : std::string();
                annotation.args.push_back(resolvedType);
                parameterTypes.push_back(resolvedType);
            }
        }

        annotation.metadata = IRType::IRMetadata::functionSignature(
            signatureName,
            returnTypeName,
            0,
            std::move(parameterTypes),
            codeBlock.ownerClassName,
            codeBlock.isOperatorOverload && !codeBlock.overloadedOps.empty()
                ? _operatorSymbolName(codeBlock.overloadedOps.front())
                : std::string()
        );

        return annotation;
    }

    static IRType::IRInstr makeExternalFunctionSignatureAnnotation(
        const ZeusParser::FunctionSignatureSyntax& signature
    ) {
        IRType::IRInstr annotation;
        annotation.op = VMOperator::IR_ANNOTATION;
        annotation.args.push_back(std::string("external_function_signature"));
        std::string signatureName = signature.name;
        if (IRScope::isCompilingPackage && !IRScope::currentPackageName.empty() &&
            !signatureName.empty()) {
            signatureName = IRScope::currentPackageName + "." + signatureName;
        }
        annotation.args.push_back(signatureName);
        const std::string returnType = _canonicalIRTypeName(signature.returnTypeName);
        annotation.args.push_back(returnType);
        annotation.args.push_back(LexerType::Value(0));
        annotation.args.push_back(LexerType::Value(
            static_cast<int>(signature.parameters.size())
        ));

        std::vector<std::string> parameterTypes;
        parameterTypes.reserve(signature.parameters.size());
        for (const auto& parameter : signature.parameters) {
            const std::string typeName = _canonicalIRTypeName(parameter.typeName);
            annotation.args.push_back(typeName);
            parameterTypes.push_back(typeName);
        }
        if (!signature.nativeSymbol.empty()) {
            annotation.args.push_back(signature.nativeSymbol);
        }
        annotation.metadata = IRType::IRMetadata::externalFunctionSignature(
            signatureName, returnType, std::move(parameterTypes),
            signature.nativeSymbol
        );
        return annotation;
    }

    // args:
    //   [0] "direct_call"
    //   [1] 目标函数名
    static IRType::IRInstr makeDirectCallAnnotation(const std::string& name) {
        IRType::IRInstr annotation;
        const std::string targetName = IRScope::functionTargetName(name);
        annotation.op = VMOperator::IR_ANNOTATION;
        annotation.args.push_back(std::string("direct_call"));
        annotation.args.push_back(targetName);
        annotation.metadata = IRType::IRMetadata::directCall(targetName);
        return annotation;
    }

    // 封装：给已经在栈顶的 callee 追加一串后缀 () () ... 调用
    

    // 递归下降解析：
    // Expr := Unary ((op) Expr[prec+1])*
    // Unary := ('+'|'-') Unary | Primary
    // Primary := Value | Ident CallOrLoad | '(' Expr ')'
    // CallOrLoad := '(' ArgList? ')' → CALL_FUNC | ε → LOAD_VALUE

    static bool _parseExpression(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, int minPrec, std::vector<IRType::IRInstr>& out);
    static bool _parseUnary(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, std::vector<IRType::IRInstr>& out);
    static bool _parsePrimary(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, std::vector<IRType::IRInstr>& out);

    // 辅助函数: 将栈顶的底层列表包装为Array对象
    // 输入: 栈顶为底层列表 [..., list]
    // 输出: 栈顶为Array对象 [..., Array对象]
    // 生成指令: LOAD Array类 -> SWAP -> OBJ_BUILD 1
    static void _wrapListAsArray(ExprParseCtx& ctx, std::vector<IRType::IRInstr>& out) {
        // 如果使用原生模式(using original),直接返回,不包装
        if (ctx.useOriginal) {
            ZEUS_DEBUG_PRINT("[IRGen] Using original list (not wrapping as Array)");
            return;
        }
        
        ZEUS_DEBUG_PRINT("[IRGen] _wrapListAsArray called (useOriginal=%d)", ctx.useOriginal);
        
        // 加载Array类(从全局符号表)
        // 注意: Array类在ZeusBuiltins.list包中,需要使用完整的包名前缀
        IRScope::SymRef arrayRef = IRScope::resolve("ZeusBuiltins.list.Array");
        
        // 如果找不到完整路径,尝试短名称(用于向后兼容)
        if (arrayRef.kind != IRScope::Kind::GLOBAL) {
            arrayRef = IRScope::resolve("Array");
        }
        
        ZEUS_DEBUG_PRINT("[IRGen] Array resolve result: kind=%d, index=%d", (int)arrayRef.kind, arrayRef.index);
        
        if (arrayRef.kind == IRScope::Kind::GLOBAL) {
            ZEUS_DEBUG_PRINT("[IRGen] Wrapping list as Array (global index=%d)", arrayRef.index);
            
            // 当前栈: [..., list]
            // 目标: [..., Array, list] 然后 OBJ_BUILD
            // OBJ_BUILD需要栈布局: [..., ClassHandle, arg1, arg2, ...]
            
            // 1. 加载Array类: [..., list] -> [..., list, Array]
            IRType::IRInstr loadArray;
            loadArray.op = VMOperator::LOAD_VALUE;
            loadArray.args.push_back(std::string("global"));
            loadArray.args.push_back(LexerType::Value(arrayRef.index));
            out.push_back(loadArray);
            
            // 2. 交换栈顶两个元素: [..., list, Array] -> [..., Array, list]
            IRType::IRInstr swap;
            swap.op = VMOperator::SWAP;
            out.push_back(swap);
            
            // 3. 构建Array对象: [..., Array, list] -> [..., Array对象]
            // OBJ_BUILD会弹出ClassHandle和参数,创建对象并推入栈
            IRType::IRInstr objBuild;
            objBuild.op = VMOperator::OBJ_BUILD;
            objBuild.args.push_back(LexerType::Value(1)); // 1个参数
            out.push_back(objBuild);
            
        } else {
            // Array未找到,保持原始列表
            ZEUS_DEBUG_PRINT("[IRGen] Warning: Array class not found, keeping raw list");
        }
    }

    static bool _parseArguments(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, int& argc, std::vector<IRType::IRInstr>& out) {
        argc = 0;
        // 当前 pos 指向 '('
        
        ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Starting at pos=%zu, tokens.size()=%zu", pos, tokens.size());
        
        if (pos >= tokens.size() || !_isOperator(tokens[pos]) || _opSym(tokens[pos]) != "(") {
            ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Not at '(' - failed");
            return false;
        }

        pos++; // consume '('

        // 空参
        if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == ")") {
            pos++; // consume ')'
            argc = 0;
            ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Empty arguments - success");
            return true;
        }

        // 至少一个参数
        while (true) {
            size_t argStart = pos;
            
            // 检查是否是命名参数: keyword = expression
            // 在括号内,单个keyword后跟=应该被识别为命名参数,而不是赋值语句
            bool isNamedArg = false;
            std::string paramName;
            
            if (pos < tokens.size() && _isKeyword(tokens[pos])) {
                ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Token at pos=%zu is Keyword: '%s'", 
                    pos, std::get<LexerType::Keyword>(tokens[pos]).name.c_str());
                if (pos + 1 < tokens.size()) {
                    // 检查下一个token类型
                    if (std::holds_alternative<LexerType::Operator>(tokens[pos + 1])) {
                        std::string opSym = _opSym(tokens[pos + 1]);
                        ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Next token is Operator: '%s'", opSym.c_str());
                        if (opSym == "=") {
                            // 这是命名参数: keyword = value
                            isNamedArg = true;
                            paramName = std::get<LexerType::Keyword>(tokens[pos]).name;
                            pos += 2; // 跳过 name 和 =
                            ZEUS_DEBUG_PRINT("[IRGen _parseArguments] *** Found named argument (explicit =): %s ***", paramName.c_str());
                        }
                    } else if (std::holds_alternative<LexerType::Value>(tokens[pos + 1])) {
                        // TreeParser可能已经处理了 keyword=value,把=吃掉了
                        // 如果keyword后面直接跟value,很可能是命名参数
                        // 但需要确认后面是 , 或 ) 而不是其他运算符
                        ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Next token is Value (possible named arg)");
                        bool likelyNamedArg = false;
                        if (pos + 2 < tokens.size()) {
                            if (std::holds_alternative<LexerType::Operator>(tokens[pos + 2])) {
                                std::string nextOp = _opSym(tokens[pos + 2]);
                                if (nextOp == "," || nextOp == ")") {
                                    likelyNamedArg = true;
                                }
                            }
                        } else {
                            // 如果是最后两个token,也可能是命名参数
                            likelyNamedArg = true;
                        }
                        
                        if (likelyNamedArg) {
                            // 这是被TreeParser处理过的命名参数
                            isNamedArg = true;
                            paramName = std::get<LexerType::Keyword>(tokens[pos]).name;
                            pos++; // 只跳过keyword,value由_parseExpression处理
                            ZEUS_DEBUG_PRINT("[IRGen _parseArguments] *** Found named argument (implicit =): %s ***", paramName.c_str());
                        }
                    } else if (std::holds_alternative<LexerType::Keyword>(tokens[pos + 1])) {
                        ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Next token is Keyword: '%s'", 
                            std::get<LexerType::Keyword>(tokens[pos + 1]).name.c_str());
                    } else {
                        ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Next token is UNKNOWN type");
                    }
                } else {
                    ZEUS_DEBUG_PRINT("[IRGen _parseArguments] No next token");
                }
            }
            
            // 解析参数值表达式
            if (!_parseExpression(ctx, tokens, pos, 1, out)) {
                ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Failed to parse argument %d", argc + 1);
                return false;
            }
            
            // 如果是命名参数,生成PUSH_NAMED指令
            if (isNamedArg) {
                IRType::IRInstr namedInstr;
                namedInstr.op = VMOperator::PUSH_NAMED;
                namedInstr.args.push_back(paramName);
                out.push_back(namedInstr);
                ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Generated PUSH_NAMED for '%s'", paramName.c_str());
            }
            
            argc++;
            ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Parsed argument %d (pos %zu -> %zu)", argc, argStart, pos);

            if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == ",") {
                pos++; // consume ','
                continue;
            }
            break;
        }

        if (pos >= tokens.size() || !_isOperator(tokens[pos]) || _opSym(tokens[pos]) != ")") {
            ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Missing ')' - failed");
            return false;
        }
        pos++; // consume ')'
        ZEUS_DEBUG_PRINT("[IRGen _parseArguments] Success - %d arguments", argc);
        return true;
    }

    // 不再单独函数：需要区分首个 callee 是否 native

    static bool _parsePrimary(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, std::vector<IRType::IRInstr>& out) {
        if (pos >= tokens.size()) {
            return false;
        }

        const auto& tk = tokens[pos];

        if (_isKeyword(tk) && pos + 3 < tokens.size() &&
            _isOperator(tokens[pos + 1]) && _opSym(tokens[pos + 1]) == "." &&
            _isKeyword(tokens[pos + 2]) &&
            _isOperator(tokens[pos + 3]) && _opSym(tokens[pos + 3]) == "(") {
            const std::string functionName =
                std::get<LexerType::Keyword>(tokens[pos]).name + "." +
                std::get<LexerType::Keyword>(tokens[pos + 2]).name;
            const IRScope::SymRef functionRef = IRScope::resolve(functionName);
            if (functionRef.kind == IRScope::Kind::GLOBAL &&
                IRScope::isStaticFunctionName(functionName)) {
                IRType::IRInstr load;
                load.op = VMOperator::LOAD_VALUE;
                load.line = _getTokenLine(tokens[pos]);
                load.args.push_back(std::string("global"));
                load.args.push_back(LexerType::Value(functionRef.index));
                load.args.push_back(functionName);
                out.push_back(load);

                pos += 3;
                int argc = 0;
                if (!_parseArguments(ctx, tokens, pos, argc, out)) return false;

                out.push_back(makeDirectCallAnnotation(functionName));
                IRType::IRInstr call;
                call.op = VMOperator::CALL_CLOSURE;
                call.line = _getTokenLine(tokens[pos - 1]);
                call.args.push_back(LexerType::Value(argc));
                out.push_back(call);
                return true;
            }
        }

        // Value
        
        if (_isValue(tk)) {
            _emitValueOrKeyword(ctx, tk, out);
            pos++;
            return true;
        }

        // BytesLiteral
        if (_isBytesLiteral(tk)) {
            _emitValueOrKeyword(ctx, tk, out);
            pos++;
            return true;
        }

        // List literal: [ elem (, elem)* ]
        // 生成底层列表,然后包装为Array对象
        if (_isOperator(tk) && _opSym(tk) == "[") {
            ZEUS_DEBUG_PRINT("[IRGen] _parsePrimary: Detected [ array literal");
            pos++;

            int elemCount = 0;

            if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "]") {
                ZEUS_DEBUG_PRINT("[IRGen] _parsePrimary: Empty array []");
                pos++;
                // 生成空列表
                IRType::IRInstr build;
                build.op = VMOperator::LIST_BUILD;
                build.args.push_back(LexerType::Value(elemCount));
                out.push_back(build);
                
                // 包装为Array对象: new Array(list)
                _wrapListAsArray(ctx, out);
                return true;
            }

            while (pos < tokens.size()) {
                if (!_parseExpression(ctx, tokens, pos, 1, out)) {
                    return false;
                }
                elemCount++;

                if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == ",") {
                    pos++;
                    continue;
                }
                break;
            }

            if (pos >= tokens.size() || !_isOperator(tokens[pos]) || _opSym(tokens[pos]) != "]") {
                return false;
            }

            pos++;

            ZEUS_DEBUG_PRINT("[IRGen] _parsePrimary: Array literal with %d elements", elemCount);
            
            // 生成列表
            IRType::IRInstr build;
            build.op = VMOperator::LIST_BUILD;
            build.args.push_back(LexerType::Value(elemCount));
            out.push_back(build);
            
            // 包装为Array对象: new Array(list)
            _wrapListAsArray(ctx, out);
            return true;
        }

        // Dict literal: { key : value (, key : value)* }
        // 规则：在表达式位置看到 '{' 且后续存在 ':' 判定为字典字面量；否则不是（可能是语句块，不应在此处进入）。
        if (_isOperator(tk) && _opSym(tk) == "{") {
            // 试探：向前看，找到与之匹配的 '}' 之前是否出现 ':'
            size_t look = pos + 1;
            int depthBrace = 1;
            bool hasColon = false;
            while (look < tokens.size() && depthBrace > 0) {
                if (_isOperator(tokens[look])) {
                    std::string sym = _opSym(tokens[look]);
                    if (sym == "{") depthBrace++;
                    else if (sym == "}") depthBrace--;
                    else if (sym == ":" && depthBrace == 1) { hasColon = true; break; }
                }
                look++;
            }
            if (!hasColon) {
                return false; // 交给上层把它当作语句块的 '{'
            }

            // 消费 '{'
            pos++;
            int pairCount = 0;

            // 允许空字典 {}
            if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "}") {
                pos++;
                out.push_back(IRUtils::makeDictBuild(pairCount));
                return true;
            }

            auto parseOneExpr = [&](std::vector<IRType::IRInstr>& tmp) -> bool {
                return _parseExpression(ctx, tokens, pos, 1, tmp);
            };

            while (pos < tokens.size()) {
                // key expr
                std::vector<IRType::IRInstr> keyIR;
                if (!parseOneExpr(keyIR)) return false;
                // 期望 ':'
                if (pos >= tokens.size() || !_isOperator(tokens[pos]) || _opSym(tokens[pos]) != ":") return false;
                pos++; // consume ':'
                // value expr
                std::vector<IRType::IRInstr> valIR;
                if (!parseOneExpr(valIR)) return false;

                // 先push key再push value（VM从栈顶pop得到：valN, keyN, ..., val1, key1）
                out.insert(out.end(), keyIR.begin(), keyIR.end());
                out.insert(out.end(), valIR.begin(), valIR.end());
                pairCount++;

                if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == ",") {
                    pos++; // consume ',' 继续下一对
                    continue;
                }
                break;
            }

            if (pos >= tokens.size() || !_isOperator(tokens[pos]) || _opSym(tokens[pos]) != "}") return false;
            pos++; // consume '}'

            out.push_back(IRUtils::makeDictBuild(pairCount));
            return true;
        }

        // '(' Expr ')'  -  处理后继续处理可能的后缀（如链式调用）
        if (_isOperator(tk) && _opSym(tk) == "(") {
            pos++; // consume '('

            if (!_parseExpression(ctx, tokens, pos, 1, out)) {
                return false;
            }

            if (pos >= tokens.size() || !_isOperator(tokens[pos]) || _opSym(tokens[pos]) != ")") {
                return false;
            }

            pos++; // consume ')'
            
            // 处理后缀：方法调用、属性访问、下标访问、函数调用
            while (pos < tokens.size()) {
                // 函数调用 ()
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                    int callLine = _getTokenLine(tokens[pos]);
                    int argc = 0;
                    if (!_parseArguments(ctx, tokens, pos, argc, out)) {
                        return false;
                    }
                    IRType::IRInstr call;
                    call.op = VMOperator::CALL_CLOSURE;
                    call.line = callLine;
                    call.args.push_back(LexerType::Value(argc));
                    out.push_back(call);
                    continue;
                }
                
                // 方法调用/属性访问 .name 或 .name()
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == ".") {
                    if (pos + 1 >= tokens.size() || !_isKeyword(tokens[pos + 1])) break;
                    std::string attrName = std::get<LexerType::Keyword>(tokens[pos + 1]).name;
                    int attrLine = _getTokenLine(tokens[pos + 1]);
                    pos += 2; // consume '.' and name
                    
                    // 检查是否是方法调用
                    if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                        int argc = 0;
                        if (!_parseArguments(ctx, tokens, pos, argc, out)) {
                            return false;
                        }
                        IRType::IRInstr call;
                        call.op = VMOperator::OBJ_CALL_METHOD;
                        call.line = attrLine;
                        call.args.push_back(attrName);
                        call.args.push_back(LexerType::Value(argc));
                        out.push_back(call);
                    } else {
                        // 属性访问
                        IRType::IRInstr load;
                        load.op = VMOperator::OBJ_LOAD_ATTR;
                        load.line = attrLine;
                        load.args.push_back(attrName);
                        out.push_back(load);
                    }
                    continue;
                }
                
                // 下标访问 [index]
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == "[") {
                    // 找到匹配的 ]
                    int depth = 1;
                    size_t closePos = pos + 1;
                    while (closePos < tokens.size() && depth > 0) {
                        if (_isOperator(tokens[closePos])) {
                            std::string s = _opSym(tokens[closePos]);
                            if (s == "[") depth++;
                            else if (s == "]") depth--;
                        }
                        closePos++;
                    }
                    if (depth != 0) break; // 未匹配的括号
                    
                    // 解析索引表达式
                    std::vector<LexerType::Token> indexTokens;
                    for (size_t i = pos + 1; i < closePos - 1; ++i) {
                        indexTokens.push_back(tokens[i]);
                    }
                    
                    if (!indexTokens.empty()) {
                        auto indexIR = IRGen::emitExprIR(ctx.status, ctx.strictDecl, ctx.futureFuncDefs, indexTokens);
                        out.insert(out.end(), indexIR.begin(), indexIR.end());
                    } else {
                        // 空索引 - 推入 0
                        IRType::IRInstr zero;
                        zero.op = VMOperator::PUSH;
                        zero.args.push_back(LexerType::Value(0));
                        out.push_back(zero);
                    }
                    
                    IRType::IRInstr load;
                    load.op = VMOperator::DICT_LOAD_VALUE;
                    out.push_back(load);
                    
                    pos = closePos;
                    continue;
                }
                
                // 没有更多后缀
                break;
            }
            
            return true;
        }

        // new 表达式： new ClassName 或 new ClassName(...)
        if (_isKeyword(tk) && std::get<LexerType::Keyword>(tk).name == "new") {
            ZEUS_DEBUG_PRINT("[IRGen _parsePrimary] Parsing new expression");
            pos++; // consume 'new'
            
            // 解析类表达式（标识符或属性访问链，如 ClassName 或 namespace.ClassName）
            // 不能使用 _parseUnary，因为它会处理后续的函数调用 ()
            // 我们需要手动处理标识符 + 属性访问
            
            if (pos >= tokens.size() || !_isKeyword(tokens[pos])) {
                return false; // 期望标识符
            }
            
            std::string name = std::get<LexerType::Keyword>(tokens[pos]).name;
            pos++; // consume identifier
            
            // 加载初始标识符
            IRScope::SymRef s = IRScope::resolve(name);
            const bool hasMemberSuffix = pos < tokens.size() &&
                _isOperator(tokens[pos]) && _opSym(tokens[pos]) == ".";
            const std::string nativeConstructionType =
                s.kind == IRScope::Kind::UNDEFINED && !hasMemberSuffix
                    ? _nativeConstructionType(name)
                    : std::string();
            IRType::IRInstr load;
            load.op = VMOperator::LOAD_VALUE;
            
            if (!nativeConstructionType.empty()) {
                // Native type constructors are encoded on OBJ_BUILD itself;
                // they do not push a class handle onto the operand stack.
            } else if (s.kind != IRScope::Kind::UNDEFINED) {
                switch (s.kind) {
                    case IRScope::Kind::LOCAL:
                        load.args.push_back(std::string("local"));
                        load.args.push_back(LexerType::Value(s.index));
                        load.args.push_back(name); // 添加变量名用于错误报告
                        break;
                    case IRScope::Kind::GLOBAL:
                        load.args.push_back(std::string("global"));
                        load.args.push_back(LexerType::Value(s.index));
                        load.args.push_back(name); // 添加变量名用于错误报告
                        break;
                    case IRScope::Kind::UPVALUE:
                        load.args.push_back(std::string("upvalue"));
                        load.args.push_back(LexerType::Value(s.index));
                        load.args.push_back(name); // 添加变量名用于错误报告
                        break;
                    case IRScope::Kind::SHARED:
                        load.op = VMOperator::LOAD_VALUE_SHARED;
                        load.args.push_back(name);
                        break;
                    default:
                        return false;
                }
            } else {
                // 未解析的标识符，发出字符串（支持延迟解析）
                load.args.push_back(LexerType::Value(name));
            }
            if (nativeConstructionType.empty()) out.push_back(load);
            
            // 处理属性访问链（如 .JSON）
            while (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == ".") {
                pos++; // consume '.'
                
                if (pos >= tokens.size() || !_isKeyword(tokens[pos])) {
                    return false; // 期望属性名
                }
                
                std::string attrName = std::get<LexerType::Keyword>(tokens[pos]).name;
                int attrLine = _getTokenLine(tokens[pos]);
                pos++; // consume attribute name
                
                // 生成 OBJ_LOAD_ATTR
                IRType::IRInstr loadAttr;
                loadAttr.op = VMOperator::OBJ_LOAD_ATTR;
                loadAttr.line = attrLine;
                loadAttr.args.push_back(attrName);
                out.push_back(loadAttr);
            }

            // 解析构造函数参数（支持命名参数）
            int argc = 0;
            if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                // 使用_parseArguments来解析参数列表,支持命名参数
                if (!_parseArguments(ctx, tokens, pos, argc, out)) {
                    return false; // 参数列表解析失败
                }
            }

            // 生成 OBJ_BUILD argc
            IRType::IRInstr build; 
            build.op = VMOperator::OBJ_BUILD; 
            build.args.push_back(LexerType::Value(argc)); // 传递参数个数
            if (!nativeConstructionType.empty()) {
                build.args.push_back(nativeConstructionType);
            }
            out.push_back(build);
            return true;
        }

        // Identifier: 变量 / 函数 / 内置函数调用
        if (_isKeyword(tk)) {
            std::string name = std::get<LexerType::Keyword>(tk).name;
            // 内置布尔字面量
            if (name == "true" || name == "false") {
                IRType::IRInstr ins;
                ins.op = VMOperator::PUSH;
                ins.args.push_back(LexerType::Value(name == "true"));
                out.push_back(ins);
                pos++;
                return true;
            }
            // nil 关键字 -> 推入 Zeus Null 值
            if (name == "nil") {
                IRType::IRInstr ins;
                ins.op = VMOperator::PUSH;
                ins.line = _getTokenLine(tk);
                ins.args.push_back(IRType::NullLiteral{});
                out.push_back(ins);
                pos++;
                return true;
            }
            pos++; // consume ident
            enum class CalleeKind { Unknown, ClosureLike, Native };
            CalleeKind calleeKind = CalleeKind::Unknown;
            bool unresolvedIdent = false; // 记录是否未解析成符号/内置
            bool directFunctionCandidate = false;

            // 如果在包编译上下文中，优先查找内置函数，避免与包内全局变量冲突
            // 必须在resolve之前检查，因为包内可能有同名的全局函数定义
            // 例如：在 cppfunctions.zeu 中 func static input() { return input(); }
            // 这里的 return input() 应该调用内置 input，而不是递归调用自己
            if (IRScope::isCompilingPackage) {
                int bid = ZeusBuiltins::findId(name);
                if (bid >= 0) {
                    IRType::IRInstr ins;
                    ins.op = VMOperator::LOAD_NATIVE;
                    ins.args.push_back(LexerType::Value(bid));
                    out.push_back(ins);
                    calleeKind = CalleeKind::Native;
                    goto _after_resolve_in_package;
                }
            }

            // 解析用户定义符号（含引用）
            IRScope::SymRef s = IRScope::resolve(name);
            if (s.kind != IRScope::Kind::UNDEFINED) {
                if (s.kind == IRScope::Kind::LIST_ELEM_REF) {
                    IRType::IRInstr loadList;
                    loadList.op = VMOperator::LOAD_VALUE;
                    loadList.args.push_back(std::string("local"));
                    loadList.args.push_back(LexerType::Value((int)s.index));
                    out.push_back(loadList);

                    IRType::IRInstr loadIdx;
                    loadIdx.op = VMOperator::LOAD_VALUE;
                    loadIdx.args.push_back(std::string("local"));
                    loadIdx.args.push_back(LexerType::Value((int)s.aux));
                    out.push_back(loadIdx);

                    IRType::IRInstr loadElem;
                    loadElem.op = VMOperator::LIST_LOAD_ELEM;
                    out.push_back(loadElem);
                    calleeKind = CalleeKind::ClosureLike;
                }
                else if (s.kind == IRScope::Kind::DICT_KEY_REF) {
                    IRType::IRInstr loadKey;
                    loadKey.op = VMOperator::LOAD_VALUE;
                    loadKey.args.push_back(std::string("local"));
                    loadKey.args.push_back(LexerType::Value((int)s.index));
                    out.push_back(loadKey);
                    calleeKind = CalleeKind::ClosureLike;
                }
                else if (s.kind == IRScope::Kind::DICT_VAL_REF) {
                    IRType::IRInstr loadDict; loadDict.op = VMOperator::LOAD_VALUE; loadDict.args.push_back(std::string("local")); loadDict.args.push_back(LexerType::Value((int)s.index)); 
                    loadDict.line = _getTokenLine(tk);
                    out.push_back(loadDict);
                    IRType::IRInstr loadCurKey; loadCurKey.op = VMOperator::LOAD_VALUE; loadCurKey.args.push_back(std::string("local")); loadCurKey.args.push_back(LexerType::Value((int)s.aux)); 
                    loadCurKey.line = _getTokenLine(tk);
                    out.push_back(loadCurKey);
                    IRType::IRInstr dictLoad; dictLoad.op = VMOperator::DICT_LOAD_VALUE; 
                    dictLoad.line = _getTokenLine(tk);
                    out.push_back(dictLoad);
                    calleeKind = CalleeKind::ClosureLike;
                }
                else if (s.kind == IRScope::Kind::COUNT_INDEX_REF) {
                    IRType::IRInstr loadIdx;
                    loadIdx.op = VMOperator::LOAD_VALUE;
                    loadIdx.args.push_back(std::string("local"));
                    loadIdx.args.push_back(LexerType::Value((int)s.index));
                    out.push_back(loadIdx);
                    calleeKind = CalleeKind::ClosureLike;
                }
                else {
                    // 用户定义的变量优先于内置函数
                    IRType::IRInstr ins;
                    ins.op = VMOperator::LOAD_VALUE;
                    if (s.kind == IRScope::Kind::SHARED) {
                        // 共享变量：使用专用加载指令
                        ins.op = VMOperator::LOAD_VALUE_SHARED;
                        ins.args.push_back(name);
                    }
                    else if (s.kind == IRScope::Kind::LOCAL) {
                        ins.args.push_back(std::string("local"));
                        ins.args.push_back(LexerType::Value((int)s.index));
                    }
                    else if (s.kind == IRScope::Kind::GLOBAL) {
                        ins.args.push_back(std::string("global"));
                        ins.args.push_back(LexerType::Value((int)s.index));
                    }
                    else if (s.kind == IRScope::Kind::UPVALUE) {
                        ins.args.push_back(std::string("upvalue"));
                        ins.args.push_back(LexerType::Value((int)s.index));
                    }
                    out.push_back(ins);
                    calleeKind = CalleeKind::ClosureLike;
                    directFunctionCandidate = s.kind == IRScope::Kind::GLOBAL &&
                                              IRScope::isStaticFunctionName(name);
                }
            }
            else {
                // 如果在包编译上下文中，优先查找内置函数，避免与包内全局变量冲突
                // 例如：在 cppfunctions.zeu 中调用 input()，应该解析为内置的 input 而不是 ZeusBuiltins.cppfunctions.input
                if (IRScope::isCompilingPackage) {
                    int bid = ZeusBuiltins::findId(name);
                    if (bid >= 0) {
                        IRType::IRInstr ins;
                        ins.op = VMOperator::LOAD_NATIVE;
                        ins.args.push_back(LexerType::Value(bid));
                        out.push_back(ins);
                        calleeKind = CalleeKind::Native;
                        goto _after_resolve_in_package;
                    }
                }
                
                // 尝试匹配内置
                int bid = ZeusBuiltins::findId(name);
                if (bid >= 0) {
                    IRType::IRInstr ins;
                    ins.op = VMOperator::LOAD_NATIVE;
                    ins.args.push_back(LexerType::Value(bid));
                    out.push_back(ins);
                    calleeKind = CalleeKind::Native;

                } else {
                    // 未解析且不是内置 - 生成 LOAD_VALUE 用于运行时解析
                    unresolvedIdent = true;
                    IRType::IRInstr ins;
                    ins.op = VMOperator::LOAD_VALUE;
                    ins.args.push_back(name);  // 字符串参数，VM 会尝试解析并报错
                    out.push_back(ins);
                    calleeKind = CalleeKind::ClosureLike;
                    directFunctionCandidate = IRScope::isStaticFunctionName(name);
                }
            }

            _after_resolve_in_package:
            // 处理零个或多个调用后缀
            // 如果是严格模式且该标识符未解析，同时后面紧跟 '('，判定为对未声明函数的调用 -> 编译期报错
            if (ctx.strictDecl && unresolvedIdent && pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                // 构造调用片段：标识符 + 后续括号内直到匹配 ')'
                size_t tmp = pos; // 当前指向'('
                int depth = 0;
                std::ostringstream callSnippet;
                callSnippet << name;
                if (tmp < tokens.size()) {
                    callSnippet << " ";
                }
                while (tmp < tokens.size()) {
                    const auto &ctk = tokens[tmp];
                    if (_isOperator(ctk)) {
                        std::string sym = _opSym(ctk);
                        callSnippet << sym << ' ';
                        if (sym == "(") depth++;
                        else if (sym == ")") {
                            depth--;
                            if (depth == 0) { tmp++; break; }
                        }
                    } else if (_isValue(ctk)) {
                        callSnippet << std::get<LexerType::Value>(ctk).toString() << ' ';
                    } else if (_isKeyword(ctk)) {
                        callSnippet << std::get<LexerType::Keyword>(ctk).name << ' ';
                    }
                    tmp++;
                    if (depth <= 0 && tmp > pos) break;
                }
                std::string snippet = callSnippet.str();
                bool willDefineLater = ctx.futureFuncDefs.find(name) != ctx.futureFuncDefs.end();
                std::string baseMsg;
                std::string pretty;
                if (willDefineLater) {
                    baseMsg = "Strict mode: call to function '" + name + "' before its definition without forward declaration";
                    pretty = "[IR Error] call-before-definition without forward decl: '" + name + "' | snippet: " + snippet;
                } else {
                    baseMsg = "Strict mode: call to undefined function '" + name + "' (needs forward declaration)";
                    pretty = "[IR Error] undefined function '" + name + "' in strict mode | snippet: " + snippet;
                }
                {
                    TreeParser::Node dummy;
                    IRExceptionNS::IRException ex(baseMsg, 0, dummy);
                    ex._throw(ctx.status);
                    return false;
                }
            }

            while (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                int callLine = _getTokenLine(tokens[pos]);
                int argc = 0;

                if (!_parseArguments(ctx, tokens, pos, argc, out)) {
                    return false;
                }

                if (directFunctionCandidate && calleeKind == CalleeKind::ClosureLike) {
                    out.push_back(makeDirectCallAnnotation(name));
                }
                
                IRType::IRInstr callInstr;

                if (calleeKind == CalleeKind::Native) {
                    callInstr.op = VMOperator::CALL_NATIVE;
                }
                else {
                    callInstr.op = VMOperator::CALL_CLOSURE;
                }

                callInstr.line = callLine;
                callInstr.args.push_back(LexerType::Value(argc));
                out.push_back(callInstr);

                // 多次链式调用：后续都视为普通闭包调用（前一次返回值可能是闭包）
                calleeKind = CalleeKind::ClosureLike;
                directFunctionCandidate = false;
            }

            // 通用后缀解析：属性链和下标访问
            // 支持： ident '.' attr ('.' attr)* 以及 ident '[' expr ']'
            // 说明：此前 emitExprIR 有“前缀”快速路径用于顶层；这里补齐一般情况下（例如作为函数参数）
            while (pos < tokens.size()) {
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                    // Chain call: f()() or c.method()()
                    int callLine = _getTokenLine(tokens[pos]);
                    int argc = 0;
                    if (!_parseArguments(ctx, tokens, pos, argc, out)) {
                        return false;
                    }
                    IRType::IRInstr call;
                    call.op = VMOperator::CALL_CLOSURE;
                    call.line = callLine;
                    call.args.push_back(LexerType::Value(argc));
                    out.push_back(call);
                    continue;
                }
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == ".") {
                    // 属性访问：要求后面紧跟标识符
                    // Lookahead: if pattern is . ident '(' -> method call on object
                    if (pos + 2 < tokens.size() && _isKeyword(tokens[pos+1]) && _isOperator(tokens[pos+2]) && _opSym(tokens[pos+2]) == "(") {
                        // consume '.'
                        pos++;
                        std::string method = std::get<LexerType::Keyword>(tokens[pos]).name;
                        int methodLine = _getTokenLine(tokens[pos]);
                        pos++; // consume method name
                        // parse arguments starting at '('
                        int argc = 0;
                        if (!_parseArguments(ctx, tokens, pos, argc, out)) {
                            return false;
                        }
                        IRType::IRInstr call; call.op = VMOperator::OBJ_CALL_METHOD; call.line = methodLine; call.args.push_back(method); call.args.push_back(LexerType::Value(argc)); out.push_back(call);
                        continue;
                    } else {
                        pos++; // consume '.'
                        if (pos >= tokens.size() || !_isKeyword(tokens[pos])) {
                            return false; // 语法错误
                        }
                        std::string attr = std::get<LexerType::Keyword>(tokens[pos]).name;
                        int attrLine = _getTokenLine(tokens[pos]);
                        pos++; // consume attr
                        IRType::IRInstr get; get.op = VMOperator::OBJ_LOAD_ATTR; get.line = attrLine; get.args.push_back(attr);
                        out.push_back(get);
                        continue;
                    }
                }
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == "[") {
                    // 下标访问：收集直到匹配 ']' 的表达式 tokens，然后发射 key IR + DICT_LOAD_VALUE
                    pos++; // consume '['
                    int depth = 1;
                    size_t startKey = pos;
                    size_t endKey = pos;
                    while (pos < tokens.size() && depth > 0) {
                        if (_isOperator(tokens[pos])) {
                            std::string s = _opSym(tokens[pos]);
                            if (s == "[") depth++;
                            else if (s == "]") { depth--; if (depth == 0) { endKey = pos; break; } }
                        }
                        pos++;
                    }
                    if (depth != 0) {
                        return false; // 缺少匹配的 ']'
                    }
                    // 构造 key tokens 片段 [startKey, endKey)
                    std::vector<LexerType::Token> keyTokens;
                    for (size_t k = startKey; k < endKey; ++k) keyTokens.push_back(tokens[k]);
                    // 消费 ']'
                    pos++; // now pos 指向 ']' 之后
                    if (!keyTokens.empty()) {
                        auto keyIR = emitExprIR(ctx.status, ctx.strictDecl, ctx.futureFuncDefs, keyTokens);
                        out.insert(out.end(), keyIR.begin(), keyIR.end());
                    } else {
                        // 空 key：推入 0 以避免 VM 下溢（理论上语法不允许）
                        IRType::IRInstr zero; zero.op = VMOperator::PUSH; zero.args.push_back(LexerType::Value(0)); out.push_back(zero);
                    }
                    IRType::IRInstr loadIdx; 
                    loadIdx.op = VMOperator::DICT_LOAD_VALUE;
                    // Set line from key tokens or the '[' token before startKey
                    if (!keyTokens.empty()) {
                        loadIdx.line = _getTokenLine(keyTokens[0]);
                    } else if (startKey > 0) {
                        loadIdx.line = _getTokenLine(tokens[startKey - 1]); // '[' token
                    }
                    out.push_back(loadIdx);
                    continue;
                }
                break; // 非后缀，结束
            }
            return true;
        }

        return false;
    }

    static bool _parseUnary(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, std::vector<IRType::IRInstr>& out) {
        if (pos < tokens.size() && _isOperator(tokens[pos])) {
            std::string s = _opSym(tokens[pos]);
            if (s == "+") {
                pos++; // 一元正号，忽略
                return _parseUnary(ctx, tokens, pos, out);
            } else if (s == "-") {
                pos++; // 一元负号，转为 0 - expr
                // 先发 0
                IRType::IRInstr z;
                z.op = VMOperator::PUSH;
                z.args.push_back(LexerType::Value(0));
                out.push_back(z);
                // 再发表达式
                if (!_parseUnary(ctx, tokens, pos, out)) return false;
                // 发 SUBTRACT
                out.push_back(IRType::IRInstr{ VMOperator::SUBTRACT, {} });
                return true;
            } else if (s == "!") {
                const int sourceLine = _getTokenLine(tokens[pos]);
                ++pos;
                if (!_parseUnary(ctx, tokens, pos, out)) return false;
                IRType::IRInstr ins = IRUtils::makeNotOp();
                ins.line = sourceLine;
                out.push_back(std::move(ins));
                return true;
            }
        }
        return _parsePrimary(ctx, tokens, pos, out);
    }

    static bool _parseExpression(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, int minPrec, std::vector<IRType::IRInstr>& out) {
        if (!_parseUnary(ctx, tokens, pos, out)) {
            return false;
        }

        while (pos < tokens.size() && _isOperator(tokens[pos]) && isBinaryOp(tokens[pos])) {
            std::string op = _opSym(tokens[pos]);
            int prec = opPrecedence(op);
            if (prec < minPrec) {
                break;
            }

            // consume op
            pos++;

            int nextMinPrec = prec + 1; // 左结合
            if (!_parseExpression(ctx, tokens, pos, nextMinPrec, out)) {
                return false;
            }

            {
                IRType::IRInstr ins = IRUtils::makeBinaryOpBySymbol(op);
                out.push_back(ins);

            }
        }

        return true;
    }

    // Parse only the trailing binary operators when the left-hand side has already been
    // emitted into 'out' (used when a leading prefix like attr-chain/new/obj-call was
    // consumed and emitted before calling the general expression parser).
    static bool _parseExpressionTail(ExprParseCtx& ctx, const std::vector<LexerType::Token>& tokens, size_t& pos, int minPrec, std::vector<IRType::IRInstr>& out) {
        // Ensure there is a left-hand side already emitted
        // If not, fall back to full expression parse
        if (out.empty()) return _parseExpression(ctx, tokens, pos, minPrec, out);

        while (pos < tokens.size() && _isOperator(tokens[pos]) && isBinaryOp(tokens[pos])) {
            std::string op = _opSym(tokens[pos]);
            int prec = opPrecedence(op);
            if (prec < minPrec) {
                break;
            }

            // consume op
            pos++;

            int nextMinPrec = prec + 1; // left-associative
            // parse right-hand side as a full expression
            if (!_parseExpression(ctx, tokens, pos, nextMinPrec, out)) {
                return false;
            }

            {
                IRType::IRInstr ins = IRUtils::makeBinaryOpBySymbol(op);
                out.push_back(ins);

            }
        }

        return true;
    }

    // 入口：表达式 -> IR
    std::vector<IRType::IRInstr> emitExprIR(IRType::IRCompilerStatus& status, bool strictDecl, const std::unordered_set<std::string>& futureFuncDefs, const std::vector<LexerType::Token>& inTokens) {
        std::vector<IRType::IRInstr> out;

        // 安全折叠（调用外 + 顶层加法合并）
        // 根本性修复: priorityFoldExpression 设计用于常量折叠优化,
        // 但它在处理带有 [], (), {} 的复杂表达式时存在 token 丢失问题。
        // 检查表达式是否包含这些结构符号,如果包含则完全跳过 fold 优化。
        auto hasComplexStructure = [&]() -> bool {
            for (const auto& token : inTokens) {
                if (std::holds_alternative<LexerType::Operator>(token)) {
                    const std::string& sym = std::get<LexerType::Operator>(token).symbol;
                    if (sym == "[" || sym == "]" || sym == "{" || sym == "}" || sym == "(" || sym == ")") {
                        return true;
                    }
                }

                if (std::holds_alternative<LexerType::BytesLiteral>(token)) {
                    return true;
                }
            }
            return false;
        };

        // 特殊处理: 空列表 [] 和空字典 {}
        if (inTokens.size() == 2) {
            if (std::holds_alternative<LexerType::Operator>(inTokens[0]) && 
                std::holds_alternative<LexerType::Operator>(inTokens[1])) {
                std::string sym0 = std::get<LexerType::Operator>(inTokens[0]).symbol;
                std::string sym1 = std::get<LexerType::Operator>(inTokens[1]).symbol;
                
                if (sym0 == "[" && sym1 == "]") {
                    // 空列表 []
                    IRType::IRInstr instr;
                    instr.op = VMOperator::LIST_BUILD;
                    instr.line = _getTokenLine(inTokens[0]);
                    instr.args.push_back(LexerType::Value(0));
                    out.push_back(instr);
                    
                    // 包装为Array对象
                    if (ZeusBuiltins::shouldWrapLiteral(
                            ZeusBuiltins::BuiltinLiteralKind::Array,
                            IRScope::isUsingOriginal(
                                ZeusBuiltins::BuiltinLiteralKind::Array))) {
                        IRScope::SymRef arrayRef = IRScope::resolve("ZeusBuiltins.list.Array");
                        if (arrayRef.kind != IRScope::Kind::GLOBAL) {
                            arrayRef = IRScope::resolve("Array");
                        }
                        
                        ZEUS_DEBUG_PRINT("[IRGen] Empty array []: useOriginal=%d, arrayRef.kind=%d",
                            IRScope::isUsingOriginal(), (int)arrayRef.kind);
                        
                        if (arrayRef.kind == IRScope::Kind::GLOBAL) {
                            IRType::IRInstr loadArray;
                            loadArray.op = VMOperator::LOAD_VALUE;
                            loadArray.args.push_back(std::string("global"));
                            loadArray.args.push_back(LexerType::Value(arrayRef.index));
                            out.push_back(loadArray);
                            
                            IRType::IRInstr swap;
                            swap.op = VMOperator::SWAP;
                            out.push_back(swap);
                            
                            IRType::IRInstr objBuild;
                            objBuild.op = VMOperator::OBJ_BUILD;
                            objBuild.args.push_back(LexerType::Value(1));
                            out.push_back(objBuild);
                        }
                    }
                    
                    return out;
                } else if (sym0 == "{" && sym1 == "}") {
                    // 空字典 {}
                    IRType::IRInstr instr;
                    instr.op = VMOperator::DICT_BUILD;
                    instr.line = _getTokenLine(inTokens[0]);
                    instr.args.push_back(LexerType::Value(0));
                    out.push_back(instr);
                    return out;
                }
            }
        }

        // 特殊：检测列表字面量形如 [ start .. end ( .. step ) ] 或 [ start ..= end ( .. step ) ]
        if (inTokens.size() >= 5) {
            if (std::holds_alternative<LexerType::Operator>(inTokens.front()) && std::get<LexerType::Operator>(inTokens.front()).symbol == "["
                && std::holds_alternative<LexerType::Operator>(inTokens.back()) && std::get<LexerType::Operator>(inTokens.back()).symbol == "]") {

                // 查找 '..' 位置
                int dotIdx = -1;
                for (size_t i = 1; i + 1 < inTokens.size(); ++i) {
                    if (std::holds_alternative<LexerType::Operator>(inTokens[i]) && std::get<LexerType::Operator>(inTokens[i]).symbol == "."
                        && std::holds_alternative<LexerType::Operator>(inTokens[i+1]) && std::get<LexerType::Operator>(inTokens[i+1]).symbol == ".") {
                        dotIdx = (int)i; break;
                    }
                }

                if (dotIdx > 0) {
                    // start tokens: 1..dotIdx-1 ; after dots: dotIdx+2 .. last-1
                    std::vector<LexerType::Token> startTok(inTokens.begin() + 1, inTokens.begin() + dotIdx);
                    // detect '..=': if token at dotIdx+2 is '=' then inclusive marker
                    bool inclusive = false;
                    int postIdx = dotIdx + 2;
                    if (postIdx < (int)inTokens.size() && std::holds_alternative<LexerType::Operator>(inTokens[postIdx]) && std::get<LexerType::Operator>(inTokens[postIdx]).symbol == "=") {
                        inclusive = true; postIdx += 1;
                    }
                    // find optional second '..' for step (support start..end..step)
                    int nextDot = -1;
                    for (int i = postIdx; i + 1 < (int)inTokens.size() - 1; ++i) {
                        if (std::holds_alternative<LexerType::Operator>(inTokens[i]) && std::get<LexerType::Operator>(inTokens[i]).symbol == "."
                            && std::holds_alternative<LexerType::Operator>(inTokens[i+1]) && std::get<LexerType::Operator>(inTokens[i+1]).symbol == ".") {
                            nextDot = i; break;
                        }
                    }

                    std::vector<LexerType::Token> endTok;
                    std::vector<LexerType::Token> stepTok;
                    if (nextDot >= 0) {
                        endTok.assign(inTokens.begin() + postIdx, inTokens.begin() + nextDot);
                        stepTok.assign(inTokens.begin() + nextDot + 2, inTokens.end() - 1);
                    } else {
                        endTok.assign(inTokens.begin() + postIdx, inTokens.end() - 1);
                    }

                    // Only handle when start/end/step (if present) are single literal Value tokens
                    auto parseSignedBigInt = [&](const std::vector<LexerType::Token>& tks, ZeusType::BigInteger &out)->bool {
                        if (tks.empty()) return false;
                        if (tks.size() == 1 && std::holds_alternative<LexerType::Value>(tks[0])) {
                            const auto &v = std::get<LexerType::Value>(tks[0]);
                            if (std::holds_alternative<ZeusType::BigInteger>(v.data)) { out = std::get<ZeusType::BigInteger>(v.data); return true; }
                            return false;
                        }
                        if (tks.size() == 2 && std::holds_alternative<LexerType::Operator>(tks[0]) && (std::get<LexerType::Operator>(tks[0]).symbol == "-" || std::get<LexerType::Operator>(tks[0]).symbol == "+")
                            && std::holds_alternative<LexerType::Value>(tks[1])) {
                            const auto &v = std::get<LexerType::Value>(tks[1]);
                            if (!std::holds_alternative<ZeusType::BigInteger>(v.data)) return false;
                            out = std::get<ZeusType::BigInteger>(v.data);
                            if (std::get<LexerType::Operator>(tks[0]).symbol == "-") out = out * ZeusType::BigInteger(-1);
                            return true;
                        }
                        return false;
                    };

                    ZeusType::BigInteger startInt, endInt, stepInt;
                    bool okStart = parseSignedBigInt(startTok, startInt);
                    bool okEnd   = parseSignedBigInt(endTok, endInt);
                    bool okStep  = true;
                    if (!stepTok.empty()) {
                        okStep = parseSignedBigInt(stepTok, stepInt);
                    } else {
                        stepInt = ZeusType::BigInteger(1);
                    }

                    if (okStart && okEnd && okStep) {
                        std::vector<ZeusType::BigInteger> elems;
                        if (stepInt == ZeusType::BigInteger(0)) {
                            // avoid infinite compile-time expansion: produce empty list
                        } else if (stepInt > ZeusType::BigInteger(0)) {
                            ZeusType::BigInteger cur = startInt;
                            while (true) {
                                if (inclusive) {
                                    if (cur > endInt) break;
                                } else {
                                    if (cur >= endInt) break;
                                }
                                elems.push_back(cur);
                                cur = cur + stepInt;
                            }
                        } else {
                            ZeusType::BigInteger cur = startInt;
                            while (true) {
                                if (inclusive) {
                                    if (cur < endInt) break;
                                } else {
                                    if (cur <= endInt) break;
                                }
                                elems.push_back(cur);
                                cur = cur + stepInt;
                            }
                        }

                        // Emit pushes for each element then LIST_BUILD
                        for (auto &e : elems) {
                            IRType::IRInstr p; p.op = VMOperator::PUSH; p.args.push_back(LexerType::Value(e)); out.push_back(p);
                        }
                        IRType::IRInstr build; build.op = VMOperator::LIST_BUILD; build.args.push_back(LexerType::Value((int)elems.size())); out.push_back(build);
                        
                        // 包装为Array对象 (内联_wrapListAsArray逻辑)
                        IRScope::SymRef arrayRef = IRScope::resolve("ZeusBuiltins.list.Array");
                        if (arrayRef.kind != IRScope::Kind::GLOBAL) {
                            arrayRef = IRScope::resolve("Array");
                        }
                        if (arrayRef.kind == IRScope::Kind::GLOBAL) {
                            // LOAD Array类
                            IRType::IRInstr loadArray;
                            loadArray.op = VMOperator::LOAD_VALUE;
                            loadArray.args.push_back(std::string("global"));
                            loadArray.args.push_back(LexerType::Value(arrayRef.index));
                            out.push_back(loadArray);
                            
                            // SWAP (交换list和Array的位置)
                            IRType::IRInstr swap;
                            swap.op = VMOperator::SWAP;
                            out.push_back(swap);
                            
                            // OBJ_BUILD 1 (构建Array对象)
                            IRType::IRInstr objBuild;
                            objBuild.op = VMOperator::OBJ_BUILD;
                            objBuild.args.push_back(LexerType::Value(1));
                            out.push_back(objBuild);
                        }
                        
                        return out;
                    }
                }
            }
        }

        std::vector<LexerType::Token> tokens;
        
        // 只对纯算术表达式(不含 [], {}, () 的简单运算)执行 fold 优化
        if (!hasComplexStructure()) {
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: Pure arithmetic, applying fold optimization");
            // Use the overall parser status so that fold-time errors (e.g. division by zero)
            // update the real compiler status and abort emission instead of only printing.
            tokens = IRGen::priorityFoldExpression(status, inTokens);

            if (status != IRType::IRCompilerStatus::Running) {
                // fold 遇到严重错误，直接中止 emit（保持 status 为 Stopped），
                // 返回空 out 由上层检测并停止整个编译流程。
                return out;
            }
        } else {
            // 包含复杂结构,跳过 fold 优化以避免 token 丢失
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: Complex expression detected, skipping fold");
            tokens = inTokens;
        }

        if (tokens.empty()) return out;

        // 单 token 快速路径
        if (tokens.size() == 1) {
            const auto& tk = tokens[0];

            // Literal wrapping policy is registered centrally for each built-in type.
            if (std::holds_alternative<LexerType::BytesLiteral>(tk) &&
                ZeusBuiltins::shouldWrapLiteral(
                    ZeusBuiltins::BuiltinLiteralKind::Bytes,
                    IRScope::isUsingOriginal(
                        ZeusBuiltins::BuiltinLiteralKind::Bytes))) {
                const auto& bytesLit = std::get<LexerType::BytesLiteral>(tk);
                ZeusType::Bytes bytes(bytesLit.data);
                int tokenLine = _getTokenLine(tk);

                // PUSH bytes数据
                IRType::IRInstr pushBytes;
                pushBytes.op = VMOperator::PUSH;
                pushBytes.line = tokenLine;
                pushBytes.args.push_back(bytes);
                out.push_back(pushBytes);

                // 查找Byte类
                const auto* wrapper = ZeusBuiltins::findLiteralWrapper(
                    ZeusBuiltins::BuiltinLiteralKind::Bytes);
                IRScope::SymRef byteRef = wrapper
                    ? IRScope::resolve(std::string(wrapper->qualifiedClassName))
                    : IRScope::SymRef{};
                if (byteRef.kind != IRScope::Kind::GLOBAL) {
                    byteRef = wrapper
                        ? IRScope::resolve(std::string(wrapper->shortClassName))
                        : IRScope::SymRef{};
                }

                if (byteRef.kind == IRScope::Kind::GLOBAL) {
                    // LOAD Byte类
                    IRType::IRInstr loadByte;
                    loadByte.op = VMOperator::LOAD_VALUE;
                    loadByte.args.push_back(std::string("global"));
                    loadByte.args.push_back(LexerType::Value(byteRef.index));
                    out.push_back(loadByte);

                    // SWAP: 使bytes在栈顶下方，Byte类在栈顶
                    IRType::IRInstr swap;
                    swap.op = VMOperator::SWAP;
                    out.push_back(swap);

                    // OBJ_BUILD 1 (构建Byte对象，传入1个参数：bytes数据)
                    IRType::IRInstr objBuild;
                    objBuild.op = VMOperator::OBJ_BUILD;
                    objBuild.args.push_back(LexerType::Value(1));
                    out.push_back(objBuild);
                }
                
                // No matter if Byte class is found, we have pushed bytes data, return directly
                return out;
            }

            // Handle string literals through the registered built-in wrapper policy.
            if (std::holds_alternative<LexerType::Value>(tk)) {
                const auto& val = std::get<LexerType::Value>(tk);
                if (std::holds_alternative<std::string>(val.data) &&
                    ZeusBuiltins::shouldWrapLiteral(
                        ZeusBuiltins::BuiltinLiteralKind::String,
                        IRScope::isUsingOriginal(
                            ZeusBuiltins::BuiltinLiteralKind::String))) {
                    int tokenLine = _getTokenLine(tk);

                    // PUSH string data
                    IRType::IRInstr pushStr;
                    pushStr.op = VMOperator::PUSH;
                    pushStr.line = tokenLine;
                    pushStr.args.push_back(val);
                    out.push_back(pushStr);

                    // Resolve String class
                    const auto* wrapper = ZeusBuiltins::findLiteralWrapper(
                        ZeusBuiltins::BuiltinLiteralKind::String);
                    IRScope::SymRef stringRef = wrapper
                        ? IRScope::resolve(std::string(wrapper->qualifiedClassName))
                        : IRScope::SymRef{};
                    ZEUS_DEBUG_PRINT("[IRGen] String wrapping: trying 'ZeusBuiltins.string.String', kind=%d", (int)stringRef.kind);
                    if (stringRef.kind != IRScope::Kind::GLOBAL) {
                        stringRef = wrapper
                            ? IRScope::resolve(std::string(wrapper->shortClassName))
                            : IRScope::SymRef{};
                        ZEUS_DEBUG_PRINT("[IRGen] String wrapping: fallback to 'String', kind=%d", (int)stringRef.kind);
                    }

                    if (stringRef.kind == IRScope::Kind::GLOBAL) {
                        ZEUS_DEBUG_PRINT("[IRGen] String wrapping: Found String class at global index %d", (int)stringRef.index);
                        // LOAD String class
                        IRType::IRInstr loadString;
                        loadString.op = VMOperator::LOAD_VALUE;
                        loadString.args.push_back(std::string("global"));
                        loadString.args.push_back(LexerType::Value(stringRef.index));
                        out.push_back(loadString);

                        // SWAP: put string below, String class on top
                        IRType::IRInstr swap;
                        swap.op = VMOperator::SWAP;
                        out.push_back(swap);

                        // OBJ_BUILD 1 (construct String object with 1 argument: string data)
                        IRType::IRInstr objBuild;
                        objBuild.op = VMOperator::OBJ_BUILD;
                        objBuild.args.push_back(LexerType::Value(1));
                        out.push_back(objBuild);
                    }

                    // No matter if String class is found, we have pushed string data, return directly
                    return out;
                }
            }

            // Build temporary ctx for single token handling
            ExprParseCtx ctx{status, strictDecl, futureFuncDefs,
                IRScope::isUsingOriginal(ZeusBuiltins::BuiltinLiteralKind::Array)};
            _emitValueOrKeyword(ctx, tk, out);
            return out;
        }

        // 剥离最外层括号(如果整个表达式被括号包裹)
        // 这允许 (new Array(...)).method() 这样的链式调用正确解析
        if (tokens.size() >= 3 && _isOperator(tokens[0]) && _opSym(tokens[0]) == "(") {
            ZEUS_DEBUG_PRINT("[IRGen] Checking if expression is wrapped in parentheses...");
            // 检查是否整个表达式被一对括号包裹
            int depth = 1;
            size_t closePos = 1;
            while (closePos < tokens.size() && depth > 0) {
                if (_isOperator(tokens[closePos])) {
                    std::string sym = _opSym(tokens[closePos]);
                    if (sym == "(") depth++;
                    else if (sym == ")") depth--;
                }
                closePos++;
            }
            // 如果找到的 ')' 就是最后一个 token,说明整个表达式被括号包裹
            if (depth == 0 && closePos == tokens.size()) {
                ZEUS_DEBUG_PRINT("[IRGen] Stripping outer parentheses, recursing...");
                // 剥离括号,递归处理内部表达式
                std::vector<LexerType::Token> inner(tokens.begin() + 1, tokens.begin() + closePos - 1);
                return emitExprIR(status, strictDecl, futureFuncDefs, inner);
            } else {
                ZEUS_DEBUG_PRINT("[IRGen] Not fully wrapped (depth=%d, closePos=%zu, size=%zu)", depth, closePos, tokens.size());
            }
        }

        // 特殊模式：
        // 1) 前缀静态方法调用：形态 <ident> '.' <ident> '(' args ')' -> CLASS_STATIC_METHOD_CALL
        // 2) 前缀点属性链读取：形态 <ident> '.' <attr> ('.' <attr>)* ；
        //    生成：LOAD_VALUE base + 多次 OBJ_LOAD_ATTR；允许后续继续二元解析（例如 a.b + 1）。
        // 3) 下标读取（仅一层）形态： <ident> '[' ... ']' (+ 可能后续继续出现二元运算)
        //    为了与通用递归下降不互相干扰，这里先做一次线性扫描把最左的原子打包成临时指令序列，
        //    然后把它视为一个“前缀”放入 out，再对余下 token（如果有）走通用二元解析。

        auto tryEmitLeadingStaticCall = [&](const std::vector<LexerType::Token>& toks) -> size_t {
            if (toks.size() < 5) return 0; // a . m ( ) 至少 5 个 token
            if (!_isKeyword(toks[0])) return 0;
            if (!_isOperator(toks[1]) || _opSym(toks[1]) != ".") return 0;
            if (!_isKeyword(toks[2])) return 0;
            if (!_isOperator(toks[3]) || _opSym(toks[3]) != "(") return 0;

            // 找到匹配的 ')'
            int depth = 0; size_t closePos = 0; bool found=false;
            for (size_t i = 3; i < toks.size(); ++i) {
                if (_isOperator(toks[i])) {
                    std::string s = _opSym(toks[i]);
                    if (s == "(") depth++;
                    else if (s == ")") { depth--; if (depth == 0) { closePos = i; found = true; break; } }
                }
            }
            if (!found) return 0;

            // 收集参数 tokens: (4..closePos-1) 可能含逗号分隔
            std::vector<std::vector<LexerType::Token>> argTokList;
            {
                size_t p = 4; size_t end = closePos; int brace=0, brack=0, paren=0; bool started=false; size_t segStart=p;
                while (p < end) {
                    if (_isOperator(toks[p])) {
                        std::string sym = _opSym(toks[p]);
                        if (sym == "(") paren++;
                        else if (sym == ")") paren--;
                        else if (sym == "[") brack++;
                        else if (sym == "]") brack--;
                        else if (sym == "{") brace++;
                        else if (sym == "}") brace--;
                        else if (sym == "," && brace==0 && brack==0 && paren==0) {
                            // 完整一个参数
                            std::vector<LexerType::Token> seg;
                            for (size_t k = segStart; k < p; ++k) seg.push_back(toks[k]);
                            argTokList.push_back(seg);
                            segStart = p + 1;
                        }
                    }
                    p++;
                }
                if (segStart < end) {
                    std::vector<LexerType::Token> seg;
                    for (size_t k = segStart; k < end; ++k) seg.push_back(toks[k]);
                    argTokList.push_back(seg);
                }
                if (end == 4) {
                    // 空参
                }
            }

            // 先检查 prefix.method 是否是完整的全局函数名(用于包导入)
            std::string leftName = std::get<LexerType::Keyword>(toks[0]).name;
            std::string methodName = std::get<LexerType::Keyword>(toks[2]).name;
            std::string fullName = leftName + "." + methodName;
            
            IRScope::SymRef fullRef = IRScope::resolve(fullName);
            ZEUS_DEBUG_PRINT("[IRGen] static-call lookup '%s': kind=%d static=%d",
                fullName.c_str(), (int)fullRef.kind,
                IRScope::isStaticFunctionName(fullName) ? 1 : 0);
            
            if (fullRef.kind == IRScope::Kind::GLOBAL &&
                IRScope::isStaticFunctionName(fullName)) {
                // 找到了完整的全局函数名,直接调用
                ZEUS_DEBUG_PRINT("[IRGen    ] %s -> CALL (package function)", fullName.c_str());
                
                IRType::IRInstr load; 
                load.op = VMOperator::LOAD_VALUE;
                load.args.push_back(std::string("global"));  
                load.args.push_back(LexerType::Value(fullRef.index));
                load.args.push_back(fullName); // 添加函数名用于错误报告
                out.push_back(load);
                
                int argc = 0;
                for (auto &seg : argTokList) {
                    if (!seg.empty()) {
                        auto argIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, seg);
                        out.insert(out.end(), argIR.begin(), argIR.end());
                        argc++;
                    }
                }
                
                out.push_back(makeDirectCallAnnotation(fullName));

                IRType::IRInstr call;
                call.op = VMOperator::CALL_CLOSURE;
                call.args.push_back(LexerType::Value(argc)); 
                out.push_back(call);
                
                return closePos + 1;
            }

            // 解析左侧标识符，判断是类名还是变量(实例)
            IRScope::SymRef lhsRef = IRScope::resolve(leftName);

            // 先生成左侧值的 LOAD 指令（无论是 class handle 还是对象变量）
            {
                IRType::IRInstr load; load.op = VMOperator::LOAD_VALUE;
                switch (lhsRef.kind) {
                    case IRScope::Kind::LOCAL:   load.args.push_back(std::string("local"));   load.args.push_back(LexerType::Value(lhsRef.index)); load.args.push_back(leftName); break;
                    case IRScope::Kind::GLOBAL:  load.args.push_back(std::string("global"));  load.args.push_back(LexerType::Value(lhsRef.index)); load.args.push_back(leftName); break;
                    case IRScope::Kind::UPVALUE: load.args.push_back(std::string("upvalue")); load.args.push_back(LexerType::Value(lhsRef.index)); load.args.push_back(leftName); break;
                    case IRScope::Kind::SHARED:
                        load.op = VMOperator::LOAD_VALUE_SHARED;
                        load.args.push_back(leftName);
                        break;
                    default: load.args.push_back(std::string("global")); load.args.push_back(LexerType::Value(-1)); load.args.push_back(leftName); break;
                }
                out.push_back(load);
            }

            int argc = 0;
            for (auto &seg : argTokList) {
                if (!seg.empty()) {
                    auto argIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, seg);
                    out.insert(out.end(), argIR.begin(), argIR.end());
                    argc++;
                }
            }

            int methodLine = _getTokenLine(toks[2]);
            // 如果左侧解析到的是变量/实例(LOCAL/GLOBAL/UPVALUE/SHARED)，生成对象方法调用指令；
            // 否则当作类名生成 CLASS_STATIC_METHOD_CALL
            if (lhsRef.kind == IRScope::Kind::LOCAL || lhsRef.kind == IRScope::Kind::GLOBAL || lhsRef.kind == IRScope::Kind::UPVALUE || lhsRef.kind == IRScope::Kind::SHARED) {
                ZEUS_DEBUG_PRINT("[IRGen    ] %s.%s() -> OBJ_CALL_METHOD (instance)", 
                                 leftName.c_str(), std::get<LexerType::Keyword>(toks[2]).name.c_str());
                IRType::IRInstr call; call.op = VMOperator::OBJ_CALL_METHOD; call.line = methodLine; call.args.push_back(std::get<LexerType::Keyword>(toks[2]).name); call.args.push_back(LexerType::Value(argc)); out.push_back(call);
            } else {
                ZEUS_DEBUG_PRINT("[IRGen    ] %s.%s() -> CLASS_STATIC_METHOD_CALL (static)", 
                                 leftName.c_str(), std::get<LexerType::Keyword>(toks[2]).name.c_str());
                IRType::IRInstr call; call.op = VMOperator::CLASS_STATIC_METHOD_CALL; call.line = methodLine; call.args.push_back(std::get<LexerType::Keyword>(toks[2]).name); call.args.push_back(LexerType::Value(argc)); out.push_back(call);
            }

            return closePos + 1; // 消费到 ')'
        };

        auto tryEmitLeadingNew = [&](const std::vector<LexerType::Token>& toks) -> size_t {
            if (toks.size() < 2) {
                return 0;
            }
            if (!_isKeyword(toks[0]) || std::get<LexerType::Keyword>(toks[0]).name != "new") {
                return 0;
            }
            
            // 找到最外层的构造函数参数括号对
            // 策略：从后往前找，只跟踪圆括号深度，找到第一个深度为0的 '('
            // 注意：花括号和方括号的匹配不影响圆括号的匹配
            int lastOpenParen = -1;
            int parenDepth = 0;      // 只跟踪 ()
            
            // 从后往前扫描，找到 parenDepth == 0 时的第一个 '('
            for (int i = toks.size() - 1; i >= 1; --i) {
                if (_isOperator(toks[i])) {
                    std::string sym = _opSym(toks[i]);
                    if (sym == "(") {
                        // 从后往前：遇到 '(' 时，如果 depth == 1，说明这个 '(' 只被一层 ')' 包裹
                        // 即它是最外层的构造函数参数左括号
                        if (parenDepth == 1) {
                            lastOpenParen = i;
                            break;
                        } else if (parenDepth > 1) {
                            parenDepth--;
                        }
                        // 如果 depth == 0，说明括号不匹配，跳过
                    } else if (sym == ")") {
                        parenDepth++;
                    }
                    // 忽略 {} 和 []，它们不影响圆括号的配对
                }
            }
            
            if (lastOpenParen <= 0) return 0; // 没找到有效的构造函数参数括号
            
            // 提取 new 和最后一个 ( 之间的表达式作为类表达式
            std::vector<LexerType::Token> classExprTokens(toks.begin() + 1, toks.begin() + lastOpenParen);
            
            if (classExprTokens.empty()) return 0;

            std::string nativeConstructionType;
            if (classExprTokens.size() == 1 &&
                std::holds_alternative<LexerType::Keyword>(classExprTokens.front())) {
                const std::string& targetName =
                    std::get<LexerType::Keyword>(classExprTokens.front()).name;
                if (IRScope::resolve(targetName).kind == IRScope::Kind::UNDEFINED) {
                    nativeConstructionType = _nativeConstructionType(targetName);
                }
            }
            
            // 递归调用emitExprIR来计算类表达式（可能包含函数调用、属性访问等）
            if (nativeConstructionType.empty()) {
                auto classExprIR = emitExprIR(status, strictDecl, futureFuncDefs, classExprTokens);
                if (classExprIR.empty()) return 0;
            
                // 将类表达式的IR添加到输出（结果在栈顶就是要实例化的类）
                out.insert(out.end(), classExprIR.begin(), classExprIR.end());
            }
            
            size_t pos2 = lastOpenParen;
            int argc = 0;
            
            // 解析构造函数参数（支持命名参数）
            // 创建临时ExprParseCtx并调用_parseArguments
            ExprParseCtx tempCtx{status, strictDecl, futureFuncDefs,
                IRScope::isUsingOriginal(ZeusBuiltins::BuiltinLiteralKind::Array)};
            
            if (!_parseArguments(tempCtx, toks, pos2, argc, out)) {
                return 0; // 参数解析失败
            }
            
            // 生成 OBJ_BUILD argc
            IRType::IRInstr build; 
            build.op = VMOperator::OBJ_BUILD; 
            build.args.push_back(LexerType::Value(argc));
            if (!nativeConstructionType.empty()) {
                build.args.push_back(nativeConstructionType);
            }
            out.push_back(build);
            
            ZEUS_DEBUG_PRINT("[IRGen tryEmitLeadingNew] new (expression) with %d args", argc);
            return pos2;
        };

        auto tryEmitLeadingAttrChain = [&](const std::vector<LexerType::Token>& toks) -> size_t {
            // 返回已消费 token 数；0 表示不匹配
            if (toks.size() < 3) return 0; // 至少 a . b
            if (!_isKeyword(toks[0])) return 0;
            if (!_isOperator(toks[1]) || _opSym(toks[1]) != ".") return 0;

            // 统计连续的 . ident 序列，但不包括后面跟着 '(' 的属性（那是方法调用）
            size_t pos2 = 2;
            std::vector<std::string> attrs;
            std::vector<size_t> attrPositions; // 记录每个属性的起始位置（包括前面的 '.'）
            while (pos2 < toks.size()) {
                if (!_isKeyword(toks[pos2])) break;
                std::string attr = std::get<LexerType::Keyword>(toks[pos2]).name;
                size_t attrDotPos = pos2 - 1; // 属性前面的 '.' 的位置（第一个属性没有，特殊处理）
                pos2++;
                
                // 检查这个属性后面是否跟着 '('，如果是，则不消费它，留给后缀处理循环
                if (pos2 < toks.size() && _isOperator(toks[pos2]) && _opSym(toks[pos2]) == "(") {
                    // 最后一个属性是方法调用，不加入attrs列表
                    // 回退pos2到这个属性前的 '.' 位置
                    if (attrs.empty()) {
                        // 这是 a.method() 的情况，只有一个属性且是方法调用
                        // 返回0让 tryEmitLeadingObjCall 处理
                        return 0;
                    }
                    // 多个属性，回退到最后一个属性之前（包括 '.'）
                    pos2 = attrDotPos;
                    break;
                }
                
                attrs.push_back(attr);
                attrPositions.push_back(attrDotPos);
                
                if (pos2 >= toks.size()) break;
                if (!_isOperator(toks[pos2]) || _opSym(toks[pos2]) != ".") break;
                pos2++; // 消费 '.' 继续下一轮
            }
            if (attrs.empty()) return 0; // 没有形成 a.b

            // base: a
            // Prefer using the general emitter which resolves locals/upvalues correctly (important for 'this' param).
            // This avoids emitting a placeholder global -1 for unresolved names.
            ExprParseCtx tempCtx{status, strictDecl, futureFuncDefs,
                IRScope::isUsingOriginal(ZeusBuiltins::BuiltinLiteralKind::Array)};
            _emitValueOrKeyword(tempCtx, toks[0], out);
            // 发出链式属性读取
            for (size_t i = 0; i < attrs.size(); ++i) {
                IRType::IRInstr get; get.op = VMOperator::OBJ_LOAD_ATTR;
                // 属性 token 在 attrPositions[i] 的下一个位置
                if (i < attrPositions.size() && attrPositions[i] + 1 < toks.size()) {
                    get.line = _getTokenLine(toks[attrPositions[i] + 1]);
                }
                get.args.push_back(attrs[i]); out.push_back(get);
            }
            return pos2; // 消费到最后一个属性名之后
        };

        auto tryEmitLeadingObjCall = [&](const std::vector<LexerType::Token>& toks) -> size_t {
            // match: <ident> '.' <ident> '(' args ')'  -> LOAD_VALUE base + args IR + OBJ_CALL_METHOD
            if (toks.size() < 6) return 0;
            if (!_isKeyword(toks[0])) return 0;
            if (!_isOperator(toks[1]) || _opSym(toks[1]) != ".") return 0;
            if (!_isKeyword(toks[2])) return 0;
            if (!_isOperator(toks[3]) || _opSym(toks[3]) != "(") return 0;

            // find matching ')'
            int depth = 0; size_t closePos = 0; bool found=false;
            for (size_t i = 3; i < toks.size(); ++i) {
                if (_isOperator(toks[i])) {
                    std::string s = _opSym(toks[i]);
                    if (s == "(") depth++;
                    else if (s == ")") { depth--; if (depth == 0) { closePos = i; found = true; break; } }
                }
            }
            if (!found) return 0;

            // split args (4..closePos-1)
            std::vector<std::vector<LexerType::Token>> argTokList;
            {
                size_t p = 4; size_t end = closePos; int brace=0, brack=0, paren=0; size_t segStart=p;
                while (p < end) {
                    if (_isOperator(toks[p])) {
                        std::string sym = _opSym(toks[p]);
                        if (sym == "(") paren++;
                        else if (sym == ")") paren--;
                        else if (sym == "[") brack++;
                        else if (sym == "]") brack--;
                        else if (sym == "{") brace++;
                        else if (sym == "}") brace--;
                        else if (sym == "," && brace==0 && brack==0 && paren==0) {
                            std::vector<LexerType::Token> seg;
                            for (size_t k = segStart; k < p; ++k) seg.push_back(toks[k]);
                            argTokList.push_back(seg);
                            segStart = p + 1;
                        }
                    }
                    p++;
                }
                if (segStart < end) {
                    std::vector<LexerType::Token> seg;
                    for (size_t k = segStart; k < end; ++k) seg.push_back(toks[k]);
                    argTokList.push_back(seg);
                }
            }

            // emit: LOAD_VALUE base
            std::string baseName = std::get<LexerType::Keyword>(toks[0]).name;
            IRScope::SymRef base = IRScope::resolve(baseName);
            IRType::IRInstr load; load.op = VMOperator::LOAD_VALUE;
            switch (base.kind) {
                case IRScope::Kind::LOCAL:   load.args.push_back(std::string("local"));   load.args.push_back(LexerType::Value(base.index)); load.args.push_back(baseName); break;
                case IRScope::Kind::GLOBAL:  load.args.push_back(std::string("global"));  load.args.push_back(LexerType::Value(base.index)); load.args.push_back(baseName); break;
                case IRScope::Kind::UPVALUE: load.args.push_back(std::string("upvalue")); load.args.push_back(LexerType::Value(base.index)); load.args.push_back(baseName); break;
                case IRScope::Kind::SHARED:
                    load.op = VMOperator::LOAD_VALUE_SHARED;
                    load.args.push_back(baseName);
                    break;
                default: load.args.push_back(baseName); break;
            }
            out.push_back(load);

            int argc = 0;
            for (auto &seg : argTokList) {
                if (!seg.empty()) {
                    auto argIR = emitExprIR(status, strictDecl, futureFuncDefs, seg);
                    out.insert(out.end(), argIR.begin(), argIR.end());
                    argc++;
                }
            }

            int methodLine = _getTokenLine(toks[2]);
            IRType::IRInstr call; call.op = VMOperator::OBJ_CALL_METHOD; call.line = methodLine; call.args.push_back(std::get<LexerType::Keyword>(toks[2]).name); call.args.push_back(LexerType::Value(argc)); out.push_back(call);
            return closePos + 1;
        };

        auto tryEmitLeadingSubscript = [&](const std::vector<LexerType::Token>& toks) -> size_t {
            // 返回已消费 token 数；0 表示不匹配
            ZEUS_DEBUG_PRINT("[IRGen] tryEmitLeadingSubscript: toks.size()=%zu", toks.size());
            for (size_t i = 0; i < toks.size() && i < 5; ++i) {
                if (_isKeyword(toks[i])) {
                    ZEUS_DEBUG_PRINT("[IRGen]   tok[%zu]: keyword '%s'", i, std::get<LexerType::Keyword>(toks[i]).name.c_str());
                } else if (_isOperator(toks[i])) {
                    ZEUS_DEBUG_PRINT("[IRGen]   tok[%zu]: operator '%s'", i, _opSym(toks[i]).c_str());
                } else if (_isValue(toks[i])) {
                    ZEUS_DEBUG_PRINT("[IRGen]   tok[%zu]: value", i);
                }
            }
            if (toks.size() < 4) return 0; // a [ x ] 至少 4 token
            if (!_isKeyword(toks[0])) return 0;
            if (!_isOperator(toks[1]) || _opSym(toks[1]) != "[") return 0;
            // 匹配到对应 ']'（不支持嵌套括号里再出现未闭合 ']' 的异常场景；递归交给 emitExprIR 再次解析）
            int depth = 0; size_t closePos = toks.size();
            for (size_t i = 1; i < toks.size(); ++i) {
                if (_isOperator(toks[i])) {
                    std::string s = _opSym(toks[i]);
                    if (s == "[") depth++;
                    else if (s == "]") {
                        depth--;
                        if (depth == 0) { closePos = i; break; }
                    }
                }
            }
            if (closePos == toks.size()) return 0; // 没找到匹配 ']'。
            // key expr = (2 .. closePos-1)
            std::vector<LexerType::Token> keyTokens;
            for (size_t k = 2; k < closePos; ++k) keyTokens.push_back(toks[k]);
            // 生成：LOAD_VALUE baseVar + keyExprIR + DICT_LOAD_VALUE
            std::string baseName = std::get<LexerType::Keyword>(toks[0]).name;
            IRScope::SymRef s = IRScope::resolve(baseName);
            ZEUS_DEBUG_PRINT("[IRGen] tryEmitLeadingSubscript: baseName='%s' kind=%d", baseName.c_str(), (int)s.kind);
            if (s.kind == IRScope::Kind::UNDEFINED) {
                // 尝试强制查找全局变量(可能刚声明但还未完成STORE)
                // 如果确实不存在,则让VM在运行时报错
                s.kind = IRScope::Kind::GLOBAL;
                auto it = IRScope::globals.find(baseName);
                if (it != IRScope::globals.end()) {
                    s.index = it->second;
                    ZEUS_DEBUG_PRINT("[IRGen] tryEmitLeadingSubscript: found in globals, index=%d", s.index);
                } else {
                    // 变量真的不存在,返回0让后续逻辑处理
                    ZEUS_DEBUG_PRINT("[IRGen] tryEmitLeadingSubscript: not found in globals, strictDecl=%d", strictDecl);
                    if (strictDecl) return 0;
                    s.index = -1; // 占位,VM会报错
                }
            }
            // base load
            {
                IRType::IRInstr load; load.op = VMOperator::LOAD_VALUE;
                load.line = _getTokenLine(toks[0]); // 从 baseName token 获取行号
                switch (s.kind) {
                    case IRScope::Kind::LOCAL:   load.args.push_back(std::string("local"));   load.args.push_back(LexerType::Value(s.index)); load.args.push_back(baseName); break;
                    case IRScope::Kind::GLOBAL:  load.args.push_back(std::string("global"));  load.args.push_back(LexerType::Value(s.index)); load.args.push_back(baseName); break;
                    case IRScope::Kind::UPVALUE: load.args.push_back(std::string("upvalue")); load.args.push_back(LexerType::Value(s.index)); load.args.push_back(baseName); break;
                    case IRScope::Kind::SHARED:
                        load.op = VMOperator::LOAD_VALUE_SHARED;
                        load.args.push_back(baseName);
                        break;
                    default: load.args.push_back(std::string("global")); load.args.push_back(LexerType::Value(-1)); load.args.push_back(baseName); break; // 占位，后续 VM 若遇 -1 会出错
                }
                out.push_back(load);
            }
            // key expr
            if (!keyTokens.empty()) {
                auto keyIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, keyTokens);
                out.insert(out.end(), keyIR.begin(), keyIR.end());
            } else {
                // 空 key，直接返回 0（或错误）；这里推入一个 nil/0 以避免 VM 下溢
                IRType::IRInstr zero; zero.op = VMOperator::PUSH; 
                zero.line = _getTokenLine(toks[1]); // 从 '[' token 获取行号
                zero.args.push_back(LexerType::Value(0)); 
                out.push_back(zero);
            }
            {
                IRType::IRInstr load; load.op = VMOperator::DICT_LOAD_VALUE; 
                load.line = !keyTokens.empty() ? _getTokenLine(keyTokens[0]) : _getTokenLine(toks[1]); // 从 key 或 '[' 获取行号
                out.push_back(load);
            }
            return closePos + 1; // 已消费到 ']' 之后
        };

        // 处理括号包裹的表达式作为前缀，例如 (new Array(...)).method()
        auto tryEmitLeadingParenExpr = [&](const std::vector<LexerType::Token>& toks) -> size_t {
            ZEUS_DEBUG_PRINT("[IRGen] tryEmitLeadingParenExpr: toks.size()=%zu", toks.size());
            if (toks.size() < 3) return 0;
            if (!_isOperator(toks[0]) || _opSym(toks[0]) != "(") return 0;
            
            // 找到匹配的右括号
            int depth = 1;
            size_t closePos = 1;
            while (closePos < toks.size() && depth > 0) {
                if (_isOperator(toks[closePos])) {
                    std::string sym = _opSym(toks[closePos]);
                    if (sym == "(") depth++;
                    else if (sym == ")") depth--;
                }
                closePos++;
            }
            
            if (depth != 0) return 0; // 未匹配的括号
            
            // 提取括号内的 tokens
            std::vector<LexerType::Token> innerTokens(toks.begin() + 1, toks.begin() + closePos - 1);
            
            // 递归解析括号内的表达式
            auto innerIR = emitExprIR(status, strictDecl, futureFuncDefs, innerTokens);
            out.insert(out.end(), innerIR.begin(), innerIR.end());
            
            ZEUS_DEBUG_PRINT("[IRGen] tryEmitLeadingParenExpr: parsed (...) expression, consumed %zu tokens, generated %zu instructions", closePos, innerIR.size());
            return closePos; // 返回消费的 token 数量（包括两个括号）
        };

        size_t consumed = 0;
        // 优先尝试 new，再尝试静态调用、属性链、下标、括号表达式
        consumed = tryEmitLeadingNew(tokens);
        ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: tryEmitLeadingNew returned %zu", consumed);
        if (consumed == 0) {
            consumed = tryEmitLeadingStaticCall(tokens);
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: tryEmitLeadingStaticCall returned %zu", consumed);
        }
        if (consumed == 0) {
            consumed = tryEmitLeadingObjCall(tokens);
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: tryEmitLeadingObjCall returned %zu", consumed);
        }
        if (consumed == 0) {
            consumed = tryEmitLeadingAttrChain(tokens);
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: tryEmitLeadingAttrChain returned %zu", consumed);
        }
        if (consumed == 0) {
            consumed = tryEmitLeadingParenExpr(tokens);
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: tryEmitLeadingParenExpr returned %zu", consumed);
        }
        if (consumed == 0) {
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: trying tryEmitLeadingSubscript, tokens.size()=%zu", tokens.size());
            consumed = tryEmitLeadingSubscript(tokens);
            ZEUS_DEBUG_PRINT("[IRGen] emitExprIR: tryEmitLeadingSubscript returned %zu", consumed);
        }
        size_t pos = 0;
        if (consumed > 0) {
            pos = consumed; // remaining tokens should be parsed as expression tail (binary ops etc.)
            if (pos >= tokens.size()) return out; // only the prefix
            
            // Handle additional suffixes: method calls, attribute access, subscripts
            // This handles patterns like obj.method1().method2() or obj.method()[0].attr
            ExprParseCtx suffixCtx { status, strictDecl, futureFuncDefs,
                IRScope::isUsingOriginal(ZeusBuiltins::BuiltinLiteralKind::Array) };
            while (pos < tokens.size()) {
                // Check for call suffix: ()
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                    int argc = 0;
                    if (!_parseArguments(suffixCtx, tokens, pos, argc, out)) {
                        return std::vector<IRType::IRInstr>();
                    }
                    IRType::IRInstr call;
                    call.op = VMOperator::CALL_CLOSURE;
                    call.args.push_back(LexerType::Value(argc));
                    out.push_back(call);
                    continue;
                }
                
                // Check for attribute access: .attr
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == ".") {
                    if (pos + 1 >= tokens.size() || !_isKeyword(tokens[pos + 1])) break;
                    std::string attrName = std::get<LexerType::Keyword>(tokens[pos + 1]).name;
                    int attrLine = _getTokenLine(tokens[pos + 1]);
                    pos += 2; // consume '.' and attribute name
                    
                    // Check if followed by () -> method call
                    if (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(") {
                        // Push arguments for method call
                        int argc = 0;
                        if (!_parseArguments(suffixCtx, tokens, pos, argc, out)) {
                            return std::vector<IRType::IRInstr>();
                        }
                        // Generate OBJ_CALL_METHOD
                        IRType::IRInstr call;
                        call.op = VMOperator::OBJ_CALL_METHOD;
                        call.line = attrLine;
                        call.args.push_back(attrName);
                        call.args.push_back(LexerType::Value(argc));
                        out.push_back(call);
                    } else {
                        // Just attribute access
                        IRType::IRInstr load;
                        load.op = VMOperator::OBJ_LOAD_ATTR;
                        load.line = attrLine;
                        load.args.push_back(attrName);
                        out.push_back(load);
                    }
                    continue;
                }
                
                // Check for subscript: [index]
                if (_isOperator(tokens[pos]) && _opSym(tokens[pos]) == "[") {
                    // Find matching ]
                    int depth = 1;
                    size_t closePos = pos + 1;
                    while (closePos < tokens.size() && depth > 0) {
                        if (_isOperator(tokens[closePos])) {
                            std::string s = _opSym(tokens[closePos]);
                            if (s == "[") depth++;
                            else if (s == "]") depth--;
                        }
                        closePos++;
                    }
                    if (depth != 0) break; // Unmatched brackets
                    
                    // Parse index expression
                    std::vector<LexerType::Token> indexTokens;
                    for (size_t i = pos + 1; i < closePos - 1; ++i) {
                        indexTokens.push_back(tokens[i]);
                    }
                    
                    if (!indexTokens.empty()) {
                        auto indexIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, indexTokens);
                        out.insert(out.end(), indexIR.begin(), indexIR.end());
                    } else {
                        // Empty index - push 0
                        IRType::IRInstr zero;
                        zero.op = VMOperator::PUSH;
                        zero.line = _getTokenLine(tokens[pos]); // 从 '[' token 获取行号
                        zero.args.push_back(LexerType::Value(0));
                        out.push_back(zero);
                    }
                    
                    IRType::IRInstr load;
                    load.op = VMOperator::DICT_LOAD_VALUE;
                    load.line = !indexTokens.empty() ? _getTokenLine(indexTokens[0]) : _getTokenLine(tokens[pos]); // 从索引表达式或 '[' 获取行号
                    out.push_back(load);
                    
                    pos = closePos;
                    continue;
                }
                
                // No more suffixes
                break;
            }
        }
        ExprParseCtx ctx { status, strictDecl, futureFuncDefs,
            IRScope::isUsingOriginal(ZeusBuiltins::BuiltinLiteralKind::Array) };
        bool ok;
        if (consumed > 0) {
            // left-hand side already emitted into 'out' by the prefix emitter
            ok = _parseExpressionTail(ctx, tokens, pos, 0, out);
        } else {
            ok = _parseExpression(ctx, tokens, pos, 0, out);
        }
        if (!ok || pos != tokens.size()) {
            // 解析失败，返回空（或抛异常也可）
            return std::vector<IRType::IRInstr>();
        }
        
        // Set line numbers for all generated instructions based on first token
        // Use 'tokens' (processed) instead of 'inTokens' (original) because tokens is what we actually parsed
        if (!tokens.empty() && !out.empty()) {
            int line = _getTokenLine(tokens[0]);
            if (line > 0) {
                for (auto& instr : out) {
                    if (instr.line == -1) {
                        instr.line = line;
                    }
                }
            }
        }
        
        return out;
    }

} // namespace IRGen









// IR Generate part - helper: 显式传入所需最小上下文，避免在类定义前使用不完整类型访问成员引起 C2027
void parseVarDecl(
    IRType::IRCompilerStatus& status,
    bool strictDecl,
    const std::unordered_set<std::string>& futureFuncDefs,
    const TreeParser::Node& node,
    std::vector<IRType::IRInstr>& instructions
) {

    if (node.type == NodeType::VAR_DECL_STMT) {
        bool isGlobal = false, isConst = false, isShared = false, savingRightValue = false;
        bool readingTypeAnnotation = false;
        std::string varName;
        std::vector<LexerType::Token> rightValue;
        // 处于顶层（非函数体）时，所有 let/var 一律使用全局槽，避免函数内部引用顶层局部被当作 local 而无法捕获的问题
        bool atTopLevel = IRScope::funcStack.empty();
        bool usedLet = false;  // 用于检测 let shared 错误

        for (const auto& token : node.tokens) {
            if (std::holds_alternative<LexerType::Operator>(token)) {
                const auto& op = std::get<LexerType::Operator>(token);

                if (!savingRightValue && op.symbol == ":") {
                    readingTypeAnnotation = true;
                    continue;
                }

                // 只有第一个 = 用于分割左右两侧,后续的 = 应该保留在 rightValue 中(用于命名参数)
                if (!savingRightValue && op.symbol == "=") {
                    savingRightValue = true;
                    continue; 
                }
            }

            if (!savingRightValue) {
                if (std::holds_alternative<LexerType::Keyword>(token)) {
                    const auto& keyword = std::get<LexerType::Keyword>(token);

                    if (readingTypeAnnotation) {
                        readingTypeAnnotation = false;
                        continue;
                    }

                    if      (keyword.name == "var") {
                        isGlobal = true; // var 显式全局
                    }
                    else if (keyword.name == "let") {
                        // 顶层 let 也升级为全局；函数内 let 仍为局部
                        isGlobal = atTopLevel ? true : false;
                        usedLet = true;
                    }
                    else if (keyword.name == "const") {
                        isConst  = true;
                    }
                    else if (keyword.name == "shared") {
                        isShared = true;
                    }
                    else {
                        varName  = keyword.name;
                    }
                }

            } else rightValue.push_back(token);
        }

        // 检查 let shared 错误：shared 只能与 var 配合使用
        if (isShared && usedLet) {
            int errLine = node.tokens.empty() ? 0 : 
                (std::holds_alternative<LexerType::Keyword>(node.tokens[0]) ? 
                    std::get<LexerType::Keyword>(node.tokens[0]).line : 0);
            IR_THROW_NODE(errLine, node, "Cannot use 'let shared'. Shared variables must use 'var shared' syntax.", status);
            return;
        }

        ZEUS_DEBUG_EXEC({
            ZEUS_DEBUG_PRINT("[IRGen    ] {VarDecl} RV tokens (size=%zu):", rightValue.size());
            IRDebugUtils::printTokens(rightValue);
        });

        // 统一由 emitExprIR 处理表达式 -> IR
        {
            bool injectedEmptyDict = false;
            // 优先匹配：new ClassName(…)?
            bool handledNew = false;
            if (rightValue.size() >= 2 &&
                std::holds_alternative<LexerType::Keyword>(rightValue[0]) &&
                std::get<LexerType::Keyword>(rightValue[0]).name == "new" &&
                std::holds_alternative<LexerType::Keyword>(rightValue[1])) {
                
                // 检查是否有括号 - 向前查找第一个 '(' （可能在属性访问链之后）
                bool hasParens = false;
                for (size_t i = 2; i < rightValue.size(); ++i) {
                    if (std::holds_alternative<LexerType::Operator>(rightValue[i])) {
                        const std::string& sym = std::get<LexerType::Operator>(rightValue[i]).symbol;
                        if (sym == "(") {
                            hasParens = true;
                            break;
                        }
                        // 如果遇到其他非 '.' 的操作符，停止查找
                        if (sym != ".") break;
                    } else if (!std::holds_alternative<LexerType::Keyword>(rightValue[i])) {
                        // 遇到非关键字（如值），停止查找
                        break;
                    }
                }
                
                if (!hasParens) {
                    // 没有参数的构造函数: new ClassName
                    std::string clsName = std::get<LexerType::Keyword>(rightValue[1]).name;
                    const std::string nativeConstructionType = rightValue.size() == 2 &&
                        IRScope::resolve(clsName).kind == IRScope::Kind::UNDEFINED
                            ? IRGen::_nativeConstructionType(clsName)
                            : std::string();
                    // 生成：LOAD_VALUE class + OBJ_BUILD
                    // 优先尝试解析为符号，如果失败则使用类名字符串（支持 forward declaration）
                    IRScope::SymRef cls = IRScope::resolve(clsName);
                    if (nativeConstructionType.empty()) {
                        IRType::IRInstr load; load.op = VMOperator::LOAD_VALUE;
                        if (cls.kind == IRScope::Kind::LOCAL) {
                            load.args.push_back(std::string("local"));
                            load.args.push_back(LexerType::Value((int)cls.index));
                            load.args.push_back(clsName); // 添加类名用于错误报告
                        } else if (cls.kind == IRScope::Kind::GLOBAL) {
                            load.args.push_back(std::string("global"));
                            load.args.push_back(LexerType::Value((int)cls.index));
                            load.args.push_back(clsName); // 添加类名用于错误报告
                        } else if (cls.kind == IRScope::Kind::UPVALUE) {
                            load.args.push_back(std::string("upvalue"));
                            load.args.push_back(LexerType::Value((int)cls.index));
                            load.args.push_back(clsName); // 添加类名用于错误报告
                        } else {
                            // Forward declaration: 使用类名字符串，运行时解析
                            // VM 会直接压入 args[0] 的值
                            load.args.push_back(clsName);
                            load.args.push_back(LexerType::Value(0)); // dummy arg[1]
                        }
                        instructions.push_back(load);
                    }
                    IRType::IRInstr build; build.op = VMOperator::OBJ_BUILD; 
                    build.args.push_back(LexerType::Value(0)); // 0 个参数
                    if (!nativeConstructionType.empty()) {
                        build.args.push_back(nativeConstructionType);
                    }
                    instructions.push_back(build);
                    handledNew = true;
                }
            }
            if (!handledNew) {
          // 处理空字典 {}
          if ((rightValue.size() == 2 \
              && std::holds_alternative<LexerType::Operator>(rightValue[0]) \
              && std::holds_alternative<LexerType::Operator>(rightValue[1]) \
              && std::get<LexerType::Operator>(rightValue[0]).symbol == "{" \
              && std::get<LexerType::Operator>(rightValue[1]).symbol == "}") ||
                (rightValue.size() == 1 && std::holds_alternative<LexerType::Operator>(rightValue[0]) && std::get<LexerType::Operator>(rightValue[0]).symbol == "{")) {
                // 情况1: 标准 {} 两 token
                // 情况2: 兼容旧 fallback（单 '{'）
                IRType::IRInstr buildEmpty; buildEmpty.op = VMOperator::DICT_BUILD; buildEmpty.args.push_back(LexerType::Value(0)); instructions.push_back(buildEmpty); injectedEmptyDict = true;
            }
          // 处理空列表 []
          else if (rightValue.size() == 2 \
              && std::holds_alternative<LexerType::Operator>(rightValue[0]) \
              && std::holds_alternative<LexerType::Operator>(rightValue[1]) \
              && std::get<LexerType::Operator>(rightValue[0]).symbol == "[" \
              && std::get<LexerType::Operator>(rightValue[1]).symbol == "]") {
                IRType::IRInstr buildEmpty; 
                buildEmpty.op = VMOperator::LIST_BUILD; 
                buildEmpty.args.push_back(LexerType::Value(0)); 
                instructions.push_back(buildEmpty); 
                
                // 包装为Array对象
                if (ZeusBuiltins::shouldWrapLiteral(
                        ZeusBuiltins::BuiltinLiteralKind::Array,
                        IRScope::isUsingOriginal(
                            ZeusBuiltins::BuiltinLiteralKind::Array))) {
                    IRScope::SymRef arrayRef = IRScope::resolve("ZeusBuiltins.list.Array");
                    if (arrayRef.kind != IRScope::Kind::GLOBAL) {
                        arrayRef = IRScope::resolve("Array");
                    }
                    
                    if (arrayRef.kind == IRScope::Kind::GLOBAL) {
                        IRType::IRInstr loadArray;
                        loadArray.op = VMOperator::LOAD_VALUE;
                        loadArray.args.push_back(std::string("global"));
                        loadArray.args.push_back(LexerType::Value(arrayRef.index));
                        instructions.push_back(loadArray);
                        
                        IRType::IRInstr swap;
                        swap.op = VMOperator::SWAP;
                        instructions.push_back(swap);
                        
                        IRType::IRInstr objBuild;
                        objBuild.op = VMOperator::OBJ_BUILD;
                        objBuild.args.push_back(LexerType::Value(1));
                        instructions.push_back(objBuild);
                    }
                }
                
                injectedEmptyDict = true;
            } else {
                std::vector<IRType::IRInstr> rhs = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, rightValue);
                if (!rhs.empty()) {
                    instructions.insert(instructions.end(), rhs.begin(), rhs.end());
                }

                ZEUS_DEBUG_EXEC({
                    ZEUS_DEBUG_PRINT("[IRGen    ] {VarDecl} RV IR:");
                    IRDebugUtils::printVectorInstructions(rhs);
                });
            }
            }

            if (injectedEmptyDict) {
                ZEUS_DEBUG_PRINT("[IRGen    ] {VarDecl} RV IR: (empty dict injected)");
            } else if (rightValue.size()>=2 && std::holds_alternative<LexerType::Operator>(rightValue.front()) && std::get<LexerType::Operator>(rightValue.front()).symbol=="{" && std::holds_alternative<LexerType::Operator>(rightValue.back()) && std::get<LexerType::Operator>(rightValue.back()).symbol=="}") {
                // 如果 emitExprIR 仍然没有输出（rhs 空），尝试再次手动解析字典字面量（退化路径）
                bool hasColon=false; for(size_t i=1;i+1<rightValue.size();++i){ if(std::holds_alternative<LexerType::Operator>(rightValue[i]) && std::get<LexerType::Operator>(rightValue[i]).symbol==":"){ hasColon=true; break; } }
                if(hasColon) {
                    // 简单单次线性解析：keyExpr ':' valExpr 以 ',' 分隔
                    size_t pos=1; size_t end=rightValue.size()-1; int pairCount=0; std::vector<IRType::IRInstr> manual;
                    auto emitExpr=[&](size_t start,size_t stop){ // [start,stop)
                        std::vector<LexerType::Token> seg; for(size_t k=start;k<stop;k++) seg.push_back(rightValue[k]);
                        auto segIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, seg);
                        manual.insert(manual.end(), segIR.begin(), segIR.end());
                    };
                    while(pos<end){ size_t keyStart=pos; // 找到 ':'
                        size_t colonPos=end; for(size_t t=pos;t<end;++t){ if(std::holds_alternative<LexerType::Operator>(rightValue[t]) && std::get<LexerType::Operator>(rightValue[t]).symbol==":"){ colonPos=t; break; } }
                        if(colonPos==end) break; // 失败退出
                        emitExpr(keyStart, colonPos);
                        // value
                        size_t valStart=colonPos+1; size_t valEnd=end; bool hasComma=false; size_t commaPos=end; for(size_t t=valStart;t<end;++t){ if(std::holds_alternative<LexerType::Operator>(rightValue[t]) && std::get<LexerType::Operator>(rightValue[t]).symbol==",") { hasComma=true; commaPos=t; break; } }
                        if(hasComma){ valEnd=commaPos; }
                        emitExpr(valStart, valEnd);
                        pairCount++;
                        if(hasComma) pos=commaPos+1; else break;
                    }
                    if(pairCount>0){ manual.push_back(IRUtils::makeDictBuild(pairCount)); instructions.insert(instructions.end(), manual.begin(), manual.end()); ZEUS_DEBUG_PRINT("[IRGen    ] {VarDecl} RV IR: (manual dict fallback, pairs=%d)", pairCount); }
                }
            }
        }

        // STORE：改为索引参数（第一参数为槽位索引的 Value(int)）
        if (!varName.empty()) {
            IRType::IRInstr st;

            if (isShared) {
                // 共享变量：存储到进程级共享堆
                IRScope::declareShared(varName);
                st.op = VMOperator::STORE_VALUE_SHARED;
                st.args.push_back(varName);  // 按名称访问
                st.args.push_back(std::string(isConst ? "const" : "mutable"));
            }
            else if (isGlobal) {
                int gid = IRScope::declareGlobal(varName);
                st.op = VMOperator::STORE_VALUE_GLOBAL;
                st.args.push_back(LexerType::Value(gid));
                st.args.push_back(std::string(isConst ? "const" : "mutable"));
                st.args.push_back(varName); // inline 名称，供 VM 旧格式登记
            }
            else {
                int lid = IRScope::declareLocal(varName);
                st.op = VMOperator::STORE_VALUE_LOCAL;
                st.args.push_back(LexerType::Value(lid));
                st.args.push_back(std::string(isConst ? "const" : "mutable"));
            }

            instructions.push_back(st);

            // Record constness information for locals so resolve() can reflect it during assignment checks
            if (!isGlobal && !isShared) {
                IRScope::markLocalConst(varName, isConst);
            }
        }
    }
}


void parseVarAssignStmt(
    IRType::IRCompilerStatus& status,
    bool strictDecl,
    const std::unordered_set<std::string>& futureFuncDefs,
    const TreeParser::Node& node,
    std::vector<IRType::IRInstr>& instructions) {
    // tokens: <identifier> = <expr>
    std::string varName;
    bool afterEq = false;
    std::vector<LexerType::Token> exprTokens;          // 右侧表达式 tokens
    std::vector<LexerType::Token> leftTokens;          // '=' 左侧全部 token（用于识别 d[i] 形式）
    // 构造一个用于错误打印的镜像节点（沿用原 node 方便 tokens 打印）
    TreeParser::Node mirror = node;

    for (const auto &tk : node.tokens) {
        if (std::holds_alternative<LexerType::Operator>(tk) && std::get<LexerType::Operator>(tk).symbol == "=") {
            afterEq = true;
            continue;
        }
        if (!afterEq) {
            leftTokens.push_back(tk);
            continue;
        }
        exprTokens.push_back(tk);
    }

    // 提取基础变量名（左侧第一个 keyword）
    for (auto &tk : leftTokens) {
        if (std::holds_alternative<LexerType::Keyword>(tk)) {
            varName = std::get<LexerType::Keyword>(tk).name;
            break;
        }
    }

    if (varName.empty()) return;

    IRScope::SymRef s = IRScope::resolve(varName);

    // 如果禁止隐式捕获并且解析不到符号（即可能是引用外层局部），则强制要求显式捕获/声明
    if (s.kind == IRScope::Kind::UNDEFINED && !IRScope::allowImplicitCapture && !IRScope::isCaptureAllActive()) {
        int errLine = mirror.tokens.empty() ? -1 : IRGen::_getTokenLine(mirror.tokens[0]);
        std::string fname = IRScope::funcStack.empty() ? std::string("<top>") : IRScope::funcStack.back().name;
        std::string msg = "Cannot find local variable '" + varName + "' in function '" + fname + "'";
        IR_THROW_NODE(errLine, mirror, msg, status);
        return;
    }

    // 检测下标形式 d[ keyExpr ] = expr
    bool isSubscriptAssign = false;
    if (leftTokens.size() >= 4) {
        // pattern: <ident> '[' ... ']' （中间允许任意表达式 token）
        if (std::holds_alternative<LexerType::Keyword>(leftTokens[0]) &&
            std::holds_alternative<LexerType::Operator>(leftTokens[1]) && std::get<LexerType::Operator>(leftTokens[1]).symbol == "[") {
            // 找到匹配的 closing bracket 末尾
            if (std::holds_alternative<LexerType::Operator>(leftTokens.back()) && std::get<LexerType::Operator>(leftTokens.back()).symbol == "]") {
                isSubscriptAssign = true;
            }
        }
    }

    if (isSubscriptAssign) {
        // key tokens = between first '[' and final ']'
        std::vector<LexerType::Token> keyTokens;
        bool inside = false;
        for (size_t i = 1; i < leftTokens.size(); ++i) {
            const auto &tk = leftTokens[i];
            if (!inside) {
                if (std::holds_alternative<LexerType::Operator>(tk) && std::get<LexerType::Operator>(tk).symbol == "[") {
                    inside = true; continue;
                }
            } else {
                if (std::holds_alternative<LexerType::Operator>(tk) && std::get<LexerType::Operator>(tk).symbol == "]") {
                    break; // 完成 key 收集
                }
                keyTokens.push_back(tk);
            }
        }

        if (s.isConst) {
            IR_THROW_NODE(0, mirror, "assignment to const variable (subscript base)", status);
            return;
        }

        // 生成：LOAD_VALUE baseVar  + keyExpr IR + rhsExpr IR + DICT_STORE_VALUE
        // base variable
        {
            IRType::IRInstr load;
            load.op = VMOperator::LOAD_VALUE;
            if (s.kind == IRScope::Kind::UNDEFINED) {
                // 严格显式捕获模式下禁止隐式 forceCapture；在严格模式下提示用户显式声明捕获
                if (!IRScope::allowImplicitCapture && !IRScope::isCaptureAllActive()) {
                    int errLine = !leftTokens.empty() ? IRGen::_getTokenLine(leftTokens[0]) : (mirror.tokens.empty() ? -1 : IRGen::_getTokenLine(mirror.tokens[0]));
                    std::string fname = IRScope::funcStack.empty() ? std::string("<top>") : IRScope::funcStack.back().name;
                    std::string msg = "找不到函数 '" + fname + "' 中的局部变量 '" + varName + "'";
                    IR_THROW_NODE(errLine, mirror, msg, status);
                    return;
                }
                // 尝试强制建立捕获（应对 [&alias] 但尚未形成 upvalue 的情况）
                s = IRScope::forceCapture(varName);
            }
            switch (s.kind) {
                case IRScope::Kind::LOCAL:
                    load.args.push_back(std::string("local"));
                    load.args.push_back(LexerType::Value((int)s.index));
                    load.args.push_back(varName); // 添加变量名用于错误报告
                    break;
                case IRScope::Kind::GLOBAL:
                    load.args.push_back(std::string("global"));
                    load.args.push_back(LexerType::Value((int)s.index));
                    load.args.push_back(varName); // 添加变量名用于错误报告
                    break;
                case IRScope::Kind::UPVALUE:
                    load.args.push_back(std::string("upvalue"));
                    load.args.push_back(LexerType::Value((int)s.index));
                    load.args.push_back(varName); // 添加变量名用于错误报告
                    break;
                case IRScope::Kind::SHARED:
                    load.op = VMOperator::LOAD_VALUE_SHARED;
                    load.args.push_back(varName);
                    break;
                default:
                    IR_THROW_NODE(0, mirror, "unsupported subscript base (not local/global/upvalue/shared)", status);
                    return;
            }
            instructions.push_back(load);
        }

        // key expr
        {
            auto keyIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, keyTokens);
            instructions.insert(instructions.end(), keyIR.begin(), keyIR.end());
        }

        // rhs expr (可能包含再次的 g[ key ] 访问；如果 emitExprIR 尚未实现下标读取，这里做一个快速 fallback)
        bool emittedRHS = false;
        if (!exprTokens.empty()) {
            // 检测前缀是否与左侧相同的 <ident> '[' pattern
            bool rhsStartsWithSameSub = false;
            if (exprTokens.size() >= 4 && std::holds_alternative<LexerType::Keyword>(exprTokens[0]) && std::holds_alternative<LexerType::Operator>(exprTokens[1]) && std::get<LexerType::Operator>(exprTokens[1]).symbol == "[") {
                // 找到对应 ']'，简单线性匹配
                int depth = 0; size_t closePos = exprTokens.size();
                for (size_t i = 1; i < exprTokens.size(); ++i) {
                    if (std::holds_alternative<LexerType::Operator>(exprTokens[i])) {
                        auto sym = std::get<LexerType::Operator>(exprTokens[i]).symbol;
                        if (sym == "[") depth++;
                        else if (sym == "]") { depth--; if (depth == 0) { closePos = i; break; } }
                    }
                }
                if (closePos < exprTokens.size() &&
                    std::get<LexerType::Keyword>(exprTokens[0]).name == varName &&
                    closePos - 2 == keyTokens.size()) {
                    rhsStartsWithSameSub = true;
                    for (size_t i = 0; i < keyTokens.size(); ++i) {
                        const auto& lhsKeyToken = keyTokens[i];
                        const auto& rhsKeyToken = exprTokens[i + 2];
                        if (lhsKeyToken.index() != rhsKeyToken.index() ||
                            std::visit([](const auto& token) {
                                return token.toString();
                            }, lhsKeyToken) !=
                            std::visit([](const auto& token) {
                                return token.toString();
                            }, rhsKeyToken)) {
                            rhsStartsWithSameSub = false;
                            break;
                        }
                    }
                }
            }
            if (rhsStartsWithSameSub) {
                // 简单拆分：subscriptReadTokens (+ 余下 remainderTokens)
                std::vector<LexerType::Token> remainderTokens;
                // 假设 key 与左侧相同，复用 keyTokens（更严谨可以重新提取，但此处优化路径）
                // 生成读取 g[ key ]
                IRType::IRInstr loadBase; loadBase.op = VMOperator::LOAD_VALUE;
                switch (s.kind) {
                    case IRScope::Kind::LOCAL: loadBase.args.push_back(std::string("local")); loadBase.args.push_back(LexerType::Value(s.index)); break;
                    case IRScope::Kind::GLOBAL: loadBase.args.push_back(std::string("global")); loadBase.args.push_back(LexerType::Value(s.index)); break;
                    case IRScope::Kind::UPVALUE: loadBase.args.push_back(std::string("upvalue")); loadBase.args.push_back(LexerType::Value(s.index)); break;
                    case IRScope::Kind::SHARED:
                        loadBase.op = VMOperator::LOAD_VALUE_SHARED;
                        loadBase.args.push_back(varName);
                        break;
                    default: loadBase.args.push_back(std::string("global")); loadBase.args.push_back(LexerType::Value(-1)); break;
                }
                instructions.push_back(loadBase);
                // key expr 复用 keyTokens
                auto keyIR2 = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, keyTokens);
                instructions.insert(instructions.end(), keyIR2.begin(), keyIR2.end());
                {
                    IRType::IRInstr loadDictVal; 
                    loadDictVal.op = VMOperator::DICT_LOAD_VALUE; 
                    loadDictVal.line = !leftTokens.empty() ? IRGen::_getTokenLine(leftTokens[0]) : -1; // 从基础变量 token 获取行号
                    instructions.push_back(loadDictVal);
                }
                // 剩余 tokens: 从 subscript 结束后
                // 找 ']' 位置
                size_t closePos = 0; int depth2 = 0; bool found=false;
                for (size_t i=1;i<exprTokens.size();++i){
                    if (std::holds_alternative<LexerType::Operator>(exprTokens[i])) {
                        auto sym = std::get<LexerType::Operator>(exprTokens[i]).symbol;
                        if (sym=="[") depth2++;
                        else if (sym=="]") { depth2--; if (depth2==0){ closePos=i; found=true; break; } }
                    }
                }
                if (found && closePos + 1 < exprTokens.size()) {
                    for (size_t j = closePos + 1; j < exprTokens.size(); ++j) remainderTokens.push_back(exprTokens[j]);
                }
                if (!remainderTokens.empty()) {
                    // 处理形如: g[key] = g[key] + 1  (remainder 以二元运算符开头) 的情况：
                    // remainderTokens: ['+', '1'] -> 旧值已在栈顶，只需解析 RHS 并追加 ADD
                    if (std::holds_alternative<LexerType::Operator>(remainderTokens[0])) {
                        std::string leadOp = std::get<LexerType::Operator>(remainderTokens[0]).symbol;
                        static const std::unordered_set<std::string> binOps{ "+", "-", "*", "/", "%" };
                        if (binOps.count(leadOp)) {
                            std::vector<LexerType::Token> rhsTokens;
                            for (size_t r = 1; r < remainderTokens.size(); ++r) rhsTokens.push_back(remainderTokens[r]);
                            if (!rhsTokens.empty()) {
                                auto rhsIR2 = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, rhsTokens);
                                instructions.insert(instructions.end(), rhsIR2.begin(), rhsIR2.end());
                                // 追加对应二元操作，将 (旧值 op RHS) 规约
                                IRType::IRInstr binIns = IRUtils::makeBinaryOpBySymbol(leadOp);
                                instructions.push_back(binIns);
                            }
                        } else {
                            auto tailIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, remainderTokens);
                            instructions.insert(instructions.end(), tailIR.begin(), tailIR.end());
                        }
                    } else {
                        auto tailIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, remainderTokens);
                        instructions.insert(instructions.end(), tailIR.begin(), tailIR.end());
                    }
                }
                emittedRHS = true;
            }
        }
        if (!emittedRHS) {
            auto rhsIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
            instructions.insert(instructions.end(), rhsIR.begin(), rhsIR.end());
        }

        // store (当前仅实现字典，后续可加入运行时判别 list/dict 分派)
        {
            IRType::IRInstr store; store.op = VMOperator::DICT_STORE_VALUE; instructions.push_back(store);
        }
        return;
    }

    // 对象属性赋值： pattern <ident> '.' <attr> '=' expr
    // 但要排除 <ident> '.' <attr> '[' ... ']' '=' expr（这是下标赋值，不是属性赋值）
    if (leftTokens.size() >= 3) {
        // Multi-level dot-chain assignment: obj.a.b.c = expr
        // Previously this was miscompiled as obj.a = expr (only the first attr was used).
        if (std::holds_alternative<LexerType::Keyword>(leftTokens[0])) {
            bool ok = true;
            bool hasSubscript = false;
            std::vector<std::string> attrs;

            size_t i = 1;
            while (i < leftTokens.size()) {
                if (!std::holds_alternative<LexerType::Operator>(leftTokens[i])) {
                    ok = false;
                    break;
                }
                std::string sym = std::get<LexerType::Operator>(leftTokens[i]).symbol;
                if (sym == "[") {
                    hasSubscript = true;
                }
                if (sym != ".") {
                    ok = false;
                    break;
                }
                if (i + 1 >= leftTokens.size() || !std::holds_alternative<LexerType::Keyword>(leftTokens[i + 1])) {
                    ok = false;
                    break;
                }
                attrs.push_back(std::get<LexerType::Keyword>(leftTokens[i + 1]).name);
                i += 2;
            }

            // Only handle dot-only chains, and only when chain depth > 1 (single-level falls through to legacy path)
            if (ok && !hasSubscript && attrs.size() > 1) {
                if (s.isConst) {
                    IR_THROW_NODE(0, mirror, "assignment to const variable (object base)", status);
                    return;
                }

                // 1) load base object
                {
                    IRType::IRInstr load;
                    load.op = VMOperator::LOAD_VALUE;
                    if (s.kind == IRScope::Kind::UNDEFINED) {
                        if (!IRScope::allowImplicitCapture && !IRScope::isCaptureAllActive()) {
                            int errLine = mirror.tokens.empty() ? -1 : IRGen::_getTokenLine(mirror.tokens[0]);
                            std::string fname = IRScope::funcStack.empty() ? std::string("<top>") : IRScope::funcStack.back().name;
                            std::string msg = "鎵句笉鍒板嚱鏁?'" + fname + "' 涓殑灞€閮ㄥ彉閲?'" + varName + "'";
                            IR_THROW_NODE(errLine, mirror, msg, status);
                            return;
                        }
                        s = IRScope::forceCapture(varName);
                    }
                    if (s.kind == IRScope::Kind::UNDEFINED) {
                        ZEUS_DEBUG_PRINT("[IRGen][ObjAssign] Retrying resolve for '%s'", varName.c_str());
                        s = IRScope::resolve(varName);
                    }
                    switch (s.kind) {
                        case IRScope::Kind::LOCAL:
                            load.args.push_back(std::string("local"));
                            load.args.push_back(LexerType::Value((int)s.index));
                            load.args.push_back(varName);
                            break;
                        case IRScope::Kind::GLOBAL:
                            load.args.push_back(std::string("global"));
                            load.args.push_back(LexerType::Value((int)s.index));
                            load.args.push_back(varName);
                            break;
                        case IRScope::Kind::UPVALUE:
                            load.args.push_back(std::string("upvalue"));
                            load.args.push_back(LexerType::Value((int)s.index));
                            load.args.push_back(varName);
                            break;
                        case IRScope::Kind::SHARED:
                            load.op = VMOperator::LOAD_VALUE_SHARED;
                            load.args.push_back(varName);
                            break;
                        default:
                            IR_THROW_NODE(0, mirror, "unsupported object base (not local/global/upvalue/shared)", status);
                            return;
                    }
                    instructions.push_back(load);
                }

                // 2) load intermediate attrs (except last): obj.a.b.c = rhs
                for (size_t ai = 0; ai + 1 < attrs.size(); ++ai) {
                    IRType::IRInstr loadAttr;
                    loadAttr.op = VMOperator::OBJ_LOAD_ATTR;
                    loadAttr.args.push_back(attrs[ai]);
                    instructions.push_back(loadAttr);
                }

                // 3) RHS
                auto rhsIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
                instructions.insert(instructions.end(), rhsIR.begin(), rhsIR.end());

                // 4) store last attr
                {
                    IRType::IRInstr st;
                    st.op = VMOperator::OBJ_STORE_ATTR;
                    st.args.push_back(attrs.back());
                    instructions.push_back(st);
                }
                return;
            }
        }

        bool isAttrAssign = false;
        std::string attrName;
        if (std::holds_alternative<LexerType::Keyword>(leftTokens[0]) &&
            std::holds_alternative<LexerType::Operator>(leftTokens[1]) && std::get<LexerType::Operator>(leftTokens[1]).symbol == "." &&
            std::holds_alternative<LexerType::Keyword>(leftTokens[2])) {
            // 检查是否是 obj.attr[...] 而不是 obj.attr
            bool hasSubscript = false;
            if (leftTokens.size() > 3 && std::holds_alternative<LexerType::Operator>(leftTokens[3]) &&
                std::get<LexerType::Operator>(leftTokens[3]).symbol == "[") {
                hasSubscript = true;
            }
            if (!hasSubscript) {
                isAttrAssign = true;
                attrName = std::get<LexerType::Keyword>(leftTokens[2]).name;
            }
        }
        if (isAttrAssign) {
            if (s.isConst) {
                IR_THROW_NODE(0, mirror, "assignment to const variable (object base)", status);
                return;
            }
            
            // 策略：先加载对象，再计算RHS，OBJ_STORE_ATTR期望栈布局 [object, value]
            // 1. 加载对象
            {
                IRType::IRInstr load; load.op = VMOperator::LOAD_VALUE;
                if (s.kind == IRScope::Kind::UNDEFINED) {
                    if (!IRScope::allowImplicitCapture && !IRScope::isCaptureAllActive()) {
                        int errLine = mirror.tokens.empty() ? -1 : IRGen::_getTokenLine(mirror.tokens[0]);
                        std::string fname = IRScope::funcStack.empty() ? std::string("<top>") : IRScope::funcStack.back().name;
                        std::string msg = "找不到函数 '" + fname + "' 中的局部变量 '" + varName + "'";
                        IR_THROW_NODE(errLine, mirror, msg, status);
                        return;
                    }
                    s = IRScope::forceCapture(varName);
                }
                if (s.kind == IRScope::Kind::UNDEFINED) {
                    ZEUS_DEBUG_PRINT("[IRGen][ObjAssign] Retrying resolve for '%s'", varName.c_str());
                    s = IRScope::resolve(varName);
                }
                switch (s.kind) {
                    case IRScope::Kind::LOCAL:   load.args.push_back(std::string("local"));   load.args.push_back(LexerType::Value((int)s.index)); load.args.push_back(varName); break;
                    case IRScope::Kind::GLOBAL:  load.args.push_back(std::string("global"));  load.args.push_back(LexerType::Value((int)s.index)); load.args.push_back(varName); break;
                    case IRScope::Kind::UPVALUE: load.args.push_back(std::string("upvalue")); load.args.push_back(LexerType::Value((int)s.index)); load.args.push_back(varName); break;
                    case IRScope::Kind::SHARED:
                        load.op = VMOperator::LOAD_VALUE_SHARED;
                        load.args.push_back(varName);
                        break;
                    default:
                        IR_THROW_NODE(0, mirror, "unsupported object base (not local/global/upvalue/shared)", status);
                        return;
                }
                instructions.push_back(load);
            }
            
            // 2. 计算 RHS 表达式
            // 注意：如果 RHS 中访问同一对象的属性（如 myObj.v + 1），emitExprIR 会重新加载对象
            // 这会导致栈上有多个对象引用，但最终我们需要 [object, value] 布局
            auto rhsIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
            instructions.insert(instructions.end(), rhsIR.begin(), rhsIR.end());
            
            // 3. 存储属性
            {
                IRType::IRInstr st; st.op = VMOperator::OBJ_STORE_ATTR; st.args.push_back(attrName); instructions.push_back(st);
            }
            return;
        }
    }

    // 对象属性下标赋值：pattern <ident> '.' <attr> '[' ... ']' '=' expr
    // 例如：this.data[i] = value
    if (leftTokens.size() >= 6) {
        bool isObjAttrSubscriptAssign = false;
        std::string attrName;
        std::vector<LexerType::Token> indexTokens;
        
        // 检查pattern: ident . attr [ ... ]
        if (std::holds_alternative<LexerType::Keyword>(leftTokens[0]) &&
            leftTokens.size() > 3 &&
            std::holds_alternative<LexerType::Operator>(leftTokens[1]) && std::get<LexerType::Operator>(leftTokens[1]).symbol == "." &&
            std::holds_alternative<LexerType::Keyword>(leftTokens[2]) &&
            std::holds_alternative<LexerType::Operator>(leftTokens[3]) && std::get<LexerType::Operator>(leftTokens[3]).symbol == "[") {
            
            // 找到匹配的 ]
            int depth = 0;
            size_t closePos = 0;
            for (size_t i = 3; i < leftTokens.size(); ++i) {
                if (std::holds_alternative<LexerType::Operator>(leftTokens[i])) {
                    std::string sym = std::get<LexerType::Operator>(leftTokens[i]).symbol;
                    if (sym == "[") depth++;
                    else if (sym == "]") {
                        depth--;
                        if (depth == 0) {
                            closePos = i;
                            break;
                        }
                    }
                }
            }
            
            if (closePos > 0 && closePos == leftTokens.size() - 1) {
                // 整个leftTokens以 ] 结尾
                isObjAttrSubscriptAssign = true;
                attrName = std::get<LexerType::Keyword>(leftTokens[2]).name;
                
                // 提取索引表达式: tokens between [ and ]
                for (size_t i = 4; i < closePos; ++i) {
                    indexTokens.push_back(leftTokens[i]);
                }
            }
        }
        
        if (isObjAttrSubscriptAssign) {
            // 生成 IR:
            // 1. LOAD_VALUE ident (对象)
            // 2. OBJ_LOAD_ATTR attr (获取属性，应该是list/dict)
            // 3. 计算索引表达式
            // 4. 计算 RHS 表达式
            // 5. DICT_STORE_VALUE (或 LIST_STORE_ELEM)
            
            // 1. Load object
            {
                IRType::IRInstr load;
                load.op = VMOperator::LOAD_VALUE;
                switch (s.kind) {
                    case IRScope::Kind::LOCAL:
                        load.args.push_back(std::string("local"));
                        load.args.push_back(LexerType::Value((int)s.index));
                        load.args.push_back(varName); // 添加变量名用于错误报告
                        break;
                    case IRScope::Kind::GLOBAL:
                        load.args.push_back(std::string("global"));
                        load.args.push_back(LexerType::Value((int)s.index));
                        load.args.push_back(varName); // 添加变量名用于错误报告
                        break;
                    case IRScope::Kind::UPVALUE:
                        load.args.push_back(std::string("upvalue"));
                        load.args.push_back(LexerType::Value((int)s.index));
                        load.args.push_back(varName); // 添加变量名用于错误报告
                        break;
                    case IRScope::Kind::SHARED:
                        load.op = VMOperator::LOAD_VALUE_SHARED;
                        load.args.push_back(varName);
                        break;
                    default:
                        IR_THROW_NODE(0, mirror, "unsupported object base kind for obj.attr[index]", status);
                        return;
                }
                instructions.push_back(load);
            }
            
            // 2. Load attribute (the list/dict)
            {
                IRType::IRInstr loadAttr;
                loadAttr.op = VMOperator::OBJ_LOAD_ATTR;
                loadAttr.args.push_back(attrName);
                instructions.push_back(loadAttr);
            }
            
            // 3. Calculate index
            {
                auto indexIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, indexTokens);
                instructions.insert(instructions.end(), indexIR.begin(), indexIR.end());
            }
            
            // 4. Calculate RHS
            {
                auto rhsIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
                instructions.insert(instructions.end(), rhsIR.begin(), rhsIR.end());
            }
            
            // 5. Store to list/dict
            {
                IRType::IRInstr store;
                store.op = VMOperator::DICT_STORE_VALUE;
                instructions.push_back(store);
            }
            
            return;
        }
    }

    // 列表元素引用赋值
    if (s.kind == IRScope::Kind::LIST_ELEM_REF) {
        if (s.isConst) {
            IR_THROW_NODE(0, mirror, "assignment to const list element reference", status);
            return;
        }

        IRType::IRInstr loadList;
        loadList.op = VMOperator::LOAD_VALUE;
        loadList.args.push_back(std::string("local"));
        loadList.args.push_back(LexerType::Value((int)s.index));
        instructions.push_back(loadList);

        IRType::IRInstr loadIdx;
        loadIdx.op = VMOperator::LOAD_VALUE;
        loadIdx.args.push_back(std::string("local"));
        loadIdx.args.push_back(LexerType::Value((int)s.aux));
        instructions.push_back(loadIdx);

    auto rhsIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
        instructions.insert(instructions.end(), rhsIR.begin(), rhsIR.end());

        IRType::IRInstr store; store.op = VMOperator::LIST_STORE_ELEM; instructions.push_back(store);
        return;
    }

    // 字典键引用赋值：禁止（键永远 const）
    if (s.kind == IRScope::Kind::DICT_KEY_REF) {
        IR_THROW_NODE(0, mirror, "assignment to dict key (immutable)", status);
        return;
    }

    // 字典值引用赋值
    if (s.kind == IRScope::Kind::DICT_VAL_REF) {
        if (s.isConst) {
            IR_THROW_NODE(0, mirror, "assignment to const dict value reference", status);
            return;
        }
        // 加载 dict 槽 + key 槽
        {
            IRType::IRInstr loadDict; loadDict.op = VMOperator::LOAD_VALUE; loadDict.args.push_back(std::string("local")); loadDict.args.push_back(LexerType::Value((int)s.index)); instructions.push_back(loadDict);
            IRType::IRInstr loadKey; loadKey.op = VMOperator::LOAD_VALUE; loadKey.args.push_back(std::string("local")); loadKey.args.push_back(LexerType::Value((int)s.aux)); instructions.push_back(loadKey);
        }
        auto rhsIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
        instructions.insert(instructions.end(), rhsIR.begin(), rhsIR.end());
        {
            IRType::IRInstr store; store.op = VMOperator::DICT_STORE_VALUE; instructions.push_back(store);
        }
        return;
    }

    // 普通局部 / 全局 / 共享 / 计数索引引用（当作局部槽）
    auto rhsIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
    instructions.insert(instructions.end(), rhsIR.begin(), rhsIR.end());

    // 共享变量赋值
    if (s.kind == IRScope::Kind::SHARED) {
        IRType::IRInstr ass;
        ass.op = VMOperator::ASSIGN_VALUE_SHARED;
        ass.args.push_back(varName);
        instructions.push_back(ass);
        return;
    }

    if (s.kind == IRScope::Kind::LOCAL) {
        if (s.isConst) {
            IR_THROW_NODE(0, mirror, "assignment to const local variable", status);
            return;
        }

        IRType::IRInstr ass;
        ass.op = VMOperator::ASSIGN_VALUE_LOCAL;
        ass.args.push_back(LexerType::Value((int)s.index));
        instructions.push_back(ass);
        return;
    }

    if (s.kind == IRScope::Kind::GLOBAL) {
        if (s.isConst) {
            IR_THROW_NODE(0, mirror, "assignment to const global variable", status);
            return;
        }

        IRType::IRInstr ass;
        ass.op = VMOperator::ASSIGN_VALUE_GLOBAL;
        ass.args.push_back(LexerType::Value((int)s.index));
        instructions.push_back(ass);
        return;
    }
    if (s.kind == IRScope::Kind::UPVALUE) {
        if (s.isConst) {
            IR_THROW_NODE(0, mirror, "assignment to const captured variable", status);
            return;
        }
        IRType::IRInstr ass;
        ass.op = VMOperator::ASSIGN_VALUE_UPVALUE;
        ass.args.push_back(LexerType::Value((int)s.index));
        instructions.push_back(ass);
        return;
    }
    if (s.kind == IRScope::Kind::COUNT_INDEX_REF) {
        if (s.isConst) {
            IR_THROW_NODE(0, mirror, "assignment to const count index reference", status);
            return;
        }
        IRType::IRInstr ass;
        ass.op = VMOperator::ASSIGN_VALUE_LOCAL;
        ass.args.push_back(LexerType::Value((int)s.index));
        instructions.push_back(ass);
        return;
    }
}

namespace IRGen {

    class IRGenerator {

        public:
            bool strictDecl = false; // --strict-decl 选项
            IRType::IRCompilerStatus status = IRType::IRCompilerStatus::Running; // 编译状态
            std::unordered_set<std::string> futureFuncDefs; // 严格模式：未来将出现的定义
            int currentLine = 1; // Track current source line for error reporting
            struct FuncProto {
                std::string name;
                int paramCount = 0;
                std::vector<std::string> paramNames; // forward 声明参数名次序，用于与定义比对
                std::vector<std::string> paramTypeNames;
                std::string returnTypeName;
                int globalIndex = -1;
                int protoChunkIndex = -1;
                bool fromForwardDecl = false;
                bool hasDefinition = false;
            };
            std::vector<FuncProto> collected; // 仅 strict 模式使用
            std::vector<std::vector<IRType::IRInstr>> bytecodes;
            std::vector<IRType::IRInstr>* funcSink = nullptr; // 内部：当处于函数体生成阶段时，子节点的 IR 将汇聚到该 sink
            std::vector<std::string> globalNamesByIndex; // index -> name
            std::vector<std::string> returnReferenceTypeStack;
            std::unordered_map<TreeParser::NodeId, std::string> inferredFunctionReturnTypes;

            // --- Forward Declaration Helpers (Refactored) ---
            FuncProto* findProto(const std::string& name) {
                for (auto &fp : collected) {
                    if (fp.name == name) {
                        return &fp;
                    }
                }
                return nullptr;
            }

            // 注册 forward declaration：
            //  - 复用（幂等）已有全局槽位。
            //  - 如果已存在原型但参数个数不同，抛出冲突异常。
            //  - 否则新增一条 fromForwardDecl=true, hasDefinition=false 记录。
            void registerForwardDecl(
                const std::string& name,
                int paramCount,
                const std::vector<std::string>& paramNames = {},
                const std::vector<std::string>& paramTypeNames = {},
                const std::string& returnTypeName = {}
            ) {
                // 幂等分配/获取全局槽
                int gid = IRScope::declareGlobal(name);

                if (auto* existing = findProto(name)) {
                    if (existing->paramCount != paramCount) {
                        {
                            TreeParser::Node dummy;
                            IRExceptionNS::IRException ex(
                                "Conflicting forward declarations for '" + name + "'",
                                0,
                                dummy
                            );
                            ex._throw(status);
                        }
                    }
                    if (!existing->returnTypeName.empty() && !returnTypeName.empty() &&
                        existing->returnTypeName != returnTypeName) {
                        TreeParser::Node dummy;
                        IRExceptionNS::IRException ex(
                            "Conflicting forward return types for '" + name + "'",
                            0,
                            dummy
                        );
                        ex._throw(status);
                    }
                    // 相同签名：直接返回（保持幂等）
                    return;
                }

                FuncProto fp;
                fp.name = name;
                fp.paramCount = paramCount;
                fp.paramNames = paramNames;
                fp.paramTypeNames = paramTypeNames;
                fp.returnTypeName = returnTypeName;
                fp.globalIndex = gid;
                fp.fromForwardDecl = true;
                fp.hasDefinition = false;
                collected.push_back(fp);
            }

            // 标记函数定义：
            //  - 若存在 forward decl，校验参数匹配并填充 protoChunkIndex / hasDefinition。
            //  - 若不存在 forward decl，记录一条 fromForwardDecl=false, hasDefinition=true 的定义记录。
            // 这允许：strict 模式中，后续如果调用点早于定义且没 forward decl，后面将加的检查可以识别。
            void markDefinition(
                const std::string& name,
                int paramCount,
                int protoChunkIndex,
                const std::vector<std::string>& defParamNames = {},
                const std::vector<std::string>& defParamTypes = {},
                const std::string& defReturnType = {}
            ) {
                // 确保全局槽存在（避免后续 LOAD_VALUE 解析失败）
                int gid = IRScope::declareGlobal(name);
                (void)gid;

                if (auto* existing = findProto(name)) {
                    if (existing->paramCount != paramCount) {
                        {
                            TreeParser::Node dummy;
                            IRExceptionNS::IRException ex(
                                "Function definition parameter count mismatch for '" + name + "'",
                                0,
                                dummy
                            );
                            ex._throw(status);
                        }
                    }
                    if (!existing->returnTypeName.empty() && !defReturnType.empty() &&
                        existing->returnTypeName != defReturnType) {
                        TreeParser::Node dummy;
                        IRExceptionNS::IRException ex(
                            "Function definition return type mismatch for '" + name + "'",
                            0,
                            dummy
                        );
                        ex._throw(status);
                    }
                    if (!existing->paramTypeNames.empty() && !defParamTypes.empty() &&
                        existing->paramTypeNames.size() == defParamTypes.size()) {
                        for (size_t i = 0; i < existing->paramTypeNames.size(); ++i) {
                            if (!existing->paramTypeNames[i].empty() &&
                                !defParamTypes[i].empty() &&
                                existing->paramTypeNames[i] != defParamTypes[i]) {
                                TreeParser::Node dummy;
                                IRExceptionNS::IRException ex(
                                    "Function definition parameter type mismatch for '" + name +
                                    "' at position " + std::to_string(i),
                                    0,
                                    dummy
                                );
                                ex._throw(status);
                            }
                        }
                    }
                    existing->hasDefinition = true;
                    existing->protoChunkIndex = protoChunkIndex;
                    // 参数名一致性校验
                    if (!existing->paramNames.empty() && !defParamNames.empty() && existing->paramNames.size() == defParamNames.size()) {
                        for (size_t i = 0; i < existing->paramNames.size(); ++i) {
                            if (existing->paramNames[i] != defParamNames[i]) {
                                {
                                    TreeParser::Node dummy;
                                    IRExceptionNS::IRException ex(
                                        "Parameter name mismatch for function '" + name + "' at position " + std::to_string(i) +
                                            ": forward decl has '" + existing->paramNames[i] + "' but definition uses '" + defParamNames[i] + "'",
                                        0,
                                        dummy
                                    );
                                    ex._throw(status);
                                }
                            }
                        }
                    }
                    return;
                }

                // 无 forward decl：按“定义记录”存入
                FuncProto fp;
                fp.name = name;
                fp.paramCount = paramCount;
                fp.paramNames = defParamNames;
                fp.globalIndex = gid;
                fp.fromForwardDecl = false;
                fp.hasDefinition = true;
                fp.protoChunkIndex = protoChunkIndex;
                collected.push_back(fp);
            }

            void collectForwardDeclsOnly(const std::vector<TreeParser::NodeAndBlock>& nodes) {
                for (const auto& node : nodes) {
                    if (!std::holds_alternative<TreeParser::Node>(node)) {
                        continue;
                    }

                    const auto& nd = std::get<TreeParser::Node>(node);
                    if (nd.type != NodeType::FUNC_FWD_DECL_STMT) {
                        continue;
                    }

                    const auto signature = ZeusParser::parseForwardFunctionSignature(nd.tokens);
                    if (!signature.valid || !signature.nativeSymbol.empty()) continue;
                    std::vector<std::string> names;
                    std::vector<std::string> types;
                    names.reserve(signature.parameters.size());
                    types.reserve(signature.parameters.size());
                    for (const auto& parameter : signature.parameters) {
                        names.push_back(parameter.name);
                        types.push_back(parameter.typeName);
                    }
                    registerForwardDecl(
                        signature.name,
                        static_cast<int>(names.size()),
                        names,
                        types,
                        signature.returnTypeName
                    );
                }
            }

            // Helper: set line number for all instructions in a chunk
            void setLineNumbers(std::vector<IRType::IRInstr>& instructions) {
                for (auto& instr : instructions) {
                    if (instr.line == -1) {  // Only set if not already set
                        instr.line = currentLine;
                    }
                }
            }

            void collectFunctionNames(
                const std::vector<TreeParser::NodeAndBlock>& blockNodes
            ) {
                for (const auto& node : blockNodes) {
                    if (std::holds_alternative<TreeParser::Node>(node)) {
                        const auto& statement = std::get<TreeParser::Node>(node);
                        if (statement.type == NodeType::FUNC_FWD_DECL_STMT) {
                            const auto signature = ZeusParser::parseForwardFunctionSignature(
                                statement.tokens
                            );
                            if (signature.valid && !signature.name.empty()) {
                                IRScope::functionNames.insert(signature.name);
                            }
                        }
                        continue;
                    }

                    const auto& block = std::get<std::unique_ptr<TreeParser::CodeBlock>>(node);
                    if (!block) continue;

                    if (block->type == CodeBlockType::FUNC && !block->funcName.empty()) {
                        IRScope::functionNames.insert(block->funcName);
                    }

                    collectFunctionNames(block->nodes);
                }
            }

            void parse(std::vector<TreeParser::NodeAndBlock>& nodes) {
                // 将NodeAndBlock编译为IR
                if (IRScope::scopeStack.empty()) IRScope::pushScope();
                status = IRType::IRCompilerStatus::Running;

                // 先收集函数名，使定义前的直接调用也能获得后端标注。
                IRScope::functionNames.clear();
                collectFunctionNames(nodes);

                ZEUS_DEBUG_PRINT("[IRGen] ===== PARSE START (top-level variant count=%zu) =====", nodes.size());

                if (strictDecl) {
                    ZEUS_DEBUG_PRINT("[IRGen] (strictDecl) collecting forward declarations...");
                    collectForwardDeclsOnly(nodes);
                    ZEUS_DEBUG_PRINT("[IRGen] (strictDecl) collected %zu forward decl(s).", collected.size());
                }
                size_t topIndex = 0;

                for (const auto& node : nodes) {
                    ZEUS_DEBUG_PRINT("[IRGen] --- TopVariant #%zu ---", topIndex++);

                    if (std::holds_alternative<TreeParser::Node>(node)) {
                        const auto& nd = std::get<TreeParser::Node>(node);
                        ZEUS_DEBUG_PRINT("[IRGen] Top Node kind=%d tokens=%zu", (int)nd.type, nd.tokens.size());
                        parseNodeToBytecodes(nd);
                    }
                    else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                        auto* blkPtr = std::get<std::unique_ptr<TreeParser::CodeBlock>>(node).get();
                        ZEUS_DEBUG_PRINT("[IRGen] Top CodeBlock addr=%p type=%d childCount=%zu condTokens=%zu", (void*)blkPtr, (int)blkPtr->type, blkPtr->nodes.size(), blkPtr->conditionTokens.size());
                        parseCodeBlock(*blkPtr);
                        ZEUS_DEBUG_PRINT("[IRGen] Returned from Top CodeBlock addr=%p (type=%d)", (void*)blkPtr, (int)blkPtr->type);
                    }
                }
                ZEUS_DEBUG_PRINT("[IRGen] ===== PARSE END | total chunks=%zu =====", bytecodes.size());
                // 收尾：若全局表达式阶段产生错误，成员状态同步
                // 表达式阶段直接写入成员 status，无需全局同步

                if (strictDecl) {
                    for (auto const& fp : collected) {
                        if (fp.fromForwardDecl && !fp.hasDefinition) {
                            {
                                TreeParser::Node dummy;
                                IRExceptionNS::IRException ex(
                                    "Forward declared function '" + fp.name + "' has no definition",
                                    0,
                                    dummy
                                );
                                ex._throw(status);
                            }
                        }
                    }
                }
                // 构建全局名称映射
                if (!IRScope::globals.empty()) {
                    size_t maxIdx = 0;
                    for (auto const& kv : IRScope::globals) if ((size_t)kv.second > maxIdx) maxIdx = kv.second;
                    globalNamesByIndex.assign(maxIdx + 1, std::string());
                    for (auto const& kv : IRScope::globals) {
                        if ((size_t)kv.second < globalNamesByIndex.size()) globalNamesByIndex[kv.second] = kv.first;
                    }
                }
            }


            // Node 转换 IR 并 push 到 bytecodes（或函数 sink）
            void parseNodeToBytecodes(const TreeParser::Node& node) {
                std::vector<IRType::IRInstr> instructions;

                switch (node.type) {

                    case NodeType::VAR_DECL_STMT: {

                        parseVarDecl(status, strictDecl, futureFuncDefs, node, instructions);
                        ZEUS_DEBUG_EXEC({
                            ZEUS_DEBUG_PRINT("[IRGen    ] {VarDecl} Instr:");
                            IRDebugUtils::printVectorInstructions(instructions);
                        });
                        break;
                    }


                    case NodeType::RETURN_STMT: {

                        std::vector<LexerType::Token> exprTokens;
                        bool first = true;

                        for (auto const& tk : node.tokens) {
                            if (first) {  // skip 'return'
                                first = false;
                                continue;
                            }

                            exprTokens.push_back(tk);
                        }

                        const std::string referenceTargetType =
                            returnReferenceTypeStack.empty()
                                ? std::string()
                                : returnReferenceTypeStack.back();
                        const bool returnsReference = !referenceTargetType.empty();
                        const bool returnsScalarCellReference = returnsReference &&
                            (referenceTargetType == "int" ||
                             referenceTargetType == "int32" ||
                             referenceTargetType == "int64" ||
                             referenceTargetType == "bool") &&
                            exprTokens.size() == 1 &&
                            std::holds_alternative<LexerType::Keyword>(exprTokens.front());
                        if (returnsScalarCellReference) {
                            const auto& name = std::get<LexerType::Keyword>(exprTokens.front()).name;
                            const auto symbol = IRScope::resolve(name);
                            if (symbol.kind != IRScope::Kind::GLOBAL) {
                                IR_THROW_NODE(node.id, node,
                                    "reference return currently requires stable global storage", status);
                                break;
                            }
                            IRType::IRInstr reference;
                            reference.op = VMOperator::LOAD_CELL_REFERENCE;
                            reference.args.push_back(std::string("global"));
                            reference.args.push_back(LexerType::Value(symbol.index));
                            reference.args.push_back(name);
                            instructions.push_back(std::move(reference));
                        } else if (!exprTokens.empty()) {
                            auto rhs = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
                            instructions.insert(instructions.end(), rhs.begin(), rhs.end());
                        } else if (returnsReference) {
                            IR_THROW_NODE(node.id, node,
                                "reference return requires a value expression", status);
                            break;
                        } else {
                            IRType::IRInstr nil;
                            nil.op = VMOperator::PUSH;
                            nil.args.push_back(IRType::NullLiteral{});
                            instructions.push_back(nil);
                        }

                        IRType::IRInstr ret;
                        ret.op = VMOperator::RETURN;
                        instructions.push_back(ret);

                        ZEUS_DEBUG_EXEC({
                            ZEUS_DEBUG_PRINT("[IRGen    ] {Return} RV tokens:");
                            IRDebugUtils::printTokens(exprTokens);
                        });

                        break;
                    }


                    case NodeType::THROW_STMT: {
                        // throw Expression;
                        // 解析 throw 后的表达式，生成 THROW 指令
                        std::vector<LexerType::Token> exprTokens;
                        bool first = true;

                        for (auto const& tk : node.tokens) {
                            if (first) {  // skip 'throw'
                                first = false;
                                continue;
                            }
                            exprTokens.push_back(tk);
                        }

                        if (!exprTokens.empty()) {
                            auto exprIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
                            instructions.insert(instructions.end(), exprIR.begin(), exprIR.end());
                        }

                        IRType::IRInstr throwInstr;
                        throwInstr.op = VMOperator::THROW;
                        instructions.push_back(throwInstr);

                        ZEUS_DEBUG_EXEC({
                            ZEUS_DEBUG_PRINT("[IRGen    ] {Throw} exception expr tokens:");
                            IRDebugUtils::printTokens(exprTokens);
                        });

                        break;
                    }


                    case NodeType::BREAK_STMT: {
                        // 生成 LOOP_BREAK 占位符指令
                        // 实际的跳转偏移会在循环 IR 生成完成后由 patchBreakPlaceholders 回填
                        IRType::IRInstr breakInstr = IRUtils::makeLoopBreakPlaceholder();
                        instructions.push_back(breakInstr);

                        ZEUS_DEBUG_PRINT("[IRGen    ] {Break} Generated LOOP_BREAK placeholder");
                        break;
                    }


                    case NodeType::DELETE_STMT: {
                        // 解析 delete 语句: delete var1, var2, var3;
                        // 为每个变量生成 DELETE_VAR 指令，包含变量类型和索引
                        for (size_t i = 1; i < node.tokens.size(); ++i) {
                            const auto& tk = node.tokens[i];
                            
                            if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                const auto& kw = std::get<LexerType::Keyword>(tk);
                                std::string varName = kw.name;
                                
                                // 解析变量，确定其类型和索引
                                IRScope::SymRef ref = IRScope::resolve(varName);
                                
                                IRType::IRInstr deleteInstr;
                                deleteInstr.op = VMOperator::DELETE_VAR;
                                deleteInstr.args.push_back(varName);
                                
                                // 添加变量类型: "local", "upvalue", "global"
                                std::string kindStr;
                                int varIndex = ref.index;
                                
                                switch (ref.kind) {
                                    case IRScope::Kind::LOCAL:
                                        kindStr = "local";
                                        break;
                                    case IRScope::Kind::UPVALUE:
                                        kindStr = "upvalue";
                                        break;
                                    case IRScope::Kind::GLOBAL:
                                        kindStr = "global";
                                        break;
                                    default:
                                        kindStr = "global"; // 未定义变量默认按全局处理
                                        varIndex = -1;
                                        break;
                                }
                                
                                deleteInstr.args.push_back(kindStr);
                                deleteInstr.args.push_back(LexerType::Value(varIndex));
                                instructions.push_back(deleteInstr);
                                
                                ZEUS_DEBUG_PRINT("[IRGen    ] {Delete} Generated DELETE_VAR for %s variable: %s (index=%d)", 
                                    kindStr.c_str(), varName.c_str(), varIndex);
                            }
                            // 跳过逗号和分号
                        }
                        break;
                    }


                    case NodeType::IMPORT_STMT: {
                        // 解析三种import模式:
                        // 1. import package.name;         - 全局导入(默认)
                        // 2. import package.name [&];     - 命名空间导入,使用包名
                        // 3. import package.name [&alias]; - 命名空间导入,使用别名
                        
                        std::string packageName;
                        std::string alias;
                        bool isNamespaceImport = false;  // 是否为命名空间导入(有[&]或[&alias])
                        
                        // 解析包名和别名: 跳过'import'关键字,收集包名tokens,提取别名
                        bool first = true;
                        bool inBracket = false;
                        bool expectAmpersand = false;
                        bool expectAlias = false;
                        
                        for (const auto& tk : node.tokens) {
                            if (first) {  // skip 'import'
                                first = false;
                                continue;
                            }
                            
                            if (std::holds_alternative<LexerType::Operator>(tk)) {
                                const auto& op = std::get<LexerType::Operator>(tk);
                                
                                if (op.toString() == ";") {
                                    break;  // 语句结束
                                }
                                
                                if (op.toString() == ".") {
                                    if (!inBracket) {
                                        packageName += ".";
                                    }
                                    continue;
                                }
                                
                                if (op.toString() == "[") {
                                    inBracket = true;
                                    expectAmpersand = true;
                                    continue;
                                }
                                
                                if (op.toString() == "]") {
                                    inBracket = false;
                                    continue;
                                }
                                
                                if (op.toString() == "&" && expectAmpersand) {
                                    expectAmpersand = false;
                                    expectAlias = true;
                                    isNamespaceImport = true;  // 标记为命名空间导入
                                    continue;
                                }
                            }
                            
                            if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                const auto& kw = std::get<LexerType::Keyword>(tk);
                                if (!inBracket) {
                                    packageName += kw.name;
                                } else if (expectAlias) {
                                    alias = kw.name;
                                    expectAlias = false;
                                }
                            }
                        }
                        
                        if (packageName.empty()) {
                            ZEUS_DEBUG_PRINT("[IRGen] !! IMPORT_STMT: empty package name");
                            break;
                        }
                        
                        // 如果是命名空间导入但没有指定别名,使用包名的最后一部分作为别名
                        // 例如: import pkg.sub [&]; 使用 'sub' 作为别名
                        if (isNamespaceImport && alias.empty()) {
                            size_t lastDot = packageName.find_last_of('.');
                            alias = (lastDot != std::string::npos) ? packageName.substr(lastDot + 1) : packageName;
                        }
                        
                        ZEUS_DEBUG_PRINT("[IRGen] {Import} package: %s, alias: %s, mode: %s", 
                            packageName.c_str(), 
                            alias.empty() ? "(none)" : alias.c_str(),
                            isNamespaceImport ? "namespace" : "global");
                        
                        // 生成 IMPORT 指令
                        // args[0]: packageName (string)
                        // args[1]: alias (string, 空字符串表示无别名)
                        // args[2]: importMode (int: 0=global, 1=namespace)
                        IRType::IRInstr importInstr;
                        importInstr.op = VMOperator::IMPORT;
                        importInstr.args.push_back(LexerType::Value(packageName));
                        importInstr.args.push_back(LexerType::Value(alias));
                        importInstr.args.push_back(LexerType::Value((long long)(isNamespaceImport ? 1 : 0)));
                        instructions.push_back(importInstr);
                        
                        ZEUS_DEBUG_PRINT("[IRGen] {Import} Generated IMPORT instruction for: %s (alias: %s, mode: %s)", 
                            packageName.c_str(), 
                            alias.empty() ? "(none)" : alias.c_str(),
                            isNamespaceImport ? "namespace" : "global");
                        
                        break;
                    }

                    case NodeType::USING_STMT: {
                        // 解析两种using:
                        // 1. using &this;  (类内实例别名,已在类处理中处理,这里忽略)
                        // 2. using original; (文件级:不自动包装[]为Array)
                        
                        bool isOriginal = false;
                        bool hasAmpersand = false;
                        
                        // 检查tokens
                        for (const auto& tk : node.tokens) {
                            if (std::holds_alternative<LexerType::Operator>(tk)) {
                                const auto& op = std::get<LexerType::Operator>(tk);
                                if (op.toString() == "&") {
                                    hasAmpersand = true;
                                    break;
                                }
                            }
                            if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                const auto& kw = std::get<LexerType::Keyword>(tk);
                                if (kw.name == "original") {
                                    isOriginal = true;
                                }
                            }
                        }
                        
                        // 如果有&符号,说明是类内using &this,忽略
                        if (hasAmpersand) {
                            ZEUS_DEBUG_PRINT("[IRGen] {Using} Skipping class-level using statement");
                            break;
                        }
                        
                        // 处理 using original;
                        if (isOriginal) {
                            // The registry decides which built-in literals receive
                            // object wrappers; "using original" disables that policy.
                            IRScope::enableOriginalMode();
                            
                            ZEUS_DEBUG_PRINT("[IRGen] {Using} Set useOriginalList=true, useOriginalBytes=true, and useOriginalString=true for this compilation unit");
                        } else {
                            // 其他using语句(如 using &this)应该在CLASS处理中处理
                            // 这里忽略,不生成IR
                            ZEUS_DEBUG_PRINT("[IRGen] {Using} Ignoring non-original using statement");
                        }
                        
                        // using语句本身不生成任何IR指令,只影响编译行为
                        break;
                    }
                    
                    case NodeType::VAR_ASSIGN_STMT: {

                        parseVarAssignStmt(status, strictDecl, futureFuncDefs, node, instructions);
                        ZEUS_DEBUG_EXEC({
                            ZEUS_DEBUG_PRINT("[IRGen    ] {VarAssign} RV IR:");
                            IRDebugUtils::printVectorInstructions(instructions);
                        });
                        break;

                    }

                    case NodeType::EXP_STMT: {
                        // 整行表达式 -> 结果丢弃
                        std::vector<LexerType::Token> exprTokens = node.tokens; // 全部 token 当作表达式

                        ZEUS_DEBUG_EXEC({
                            ZEUS_DEBUG_PRINT("[IRGen    ] {ExpStmt} tokens:");
                            IRDebugUtils::printTokens(exprTokens);
                        });

                        auto ir = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, exprTokens);
                        instructions.insert(instructions.end(), ir.begin(), ir.end());

                        ZEUS_DEBUG_EXEC({
                            ZEUS_DEBUG_PRINT("[IRGen    ] {ExpStmt} IR (%zu instrs):", ir.size());
                            IRDebugUtils::printVectorInstructions(ir);
                        });

                        IRType::IRInstr pop;
                        pop.op = VMOperator::POP;
                        instructions.push_back(pop);
                        break;
                    }

                    case NodeType::FUNC_FWD_DECL_STMT: {
                        const auto signature = ZeusParser::parseForwardFunctionSignature(
                            node.tokens
                        );
                        if (!signature.valid) {
                            IR_THROW_NODE(0, node, "invalid function declaration signature", status);
                            break;
                        }

                        std::vector<std::string> paramNames;
                        std::vector<std::string> paramTypes;
                        for (const auto& parameter : signature.parameters) {
                            paramNames.push_back(parameter.name);
                            paramTypes.push_back(parameter.typeName);
                        }

                        if (funcSink == nullptr) {
                            const bool isNativeDeclaration = !signature.nativeSymbol.empty();
                            if (strictDecl && !isNativeDeclaration) {
                                registerForwardDecl(
                                    signature.name,
                                    static_cast<int>(paramNames.size()),
                                    paramNames,
                                    paramTypes,
                                    signature.returnTypeName
                                );
                            } else if (!strictDecl && !isNativeDeclaration) {
                                IRScope::declareGlobal(signature.name);
                            }
                            instructions.push_back(
                                IRGen::makeExternalFunctionSignatureAnnotation(signature)
                            );
                        } else if (signature.nativeSymbol.empty()) {
                            IRScope::declareLocal(signature.name);
                        }
                        break; }

                    // TODO: 其他 node 类型
                    default:
                        break;
                }


                if (!instructions.empty()) {
                    if (funcSink) {
                        funcSink->insert(funcSink->end(), instructions.begin(), instructions.end());
                    } else {
                        bytecodes.push_back(std::move(instructions));
                    }
                }
            }


            // 遍历 CodeBlock 内部节点
            void parseCodeBlock(TreeParser::CodeBlock& codeBlock) {
                struct ScopeGuard { ~ScopeGuard(){ IRScope::popScope(); } } guard;
                IRScope::pushScope();

          ZEUS_DEBUG_PRINT("[IRGen] >> Enter CodeBlock addr=%p type=%d childCount=%zu condTokens=%zu funcName='%s'", (void*)&codeBlock, (int)codeBlock.type, codeBlock.nodes.size(), codeBlock.conditionTokens.size(), codeBlock.funcName.c_str());

                // 根块 future defs 扫描
                if (funcSink == nullptr && codeBlock.type == CodeBlockType::BLOCK && strictDecl) {
                    futureFuncDefs.clear();
                    for (const auto& n : codeBlock.nodes) {
                        if (!std::holds_alternative<TreeParser::Node>(n)) continue;

                        const auto& ndPre = std::get<TreeParser::Node>(n);

                        if (ndPre.type != NodeType::FUNC_DEF_STMT) continue;
                        bool seenFuncKw = false;
                        for (auto &tk : ndPre.tokens) {
                            if (!std::holds_alternative<LexerType::Keyword>(tk)) continue;

                            auto &kw = std::get<LexerType::Keyword>(tk);

                            if (!seenFuncKw) {
                                if (kw.name == "func") { 
                                    seenFuncKw = true;
                                } 
                            
                                continue; 
                            }
                            if (kw.name != "func" && kw.name != "decl") {
                                futureFuncDefs.insert(kw.name); 
                                break; 
                            }
                        }
                    }
                }

                // 分派不同 CodeBlock 类型
                switch (codeBlock.type) {
                    case CodeBlockType::FUNC:
                        handleFunctionCodeBlock(codeBlock);
                        ZEUS_DEBUG_PRINT("[IRGen] << Leave FUNC CodeBlock addr=%p (proto emitted)", (void*)&codeBlock);
                        return; // guard 自动 pop
                        
                    case CodeBlockType::IF:
                        handleIfCodeBlock(codeBlock);
                        ZEUS_DEBUG_PRINT("[IRGen] .. IF END addr=%p", (void*)&codeBlock);
                        return; // guard 自动 pop

                    case CodeBlockType::CLASS: {
                        // 语义：在进入类体前注册一个类原型
                        // 1) 解析类名
                        std::string className = codeBlock.className;
                        if (className.empty()) {
                            ZEUS_DEBUG_PRINT("[IRGen] !! CLASS block without className, skip");
                            return;
                        }

                        // 提前注册类名到 IRScope，以便类体内可以引用（例如 new String）
                        // 注意：这只是占位注册，实际的 ClassHandle 会在类定义结束后存储
                        IRScope::declareGlobal(className);
                        ZEUS_DEBUG_PRINT("[IRGen] Pre-registered class name: %s", className.c_str());

                        // Prepare collectors
                        // when non-empty, this contains the alias name declared via `using &alias;` (e.g. "self"),
                        // empty means no default alias declared
                        std::string usingThisAlias;
                        // method name -> protoId (for static/instance)
                        std::unordered_map<std::string,int> staticMethodsMap;
                        std::unordered_map<std::string,int> instanceMethodsMap;
                        // operator symbol -> protoId (for operator overloads)
                        std::unordered_map<std::string,int> operatorMethodsMap;
                        // fields: keep source order for the native layout, while the
                        // unordered map remains the VM's name-based field collector.
                        struct FieldInfo {
                            std::vector<IRType::IRInstr> defaultIR;
                            std::string typeName;
                            bool isConst = false;
                        };
                        std::unordered_map<std::string, FieldInfo> fieldsMap;
                        std::vector<std::string> fieldOrder;

                        // First pass: scan class body nodes to collect `using &this;`, let fields, and func definitions
                        for (const auto& nodeVar : codeBlock.nodes) {
                            if (!std::holds_alternative<TreeParser::Node>(nodeVar)) continue;
                            const auto& nd = std::get<TreeParser::Node>(nodeVar);
                                    // Detect simple using &alias; (as tokens: using & alias ; )
                            if (nd.type == NodeType::EXP_STMT) {
                                ZEUS_DEBUG_PRINT("[IRGen    ] class '%s' saw EXP_STMT with %zu tokens", className.c_str(), nd.tokens.size());
                                ZEUS_DEBUG_EXEC({ IRDebugUtils::printTokens(nd.tokens); });
                                if (!nd.tokens.empty()) {
                                    // naive pattern match
                                            if (nd.tokens.size() >= 3 && std::holds_alternative<LexerType::Keyword>(nd.tokens[0]) && std::get<LexerType::Keyword>(nd.tokens[0]).name == "using" &&
                                                std::holds_alternative<LexerType::Operator>(nd.tokens[1]) && std::get<LexerType::Operator>(nd.tokens[1]).symbol == "&" &&
                                                std::holds_alternative<LexerType::Keyword>(nd.tokens[2])) {
                                                usingThisAlias = std::get<LexerType::Keyword>(nd.tokens[2]).name;
                                                ZEUS_DEBUG_PRINT("[IRGen    ] found using alias in class '%s' -> '%s'", className.c_str(), usingThisAlias.c_str());
                                        continue;
                                    }
                                }
                            }                            // Field declarations: VAR_DECL inside class
                            if (nd.type == NodeType::VAR_DECL_STMT) {
                                // extract name, optional type annotation and RHS
                                std::string vname;
                                std::string fieldType;
                                bool isConst = false;
                                std::vector<LexerType::Token> rhs;
                                bool saving = false;
                                bool readingType = false;
                                for (auto &tk : nd.tokens) {
                                    if (std::holds_alternative<LexerType::Operator>(tk)) {
                                        const auto& op = std::get<LexerType::Operator>(tk).symbol;
                                        if (op == "=") { saving = true; continue; }
                                        if (!saving && op == ":") {
                                            readingType = true;
                                            continue;
                                        }
                                    }
                                    if (!saving) {
                                        if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                            auto &kw = std::get<LexerType::Keyword>(tk);
                                            if (kw.name == "const") { isConst = true; continue; }
                                            if (kw.name == "let" || kw.name == "var" || kw.name == "const") continue;
                                            if (readingType) {
                                                fieldType = kw.name;
                                                readingType = false;
                                            } else if (vname.empty()) {
                                                vname = kw.name;
                                            }
                                        }
                                    } else rhs.push_back(tk);
                                }
                                if (!vname.empty()) {
                                    FieldInfo finfo;
                                    finfo.isConst = isConst;
                                    finfo.typeName = fieldType;
                                    if (finfo.typeName.empty() && rhs.size() == 1 &&
                                        std::holds_alternative<LexerType::Value>(rhs[0])) {
                                        const auto& value = std::get<LexerType::Value>(rhs[0]);
                                        if (std::holds_alternative<ZeusType::BigInteger>(value.data)) {
                                            finfo.typeName = "int";
                                        } else if (std::holds_alternative<ZeusType::BigDecimal>(value.data)) {
                                            finfo.typeName = "float";
                                        } else if (std::holds_alternative<bool>(value.data)) {
                                            finfo.typeName = "bool";
                                        } else if (std::holds_alternative<std::string>(value.data)) {
                                            finfo.typeName = "string";
                                        }
                                    }
                                    if (!rhs.empty()) {
                                        auto ir = emitExprIR(status, strictDecl, futureFuncDefs, rhs);
                                        finfo.defaultIR = std::move(ir);
                                    }
                                    if (fieldsMap.find(vname) == fieldsMap.end()) {
                                        fieldOrder.push_back(vname);
                                    }
                                    fieldsMap[vname] = std::move(finfo);
                                }
                                continue;
                            }

                            // Function definitions inside class: treat as methods
                            if (nd.type == NodeType::FUNC_DECL_STMT) {
                                // We need to emit a function proto chunk (like top-level), so create a temporary CodeBlock for the function
                                // Minimal approach: rely on existing parseCodeBlock to generate MAKE_FUNCTION later: here we only ensure declaration slot
                                // Extract function name and register a global proto slot now
                                std::string funcName;
                                for (auto &tk : nd.tokens) {
                                    if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                        auto &kw = std::get<LexerType::Keyword>(tk);
                                        if (kw.name == "func") continue;
                                        funcName = kw.name; break;
                                    }
                                }
                                if (!funcName.empty()) {
                                    // Reserve global slot so MAKE_FUNCTION will write there when parseCodeBlock handles the nested function
                                    int gid = IRScope::declareGlobal(funcName);
                                    (void)gid;
                                }
                                continue;
                            }
                        }

                        // Second pass: first emit function protos for methods so we know protoIds,
                        // then emit CLASS_BUILD with instanceMethods populated.

                        // Emit method prototypes by parsing function CodeBlocks now. We scan nodes and when
                        // encountering a function CodeBlock we call parseCodeBlock on it which will append
                        // the FUNC chunk + MAKE_FUNCTION sequence to bytecodes. We then find the FUNC chunk
                        // index (protoId) from the newly appended chunks.
                        for (const auto& node : codeBlock.nodes) {
                            if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                auto const &cbPtr = std::get<std::unique_ptr<TreeParser::CodeBlock>>(node);
                                if (cbPtr && cbPtr->type == CodeBlockType::FUNC && !cbPtr->funcName.empty()) {
                                        cbPtr->ownerClassName = className;
                                        // Mark as class method so handleFunctionCodeBlock won't emit MAKE_FUNCTION/STORE
                                        cbPtr->isClassMethod = true;
                                        
                                        // Determine method classification strictly (see note.md):
                                        // - If class declares `using &alias;` then:
                                        //     * method with `static` modifier => static method
                                        //     * method without `static` => instance method (implicit alias parameter)
                                        //     * explicit capture list [&alias] on method declaration is forbidden
                                        // - If class does NOT declare using:
                                        //     * method with explicit capture [&name] => instance method (alias = name)
                                        //     * otherwise => static method (unless static modifier is present)

                                        bool methodIsStaticByModifier = cbPtr->isStaticMethod;
                                        bool methodHasCaptureDecl = !cbPtr->captureVars.empty();
                                        std::string declaredCaptureAlias = methodHasCaptureDecl ? cbPtr->captureVars[0] : std::string();
                                        if (methodHasCaptureDecl) {
                                            std::string conc;
                                            for (auto &c : cbPtr->captureVars) conc += c + ",";
                                            ZEUS_DEBUG_PRINT("[IRGen][Debug] method '%s' captureVars: %s", cbPtr->funcName.c_str(), conc.c_str());
                                        } else {
                                            ZEUS_DEBUG_PRINT("[IRGen][Debug] method '%s' has no captureVars", cbPtr->funcName.c_str());
                                        }

                                        // validation and classification
                                        bool classifyAsInstance = false;
                                        if (!usingThisAlias.empty()) {
                                            // class has using &alias
                                            if (methodIsStaticByModifier) {
                                                classifyAsInstance = false; // explicit static
                                            } else {
                                                // no static modifier => instance method (implicit alias)
                                                classifyAsInstance = true;
                                            }

                                            // A method of a class that already declares
                                            // `using &alias` must not capture that same alias
                                            // again: the receiver is injected as the first
                                            // parameter, so a second binding would shift every
                                            // argument. This applies to operator overloads too.
                                            if (methodHasCaptureDecl && declaredCaptureAlias == usingThisAlias) {
                                                TreeParser::Node dummy;
                                                std::string msg = "Class '" + className + "' declares using &" + usingThisAlias + "; method '" + cbPtr->funcName + "' must not explicitly capture [&" + usingThisAlias + "] in its declaration.\n";
                                                msg += "Hint: remove the capture list for this method (it's already implicitly available as '" + usingThisAlias + "'), or mark the method 'static' if you intend it to be static.";
                                                IRExceptionNS::IRException ex(msg, 0, dummy);
                                                ex._throw(status);
                                                return;
                                            }
                                        } else {
                                            // class has no using alias
                                            if (methodHasCaptureDecl) {
                                                // explicit capture -> instance
                                                classifyAsInstance = true;
                                            } else if (methodIsStaticByModifier) {
                                                classifyAsInstance = false;
                                            } else {
                                                // default: treat as static (per note.md final decision)
                                                classifyAsInstance = false;
                                            }
                                        }

                                        size_t before = bytecodes.size();

                                        if (classifyAsInstance) {
                                            // Determine alias to inject (implicit or declared)
                                            std::string aliasToInsert;
                                            if (!usingThisAlias.empty()) {
                                                aliasToInsert = usingThisAlias;
                                            } else {
                                                aliasToInsert = declaredCaptureAlias;
                                            }

                                            if (aliasToInsert.empty()) {
                                                // Unreachable: classifyAsInstance is only set when
                                                // the class declares an alias or the method captures
                                                // one. Failing loudly beats inventing a receiver name:
                                                // a hardcoded "this" would silently bind every
                                                // argument one slot over.
                                                TreeParser::Node dummy;
                                                std::string msg = "Internal error: instance method '" +
                                                    cbPtr->funcName + "' of class '" + className +
                                                    "' was classified as an instance method without a receiver name";
                                                IRExceptionNS::IRException ex(msg, 0, dummy);
                                                ex._throw(status);
                                                return;
                                            }

                                            // Ensure alias is present as the first parameter (insert persistently)
                                            if (cbPtr->paramNames.empty() || cbPtr->paramNames.front() != aliasToInsert) {
                                                const size_t oldParameterCount = cbPtr->paramNames.size();
                                                cbPtr->paramNames.insert(cbPtr->paramNames.begin(), aliasToInsert);
                                                if (cbPtr->paramTypeNames.size() == oldParameterCount) {
                                                    cbPtr->paramTypeNames.insert(
                                                        cbPtr->paramTypeNames.begin(), std::string()
                                                    );
                                                }
                                                if (cbPtr->paramDefaults.size() == oldParameterCount) {
                                                    cbPtr->paramDefaults.insert(
                                                        cbPtr->paramDefaults.begin(), std::vector<LexerType::Token>()
                                                    );
                                                }
                                            }
                                            // 额外防御：若解析阶段已标记推断别名且与当前别名不一致，以解析阶段为准
                                            if (cbPtr->isClassMethod && !cbPtr->inferredInstanceAlias.empty() && cbPtr->paramNames.front() != cbPtr->inferredInstanceAlias) {
                                                // 替换首参
                                                cbPtr->paramNames[0] = cbPtr->inferredInstanceAlias;
                                            }

                                            // Parse the function block; the declared parameter will become a local slot
                                            parseCodeBlock(*cbPtr);
                                        } else {
                                            // static method: emit normally without injecting alias
                                            parseCodeBlock(*cbPtr);
                                        }

                                    size_t after = bytecodes.size();
                                    int foundProto = -1;
                                    // Find the LAST FUNC_BEGIN chunk added (outermost function, not nested ones)
                                    for (int idx = (int)after - 1; idx >= (int)before; --idx) {
                                        if (!bytecodes[idx].empty() && bytecodes[idx].front().op == VMOperator::FUNC_BEGIN) {
                                            foundProto = idx;
                                            break;
                                        }
                                    }

                                    if (foundProto >= 0) {
                                        // convert chunk index -> protoId by counting prior FUNC_BEGIN chunks
                                        int protoId = 0;
                                        ZEUS_DEBUG_PRINT("[IRGen][Debug] before=%zu after=%zu foundProto=%d bytecodes.size()=%zu", 
                                                         before, after, foundProto, bytecodes.size());
                                        for (size_t p = 0; p < (size_t)foundProto; ++p) {
                                            if (!bytecodes[p].empty() && bytecodes[p].front().op == VMOperator::FUNC_BEGIN) protoId++;
                                        }
                                        ZEUS_DEBUG_PRINT("[IRGen    ] Method '%s': protoId=%d classifyAsInstance=%d isOperatorOverload=%d overloadedOps.size()=%zu", 
                                                         cbPtr->funcName.c_str(), protoId, classifyAsInstance, cbPtr->isOperatorOverload, cbPtr->overloadedOps.size());
                                        
                                        // 如果是运算符重载,存入operatorMethodsMap
                                        if (cbPtr->isOperatorOverload && !cbPtr->overloadedOps.empty()) {
                                            for (const auto& opSymbol : cbPtr->overloadedOps) {
                                                operatorMethodsMap[opSymbol] = protoId;
                                                ZEUS_DEBUG_PRINT("[IRGen    ]   -> Operator '%s' overloaded with protoId=%d", 
                                                                opSymbol.c_str(), protoId);
                                            }
                                        } else if (classifyAsInstance) {
                                            instanceMethodsMap[cbPtr->funcName] = protoId;
                                        } else {
                                            staticMethodsMap[cbPtr->funcName] = protoId;
                                        }
                                    }
                                }
                            }
                        }

                        std::vector<IRType::IRInstr> out;
                        out.push_back(IRUtils::makePushString(className));
                        // push the alias string (empty means no using-declared alias)
                        out.push_back(IRUtils::makePushString(usingThisAlias.empty() ? std::string("") : usingThisAlias));

                        // staticMethods dict: build from staticMethodsMap (methodName -> protoId)
                        if (staticMethodsMap.empty()) {
                            out.push_back(IRUtils::makeDictBuild(0));
                        } else {
                            for (auto &kv : staticMethodsMap) {
                                out.push_back(IRUtils::makePushString(kv.first));
                                IRType::IRInstr push; push.op = VMOperator::PUSH; push.args.push_back(LexerType::Value(kv.second)); out.push_back(push);
                            }
                            out.push_back(IRUtils::makeDictBuild((int)staticMethodsMap.size()));
                        }

                        // instanceMethods dict: build from instanceMethodsMap (methodName -> protoId)
                        if (instanceMethodsMap.empty()) {
                            out.push_back(IRUtils::makeDictBuild(0));
                        } else {
                            for (auto &kv : instanceMethodsMap) {
                                out.push_back(IRUtils::makePushString(kv.first));
                                // push protoId as integer literal
                                IRType::IRInstr push; push.op = VMOperator::PUSH; push.args.push_back(LexerType::Value(kv.second)); out.push_back(push);
                            }
                            out.push_back(IRUtils::makeDictBuild((int)instanceMethodsMap.size()));
                        }

                        // operatorMethods dict: build from operatorMethodsMap (operatorSymbol -> protoId)
                        if (operatorMethodsMap.empty()) {
                            out.push_back(IRUtils::makeDictBuild(0));
                        } else {
                            for (auto &kv : operatorMethodsMap) {
                                out.push_back(IRUtils::makePushString(kv.first));
                                IRType::IRInstr push; push.op = VMOperator::PUSH; push.args.push_back(LexerType::Value(kv.second)); out.push_back(push);
                            }
                            out.push_back(IRUtils::makeDictBuild((int)operatorMethodsMap.size()));
                        }

                        // fields dict build with collected default values
                        // Build by emitting for each field: push key, push defaultValue (or nil placeholder), then at end DICT_BUILD count
                        int fieldCount = (int)fieldsMap.size();
                        if (fieldCount == 0) {
                            out.push_back(IRUtils::makeDictBuild(0));
                        } else {
                            // emit key/value pairs
                            for (const auto& fieldName : fieldOrder) {
                                const auto& field = fieldsMap.at(fieldName);
                                out.push_back(IRUtils::makePushString(fieldName));

                                if (!field.defaultIR.empty()) {
                                    out.insert(out.end(), field.defaultIR.begin(), field.defaultIR.end());
                                } else {
                                    IRType::IRInstr pushnil;
                                    pushnil.op = VMOperator::PUSH;
                                    pushnil.args.push_back(IRType::NullLiteral{});
                                    out.push_back(pushnil);
                                }
                            }

                            out.push_back(IRUtils::makeDictBuild(fieldCount));
                        }

                        // After emitting methods, build CLASS chunk with operatorMethodsMap containing protoIds
                        // 根据是否是 shared class 生成不同的指令
                        IRType::IRInstr mk;

                        if (codeBlock.isSharedClass) {
                            mk.op = VMOperator::CLASS_BUILD_SHARED;
                            ZEUS_DEBUG_PRINT("[IRGen] Emitting CLASS_BUILD_SHARED for '%s'", className.c_str());
                        } else {
                            mk.op = VMOperator::CLASS_BUILD;
                        }

                        std::vector<std::string> fieldNames;
                        std::vector<std::string> fieldTypes;
                        std::vector<bool> fieldConstness;
                        std::vector<IRType::ClassFieldDefault> fieldDefaults;
                        fieldNames.reserve(fieldOrder.size());
                        fieldTypes.reserve(fieldOrder.size());
                        fieldConstness.reserve(fieldOrder.size());
                        fieldDefaults.reserve(fieldOrder.size());
                        for (const auto& fieldName : fieldOrder) {
                            fieldNames.push_back(fieldName);
                            const auto& field = fieldsMap.at(fieldName);
                            fieldTypes.push_back(field.typeName);
                            fieldConstness.push_back(field.isConst);

                            IRType::ClassFieldDefault defaultValue;
                            if (!field.defaultIR.empty()) {
                                if (field.defaultIR.size() == 1 &&
                                    (field.defaultIR.front().op == VMOperator::LIST_BUILD ||
                                     field.defaultIR.front().op == VMOperator::DICT_BUILD) &&
                                    !field.defaultIR.front().args.empty() &&
                                    std::holds_alternative<LexerType::Value>(
                                        field.defaultIR.front().args.front()) &&
                                    std::holds_alternative<ZeusType::BigInteger>(
                                        std::get<LexerType::Value>(
                                            field.defaultIR.front().args.front()).data)) {
                                    int64_t elementCount = -1;
                                    const auto& countValue = std::get<ZeusType::BigInteger>(
                                        std::get<LexerType::Value>(
                                            field.defaultIR.front().args.front()).data);
                                    if (countValue.tryToInt64(elementCount) && elementCount == 0) {
                                        defaultValue.kind = field.defaultIR.front().op == VMOperator::LIST_BUILD
                                            ? IRType::ClassFieldDefaultKind::EmptyArray
                                            : IRType::ClassFieldDefaultKind::EmptyDict;
                                    } else {
                                        defaultValue.kind = IRType::ClassFieldDefaultKind::Unsupported;
                                    }
                                } else if (field.defaultIR.size() == 1 &&
                                    field.defaultIR.front().op == VMOperator::PUSH &&
                                    !field.defaultIR.front().args.empty() &&
                                    std::holds_alternative<IRType::NullLiteral>(
                                        field.defaultIR.front().args.front())) {
                                    defaultValue.kind = IRType::ClassFieldDefaultKind::Null;
                                } else if (field.defaultIR.size() == 1 &&
                                    field.defaultIR.front().op == VMOperator::PUSH &&
                                    !field.defaultIR.front().args.empty() &&
                                    std::holds_alternative<LexerType::Value>(
                                        field.defaultIR.front().args.front())) {
                                    const auto& literal = std::get<LexerType::Value>(
                                        field.defaultIR.front().args.front());
                                    if (std::holds_alternative<ZeusType::BigInteger>(literal.data)) {
                                        int64_t integerValue = 0;
                                        if (std::get<ZeusType::BigInteger>(literal.data).tryToInt64(integerValue)) {
                                            defaultValue.kind = IRType::ClassFieldDefaultKind::Integer;
                                            defaultValue.integerValue = integerValue;
                                        } else {
                                            defaultValue.kind = IRType::ClassFieldDefaultKind::Unsupported;
                                        }
                                    } else if (std::holds_alternative<bool>(literal.data)) {
                                        defaultValue.kind = IRType::ClassFieldDefaultKind::Boolean;
                                        defaultValue.booleanValue = std::get<bool>(literal.data);
                                    } else if (std::holds_alternative<std::string>(literal.data)) {
                                        defaultValue.kind = IRType::ClassFieldDefaultKind::String;
                                        defaultValue.stringValue = std::get<std::string>(literal.data);
                                    } else {
                                        defaultValue.kind = IRType::ClassFieldDefaultKind::Unsupported;
                                    }
                                } else {
                                    defaultValue.kind = IRType::ClassFieldDefaultKind::Unsupported;
                                }
                            }
                            fieldDefaults.push_back(std::move(defaultValue));
                        }
                        mk.metadata = IRType::IRMetadata::classLayout(
                            className,
                            std::move(fieldNames),
                            std::move(fieldTypes),
                            std::move(fieldConstness),
                            std::move(fieldDefaults)
                        );

                        out.push_back(mk);

                        // store class handle into global (类名已在开始时注册，这里只是获取索引)
                        auto s = IRScope::resolve(className);
                        if (s.kind != IRScope::Kind::GLOBAL) {
                            // 如果不是全局变量，重新注册（向后兼容）
                            s.index = IRScope::declareGlobal(className);
                        }
                        IRType::IRInstr st = IRUtils::makeStoreGlobal(s.index, /*const*/ true, &className);
                        out.push_back(st);

                        // Append the class build chunk (place CLASS before the method MAKE_FUNCTION/STORE sequence)
                        // We insert the CLASS chunk at current bytecodes end so class registration happens after methods.
                        if (funcSink) { funcSink->insert(funcSink->end(), out.begin(), out.end()); }
                        else { bytecodes.push_back(std::move(out)); }

                        ZEUS_DEBUG_PRINT("[IRGen] .. CLASS END name=%s", className.c_str());
                        return;
                    }

                    case CodeBlockType::IMPL: {
                        if (codeBlock.implTypeName.empty()) {
                            TreeParser::Node dummy;
                            IRExceptionNS::IRException ex(
                                "impl declaration requires a target type", 0, dummy);
                            ex._throw(status);
                            return;
                        }

                        const auto* builtinType = ZeusBuiltins::findTypeImplementation(
                            codeBlock.implTypeName);
                        const std::string implTypeName = builtinType != nullptr
                            ? std::string(builtinType->canonicalName)
                            : codeBlock.implTypeName;

                        std::unordered_map<std::string, int> staticMethods;
                        std::unordered_map<std::string, int> instanceMethods;
                        std::unordered_map<std::string, int> operatorMethods;

                        for (const auto& node : codeBlock.nodes) {
                            if (!std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                continue;
                            }
                            const auto& methodPtr =
                                std::get<std::unique_ptr<TreeParser::CodeBlock>>(node);
                            if (!methodPtr) continue;
                            auto& method = *methodPtr;
                            if (method.type != CodeBlockType::FUNC || method.funcName.empty()) {
                                continue;
                            }

                            method.ownerClassName = implTypeName;
                            method.isClassMethod = true;
                            method.isStaticMethod =
                                method.isStaticMethod || method.isCompileTimeStatic;
                            const bool isStatic = method.isStaticMethod;

                            const size_t before = bytecodes.size();
                            parseCodeBlock(method);
                            const size_t after = bytecodes.size();
                            int foundChunk = -1;
                            for (int index = static_cast<int>(after) - 1;
                                 index >= static_cast<int>(before); --index) {
                                if (!bytecodes[static_cast<size_t>(index)].empty() &&
                                    bytecodes[static_cast<size_t>(index)].front().op ==
                                        VMOperator::FUNC_BEGIN) {
                                    foundChunk = index;
                                    break;
                                }
                            }
                            if (foundChunk < 0) {
                                TreeParser::Node dummy;
                                IRExceptionNS::IRException ex(
                                    "failed to emit impl method '" + method.funcName + "'",
                                    method.sourceLine, dummy);
                                ex._throw(status);
                                return;
                            }

                            int protoId = 0;
                            for (int index = 0; index < foundChunk; ++index) {
                                if (!bytecodes[static_cast<size_t>(index)].empty() &&
                                    bytecodes[static_cast<size_t>(index)].front().op ==
                                        VMOperator::FUNC_BEGIN) {
                                    ++protoId;
                                }
                            }

                            if (method.isOperatorOverload && !method.overloadedOps.empty()) {
                                for (const auto& symbol : method.overloadedOps) {
                                    if (!operatorMethods.emplace(symbol, protoId).second) {
                                        TreeParser::Node dummy;
                                        IRExceptionNS::IRException ex(
                                            "duplicate impl operator '" + symbol +
                                            "' for type '" + implTypeName + "'",
                                            method.sourceLine, dummy);
                                        ex._throw(status);
                                        return;
                                    }
                                }
                            } else {
                                auto& methods = isStatic ? staticMethods : instanceMethods;
                                if (!methods.emplace(method.funcName, protoId).second) {
                                    TreeParser::Node dummy;
                                    IRExceptionNS::IRException ex(
                                        "duplicate impl method '" + method.funcName +
                                        "' for type '" + implTypeName + "'",
                                        method.sourceLine, dummy);
                                    ex._throw(status);
                                    return;
                                }
                            }
                        }

                        IRType::IRInstr registration;
                        registration.op = VMOperator::TYPE_IMPL_BUILD;
                        registration.line = codeBlock.sourceLine;
                        registration.args.push_back(implTypeName);
                        auto appendMethodMap = [&](const std::unordered_map<std::string, int>& methods) {
                            registration.args.push_back(LexerType::Value(
                                static_cast<int>(methods.size())));
                            for (const auto& entry : methods) {
                                registration.args.push_back(entry.first);
                                registration.args.push_back(LexerType::Value(entry.second));
                            }
                        };
                        appendMethodMap(instanceMethods);
                        appendMethodMap(staticMethods);
                        appendMethodMap(operatorMethods);
                        bytecodes.push_back(std::vector<IRType::IRInstr>{std::move(registration)});
                        return;
                    }

                    case CodeBlockType::FOR: {
                            // 1. 分配/注册局部槽（复制模式）或内部槽（引用模式）
                            // 之前逻辑在无绑定的 COUNT / COUNT_SIMPLE 情况下没有分配 idx/end/step，导致后续 IR 生成 STORE_VALUE_LOCAL -1。
                            // 修复：分配内部槽与绑定无关；若存在绑定再处理用户槽或引用注册。

                            bool hasBinding = !codeBlock.forBindings.empty();
                            TreeParser::CodeBlock::ForBinding firstBinding; // 若有绑定再使用
                            if (hasBinding) firstBinding = codeBlock.forBindings[0];

                            if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::LIST) {
                                // 列表循环：需要 list / len / idx 三个内部槽
                                codeBlock.loopHidden.listSlot = IRScope::allocateInternalLocal();
                                codeBlock.loopHidden.lenSlot  = IRScope::allocateInternalLocal();
                                codeBlock.loopHidden.idxSlot  = IRScope::allocateInternalLocal();
                                if (hasBinding) {
                                    if (firstBinding.isRef) {
                                        IRScope::registerListElemRef(firstBinding.name, codeBlock.loopHidden.listSlot, codeBlock.loopHidden.idxSlot, firstBinding.isConst);
                                    } else {
                                        IRScope::declareLocal(firstBinding.name);
                                    }
                                }
                            }
                            else if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::COUNT || codeBlock.forKind == TreeParser::CodeBlock::ForKind::COUNT_SIMPLE) {
                                // 计数循环：idx / end / step，无绑定也必须分配
                                codeBlock.loopHidden.idxSlot  = IRScope::allocateInternalLocal();
                                codeBlock.loopHidden.endSlot  = IRScope::allocateInternalLocal();
                                codeBlock.loopHidden.stepSlot = IRScope::allocateInternalLocal();
                                if (hasBinding) {
                                    if (firstBinding.isRef) {
                                        IRScope::registerCountIndexRef(firstBinding.name, codeBlock.loopHidden.idxSlot, firstBinding.isConst);
                                    } else {
                                        IRScope::declareLocal(firstBinding.name);
                                    }
                                }
                            }
                            else if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::DICT) {
                                codeBlock.loopHidden.listSlot = IRScope::allocateInternalLocal(); // dictSlot
                                codeBlock.loopHidden.lenSlot  = IRScope::allocateInternalLocal(); // key count
                                codeBlock.loopHidden.idxSlot  = IRScope::allocateInternalLocal(); // index
                                codeBlock.loopHidden.dictKeysSlot   = IRScope::allocateInternalLocal(); // keys array
                                codeBlock.loopHidden.dictCurKeySlot = IRScope::allocateInternalLocal(); // current key
                                if (codeBlock.forBindings.size() == 2) {
                                    const auto &kb = codeBlock.forBindings[0];
                                    const auto &vb = codeBlock.forBindings[1];
                                    if (!kb.isRef) {
                                        IRScope::declareLocal(kb.name);
                                    } else {
                                        IRScope::registerDictKeyRef(kb.name, codeBlock.loopHidden.dictCurKeySlot);
                                    }
                                    if (vb.isRef) {
                                        IRScope::registerDictValueRef(vb.name, codeBlock.loopHidden.listSlot, codeBlock.loopHidden.dictCurKeySlot, vb.isConst);
                                    } else {
                                        IRScope::declareLocal(vb.name);
                                    }
                                }
                            }
                            else if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::COND) {
                                // 条件循环无需内部槽
                            }

                        // 构建 body IR（复用现有遍历逻辑至临时 sink）
                        std::vector<IRType::IRInstr> bodyIR;
                        auto* prevSink = funcSink;
                        funcSink = &bodyIR;
                        for (const auto& node : codeBlock.nodes) {
                            if (std::holds_alternative<TreeParser::Node>(node)) {
                                parseNodeToBytecodes(std::get<TreeParser::Node>(node));
                            } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(node));
                            }
                        }
                        funcSink = prevSink;

                        // 组装 ForBuildInput
                        IRFor::ForBuildInput input{
                            codeBlock,
                            bodyIR,
                            [this](const std::vector<LexerType::Token>& toks) {
                                return IRGen::emitExprIR(status, strictDecl, futureFuncDefs, toks);
                            }
                        };
                        std::vector<IRType::IRInstr> forIR;
                        if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::LIST) {
                            forIR = IRFor::buildListForIR(input);
                        }
                        else if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::COUNT || codeBlock.forKind == TreeParser::CodeBlock::ForKind::COUNT_SIMPLE) {
                            forIR = IRFor::buildCountForIR(input);
                        }
                        else if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::COND) {
                            forIR = IRFor::buildCondForIR(input);
                        }
                        else if (codeBlock.forKind == TreeParser::CodeBlock::ForKind::DICT) {
                            forIR = IRFor::buildDictForIR(input);
                        }
                        else {
                            // 未识别 forKind 或 header 无效：不生成 IR
                        }

                        if (!forIR.empty()) {
                            if (funcSink) {
                                funcSink->insert(funcSink->end(), forIR.begin(), forIR.end());
                            } else {
                                bytecodes.push_back(std::move(forIR));
                            }
                        }
                        ZEUS_DEBUG_PRINT("[IRGen] .. FOR END addr=%p kind=%d", (void*)&codeBlock, (int)codeBlock.forKind);
                        return;
                    }

                    case CodeBlockType::TRY: {
                        // ===== try-catch 代码块生成 =====
                        // try { ... } catch (&e) { ... }
                        // 结构：TRY 块的 nodes 中：
                        //   - 普通节点是 try 体
                        //   - 最后一个 CodeBlock (类型 CATCH) 是 catch 块

                        // 找到 catch 块
                        TreeParser::CodeBlock* catchBlock = nullptr;
                        
                        for (const auto& node : codeBlock.nodes) {
                            if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                auto& cbPtr = std::get<std::unique_ptr<TreeParser::CodeBlock>>(node);
                                if (cbPtr && cbPtr->type == CodeBlockType::CATCH) {
                                    catchBlock = cbPtr.get();
                                }
                            }
                        }

                        // 生成 try 体 IR (排除 catch 块)
                        std::vector<IRType::IRInstr> tryBodyIR;
                        auto* prevSink = funcSink;
                        funcSink = &tryBodyIR;
                        for (const auto& node : codeBlock.nodes) {
                            if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                auto& cbPtr = std::get<std::unique_ptr<TreeParser::CodeBlock>>(node);
                                if (cbPtr && cbPtr->type == CodeBlockType::CATCH) {
                                    continue; // 跳过 catch 块
                                }
                                parseCodeBlock(*cbPtr);
                            } else if (std::holds_alternative<TreeParser::Node>(node)) {
                                parseNodeToBytecodes(std::get<TreeParser::Node>(node));
                            }
                        }
                        funcSink = prevSink;

                        // 生成 catch 体 IR
                        std::vector<IRType::IRInstr> catchBodyIR;
                        int catchVarSlot = -1;  // catch 变量的局部槽索引
                        if (catchBlock) {
                            // 若 catch 有变量，声明局部槽
                            if (!catchBlock->catchVarName.empty()) {
                                catchVarSlot = IRScope::declareLocal(catchBlock->catchVarName);
                            }

                            funcSink = &catchBodyIR;
                            for (const auto& node : catchBlock->nodes) {
                                if (std::holds_alternative<TreeParser::Node>(node)) {
                                    parseNodeToBytecodes(std::get<TreeParser::Node>(node));
                                } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                    parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(node));
                                }
                            }
                            funcSink = prevSink;
                        }

                        // 组装最终 IR:
                        // TRY_BEGIN catchOffset        ; catchOffset = tryBody.size + 1 (跳过 JUMP)
                        // <tryBody IR>
                        // TRY_END
                        // JUMP endOffset               ; 跳过 catch 块
                        // CATCH_BEGIN "varName" slotIndex
                        // <catchBody IR>
                        // CATCH_END
                        // :end

                        std::vector<IRType::IRInstr> resultIR;
                        
                        // TRY_BEGIN: catchOffset = tryBody.size + 3 (跳过 tryBody + TRY_END + JUMP)
                        // 从 TRY_BEGIN 位置偏移到 CATCH_BEGIN
                        IRType::IRInstr tryBeginInstr;
                        tryBeginInstr.op = VMOperator::TRY_BEGIN;
                        tryBeginInstr.args.push_back(LexerType::Value(static_cast<int64_t>(tryBodyIR.size() + 3)));
                        resultIR.push_back(tryBeginInstr);
                        
                        // try body
                        for (auto& ins : tryBodyIR) {
                            resultIR.push_back(std::move(ins));
                        }
                        
                        // TRY_END
                        IRType::IRInstr tryEndInstr;
                        tryEndInstr.op = VMOperator::TRY_END;
                        resultIR.push_back(tryEndInstr);
                        
                        // JUMP to end (skip catch block)
                        // endOffset = catchBody.size + 2 (CATCH_BEGIN + catchBody + CATCH_END)
                        IRType::IRInstr jumpInstr;
                        jumpInstr.op = VMOperator::JUMP;
                        jumpInstr.args.push_back(LexerType::Value(static_cast<int64_t>(catchBodyIR.size() + 2)));
                        resultIR.push_back(jumpInstr);
                        
                        // CATCH_BEGIN "varName" slotIndex
                        IRType::IRInstr catchBeginInstr;
                        catchBeginInstr.op = VMOperator::CATCH_BEGIN;
                        if (catchBlock) {
                            catchBeginInstr.args.push_back(catchBlock->catchVarName);
                            catchBeginInstr.args.push_back(LexerType::Value(catchVarSlot));
                        } else {
                            catchBeginInstr.args.push_back(std::string(""));
                        }
                        resultIR.push_back(catchBeginInstr);
                        
                        // catch body
                        for (auto& ins : catchBodyIR) {
                            resultIR.push_back(std::move(ins));
                        }
                        
                        // CATCH_END
                        IRType::IRInstr catchEndInstr;
                        catchEndInstr.op = VMOperator::CATCH_END;
                        resultIR.push_back(catchEndInstr);

                        // 输出到当前 sink
                        if (funcSink) {
                            funcSink->insert(funcSink->end(), resultIR.begin(), resultIR.end());
                        } else {
                            bytecodes.push_back(std::move(resultIR));
                        }
                        
                        ZEUS_DEBUG_PRINT("[IRGen] .. TRY END addr=%p", (void*)&codeBlock);
                        return;
                    }

                    case CodeBlockType::WITH: {
                        // ===== with 代码块生成 =====
                        // with (expr) { ... }
                        // 语义：
                        // 1. 计算 expr 得到对象
                        // 2. 调用对象的 operator[with]() 进入
                        // 3. 执行 with 体
                        // 4. 结束时自动清理

                        // 生成 with 表达式 IR
                        std::vector<IRType::IRInstr> exprIR;
                        if (!codeBlock.withExprTokens.empty()) {
                            exprIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, codeBlock.withExprTokens);
                        }

                        // 如果有 capture 变量，声明局部槽
                        int withCaptureSlot = -1;
                        if (!codeBlock.withCaptureVarName.empty()) {
                            withCaptureSlot = IRScope::declareLocal(codeBlock.withCaptureVarName);
                            ZEUS_DEBUG_PRINT("[IRGen] WITH: capture var '%s' at slot %d, isRef=%d", 
                                codeBlock.withCaptureVarName.c_str(), withCaptureSlot, codeBlock.withCaptureIsRef);
                        }

                        // 生成 with 体 IR
                        std::vector<IRType::IRInstr> bodyIR;
                        auto* prevSink = funcSink;
                        funcSink = &bodyIR;
                        for (const auto& node : codeBlock.nodes) {
                            if (std::holds_alternative<TreeParser::Node>(node)) {
                                parseNodeToBytecodes(std::get<TreeParser::Node>(node));
                            } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(node));
                            }
                        }
                        funcSink = prevSink;

                        // 组装最终 IR:
                        // <expr IR>           ; 将 with 对象压栈
                        // WITH_BEGIN slotIdx  ; 调用 operator[with]，保存对象引用，可选存入 capture 槽
                        // <body IR>
                        // WITH_END            ; 清理

                        std::vector<IRType::IRInstr> resultIR;
                        
                        // expr IR
                        for (auto& ins : exprIR) {
                            resultIR.push_back(std::move(ins));
                        }
                        
                        // WITH_BEGIN slotIdx (-1 表示无 capture)
                        IRType::IRInstr withBeginInstr;
                        withBeginInstr.op = VMOperator::WITH_BEGIN;
                        withBeginInstr.args.push_back(LexerType::Value(static_cast<int64_t>(withCaptureSlot)));
                        resultIR.push_back(withBeginInstr);
                        
                        // body IR
                        for (auto& ins : bodyIR) {
                            resultIR.push_back(std::move(ins));
                        }
                        
                        // WITH_END
                        IRType::IRInstr withEndInstr;
                        withEndInstr.op = VMOperator::WITH_END;
                        resultIR.push_back(withEndInstr);

                        // 输出到当前 sink
                        if (funcSink) {
                            funcSink->insert(funcSink->end(), resultIR.begin(), resultIR.end());
                        } else {
                            bytecodes.push_back(std::move(resultIR));
                        }
                        
                        ZEUS_DEBUG_PRINT("[IRGen] .. WITH END addr=%p", (void*)&codeBlock);
                        return;
                    }

                    case CodeBlockType::USING: {
                        // ===== using original { } 作用域块处理 =====
                        // 语义：
                        // 1. 进入块时，保存当前 using original 模式
                        // 2. 设置新的 using original 模式（所有标志为 true）
                        // 3. 处理块内语句
                        // 4. 退出块时，恢复之前的模式

                        ZEUS_DEBUG_PRINT("[IRGen] >> Enter USING CodeBlock addr=%p", (void*)&codeBlock);

                        // Push new mode: all flags set to true (using original mode)
                        IRScope::pushOriginalMode(true, true, true);
                        ZEUS_DEBUG_PRINT("[IRGen] .. USING pushed mode (all flags true)");

                        // 处理块内的语句
                        for (const auto& node : codeBlock.nodes) {
                            if (std::holds_alternative<TreeParser::Node>(node)) {
                                parseNodeToBytecodes(std::get<TreeParser::Node>(node));
                            } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                                parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(node));
                            }
                        }

                        // Pop mode on exit
                        IRScope::popOriginalMode();
                        ZEUS_DEBUG_PRINT("[IRGen] << Leave USING CodeBlock addr=%p (mode popped)", (void*)&codeBlock);
                        return;
                    }

                    default: break; // 普通块走下面遍历
                }

                for (const auto& node : codeBlock.nodes) {
                    if (std::holds_alternative<TreeParser::Node>(node)) {
                        parseNodeToBytecodes(std::get<TreeParser::Node>(node));
                    } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                        parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(node));
                    }
                }
                ZEUS_DEBUG_PRINT("[IRGen] << Leave CodeBlock addr=%p type=%d (normal)", (void*)&codeBlock, (int)codeBlock.type);
            }

        private:
            void handleFunctionCodeBlock(TreeParser::CodeBlock& codeBlock) {
                // 对于嵌套函数，需要在进入子函数自身环境之前先记录父环境中用于存放该函数闭包的槽位，
                // 否则 beginFunction() 之后 resolve(name) 将看不到父环境局部，导致我们把子函数自己的局部槽误当成父槽。
                int parentStoreSlot = -1; // 仅在嵌套（funcSink!=nullptr 且自身不是顶层）时使用
                bool isNested = (funcSink != nullptr); // funcSink=nullptr 表示当前在最外层（顶层，或顶层递归入口）
                if (isNested && !codeBlock.funcName.empty()) {
                    // 解析父函数环境的该名字；若不存在则声明一个局部槽用于存放闭包。
                    auto existingParent = IRScope::resolve(codeBlock.funcName);
                    if (existingParent.kind == IRScope::Kind::LOCAL) {
                        parentStoreSlot = (int)existingParent.index;
                    } else if (existingParent.kind == IRScope::Kind::UNDEFINED) {
                        parentStoreSlot = IRScope::declareLocal(codeBlock.funcName);
                    } else if (existingParent.kind == IRScope::Kind::UPVALUE) {
                        // 极少见：父环境通过更外层捕获得到同名 upvalue，不希望覆盖；此时仍为其在父环境再申请一个局部槽。
                        parentStoreSlot = IRScope::declareLocal(codeBlock.funcName);
                    } else {
                        // GLOBAL 情况（不期望在此出现），降级为局部声明
                        parentStoreSlot = IRScope::declareLocal(codeBlock.funcName);
                    }
                }

                IRScope::beginFunction();
                bool captureAllEnabled = codeBlock.isCaptureAll || codeBlock.isCopyAll;
                IRScope::pushCaptureAll(captureAllEnabled);
                ZEUS_DEBUG_PRINT("[IRGen][Capture] Function '%s' captureAll=%d copyAll=%d captureVars=%zu copyVars=%zu",
                                 codeBlock.funcName.c_str(),
                                 (int)codeBlock.isCaptureAll,
                                 (int)codeBlock.isCopyAll,
                                 codeBlock.captureVars.size(),
                                 codeBlock.copyVars.size());
                // 若当前 codeBlock 指定了函数名，记录到 FuncEnv 以便在错误消息中使用
                if (!codeBlock.funcName.empty()) {
                    if (!IRScope::funcStack.empty()) IRScope::funcStack.back().name = codeBlock.funcName;
                }

                // 在进入函数后，立即为所有参数声明局部槽位（包括可能由 class alias 注入的隐式参数）。
                // 这必须在解析函数体或扫描子函数之前完成，以便内层嵌套函数在解析时能正确解析到外层参数作为 LOCAL/UPVALUE。
                ZEUS_DEBUG_PRINT("[IRGen][Params] Function '%s' declaring %zu parameters", 
                                 codeBlock.funcName.c_str(), codeBlock.paramNames.size());
                for (size_t i = 0; i < codeBlock.paramNames.size(); ++i) {
                    const auto& param = codeBlock.paramNames[i];
                    int slot = IRScope::declareLocal(param);
                    ZEUS_DEBUG_PRINT("[IRGen][Params]   '%s' -> local slot %d", param.c_str(), slot);
                    
                    // 调试: 打印默认值信息
                    if (i < codeBlock.paramDefaults.size() && !codeBlock.paramDefaults[i].empty()) {
                        ZEUS_DEBUG_PRINT("[IRGen][Params]   '%s' has default value (%zu tokens)", 
                                         param.c_str(), codeBlock.paramDefaults[i].size());
                    }
                }
                
                // 声明变长参数作为局部变量
                if (!codeBlock.variadicArgs.empty()) {
                    int slot = IRScope::declareLocal(codeBlock.variadicArgs);
                    ZEUS_DEBUG_PRINT("[IRGen][Params]   '*%s' -> local slot %d", codeBlock.variadicArgs.c_str(), slot);
                }
                if (!codeBlock.variadicKwargs.empty()) {
                    int slot = IRScope::declareLocal(codeBlock.variadicKwargs);
                    ZEUS_DEBUG_PRINT("[IRGen][Params]   '**%s' -> local slot %d", codeBlock.variadicKwargs.c_str(), slot);
                }

                // 预声明本函数内的局部变量，确保嵌套函数在编译时可以捕获到后续声明的局部
                for (const auto &nodeVariant : codeBlock.nodes) {
                    if (std::holds_alternative<TreeParser::Node>(nodeVariant)) {
                        const auto &nd = std::get<TreeParser::Node>(nodeVariant);
                        if (nd.type == NodeType::VAR_DECL_STMT) {
                            bool isGlobalDecl = false;
                            bool isSharedDecl = false;
                            std::string declName;
                            bool beforeAssign = true;
                            bool readingType = false;
                            for (const auto &tk : nd.tokens) {
                                if (std::holds_alternative<LexerType::Operator>(tk)) {
                                    const auto &op = std::get<LexerType::Operator>(tk);
                                    if (op.symbol == "=") {
                                        beforeAssign = false;
                                        readingType = false;
                                    } else if (beforeAssign && op.symbol == ":") {
                                        readingType = true;
                                    }
                                }
                                if (!beforeAssign || readingType) {
                                    continue;
                                }
                                if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                    const auto &kw = std::get<LexerType::Keyword>(tk);
                                    if (kw.name == "var") {
                                        isGlobalDecl = true;
                                    } else if (kw.name == "let") {
                                        isGlobalDecl = false;
                                    } else if (kw.name == "shared") {
                                        isSharedDecl = true;
                                    } else if (kw.name != "const") {
                                        declName = kw.name;
                                    }
                                }
                            }
                            if (!declName.empty() && !isGlobalDecl && !isSharedDecl) {
                                IRScope::declareLocal(declName);
                            }
                        }
                    }
                }

                // 预扫描：在函数体中先为所有子函数名字分配局部槽，避免后续 var/let 声明或重复声明造成槽位覆盖问题
                // （仅作用于此函数内部的直接子函数，不递归深入）
                std::unordered_set<std::string> localFuncNames; // 用于后面过滤 captures（不捕获同级函数名 / 自身）
                for (const auto &nodeVariant : codeBlock.nodes) {
                    if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(nodeVariant)) {
                        auto const &cbPtr = std::get<std::unique_ptr<TreeParser::CodeBlock>>(nodeVariant);
                        if (cbPtr && cbPtr->type == CodeBlockType::FUNC && !cbPtr->funcName.empty()) {
                            auto existing = IRScope::resolve(cbPtr->funcName);
                            if (existing.kind == IRScope::Kind::UNDEFINED) {
                                IRScope::declareLocal(cbPtr->funcName);
                            }
                            localFuncNames.insert(cbPtr->funcName);
                        }
                    }
                }

                // 捕获全部时，提前建立对外层局部/上值的捕获链，避免编译顺序导致的未捕获
                if (captureAllEnabled && IRScope::funcStack.size() >= 2) {
                    std::unordered_set<std::string> outerNames;
                    for (int fi = static_cast<int>(IRScope::funcStack.size()) - 2; fi >= 0; --fi) {
                        auto &outer = IRScope::funcStack[fi];
                        for (const auto &kv : outer.locals) {
                            if (kv.first == codeBlock.funcName || localFuncNames.count(kv.first)) continue;
                            outerNames.insert(kv.first);
                        }
                        for (const auto &kv : outer.upvalues) {
                            if (kv.first == codeBlock.funcName || localFuncNames.count(kv.first)) continue;
                            outerNames.insert(kv.first);
                        }
                    }
                    for (const auto &nm : outerNames) {
                        IRScope::forceCapture(nm);
                    }
                }

                // 提前根据显式捕获列表为当前函数建立 upvalue 捕获链
                std::vector<std::string> explicitCaptureNames = codeBlock.captureVars;
                explicitCaptureNames.insert(
                    explicitCaptureNames.end(),
                    codeBlock.copyVars.begin(),
                    codeBlock.copyVars.end()
                );
                if (!explicitCaptureNames.empty()) {
                    ZEUS_DEBUG_PRINT("[IRGen    ] Function '%s' pre-resolving %zu captures (funcStack depth=%zu)",
                                    codeBlock.funcName.c_str(), explicitCaptureNames.size(), IRScope::funcStack.size());
                    for (const auto &capName : explicitCaptureNames) {
                        auto resolved = IRScope::resolve(capName);
                        if (resolved.kind == IRScope::Kind::UNDEFINED) {
                            ZEUS_DEBUG_PRINT("[IRGen    ]   '%s' not found by resolve, trying forceCapture...", capName.c_str());
                            resolved = IRScope::forceCapture(capName);
                        }
                        ZEUS_DEBUG_PRINT("[IRGen    ]   '%s' -> kind=%d index=%d",
                                        capName.c_str(), (int)resolved.kind, resolved.index);
                        if (resolved.kind == IRScope::Kind::UNDEFINED) {
                            ZEUS_DEBUG_PRINT("[IRGen    ]   WARNING: '%s' still not found after forceCapture!", capName.c_str());
                        }
                    }
                }
                int preDeclaredIndex = -1;            // 本函数自身名字在其定义所在“声明域”(顶层或父函数)中的槽位
                IRScope::Kind preDeclaredKind = IRScope::Kind::UNDEFINED; // GLOBAL 或 LOCAL
                if (!codeBlock.funcName.empty()) {
                    if (strictDecl) {
                        futureFuncDefs.erase(codeBlock.funcName);
                    }
                    if (funcSink == nullptr) {
                        if (strictDecl) {
                            auto itg = IRScope::globals.find(codeBlock.funcName);
                            if (itg != IRScope::globals.end()) {
                                preDeclaredIndex = itg->second;
                                preDeclaredKind = IRScope::Kind::GLOBAL;
                            }
                            else {
                                preDeclaredIndex = IRScope::declareGlobal(codeBlock.funcName);
                                preDeclaredKind = IRScope::Kind::GLOBAL;
                            }
                        } else {
                            preDeclaredIndex = IRScope::declareGlobal(codeBlock.funcName);
                            preDeclaredKind = IRScope::Kind::GLOBAL;
                        }
                    } else {
                        // 嵌套函数：闭包要写回到父环境的 parentStoreSlot；若上面未能正确获得则兜底声明。
                        if (parentStoreSlot >= 0) {
                            preDeclaredIndex = parentStoreSlot;
                            preDeclaredKind = IRScope::Kind::LOCAL;
                        } else {
                            // 理论上不会进入：但为了安全仍声明一个父环境局部槽（此时 parentStoreSlot=-1 表示未提前记录）
                            preDeclaredIndex = IRScope::declareLocal(codeBlock.funcName);
                            preDeclaredKind = IRScope::Kind::LOCAL;
                        }
                    }
                }

                std::vector<IRType::IRInstr> funcIR;
                IRType::IRInstr begin;
                begin.op = VMOperator::FUNC_BEGIN;
                begin.args.push_back(LexerType::Value((int)codeBlock.paramNames.size()));
                // 传递变长参数元数据
                begin.args.push_back(LexerType::Value(codeBlock.variadicArgs.empty() ? 0 : 1)); // hasVariadicArgs
                begin.args.push_back(LexerType::Value(codeBlock.variadicKwargs.empty() ? 0 : 1)); // hasVariadicKwargs
                begin.args.push_back(codeBlock.variadicArgs); // variadicArgsName
                begin.args.push_back(codeBlock.variadicKwargs); // variadicKwargsName
                
                // Pass default parameter indices; their expressions are emitted in the function prologue.
                int minRequired = 0;
                int defaultCount = 0;
                for (size_t i = 0; i < codeBlock.paramNames.size(); ++i) {
                    const bool hasDefault =
                        i < codeBlock.paramDefaults.size() && !codeBlock.paramDefaults[i].empty();
                    if (!hasDefault) {
                        minRequired++;
                    } else {
                        defaultCount++;
                    }
                }
                begin.args.push_back(LexerType::Value(minRequired)); // minRequiredParams
                begin.args.push_back(LexerType::Value(defaultCount)); // 默认参数数量

                // Keep the historical (parameter index, literal payload) pair layout.
                // New bytecode evaluates the expression in the function prologue; the
                // payload is ignored whenever JUMP_IF_PARAM_PROVIDED is present.
                for (size_t i = 0; i < codeBlock.paramNames.size(); ++i) {
                    if (i < codeBlock.paramDefaults.size() && !codeBlock.paramDefaults[i].empty()) {
                        begin.args.push_back(LexerType::Value((int)i));
                        begin.args.push_back(LexerType::Value(0));
                    }
                }
                
                // 传递参数名列表(用于命名参数匹配)
                // paramNames只包含常规参数,不包括*args和**kwargs
                // args[7+2*defaultCount]: 参数名数量
                // args[8+2*defaultCount...]: 各参数名
                int regularParamCount = (int)codeBlock.paramNames.size();
                
                begin.args.push_back(LexerType::Value(regularParamCount)); // 常规参数数量
                for (int i = 0; i < regularParamCount; ++i) {
                    begin.args.push_back(codeBlock.paramNames[i]); // 参数名
                }
                
                funcIR.push_back(begin);

                // FUNC_BEGIN 必须保持 chunk 首指令，标注紧随其后，避免破坏
                // VM/编译器现有的函数块识别逻辑。
                const size_t signatureAnnotationIndex = funcIR.size();
                std::string inferredReturnType;
                const auto inferredReturn = inferredFunctionReturnTypes.find(codeBlock.id);
                if (inferredReturn != inferredFunctionReturnTypes.end()) {
                    inferredReturnType = inferredReturn->second;
                }
                funcIR.push_back(makeFunctionSignatureAnnotation(codeBlock, inferredReturnType));

                if (strictDecl && funcSink == nullptr && !codeBlock.funcName.empty()) {
                    for (auto & fp : collected) {
                        if (fp.name == codeBlock.funcName) {
                            fp.hasDefinition = true;
                            break;
                        }
                    }
                }

                // Note: params already declared above immediately after beginFunction().
                

                auto* prevSink = funcSink;
                funcSink = &funcIR;

                // Evaluate omitted defaults in parameter order for every call.
                for (size_t i = 0; i < codeBlock.paramNames.size(); ++i) {
                    if (i >= codeBlock.paramDefaults.size() || codeBlock.paramDefaults[i].empty()) {
                        continue;
                    }

                    auto defaultIR = IRGen::emitExprIR(
                        status, strictDecl, futureFuncDefs, codeBlock.paramDefaults[i]);
                    if (status != IRType::IRCompilerStatus::Running || defaultIR.empty()) {
                        if (defaultIR.empty() && status == IRType::IRCompilerStatus::Running) {
                            status = IRType::IRCompilerStatus::Error;
                            ZEUS_DEBUG_PRINT(
                                "[IRGen][Params] Failed to generate default expression for '%s.%s'",
                                codeBlock.funcName.c_str(), codeBlock.paramNames[i].c_str());
                        }
                        break;
                    }

                    IRType::IRInstr skipDefault;
                    skipDefault.op = VMOperator::JUMP_IF_PARAM_PROVIDED;
                    skipDefault.args.push_back(LexerType::Value((int)i));
                    skipDefault.args.push_back(LexerType::Value((int)defaultIR.size() + 1));
                    funcIR.push_back(std::move(skipDefault));
                    funcIR.insert(funcIR.end(), defaultIR.begin(), defaultIR.end());

                    IRType::IRInstr assignDefault;
                    assignDefault.op = VMOperator::STORE_VALUE_LOCAL;
                    assignDefault.args.push_back(LexerType::Value((int)i));
                    assignDefault.args.push_back(std::string("mutable"));
                    funcIR.push_back(std::move(assignDefault));
                }

                // 为避免局部函数声明覆盖先前 let 变量：
                // 在普通块（非函数、自身非 for 特殊处理）中，先预扫描子结点，若遇到函数 CodeBlock，且在局部环境(funcSink!=nullptr)，
                // 先提前 declareLocal 它的 funcName（若还未声明），这样后续 let 变量将使用后续槽位而不会被 MAKE_FUNCTION 回写覆盖。
                if (funcSink) {
                    for (const auto& node : codeBlock.nodes) {
                        if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                            auto &cbPtr = std::get<std::unique_ptr<TreeParser::CodeBlock>>(node);
                            if (cbPtr && cbPtr->type == CodeBlockType::FUNC && !cbPtr->funcName.empty()) {
                                // 如果符号未存在再声明
                                auto existing = IRScope::resolve(cbPtr->funcName);
                                if (existing.kind == IRScope::Kind::UNDEFINED) {
                                    IRScope::declareLocal(cbPtr->funcName);
                                }
                            }
                        }
                    }
                }

                returnReferenceTypeStack.push_back(
                    !codeBlock.returnTypeName.empty() && codeBlock.returnTypeName.front() == '&'
                        ? codeBlock.returnTypeName.substr(1)
                        : std::string()
                );
                for (const auto& node : codeBlock.nodes) {
                    if (std::holds_alternative<TreeParser::Node>(node)) {
                        parseNodeToBytecodes(std::get<TreeParser::Node>(node));
                    } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(node)) {
                        parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(node));
                    }
                }
                returnReferenceTypeStack.pop_back();
                funcSink = prevSink;

                IRScope::FuncEnv env = IRScope::endFunction();
                IRScope::popCaptureAll(captureAllEnabled);

                // 补上隐式捕获数量。其余签名信息在函数体生成前已经写入，
                // 这样不会再插入指令、也不会影响函数体内的跳转偏移。
                if (signatureAnnotationIndex < funcIR.size() &&
                    funcIR[signatureAnnotationIndex].op == VMOperator::IR_ANNOTATION &&
                    funcIR[signatureAnnotationIndex].args.size() > 3) {
                    const int captureCount = static_cast<int>(env.captures.size());
                    auto& signature = funcIR[signatureAnnotationIndex];
                    signature.args[3] = LexerType::Value(captureCount);
                    if (signature.metadata.kind == IRType::IRMetadataKind::FunctionSignature) {
                        signature.metadata.captureCount = captureCount;
                        signature.metadata.hasCaptureCount = true;
                    }
                }

                // 如果禁止隐式捕获，并且用户没有使用显式捕获列表或 capture-all 标记但编译器发现了捕获，
                // 则报错（强制用户显式声明捕获）。
                // 严格捕获模式：不在这里统一报错，具体未解析引用将在具体引用处以 token 行号报错（避免重复）
                
                std::unordered_set<std::string> refSet;
                for (auto const& n : codeBlock.captureVars) {
                    refSet.insert(n);
                }

                std::unordered_set<std::string> copySet;
                for (auto const& n : codeBlock.copyVars) {
                    copySet.insert(n);
                }

                bool captureAllRef = codeBlock.isCaptureAll;
                bool captureAllCopy = codeBlock.isCopyAll;

                // Emit CAPTURE_OUTSIDE_VAR for every captured variable discovered in env.captures.
                // The VM expects these descriptors to know per-slot mode (copy/ref) and source indices.
                for (const auto& cap : env.captures) {
                    // 跳过：当前函数内部直接子函数名字（包括自身）
                    if (localFuncNames.count(cap.name) || (!codeBlock.funcName.empty() && cap.name == codeBlock.funcName)) {
                        continue;
                    }
                    IRType::IRInstr c;
                    c.op = VMOperator::CAPTURE_OUTSIDE_VAR;
                    c.args.push_back(cap.srcKind == IRScope::Kind::LOCAL ? std::string("local") : std::string("upvalue"));
                    c.args.push_back(LexerType::Value(cap.srcIndex));
                    c.args.push_back(LexerType::Value(cap.dstIndex));
                    c.args.push_back(cap.name);

                    int modeFlag = 0; // default copy
                    if (refSet.count(cap.name)) {
                        modeFlag = 1;
                    } else if (copySet.count(cap.name)) {
                        modeFlag = 0;
                    } else {
                        if (captureAllRef) {
                            modeFlag = 1;
                        }
                    }
                    c.args.push_back(LexerType::Value(modeFlag));
                    funcIR.push_back(std::move(c));
                }
                funcIR.push_back(IRType::IRInstr{ VMOperator::FUNC_END, {} });
                bytecodes.push_back(std::move(funcIR));

                // Skip MAKE_FUNCTION and STORE for class methods (handled by CLASS_BUILD)
                if (codeBlock.isClassMethod) {
                    return;
                }

                if (!codeBlock.funcName.empty()) {
                    int protoId = static_cast<int>(bytecodes.size() - 1);
                    if (strictDecl && funcSink == nullptr) {
                        markDefinition(
                            codeBlock.funcName,
                            static_cast<int>(codeBlock.paramNames.size()),
                            protoId,
                            codeBlock.paramNames,
                            codeBlock.paramTypeNames,
                            codeBlock.returnTypeName
                        );
                    }
                    std::vector<IRType::IRInstr> seq;
                    IRType::IRInstr mk; mk.op = VMOperator::MAKE_FUNCTION; mk.args.push_back(LexerType::Value(protoId));
                    seq.push_back(std::move(mk));

                    IRType::IRInstr st;
                    if (preDeclaredKind == IRScope::Kind::GLOBAL) {
                        st.op = VMOperator::STORE_VALUE_GLOBAL;
                        st.args.push_back(LexerType::Value(preDeclaredIndex));
                        st.args.push_back(std::string("mutable"));
                        st.args.push_back(codeBlock.funcName);
                    } else {
                        st.op = VMOperator::STORE_VALUE_LOCAL;
                        st.args.push_back(LexerType::Value(preDeclaredIndex));
                        st.args.push_back(std::string("mutable"));
                    }
                    seq.push_back(std::move(st));

                    if (funcSink == nullptr) {
                        bytecodes.push_back(std::move(seq));
                    } else {
                        funcSink->insert(funcSink->end(), seq.begin(), seq.end());
                    }
                }
            }

            void handleIfCodeBlock(TreeParser::CodeBlock& codeBlock) {
                // 修复：IF 块可能包含多个语句作为 then 部分
                // else 块是最后一个节点，且必须是 CodeBlock 类型
                std::vector<IRType::IRInstr> ifIR;
                
                // 检查是否有真正的 else 块
                // else 块必须是最后一个节点，且是 CodeBlock 类型
                bool hasElse = false;
                size_t thenCount = codeBlock.nodes.size();
                if (codeBlock.nodes.size() >= 2) {
                    const auto& lastNode = codeBlock.nodes.back();
                    if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(lastNode)) {
                        // 最后一个节点是 CodeBlock，这是 else 块
                        hasElse = true;
                        thenCount = codeBlock.nodes.size() - 1;
                    }
                }
                
          ZEUS_DEBUG_PRINT("[IRGen] .. IF BEGIN addr=%p condTokens=%zu thenCount=%zu hasElse=%d", (void*)&codeBlock, codeBlock.conditionTokens.size(), thenCount, hasElse ? 1 : 0);

                // 条件表达式 IR
                if (!codeBlock.conditionTokens.empty()) {
                    auto condIR = IRGen::emitExprIR(status, strictDecl, futureFuncDefs, codeBlock.conditionTokens);
                    ZEUS_DEBUG_PRINT("[IRGen] .. IF condIR size=%zu", condIR.size());
                    ifIR.insert(ifIR.end(), condIR.begin(), condIR.end());
                    if (condIR.empty()) {
                        ZEUS_DEBUG_PRINT("[IRGen][Warn] IF condition produced no IR (possibly unsupported or parsing failure)");
                    }
                }

                // 先放 JUMP_IF_FALSE (跳到 else 或 结束)
                IRType::IRInstr jfalse; jfalse.op = VMOperator::JUMP_IF_FALSE; jfalse.args.push_back(LexerType::Value(0));
                size_t jfalseIndex = ifIR.size();
                ifIR.push_back(jfalse);
                ZEUS_DEBUG_PRINT("[IRGen] .. IF placeholder JUMP_IF_FALSE at index=%zu", jfalseIndex);

                auto* prevSink = funcSink; funcSink = &ifIR;
                
                // 生成 then 部分 IR - 处理所有非 else 的子节点
                for (size_t i = 0; i < thenCount; ++i) {
                    const auto& n = codeBlock.nodes[i];
                    ZEUS_DEBUG_PRINT("[IRGen] .. IF then-child[%zu] variantKind=%d", i, n.index());
                    if (std::holds_alternative<TreeParser::Node>(n)) {
                        parseNodeToBytecodes(std::get<TreeParser::Node>(n));
                    } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(n)) {
                        parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(n));
                    }
                }

                // 如果有 else，需要一个跳过 else 的无条件跳转
                size_t jmpOverElseIndex = (size_t)-1;
                if (hasElse) {
                    IRType::IRInstr jmpOverElse; jmpOverElse.op = VMOperator::JUMP; jmpOverElse.args.push_back(LexerType::Value(0));
                    jmpOverElseIndex = ifIR.size();
                    ifIR.push_back(jmpOverElse);
                }

                // 回填 jfalse -> 跳过 then（直接到 else 或结束）
                int skipThen = static_cast<int>(ifIR.size() - (jfalseIndex + 1));
                ifIR[jfalseIndex].args[0] = LexerType::Value(skipThen);
                ZEUS_DEBUG_PRINT("[IRGen] .. IF backpatch skipThen=%d totalIfIR=%zu", skipThen, ifIR.size());

                // 生成 else IR
                if (hasElse) {
                    const auto& en = codeBlock.nodes.back();  // else 块是最后一个节点
                    ZEUS_DEBUG_PRINT("[IRGen] .. IF else-child variantKind=%d", en.index());
                    if (std::holds_alternative<TreeParser::Node>(en)) {
                        parseNodeToBytecodes(std::get<TreeParser::Node>(en));
                    } else if (std::holds_alternative<std::unique_ptr<TreeParser::CodeBlock>>(en)) {
                        parseCodeBlock(*std::get<std::unique_ptr<TreeParser::CodeBlock>>(en));
                    }
                    // 回填跳过 else 的无条件跳转
                    int skipElse = static_cast<int>(ifIR.size() - (jmpOverElseIndex + 1));
                    ifIR[jmpOverElseIndex].args[0] = LexerType::Value(skipElse);
                    ZEUS_DEBUG_PRINT("[IRGen] .. IF backpatch skipElse=%d at jmpIndex=%zu", skipElse, jmpOverElseIndex);
                }

                funcSink = prevSink;
                if (funcSink) {
                    funcSink->insert(funcSink->end(), ifIR.begin(), ifIR.end());
                    ZEUS_DEBUG_PRINT("[IRGen] .. IF appended to funcSink (current sink size now=%zu)", funcSink->size());
                } else {
                    ZEUS_DEBUG_PRINT("[IRGen] .. IF pushed as standalone chunk (chunkIndex=%zu)", bytecodes.size());
                    bytecodes.push_back(std::move(ifIR));
                }
            }
    };
}


#endif
