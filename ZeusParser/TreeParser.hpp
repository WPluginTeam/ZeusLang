#ifndef TreeParser_hpp
#define TreeParser_hpp

#include <algorithm>
#include <string>
#include <cstdint>
#include <vector>
#include <variant>
#include <memory>
#include <iostream>
#include <unordered_set>
#include "LexerType.hpp"
#include "TreeType.hpp"
#include "FunctionTypeSyntax.hpp"
#ifdef ZEUS_DEBUG_FILE_CHANNEL
#undef ZEUS_DEBUG_FILE_CHANNEL
#endif
#define ZEUS_DEBUG_FILE_CHANNEL ::ZeusDebug::CH_PARSE
#include "../ZeusDebug/Utils.hpp"
namespace TreeParser {
    using Token = LexerType::Token;
    using NodeId = std::uint32_t;

    inline constexpr NodeId InvalidNodeId = 0;

    struct Node;

    struct CodeBlock {
        NodeId id = InvalidNodeId;
        CodeBlockType type;
        // 函数/类公共元数据
        std::string funcName; // 如果是函数块,这里记录函数名
        std::string ownerClassName; // 如果是类方法,这里记录所属类名
        std::vector<std::string> paramNames; // 函数形参名
        std::vector<std::string> paramTypeNames; // 函数形参类型名,空表示未声明
        std::string returnTypeName; // 函数返回类型名,空表示未声明
        std::vector<std::string> decorators; // 函数名前的 @decorator 修饰符
        std::string nativeSymbol; // @native("symbol") 声明的链接符号
        bool isCompileTimeStatic = false; // @static 要求 LLVM 静态检查通过
        
        // 默认参数和变长参数支持
        std::vector<std::vector<Token>> paramDefaults; // 参数默认值表达式(与paramNames对应,空表示无默认值)
        std::string variadicArgs;   // *args 参数名(空表示无变长参数)
        std::string variadicKwargs; // **kwargs 参数名(空表示无命名参数)
        
        bool isStaticMethod = false; // whether the function declaration included 'static' modifier
        std::string className; // 如果是类块,这里记录类名
        bool isSharedClass = false; // 是否是 shared class（存储到共享堆）
        std::vector<std::string> captureVars;
        std::vector<std::string> copyVars;
        // 若为 CLASS 代码块:记录在类体中通过 `using &alias;` 声明的默认实例别名(为空表示未声明)
        std::string classDefaultAlias;
        // 若为 CLASS 内的函数:标记是否为类方法以及推断的实例别名
        bool isClassMethod = false;                // 当前函数是否直接定义于类体内
        std::string inferredInstanceAlias;         // 若被判定为实例方法,记录推断的别名(可能来自 using 或捕获列表)
        // 运算符重载支持
        bool isOperatorOverload = false;           // 标记是否为运算符重载函数
        std::vector<std::string> overloadedOps;    // 重载的运算符列表(如 "+", "-", "[]")
        // 仅用于 IF：存放 if( ... ) 的条件表达式 token 序列
        std::vector<Token> conditionTokens;
        bool isElseBranch = false;
        std::vector<std::variant<Node, std::unique_ptr<CodeBlock>>> nodes;

        bool isCaptureAll = false;
        bool isCopyAll = false;

        // ForMeta: 自定义 for 语法元数据（后续解析阶段填充）
        struct ForBinding {
            std::string name;
            bool isRef = false;        // 是否使用 & 引用语义
            bool isConst = false;      // 是否为 const（支持 `const name` 或 `const &name`）
            bool isKey = false;        // 针对字典: 是否 key 绑定
            bool isValue = false;      // 针对字典: 是否 value 绑定
        };

        enum class ForKind {
            NONE,
            LIST,
            DICT,
            COUNT,
            COUNT_SIMPLE, // for (numExpr) [i] 语法糖：start=0 end=numExpr step=1
            COND
        };

        ForKind forKind = ForKind::NONE;

        std::vector<ForBinding> forBindings; // 按语法顺序保留绑定信息

        // 头部表达式 tokens（四种形态复用）
        std::vector<Token> listExprTokens;   // list 迭代表达式
        std::vector<Token> dictExprTokens;   // dict 迭代表达式
        std::vector<Token> condExprTokens;   // condition for 的条件
        std::vector<Token> countStartTokens; // count: 起始
        std::vector<Token> countEndTokens;   // count: 结束（包含式或半开式在构建 IR 阶段决定）
        std::vector<Token> countStepTokens;  // count: 步长（可空，默认 1）
        bool countEndInclusive = false;      // whether the count end uses '..=' (inclusive)

        bool forHeaderValid = false;         // 解析成功标志（失败时可抛编译期异常）

        // 隐藏循环内部槽（内部槽分配后保存，不对用户开放名称解析）
        struct LoopHidden {
            int listSlot  = -1;   // list 引用或拷贝的底层容器槽
            int lenSlot   = -1;   // list 长度缓存
            int idxSlot   = -1;   // list / count 当前索引
            int endSlot   = -1;   // count 结束边界
            int stepSlot  = -1;   // count 步长
            // dict 专用扩展：预先分配的 keys 数组槽与当前 key 临时槽（供引用 &key 和 &value 动态取值）
            int dictKeysSlot   = -1;
            int dictCurKeySlot = -1;
        } loopHidden;

        // ===== try-catch 元数据 =====
        // 用于 CATCH 块：记录捕获的异常变量名
        std::string catchVarName;        // catch (&e) 中的 e
        bool catchVarIsRef = false;      // 是否使用引用捕获

        // ===== with 元数据 =====
        // 用于 WITH 块：记录 with 表达式
        std::vector<Token> withExprTokens;
        // 用于 WITH 块：记录捕获变量 (with (expr) [&var] { ... })
        std::string withCaptureVarName;  // [&res] 中的 res
        bool withCaptureIsRef = false;   // 是否使用引用捕获 (&)
    };

    struct Node {
        NodeId id = InvalidNodeId;
        NodeType type;
        std::vector<Token> tokens;
    };

    using NodeAndBlock = std::variant<Node, std::unique_ptr<CodeBlock>>;

    inline bool hasExplicitReturnStatement(
        const std::vector<NodeAndBlock>& nodes
    ) {
        for (const auto& element : nodes) {
            if (std::holds_alternative<Node>(element)) {
                if (std::get<Node>(element).type == NodeType::RETURN_STMT) return true;
                continue;
            }

            const auto& nested = std::get<std::unique_ptr<CodeBlock>>(element);
            if (!nested || nested->type == CodeBlockType::FUNC ||
                nested->type == CodeBlockType::CLASS) {
                continue;
            }
            if (hasExplicitReturnStatement(nested->nodes)) return true;
        }
        return false;
    }

    std::vector<NodeAndBlock> parse(const std::vector<Token>& tokens) {
        std::vector<NodeAndBlock> result;      // 最外层 AST
        std::vector<Token> stack;              // Node token 缓存
        std::vector<CodeBlock*> blockStack;    // 追踪当前嵌套的 CodeBlock
        NodeId nextNodeId = 1;

        auto makeBlock = [&]() {
            auto block = std::make_unique<CodeBlock>();
            block->id = nextNodeId++;
            return block;
        };

        auto makeNode = [&](NodeType type, std::vector<Token> nodeTokens) {
            Node node;
            node.id = nextNodeId++;
            node.type = type;
            node.tokens = std::move(nodeTokens);
            return node;
        };

        auto leadingDecorators = [&]() {
            std::vector<std::string> decorators;
            if (stack.empty()) return decorators;
            const auto prefix = ZeusParser::parseFunctionDecoratorPrefix(stack);
            if (!prefix.valid) return decorators;
            for (const auto& decorator : prefix.decorators) {
                decorators.push_back(decorator.name);
            }
            return decorators;
        };

        auto leadingNativeSymbol = [&]() {
            const auto prefix = ZeusParser::parseFunctionDecoratorPrefix(stack);
            if (!prefix.valid) return std::string();
            for (const auto& decorator : prefix.decorators) {
                if (decorator.name == "native" && decorator.hasArgument) {
                    return decorator.argument;
                }
            }
            return std::string();
        };

        // 额外：表达式内字典字面量 '{' 深度计数，避免与代码块冲突
        int exprBraceDepth = 0;

        // 单语句 if/else 支持：等待单语句代码块的状态
        CodeBlock* pendingSingleStmtBlock = nullptr;  // 等待单语句的 CodeBlock
        std::vector<Token> pendingIfCondTokens;       // 待处理的 if 条件 tokens

        // 初始化全局块
        auto root = makeBlock();
        root->type = CodeBlockType::BLOCK;

        blockStack.push_back(root.get());
        result.push_back(std::move(root));

        NodeType ndtype = NodeType::MAIN;
        int counter = 0;

        for (size_t __i = 0; __i < tokens.size(); ++__i) {
            const auto & token = tokens[__i];
            counter++;

            if (std::holds_alternative<LexerType::Keyword>(token)) {
                auto const& kw = std::get<LexerType::Keyword>(token);
                auto const& kwStr = kw.toString();

                ZEUS_DEBUG_PRINT("[%d] Keyword: %s | Stat: %s", counter, kwStr.c_str(), Debug::toString(ndtype));

                // 单语句 else 支持：检测 else 关键字且下一个 token 不是 '{' 也不是 'if'
                if (counter == 1 && kwStr == "else") {
                    bool nextIsOpenBrace = false;
                    bool nextIsIf = false;
                    if (__i + 1 < tokens.size()) {
                        const auto& nextTok = tokens[__i + 1];
                        if (std::holds_alternative<LexerType::Operator>(nextTok)) {
                            if (std::get<LexerType::Operator>(nextTok).symbol == "{") {
                                nextIsOpenBrace = true;
                            }
                        }
                        if (std::holds_alternative<LexerType::Keyword>(nextTok)) {
                            if (std::get<LexerType::Keyword>(nextTok).name == "if") {
                                nextIsIf = true;
                            }
                        }
                    }
                    
                    // 如果是 else if，不创建单语句块，让后续的 if 处理
                    // 但是要重置 counter，让 if 被识别为语句开头
                    if (nextIsIf && !blockStack.empty() && !blockStack.back()->nodes.empty()) {
                        // else if 情况：将 else 压入 stack，然后重置 counter
                        // 这样后面的 if 会以 counter=1 开始，被正确识别为 IF_DECL_STMT
                        stack.push_back(token);
                        counter = 0;  // 重置 counter，让下一个 if 被视为语句开头
                        continue;
                    }
                    
                    if (!nextIsOpenBrace && !blockStack.empty() && !blockStack.back()->nodes.empty()) {
                        // 创建单语句 else CodeBlock（非 else if 的情况）
                        // 需要找到最内层的 IF 块（沿着 else 分支深入）
                        auto &lastVariant = blockStack.back()->nodes.back();
                        if (std::holds_alternative<std::unique_ptr<CodeBlock>>(lastVariant)) {
                            auto *ifBlock = std::get<std::unique_ptr<CodeBlock>>(lastVariant).get();
                            
                            // 沿着 else 分支找到最内层的 IF
                            while (ifBlock && ifBlock->type == CodeBlockType::IF && !ifBlock->nodes.empty()) {
                                auto &elseVariant = ifBlock->nodes.back();
                                if (std::holds_alternative<std::unique_ptr<CodeBlock>>(elseVariant)) {
                                    auto *innerBlock = std::get<std::unique_ptr<CodeBlock>>(elseVariant).get();
                                    if (innerBlock && innerBlock->isElseBranch &&
                                        innerBlock->type == CodeBlockType::IF) {
                                        ifBlock = innerBlock;
                                        continue;
                                    }
                                }
                                break;
                            }
                            
                            if (ifBlock && ifBlock->type == CodeBlockType::IF) {
                                auto elseBlock = makeBlock();
                                elseBlock->type = CodeBlockType::BLOCK;
                                elseBlock->isElseBranch = true;
                                
                                CodeBlock* elsePtr = elseBlock.get();
                                ifBlock->nodes.push_back(std::move(elseBlock));
                                blockStack.push_back(elsePtr);
                                pendingSingleStmtBlock = elsePtr;
                                
                                ZEUS_DEBUG_PRINT("[Parser   ] Single-stmt ELSE block created (attached to innermost IF)");
                                
                                stack.clear();
                                ndtype = NodeType::WAIT;
                                counter = 0;
                                continue;
                            }
                        }
                    }
                }

                if (kwStr == "func" && !leadingDecorators().empty()) {
                    const auto prefix = ZeusParser::parseFunctionDecoratorPrefix(stack);
                    size_t afterDecorators = prefix.nextToken;
                    if (afterDecorators < stack.size() &&
                        std::holds_alternative<LexerType::Keyword>(stack[afterDecorators]) &&
                        std::get<LexerType::Keyword>(stack[afterDecorators]).name == "decl") {
                        ndtype = NodeType::FUNC_FWD_DECL_STMT;
                    } else {
                        ndtype = NodeType::FUNC_DECL_STMT;
                    }
                }

                if (counter == 1) {
                    if (kwStr == "var")  ndtype = NodeType::VAR_DECL_STMT;
                    if (kwStr == "let")  ndtype = NodeType::VAR_DECL_STMT;
                    if (kwStr == "if")   ndtype = NodeType::IF_DECL_STMT;
                    if (kwStr == "for")  ndtype = NodeType::FOR_DECL_STMT;
                    if (kwStr == "func") ndtype = NodeType::FUNC_DECL_STMT;
                    if (kwStr == "class") ndtype = NodeType::CLASS_DECL_STMT;
                    if (kwStr == "try")  ndtype = NodeType::TRY_DECL_STMT;
                    if (kwStr == "throw") ndtype = NodeType::THROW_STMT;
                    if (kwStr == "with") ndtype = NodeType::WITH_DECL_STMT;
                    
                    // 支持 shared class ClassName 语法
                    if (kwStr == "shared") {
                        // 检查下一个 token 是否是 class
                        if (__i + 1 < tokens.size()) {
                            auto& nextTok = tokens[__i + 1];

                            if (std::holds_alternative<LexerType::Keyword>(nextTok)) {
                                if (std::get<LexerType::Keyword>(nextTok).name == "class") {
                                    ndtype = NodeType::CLASS_DECL_STMT;
                                }
                            }
                        }
                    }
                    
                    if (kwStr == "import") ndtype = NodeType::IMPORT_STMT;
                    if (kwStr == "using") {
                        // Check if next token is '&' - if so, keep as EXP_STMT (for class-level 'using &this')
                        // Otherwise, treat as USING_STMT (for file-level 'using original')
                        // Need to check pattern: 'using original {' (block scope) vs 'using original;' (global scope)
                        bool nextIsAmpersand = false;
                        bool nextIsOpenBrace = false;
                        if (__i + 1 < tokens.size()) {
                            auto& nextTok = tokens[__i + 1];
                            if (std::holds_alternative<LexerType::Operator>(nextTok)) {
                                if (std::get<LexerType::Operator>(nextTok).symbol == "&") {
                                    nextIsAmpersand = true;
                                } else if (std::get<LexerType::Operator>(nextTok).symbol == "{") {
                                    nextIsOpenBrace = true;
                                }
                            } else if (std::holds_alternative<LexerType::Keyword>(nextTok)) {
                                // Check for 'using original {' pattern
                                auto& kwNext = std::get<LexerType::Keyword>(nextTok);
                                if (kwNext.name == "original" && __i + 2 < tokens.size()) {
                                    auto& tokenAfter = tokens[__i + 2];
                                    if (std::holds_alternative<LexerType::Operator>(tokenAfter)) {
                                        if (std::get<LexerType::Operator>(tokenAfter).symbol == "{") {
                                            nextIsOpenBrace = true;
                                        }
                                    }
                                }
                            }
                        }
                        if (!nextIsAmpersand) {
                            // Check if it's 'using original {' (block scope) or 'using original;' (global scope)
                            if (nextIsOpenBrace) {
                                ndtype = NodeType::USING_BLOCK_STMT;
                            } else {
                            ndtype = NodeType::USING_STMT;
                        }
                        }
                        // else: keep as EXP_STMT for 'using &alias'
                    }
                    if (kwStr == "return") ndtype = NodeType::RETURN_STMT;
                    if (kwStr == "break") ndtype = NodeType::BREAK_STMT;
                    if (kwStr == "delete") ndtype = NodeType::DELETE_STMT;
                    if (kwStr == "decl") ndtype = NodeType::WAIT; // 需要看下一个 token 是否 func
                } else if (counter == 2) {
                    // 模式: decl func foo(...);
                    if (ndtype == NodeType::WAIT) {
                        // 第一个是 decl，第二个如果是 func -> 前向声明
                        // 将本语句类型暂定为 FUNC_FWD_DECL_STMT
                        if (kwStr == "func") ndtype = NodeType::FUNC_FWD_DECL_STMT;
                    }
                }

                stack.push_back(token);
            }

            if (std::holds_alternative<LexerType::Operator>(token)) {
                auto const& op = std::get<LexerType::Operator>(token);
                auto const& opStr = op.toString();

                ZEUS_DEBUG_PRINT("[%d] Symbol: %s | Stat: %s", counter, opStr.c_str(), Debug::toString(ndtype));

                /*
                    单个 Node 语句的结束
                */
                if (opStr == ";") {
                    // 在语句结束时，若 ndtype 仍是 MAIN/WAIT，尝试判定是表达式语句或赋值语句
                    NodeType finalType = ndtype;
                    if (ndtype == NodeType::MAIN || ndtype == NodeType::WAIT) {
                        bool hasEq = false;
                        int parenDepth = 0;
                        for (const auto& tkTmp : stack) {
                            if (std::holds_alternative<LexerType::Operator>(tkTmp)) {
                                const auto& sym = std::get<LexerType::Operator>(tkTmp).symbol;
                                if (sym == "(") parenDepth++;
                                else if (sym == ")") parenDepth--;
                                else if (sym == "=" && parenDepth == 0) { hasEq = true; break; }
                            }
                        }
                        if (hasEq) finalType = NodeType::VAR_ASSIGN_STMT; else finalType = NodeType::EXP_STMT;
                    }
                    // 如果第一 token 是非保留关键字（如函数名/内建名）或值字面量，也应当认定为表达式语句
                    if ((ndtype == NodeType::MAIN || ndtype == NodeType::WAIT) && !stack.empty()) {
                        const auto& firstTk = stack.front();
                        if (std::holds_alternative<LexerType::Keyword>(firstTk)) {
                            auto nm = std::get<LexerType::Keyword>(firstTk).name;
                            // 已处理 let/var/if/for/func/return 之外的关键字（例如 print, true/false 等）
                            if (nm != "let" && nm != "var" && nm != "if" && nm != "for" && nm != "func" && nm != "return") {
                                if (finalType != NodeType::VAR_ASSIGN_STMT) finalType = NodeType::EXP_STMT;
                            }
                        } else if (std::holds_alternative<LexerType::Value>(firstTk)) {
                            if (finalType != NodeType::VAR_ASSIGN_STMT) finalType = NodeType::EXP_STMT;
                        } else if (std::holds_alternative<LexerType::Operator>(firstTk)) {
                            const auto& sym = std::get<LexerType::Operator>(firstTk).symbol;
                            if (sym == "(" && finalType != NodeType::VAR_ASSIGN_STMT) finalType = NodeType::EXP_STMT;
                        }
                    }

                    // 若当前处于 CLASS 代码块内，并且该语句形如 `using &alias;`，则记录到类的默认实例别名中
                    if (!blockStack.empty() && blockStack.back()->type == CodeBlockType::CLASS && finalType == NodeType::EXP_STMT) {
                        if (stack.size() >= 3
                            && std::holds_alternative<LexerType::Keyword>(stack[0])
                            && std::get<LexerType::Keyword>(stack[0]).name == "using"
                            && std::holds_alternative<LexerType::Operator>(stack[1])
                            && std::get<LexerType::Operator>(stack[1]).symbol == "&") {
                            // 第三个 token 若为关键字则作为别名名
                            if (std::holds_alternative<LexerType::Keyword>(stack[2])) {
                                std::string aliasName = std::get<LexerType::Keyword>(stack[2]).name;
                                blockStack.back()->classDefaultAlias = aliasName;
                                ZEUS_DEBUG_PRINT("[Parser   ] CLASS '%s' captured using &%s", blockStack.back()->className.c_str(), aliasName.c_str());
                            }
                        }
                    }

                    // 单语句 if/else 支持：如果有 pending 单语句块，将当前语句添加到该块中
                    if (pendingSingleStmtBlock != nullptr) {
                        pendingSingleStmtBlock->nodes.push_back(makeNode(finalType, stack));
                        ZEUS_DEBUG_PRINT("[Parser   ] Single-stmt block completed, added stmt");
                        
                        // 结束单语句块
                        if (!blockStack.empty() && blockStack.back() == pendingSingleStmtBlock) {
                            blockStack.pop_back();
                        }
                        pendingSingleStmtBlock = nullptr;
                        
                        stack.clear();
                        counter = 0;
                        ndtype = NodeType::WAIT;
                        ZEUS_DEBUG_PRINT("[%d] End of Single Statement", counter);
                        continue;
                    }

                    blockStack.back()->nodes.push_back(makeNode(finalType, stack));
                    stack.clear();

                    counter = 0;
                    ndtype = NodeType::WAIT;

                    ZEUS_DEBUG_PRINT("[%d] End of Statement", counter);
                    continue;
                }

                if (opStr == "=") {
                    // 检查是否在括号内 - 如果在括号内,这可能是命名参数,不应该设置为赋值语句
                    int parenDepth = 0;
                    for (const auto& tk : stack) {
                        if (std::holds_alternative<LexerType::Operator>(tk)) {
                            const std::string& sym = std::get<LexerType::Operator>(tk).symbol;
                            if (sym == "(") parenDepth++;
                            else if (sym == ")") parenDepth--;
                        }
                    }
                    
                    // 只有在不在括号内时,才将 = 识别为赋值语句
                    if (ndtype == NodeType::WAIT && parenDepth == 0) {
                        ndtype = NodeType::VAR_ASSIGN_STMT;
                    }
                }
                
                /*
                    单语句 if 支持：检测 if (...) 后是否跟 '{'
                */
                if (ndtype == NodeType::IF_DECL_STMT && opStr == ")") {
                    // 检查是否在 if 条件的最外层括号
                    int parenDepth = 0;
                    for (const auto& tk : stack) {
                        if (std::holds_alternative<LexerType::Operator>(tk)) {
                            const std::string& sym = std::get<LexerType::Operator>(tk).symbol;
                            if (sym == "(") parenDepth++;
                            else if (sym == ")") parenDepth--;
                        }
                    }
                    
                    // 如果这是 if (...) 的最外层 ')'，检查下一个 token 是否是 '{'
                    if (parenDepth == 1) {
                        stack.push_back(token);
                        counter++;
                        
                        // 向前看下一个 token
                        bool nextIsOpenBrace = false;
                        if (__i + 1 < tokens.size()) {
                            const auto& nextTok = tokens[__i + 1];
                            if (std::holds_alternative<LexerType::Operator>(nextTok)) {
                                if (std::get<LexerType::Operator>(nextTok).symbol == "{") {
                                    nextIsOpenBrace = true;
                                }
                            }
                        }
                        
                        if (!nextIsOpenBrace) {
                            // 检查是否是 else if（stack 中有 else 关键字）
                            bool isElseIf = false;
                            for (const auto &tk : stack) {
                                if (std::holds_alternative<LexerType::Keyword>(tk) && 
                                    std::get<LexerType::Keyword>(tk).name == "else") {
                                    isElseIf = true;
                                    break;
                                }
                            }
                            
                            // 创建单语句 IF CodeBlock
                            auto newBlock = makeBlock();
                            newBlock->type = CodeBlockType::IF;
                            newBlock->isElseBranch = isElseIf;
                            
                            // 提取 if 条件表达式
                            bool seenIf = false;
                            int depth = 0;
                            bool collecting = false;
                            for (size_t i = 0; i < stack.size(); ++i) {
                                const auto &tk = stack[i];
                                if (!seenIf) {
                                    if (std::holds_alternative<LexerType::Keyword>(tk) && std::get<LexerType::Keyword>(tk).name == "if") {
                                        seenIf = true;
                                    }
                                    continue;
                                }
                                if (std::holds_alternative<LexerType::Operator>(tk)) {
                                    const auto &sym = std::get<LexerType::Operator>(tk).symbol;
                                    if (!collecting) {
                                        if (sym == "(") { collecting = true; depth = 1; }
                                        continue;
                                    }
                                    if (sym == "(") {
                                        depth++;
                                        if (depth > 1) newBlock->conditionTokens.push_back(tk);
                                        continue;
                                    }
                                    if (sym == ")") {
                                        depth--;
                                        if (depth == 0) break;
                                        newBlock->conditionTokens.push_back(tk);
                                        continue;
                                    }
                                }
                                if (collecting && depth > 0) {
                                    newBlock->conditionTokens.push_back(tk);
                                }
                            }
                            
                            CodeBlock* ptr = newBlock.get();
                            
                            // 如果是 else if，将此 IF 块附加到最内层 IF 的 else 分支
                            if (isElseIf && !blockStack.back()->nodes.empty()) {
                                auto &lastVariant = blockStack.back()->nodes.back();
                                if (std::holds_alternative<std::unique_ptr<CodeBlock>>(lastVariant)) {
                                    auto *prevIfBlock = std::get<std::unique_ptr<CodeBlock>>(lastVariant).get();
                                    
                                    // 沿着 else 分支找到最内层的 IF（没有 else 的那个）
                                    while (prevIfBlock && prevIfBlock->type == CodeBlockType::IF && !prevIfBlock->nodes.empty()) {
                                        auto &elseVariant = prevIfBlock->nodes.back();
                                        if (std::holds_alternative<std::unique_ptr<CodeBlock>>(elseVariant)) {
                                            auto *innerBlock = std::get<std::unique_ptr<CodeBlock>>(elseVariant).get();
                                            if (innerBlock && innerBlock->isElseBranch &&
                                                innerBlock->type == CodeBlockType::IF) {
                                                prevIfBlock = innerBlock;
                                                continue;
                                            }
                                        }
                                        break;
                                    }
                                    
                                    if (prevIfBlock && prevIfBlock->type == CodeBlockType::IF) {
                                        prevIfBlock->nodes.push_back(std::move(newBlock));
                                        auto &elseVariant = prevIfBlock->nodes.back();
                                        ptr = std::get<std::unique_ptr<CodeBlock>>(elseVariant).get();
                                        ZEUS_DEBUG_PRINT("[Parser   ] Single-stmt ELSE IF block attached to innermost IF, condTokens=%zu", ptr->conditionTokens.size());
                                    }
                                }
                            } else {
                                blockStack.back()->nodes.push_back(std::move(newBlock));
                                ZEUS_DEBUG_PRINT("[Parser   ] Single-stmt IF block created, condTokens=%zu", ptr->conditionTokens.size());
                            }
                            
                            blockStack.push_back(ptr);
                            pendingSingleStmtBlock = ptr;
                            
                            stack.clear();
                            ndtype = NodeType::WAIT;
                            counter = 0;
                            continue;
                        }
                        
                        // 否则，继续正常处理（等待 '{'）
                        continue;
                    }
                }

                /*
                    一般代码块的起始
                */
                if (opStr == "{") {
                    // 若当前不是在 dict 字面量内部，并且此 '{' 应判定为字典字面量起始，则不创建代码块。
                    auto canBeExprDictContext = (ndtype == NodeType::VAR_DECL_STMT || ndtype == NodeType::VAR_ASSIGN_STMT || ndtype == NodeType::EXP_STMT || ndtype == NodeType::WAIT || ndtype == NodeType::MAIN);
                    bool treatAsDictLiteral = false;
                    if (exprBraceDepth == 0 && canBeExprDictContext) {
                        // 判断 stack 中是否有 '=' （表示正在处理赋值/初始化右侧）或 当前 ndtype 为 EXP_STMT 仍未结束。
                        // 或者判断 stack 中是否有未匹配的 '(' （表示在函数参数内）
                        bool hasEqInStack = false;
                        int parenBalance = 0;
                        for (auto const &tkTmp : stack) {
                            if (std::holds_alternative<LexerType::Operator>(tkTmp)) {
                                const std::string& sym = std::get<LexerType::Operator>(tkTmp).symbol;
                                if (sym == "=") {
                                    hasEqInStack = true;
                                } else if (sym == "(") {
                                    parenBalance++;
                                } else if (sym == ")") {
                                    parenBalance--;
                                }
                            }
                        }
                        bool inParentheses = (parenBalance > 0);
                        if (hasEqInStack || inParentheses || ndtype == NodeType::EXP_STMT || ndtype == NodeType::VAR_ASSIGN_STMT || ndtype == NodeType::VAR_DECL_STMT) {
                            // 向前看匹配此 '{' 的 '}' 期间是否出现 ':'
                            int depthLook = 1;
                            bool colonAtTop = false;
                            for (size_t j = __i + 1; j < tokens.size(); ++j) {
                                const auto &lk = tokens[j];
                                if (std::holds_alternative<LexerType::Operator>(lk)) {
                                    std::string s2 = std::get<LexerType::Operator>(lk).symbol;
                                    if (s2 == "{") depthLook++;
                                    else if (s2 == "}") {
                                        depthLook--;
                                        if (depthLook == 0) break;
                                    } else if (s2 == ":" && depthLook == 1) {
                                        colonAtTop = true;
                                        break;
                                    }
                                }
                            }
                            if (colonAtTop) {
                                treatAsDictLiteral = true;
                            }
                        }
                    }
                    if (treatAsDictLiteral) {
                        exprBraceDepth++;
                        stack.push_back(token);
                        continue; // 不进入代码块流程
                    }
                    // 新增：识别空字典字面量 {} （无 ':'）
                    if (exprBraceDepth == 0 && canBeExprDictContext) {
                        // 向前看下一个非空白 token 是否是 '}'
                        if (__i + 1 < tokens.size()) {
                            const auto &nextTk = tokens[__i + 1];
                            if (std::holds_alternative<LexerType::Operator>(nextTk) && std::get<LexerType::Operator>(nextTk).symbol == "}") {
                                // 视为 {} 字典字面量
                                exprBraceDepth++;
                                stack.push_back(token); // '{'
                                // 直接也把 '}' 吃进当前语句 token，然后跳过主循环对 '}' 的处理
                                stack.push_back(nextTk);
                                ++__i; // 跳过 '}'
                                exprBraceDepth--; // 立即闭合
                                counter += 1; // 计入一个 token
                                // 不创建代码块，继续
                                continue;
                            }
                        }
                    }
                    CodeBlockType cdbType;

                    // 判断代码块类型
                    switch (ndtype) {
                        case NodeType::IF_DECL_STMT:   cdbType = CodeBlockType::IF;   break;
                        case NodeType::FOR_DECL_STMT:  cdbType = CodeBlockType::FOR;  break;
                        case NodeType::FUNC_DECL_STMT: cdbType = CodeBlockType::FUNC; break;
                        case NodeType::CLASS_DECL_STMT: cdbType = CodeBlockType::CLASS; break;
                        case NodeType::TRY_DECL_STMT:  cdbType = CodeBlockType::TRY;  break;
                        case NodeType::WITH_DECL_STMT: cdbType = CodeBlockType::WITH; break;
                        case NodeType::USING_BLOCK_STMT: cdbType = CodeBlockType::USING; break;
                        default: cdbType = CodeBlockType::BLOCK; break;
                    }

                    auto newBlock = makeBlock();
                    newBlock->type = cdbType;

                    if (cdbType == CodeBlockType::FUNC) {
                        newBlock->decorators = leadingDecorators();
                        newBlock->nativeSymbol = leadingNativeSymbol();
                        newBlock->isCompileTimeStatic = std::find(
                            newBlock->decorators.begin(),
                            newBlock->decorators.end(),
                            "static"
                        ) != newBlock->decorators.end();
                    }

                    // for 代码块头部解析（自定义语法）
                    if (cdbType == CodeBlockType::FOR) {
                        // 新（还原）语法: for ( ExprOrRange ) [ bindings ] { ... }
                        // 四形态：
                        //   1. 条件: for (condExpr) { ... }
                        //   2. 列表: for (listExpr) [ &? name ] { ... }
                        //   3. 字典: for (dictExpr) [ &? k : &? v ] { ... }
                        //   4. 计数: for (start .. end ( .. step )) [ &? i ] { ... }
                        // 说明: 原先实现的 [binding] : expr 被移除。

                        auto isOp = [](const Token& tk, const std::string& sym) -> bool {
                            return std::holds_alternative<LexerType::Operator>(tk) && std::get<LexerType::Operator>(tk).symbol == sym;
                        };

                        // 1. 收集 for 之后到 '{' 之前的全部头部 tokens
                        std::vector<Token> headerTokens;
                        bool seenFor = false;
                        for (size_t i = 0; i < stack.size(); ++i) {
                            const auto &tk = stack[i];
                            if (!seenFor) {
                                if (std::holds_alternative<LexerType::Keyword>(tk) && std::get<LexerType::Keyword>(tk).name == "for") {
                                    seenFor = true;
                                }
                                continue;
                            }
                            headerTokens.push_back(tk);
                        }

                        // 防御性清理分号
                        while (!headerTokens.empty() && isOp(headerTokens.back(), ";")) headerTokens.pop_back();

                        // 2. 提取括号内表达式/范围 tokens
                        int parenOpen = -1;
                        int parenClose = -1;
                        int depth = 0;
                        for (size_t i = 0; i < headerTokens.size(); ++i) {
                            if (isOp(headerTokens[i], "(")) {
                                if (depth == 0) parenOpen = (int)i;
                                depth++;
                            } else if (isOp(headerTokens[i], ")")) {
                                depth--;
                                if (depth == 0) { parenClose = (int)i; break; }
                            }
                        }
                        if (parenOpen >= 0 && parenClose > parenOpen) {
                            std::vector<Token> parenTokens;
                            for (int i = parenOpen + 1; i < parenClose; ++i) parenTokens.push_back(headerTokens[i]);

                            // 3. 查找可选的方括号绑定部分
                            int bracketOpen = -1;
                            int bracketClose = -1;
                            depth = 0;
                            for (size_t i = parenClose + 1; i < headerTokens.size(); ++i) {
                                if (isOp(headerTokens[i], "[")) { bracketOpen = (int)i; depth = 1; continue; }
                                if (bracketOpen >= 0) {
                                    if (isOp(headerTokens[i], "[")) depth++;
                                    else if (isOp(headerTokens[i], "]")) {
                                        depth--;
                                        if (depth == 0) { bracketClose = (int)i; break; }
                                    }
                                }
                            }

                            std::vector<Token> bindingTokens;
                            if (bracketOpen >= 0 && bracketClose > bracketOpen) {
                                for (int i = bracketOpen + 1; i < bracketClose; ++i) bindingTokens.push_back(headerTokens[i]);
                            }

                            auto hasDoubleDots = [&](const std::vector<Token>& vec) -> bool {
                                for (size_t i = 1; i < vec.size(); ++i) {
                                    if (isOp(vec[i-1], ".") && isOp(vec[i], ".")) return true;
                                }
                                return false;
                            };

                            bool isRange = hasDoubleDots(parenTokens);
                            bool hasBindingZone = bracketOpen >= 0;

                            // 4. 根据模式判定 forKind
                            if (!hasBindingZone) {
                                // 没有绑定区：三种可能
                                //  1) 含 .. 范围运算符 => 视为 COUNT 无用户索引绑定 (start..end)
                                //  2) 纯数字表达式 => 视为 COUNT_SIMPLE (0 .. end) 无用户索引绑定
                                //  3) 其他 => 条件循环 COND
                                auto isNumericExprNoBind = [&](const std::vector<Token>& vec) -> bool {
                                    if (vec.empty()) return false;
                                    // 复用后面分支里的快速数值判定逻辑：只允许 Value(int/float) 与 + - * / ( ) . 数字组合
                                    int parenDepth = 0;
                                    for (size_t i = 0; i < vec.size(); ++i) {
                                        if (std::holds_alternative<LexerType::Value>(vec[i])) continue;
                                        if (std::holds_alternative<LexerType::Operator>(vec[i])) {
                                            std::string sym = std::get<LexerType::Operator>(vec[i]).symbol;
                                            if (sym == "+" || sym == "-" || sym == "*" || sym == "/" || sym == "%" || sym == ".") continue;
                                            if (sym == "(" ) { parenDepth++; continue; }
                                            if (sym == ")" ) { if (parenDepth==0) return false; parenDepth--; continue; }
                                            return false; // 其他运算符不允许
                                        }
                                        if (std::holds_alternative<LexerType::Keyword>(vec[i])) return false;
                                    }
                                    return parenDepth==0;
                                };

                                if (isRange) {
                                    // 无绑定变量的范围循环: for(0..2) => COUNT 形态
                                    // 解析 .. 分段
                                    std::vector<int> dotIdx;
                                    for (size_t i = 0; i < parenTokens.size(); ++i) if (isOp(parenTokens[i], ".")) dotIdx.push_back((int)i);
                                    std::vector<int> pairs;
                                    for (size_t i = 1; i < dotIdx.size(); ++i) if (dotIdx[i] == dotIdx[i-1] + 1) pairs.push_back(dotIdx[i-1]);

                                    if (!pairs.empty()) {
                                        int firstPair = pairs[0];
                                        int secondPair = -1;
                                        if (pairs.size() >= 2) secondPair = pairs[1];

                                        // start: 0..firstPair-1
                                        for (int i = 0; i < firstPair; ++i) newBlock->countStartTokens.push_back(parenTokens[i]);

                                        // end: firstPair+2 到 secondPair-1 或 末尾
                                        int endStop = (secondPair >= 0) ? secondPair : (int)parenTokens.size();
                                        for (int i = firstPair + 2; i < endStop; ++i) newBlock->countEndTokens.push_back(parenTokens[i]);

                                        if (secondPair >= 0) {
                                            // 检测 '..=' 包含式标记
                                            if (secondPair + 2 < (int)parenTokens.size()) {
                                                if (isOp(parenTokens[secondPair + 2], "=")) {
                                                    newBlock->countEndInclusive = true;
                                                    for (int i = secondPair + 3; i < (int)parenTokens.size(); ++i) newBlock->countStepTokens.push_back(parenTokens[i]);
                                                } else {
                                                    for (int i = secondPair + 2; i < (int)parenTokens.size(); ++i) newBlock->countStepTokens.push_back(parenTokens[i]);
                                                }
                                            } else {
                                                for (int i = secondPair + 2; i < (int)parenTokens.size(); ++i) newBlock->countStepTokens.push_back(parenTokens[i]);
                                            }
                                        }
                                    }

                                    newBlock->forKind = CodeBlock::ForKind::COUNT;
                                    newBlock->forHeaderValid = !newBlock->countStartTokens.empty() && !newBlock->countEndTokens.empty();
                                } else if (isNumericExprNoBind(parenTokens)) {
                                    newBlock->forKind = CodeBlock::ForKind::COUNT_SIMPLE;
                                    newBlock->countEndTokens = parenTokens; // start 隐式 0
                                    newBlock->forHeaderValid = !newBlock->countEndTokens.empty();
                                } else {
                                    newBlock->forKind = CodeBlock::ForKind::COND;
                                    newBlock->condExprTokens = parenTokens;
                                    newBlock->forHeaderValid = !newBlock->condExprTokens.empty();
                                }
                            } else {
                                // 解析 bindingTokens
                                struct TmpBind { std::string name; bool isRef=false; bool isConst=false; };
                                std::vector<TmpBind> tmpBinds;
                                // 支持的形式：
                                //   name
                                //   & name
                                //   const name
                                //   const & name
                                bool pendingConst = false;
                                bool pendingRef = false;
                                for (size_t i = 0; i < bindingTokens.size(); ++i) {
                                    if (std::holds_alternative<LexerType::Keyword>(bindingTokens[i])) {
                                        std::string nm = std::get<LexerType::Keyword>(bindingTokens[i]).name;
                                        if (nm == "const") {
                                            pendingConst = true;
                                            continue;
                                        }
                                        // 这里视为标识符
                                        bool isConst = pendingConst;
                                        bool isRef = pendingRef;
                                        tmpBinds.push_back(TmpBind{nm, isRef, isConst});
                                        // 重置前缀标志，下一项独立判断
                                        pendingConst = false;
                                        pendingRef = false;
                                        continue;
                                    }
                                    if (std::holds_alternative<LexerType::Operator>(bindingTokens[i])) {
                                        const auto &op = std::get<LexerType::Operator>(bindingTokens[i]);
                                        if (op.symbol == "&") {
                                            pendingRef = true;
                                            continue;
                                        }
                                        // 其它符号忽略
                                    }
                                }

                                // 判断是否 dict 绑定: 出现 ':'
                                bool hasColon = false;
                                for (auto &tk : bindingTokens) if (isOp(tk, ":")) { hasColon = true; break; }

                                auto isNumericExpr = [&](const std::vector<Token>& vec) -> bool {
                                    if (vec.empty()) {
                                        return false;
                                    }

                                    for (const auto &tk : vec) {
                                        if (std::holds_alternative<LexerType::Value>(tk)) {
                                            continue;
                                        }
                                        if (std::holds_alternative<LexerType::Operator>(tk)) {
                                            const auto &sym = std::get<LexerType::Operator>(tk).symbol;
                                            if (sym == "+" || sym == "-" || sym == "*" || sym == "/" || sym == "%" || sym == "(" || sym == ")") {
                                                continue;
                                            }
                                            return false;
                                        }
                                        if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                            return false; // 关键字视为非纯数值表达式
                                        }
                                        return false;
                                    }
                                    return true;
                                };

                                if (isRange) {
                                    // count 形态: parenTokens 内包含 start .. end ( .. step )
                                    // 解析 .. 分段
                                    std::vector<int> dotIdx;
                                    for (size_t i = 0; i < parenTokens.size(); ++i) if (isOp(parenTokens[i], ".")) dotIdx.push_back((int)i);
                                    std::vector<int> pairs; // '..' 首位下标集合（两元素表示一对）
                                    for (size_t i = 1; i < dotIdx.size(); ++i) if (dotIdx[i] == dotIdx[i-1] + 1) pairs.push_back(dotIdx[i-1]);
                                    if (!pairs.empty()) {
                                        int firstPair = pairs[0];
                                        int secondPair = -1;
                                        if (pairs.size() >= 2) secondPair = pairs[1];
                                        // start: 0..firstPair-1
                                        for (int i = 0; i < firstPair; ++i) newBlock->countStartTokens.push_back(parenTokens[i]);
                                        // end: firstPair+2 到 secondPair-1 或 末尾
                                        int endStop = (secondPair >= 0) ? secondPair : (int)parenTokens.size();
                                        for (int i = firstPair + 2; i < endStop; ++i) newBlock->countEndTokens.push_back(parenTokens[i]);
                                            if (secondPair >= 0) {
                                                // detect '..=' inclusive marker: sequence '.' '.' '=' starts at firstPair and then '=' at secondPair+2
                                                if (secondPair + 2 < (int)parenTokens.size()) {
                                                    if (isOp(parenTokens[secondPair + 2], "=")) {
                                                        // treat as '..=' marker: mark end inclusive and consume '=' as part of range marker
                                                        newBlock->countEndInclusive = true;
                                                        // step tokens start after the '='
                                                        for (int i = secondPair + 3; i < (int)parenTokens.size(); ++i) newBlock->countStepTokens.push_back(parenTokens[i]);
                                                    } else {
                                                        for (int i = secondPair + 2; i < (int)parenTokens.size(); ++i) newBlock->countStepTokens.push_back(parenTokens[i]);
                                                    }
                                                } else {
                                                    for (int i = secondPair + 2; i < (int)parenTokens.size(); ++i) newBlock->countStepTokens.push_back(parenTokens[i]);
                                                }
                                            }
                                    }
                                    if (!tmpBinds.empty()) {
                                        CodeBlock::ForBinding b;
                                        b.name = tmpBinds[0].name;
                                        b.isRef = tmpBinds[0].isRef;
                                        b.isConst = tmpBinds[0].isConst;
                                        newBlock->forBindings.push_back(b);
                                    }
                                    newBlock->forKind = CodeBlock::ForKind::COUNT;
                                    newBlock->forHeaderValid = !newBlock->countStartTokens.empty() && !newBlock->countEndTokens.empty() && !newBlock->forBindings.empty();
                                } else if (hasColon) {
                                    // dict 形态
                                    if (tmpBinds.size() >= 2) {
                                        CodeBlock::ForBinding kb;
                                        kb.name = tmpBinds[0].name;
                                        kb.isRef = tmpBinds[0].isRef;
                                        kb.isConst = tmpBinds[0].isConst;
                                        kb.isKey = true;
                                        CodeBlock::ForBinding vb;
                                        vb.name = tmpBinds[1].name;
                                        vb.isRef = tmpBinds[1].isRef;
                                        vb.isConst = tmpBinds[1].isConst;
                                        vb.isValue = true;
                                        newBlock->forBindings.push_back(kb);
                                        newBlock->forBindings.push_back(vb);
                                    }
                                    newBlock->dictExprTokens = parenTokens;
                                    newBlock->forKind = CodeBlock::ForKind::DICT;
                                    newBlock->forHeaderValid = !newBlock->dictExprTokens.empty() && newBlock->forBindings.size() == 2;
                                } else {
                                    // 可能是 COUNT_SIMPLE 或 LIST
                                    bool numericOnly = isNumericExpr(parenTokens);
                                    bool singleBind = tmpBinds.size() == 1;
                                    if (numericOnly && singleBind) {
                                        CodeBlock::ForBinding b;
                                        b.name = tmpBinds[0].name;
                                        b.isRef = tmpBinds[0].isRef;
                                        b.isConst = tmpBinds[0].isConst;
                                        newBlock->forBindings.push_back(b);
                                        // COUNT_SIMPLE: end = parenExpr, start 隐式 0
                                        newBlock->countEndTokens = parenTokens;
                                        newBlock->forKind = CodeBlock::ForKind::COUNT_SIMPLE;
                                        newBlock->forHeaderValid = !newBlock->countEndTokens.empty();
                                    } else {
                                        if (!tmpBinds.empty()) {
                                            CodeBlock::ForBinding b;
                                            b.name = tmpBinds[0].name;
                                            b.isRef = tmpBinds[0].isRef;
                                            b.isConst = tmpBinds[0].isConst;
                                            newBlock->forBindings.push_back(b);
                                        }
                                        newBlock->listExprTokens = parenTokens;
                                        newBlock->forKind = CodeBlock::ForKind::LIST;
                                        newBlock->forHeaderValid = !newBlock->listExprTokens.empty() && !newBlock->forBindings.empty();
                                    }
                                }
                            }
                        }
                    }

                    // 如果是 IF 代码块，提取条件表达式 tokens（在最外层 '(' 与 对应 ')' 之间）
                    // 之前版本会跳过所有 '(' 但仍保留内层 ')', 造成形如 isOdd i ) 的不平衡 token 序列，导致 condIR 为空。
                    // 修复策略：保留除最外层包裹括号外的所有括号；即 depth>1 的 '(' / ')' 也加入 conditionTokens。
                    if (cdbType == CodeBlockType::IF) {
                        bool seenIf = false;
                        int depth = 0;         // 括号深度
                        bool collecting = false; // 已进入最外层括号内容
                        for (size_t i = 0; i < stack.size(); ++i) {
                            const auto &tk = stack[i];
                            if (!seenIf) {
                                if (std::holds_alternative<LexerType::Keyword>(tk) && std::get<LexerType::Keyword>(tk).name == "if") {
                                    seenIf = true;
                                }
                                continue;
                            }
                            if (std::holds_alternative<LexerType::Operator>(tk)) {
                                const auto &sym = std::get<LexerType::Operator>(tk).symbol;
                                if (!collecting) {
                                    if (sym == "(") { collecting = true; depth = 1; }
                                    continue; // 进入最外层后再继续处理下一 token
                                }
                                // 已在最外层括号内部
                                if (sym == "(") {
                                    depth++;
                                    // depth>1 表示内层括号，需要保留
                                    if (depth > 1) newBlock->conditionTokens.push_back(tk);
                                    continue;
                                }
                                if (sym == ")") {
                                    depth--;
                                    if (depth == 0) {
                                        // 结束最外层
                                        break;
                                    }
                                    // depth>=1 表示是内层 ')'
                                    newBlock->conditionTokens.push_back(tk);
                                    continue;
                                }
                            }
                            if (collecting && depth > 0) {
                                newBlock->conditionTokens.push_back(tk);
                            }
                        }
                    }

                    // ===== WITH 代码块表达式解析 =====
                    // 语法: with (expr) { ... } 或 with (expr) [&var] { ... }
                    // 提取括号内的表达式 tokens 到 withExprTokens
                    if (cdbType == CodeBlockType::WITH) {
                        bool seenWith = false;
                        int depth = 0;
                        bool collecting = false;
                        size_t exprEndIndex = 0; // 记录表达式结束位置，用于解析捕获列表
                        for (size_t i = 0; i < stack.size(); ++i) {
                            const auto &tk = stack[i];
                            if (!seenWith) {
                                if (std::holds_alternative<LexerType::Keyword>(tk) && std::get<LexerType::Keyword>(tk).name == "with") {
                                    seenWith = true;
                                }
                                continue;
                            }
                            if (std::holds_alternative<LexerType::Operator>(tk)) {
                                const auto &sym = std::get<LexerType::Operator>(tk).symbol;
                                if (!collecting) {
                                    if (sym == "(") { collecting = true; depth = 1; }
                                    continue;
                                }
                                if (sym == "(") {
                                    depth++;
                                    if (depth > 1) newBlock->withExprTokens.push_back(tk);
                                    continue;
                                }
                                if (sym == ")") {
                                    depth--;
                                    if (depth == 0) { exprEndIndex = i + 1; break; }
                                    newBlock->withExprTokens.push_back(tk);
                                    continue;
                                }
                            }
                            if (collecting && depth > 0) {
                                newBlock->withExprTokens.push_back(tk);
                            }
                        }
                        
                        // ===== WITH 捕获列表解析 =====
                        // 语法: with (expr) [&var] { ... } 或 with (expr) [var] { ... }
                        // 在表达式结束后查找 [ ... ]
                        if (exprEndIndex > 0 && exprEndIndex < stack.size()) {
                            bool seenOpenBracket = false;
                            bool isRef = false;
                            std::string captureVarName;
                            
                            for (size_t i = exprEndIndex; i < stack.size(); ++i) {
                                const auto &tk = stack[i];
                                if (std::holds_alternative<LexerType::Operator>(tk)) {
                                    const auto &sym = std::get<LexerType::Operator>(tk).symbol;
                                    if (!seenOpenBracket && sym == "[") {
                                        seenOpenBracket = true;
                                        continue;
                                    }
                                    if (seenOpenBracket && sym == "&") {
                                        isRef = true;
                                        continue;
                                    }
                                    if (seenOpenBracket && sym == "]") {
                                        break;
                                    }
                                }
                                if (seenOpenBracket && std::holds_alternative<LexerType::Keyword>(tk)) {
                                    const auto &kw = std::get<LexerType::Keyword>(tk);
                                    if (captureVarName.empty()) {
                                        captureVarName = kw.name;
                                    }
                                }
                            }
                            
                            if (!captureVarName.empty()) {
                                newBlock->withCaptureVarName = captureVarName;
                                newBlock->withCaptureIsRef = isRef;
                                ZEUS_DEBUG_PRINT("[Parser   ] WITH block: captureVar='%s', isRef=%d", captureVarName.c_str(), isRef);
                            }
                        }
                        // ===== end WITH 捕获列表解析 =====
                        
                        ZEUS_DEBUG_PRINT("[Parser   ] WITH block: exprTokens.size=%zu", newBlock->withExprTokens.size());
                    }
                    // ===== end WITH 代码块表达式解析 =====

                    // 如果是函数代码块，尝试从 stack 中提取函数名
                    if (cdbType == CodeBlockType::FUNC) {
                        bool seenFunc = false;
                        bool seenOperator = false; // 标记是否看到 operator 关键字
                        bool operatorListClosed = false; // operator符号收集是否已结束
                        bool inParams = false;
                        bool afterParams = false; // 标记已经结束参数部分，后续可能出现捕获列表
                        bool nextIsVariadicArgs = false; // 下一个参数是 *args
                        bool nextIsVariadicKwargs = false; // 下一个参数是 **kwargs
                        bool collectingDefault = false; // 正在收集默认值表达式
                        bool expectingParamType = false; // 上一个 token 是参数类型分隔符 ':'
                        std::vector<Token> currentDefault; // 当前收集的默认值tokens

                        // 先一次遍历：提取函数名与参数
                        // 说明：当前词法把标识符也当 Keyword

                        for (size_t i = 0; i < stack.size(); ++i) {
                            const auto& tk = stack[i];
                            if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                const auto& kw = std::get<LexerType::Keyword>(tk);
                                
                                if (!seenFunc) {
                                    if (kw.name == "func") { seenFunc = true; continue; }

                                } else {
                                    // 检查是否为 operator 关键字
                                    if (!seenOperator && kw.name == "operator") {
                                        seenOperator = true;
                                        newBlock->isOperatorOverload = true;
                                        newBlock->funcName = "operator"; // 临时函数名
                                        continue;
                                    }
                                    
                                    // allow 'static' modifier either before or after func name: record it
                                    if (kw.name == "static") {
                                        newBlock->isStaticMethod = true;
                                        continue;
                                    }
                                    
                                    // 处理 operator 后方括号内的关键字（如 operator[with], operator[~with]）
                                    // 在 seenOperator 且尚未进入参数列表时，将关键字也作为操作符名称的一部分
                                    if (seenOperator && !inParams && !operatorListClosed) {
                                        newBlock->overloadedOps.push_back(kw.name);
                                        ZEUS_DEBUG_PRINT("[TreeParser] collected operator keyword: '%s'", kw.name.c_str());
                                        continue;
                                    }
                                    
                                
                                }
                                // If prev token was func and 'static' appears later it will be seen in subsequent iterations
                                if (seenFunc && !newBlock->funcName.empty() && kw.name == "static") {
                                    newBlock->isStaticMethod = true;
                                    continue;
                                }
                                
                                // fallthrough for normal name/param handling
                                if (!seenFunc) {
                                    // handled above
                                } else if (!seenOperator) {
                                    // 第一個紧随其后的关键字视为函数名
                                    if (!inParams && newBlock->funcName.empty()) {
                                        newBlock->funcName = kw.name;

                                    } else if (inParams) {
                                        // 形参（当前词法把标识符也当 Keyword）
                                        // 如果正在收集默认值,继续收集
                                        if (collectingDefault) {
                                            currentDefault.push_back(tk);
                                            continue;
                                        }

                                        if (expectingParamType) {
                                            expectingParamType = false;
                                            continue;
                                        }
                                        
                                        // 检查是否为 *args 或 **kwargs
                                        if (nextIsVariadicKwargs) {
                                            newBlock->variadicKwargs = kw.name;
                                            nextIsVariadicKwargs = false;
                                        } else if (nextIsVariadicArgs) {
                                            newBlock->variadicArgs = kw.name;
                                            nextIsVariadicArgs = false;
                                        } else {
                                            newBlock->paramNames.push_back(kw.name);
                                            // 先添加空的默认值,如果后面有 = 会被替换
                                            newBlock->paramDefaults.push_back(std::vector<Token>());
                                        }
                                    }
                                } else if (inParams) {
                                    // operator函数的参数
                                    if (nextIsVariadicKwargs) {
                                        newBlock->variadicKwargs = kw.name;
                                        nextIsVariadicKwargs = false;
                                    } else if (nextIsVariadicArgs) {
                                        newBlock->variadicArgs = kw.name;
                                        nextIsVariadicArgs = false;
                                    } else if (expectingParamType) {
                                        expectingParamType = false;
                                        continue;
                                    } else {
                                        newBlock->paramNames.push_back(kw.name);
                                        newBlock->paramDefaults.push_back(std::vector<Token>()); // 默认无默认值
                                    }
                                }

                            } else if (std::holds_alternative<LexerType::Operator>(tk)) {
                                const auto& op = std::get<LexerType::Operator>(tk);

                                // 处理 operator 后的运算符符号 - 收集所有运算符直到遇到 (
                                if (seenOperator && !inParams && !operatorListClosed) {
                                    if (op.symbol == "(") {
                                        // 遇到参数列表,停止收集运算符
                                        operatorListClosed = true;
                                        inParams = true;
                                        ZEUS_DEBUG_PRINT("[TreeParser] operator symbols collected, entering params. Total symbols: %zu", newBlock->overloadedOps.size());
                                    } else {
                                        // 收集运算符符号
                                        newBlock->overloadedOps.push_back(op.symbol);
                                        ZEUS_DEBUG_PRINT("[TreeParser] collected operator symbol: '%s'", op.symbol.c_str());
                                    }
                                    continue;
                                }

                                if (op.symbol == "(") {
                                    if (!newBlock->funcName.empty() || seenOperator) inParams = true; // 进入参数列表

                                } else if (op.symbol == ")") {
                                    // 如果正在收集默认值,结束收集
                                    if (collectingDefault) {
                                        if (!newBlock->paramDefaults.empty()) {
                                            newBlock->paramDefaults.back() = currentDefault;
                                        }
                                        currentDefault.clear();
                                        collectingDefault = false;
                                    }
                                    inParams = false; // 退出参数列表
                                    expectingParamType = false;
                                    afterParams = true;

                                } else if (op.symbol == ",") {
                                    // 如果正在收集默认值,结束当前参数的默认值收集
                                    if (collectingDefault) {
                                        if (!newBlock->paramDefaults.empty()) {
                                            newBlock->paramDefaults.back() = currentDefault;
                                        }
                                        currentDefault.clear();
                                        collectingDefault = false;
                                    }
                                    expectingParamType = false;
                                    // 逗号本身不做其他处理
                                } else if (inParams) {
                                    // 如果正在收集默认值,将运算符加入
                                    if (collectingDefault) {
                                        currentDefault.push_back(tk);
                                        continue;
                                    }

                                    if (op.symbol == ":") {
                                        expectingParamType = true;
                                        continue;
                                    }
                                    
                                    // 检测 = 开始收集默认值
                                    if (op.symbol == "=") {
                                        collectingDefault = true;
                                        continue;
                                    }
                                    
                                    // 在参数列表内检测 * 和 **
                                    if (op.symbol == "*") {
                                        // 检查下一个 token 是否也是 *，如果是则合并为 **
                                        if (i + 1 < stack.size() && std::holds_alternative<LexerType::Operator>(stack[i + 1])) {
                                            const auto& nextOp = std::get<LexerType::Operator>(stack[i + 1]);
                                            if (nextOp.symbol == "*") {
                                                nextIsVariadicKwargs = true;
                                                // 跳过下一个 * token
                                                i++;
                                                continue;
                                            }
                                        }
                                        // 单个 *，是 *args
                                        nextIsVariadicArgs = true;
                                    }
                                }
                            } else if (std::holds_alternative<LexerType::Value>(tk)) {
                                // Value 类型 (数字、字符串等)
                                if (collectingDefault && inParams) {
                                    currentDefault.push_back(tk);
                                }
                            }
                        }

                        // 保存类型注解到 AST。参数名仍由上面的兼容逻辑负责解析，
                        // 这里仅补充参数类型与返回类型元数据。
                        auto isFunctionOperator = [](const Token& token, const std::string& symbol) {
                            return std::holds_alternative<LexerType::Operator>(token) &&
                                   std::get<LexerType::Operator>(token).symbol == symbol;
                        };

                        size_t openParen = stack.size();
                        for (size_t i = 0; i < stack.size(); ++i) {
                            if (isFunctionOperator(stack[i], "(")) {
                                openParen = i;
                                break;
                            }
                        }

                        if (openParen < stack.size()) {
                            size_t closeParen = openParen;
                            int parenDepth = 0;

                            for (size_t i = openParen; i < stack.size(); ++i) {
                                if (isFunctionOperator(stack[i], "(")) {
                                    parenDepth++;
                                } else if (isFunctionOperator(stack[i], ")")) {
                                    parenDepth--;
                                    if (parenDepth == 0) {
                                        closeParen = i;
                                        break;
                                    }
                                }
                            }

                            auto appendParamType = [&](size_t begin, size_t end) {
                                if (begin >= end) return;

                                std::string paramName;
                                std::string typeName;
                                size_t colon = end;

                                for (size_t i = begin; i < end; ++i) {
                                    if (std::holds_alternative<LexerType::Keyword>(stack[i])) {
                                        if (paramName.empty()) {
                                            paramName = std::get<LexerType::Keyword>(stack[i]).name;
                                        } else if (colon < end && typeName.empty()) {
                                            typeName = std::get<LexerType::Keyword>(stack[i]).name;
                                        }
                                    } else if (isFunctionOperator(stack[i], ":") && colon == end) {
                                        colon = i;
                                    } else if (isFunctionOperator(stack[i], "=")) {
                                        break;
                                    }
                                }

                                if (paramName.empty() || paramName == newBlock->variadicArgs ||
                                    paramName == newBlock->variadicKwargs) {
                                    return;
                                }

                                newBlock->paramTypeNames.push_back(typeName);
                            };

                            size_t paramBegin = openParen + 1;
                            int nestedDepth = 0;

                            for (size_t i = paramBegin; i <= closeParen; ++i) {
                                bool atEnd = (i == closeParen);
                                bool atComma = !atEnd && nestedDepth == 0 && isFunctionOperator(stack[i], ",");

                                if (atEnd || atComma) {
                                    appendParamType(paramBegin, i);
                                    paramBegin = i + 1;
                                    continue;
                                }

                                if (isFunctionOperator(stack[i], "(") || isFunctionOperator(stack[i], "[") ||
                                    isFunctionOperator(stack[i], "{")) {
                                    nestedDepth++;
                                } else if (isFunctionOperator(stack[i], ")") || isFunctionOperator(stack[i], "]") ||
                                           isFunctionOperator(stack[i], "}")) {
                                    if (nestedDepth > 0) nestedDepth--;
                                }
                            }

                            size_t returnTypePos = closeParen + 1;
                            if (returnTypePos < stack.size() && isFunctionOperator(stack[returnTypePos], "[")) {
                                int captureDepth = 1;
                                ++returnTypePos;
                                while (returnTypePos < stack.size() && captureDepth > 0) {
                                    if (isFunctionOperator(stack[returnTypePos], "[")) captureDepth++;
                                    if (isFunctionOperator(stack[returnTypePos], "]")) captureDepth--;
                                    ++returnTypePos;
                                }
                            }

                            if (returnTypePos < stack.size() &&
                                isFunctionOperator(stack[returnTypePos], "->")) {
                                size_t typePosition = returnTypePos + 1;
                                ZeusParser::parseTypeName(
                                    stack,
                                    typePosition,
                                    stack.size(),
                                    newBlock->returnTypeName
                                );
                            }
                        }

                        // 合并连续的运算符符号（例如 [, [, ], ] -> [[]]）
                        if (seenOperator && !newBlock->overloadedOps.empty()) {
                            std::string mergedOp;
                            for (const auto& opSym : newBlock->overloadedOps) {
                                mergedOp += opSym;
                            }
                            // 替换为单个合并后的运算符字符串
                            newBlock->overloadedOps.clear();
                            newBlock->overloadedOps.push_back(mergedOp);
                            ZEUS_DEBUG_PRINT("[TreeParser] Merged operator symbols into: '%s'", mergedOp.c_str());
                        }

                        // 第二次遍历：查找捕获列表（位于参数结束之后，函数体 '{' 之前）
                        // 允许形态： []  /  [&]  /  [&a, b, &x] （&标记引用捕获，其余为值拷贝）
                        // 对于operator函数，这里会正确处理capture list，因为operatorListClosed已经为true
                        bool startedCapture = false;
                        bool finishedCapture = false;
                        bool seenRightParen = false; // 确保是在参数列表之后

                        std::unordered_set<std::string> seenNames; // 去重检测

                        for (size_t i = 0; i < stack.size() && !finishedCapture; ++i) {
                            const auto& tk = stack[i];
                            if (std::holds_alternative<LexerType::Operator>(tk)) {
                                const auto& op = std::get<LexerType::Operator>(tk);
                                if (op.symbol == ")") {
                                    seenRightParen = true;
                                }
                                if (op.symbol == "[" && seenRightParen && !startedCapture) {
                                    startedCapture = true;
                                    // 进入捕获解析
                                    ++i; // 移到 '[' 后第一个 token
                                    bool expectEntry = true; // 期望 entry 或 ']' 或 '&'
                                    while (i < stack.size()) {
                                        const auto& innerTk = stack[i];
                                        if (std::holds_alternative<LexerType::Operator>(innerTk)) {
                                            const auto& iop = std::get<LexerType::Operator>(innerTk);
                                            if (iop.symbol == "]") {
                                                finishedCapture = true;
                                                break; // 捕获列表结束
                                            }
                                            if (iop.symbol == "&") {
                                                // 可能是 [&] (捕获全部) 或 &name 形式
                                                // 向前看下一个 token
                                                if (i + 1 < stack.size()) {
                                                    const auto& look = stack[i + 1];
                                                    if (std::holds_alternative<LexerType::Operator>(look) && std::get<LexerType::Operator>(look).symbol == "]") {
                                                        // [&] 形式
                                                        newBlock->isCaptureAll = true;
                                                        i += 1; // 让外层 while 看到 ']' 并关闭
                                                        continue;
                                                    } else if (std::holds_alternative<LexerType::Keyword>(look)) {
                                                        // &identifier 形式
                                                        const auto& kw = std::get<LexerType::Keyword>(look);
                                                        if (!seenNames.count(kw.name)) {
                                                            newBlock->captureVars.push_back(kw.name);
                                                            seenNames.insert(kw.name);
                                                        }
                                                        i += 2; // 跳过 & 和 标识符
                                                        // 可能后面跟 ',' 或 ']'，循环继续
                                                        continue;
                                                    } else {
                                                        // 异常形态，忽略此 '&'
                                                    }
                                                }
                                            }
                                            if (iop.symbol == ",") {
                                                ++i; // 跳过逗号继续
                                                continue;
                                            }
                                            // 其它操作符，遇到 '{' 或 '(' 说明捕获列表已结束（语法错或提前结束），直接终止
                                            if (iop.symbol == "{" || iop.symbol == "(") {
                                                finishedCapture = true;
                                                break;
                                            }
                                            ++i; // 继续扫描
                                            continue;
                                        } else if (std::holds_alternative<LexerType::Keyword>(innerTk)) {
                                            // 普通标识符 => 拷贝捕获
                                            const auto& kw = std::get<LexerType::Keyword>(innerTk);
                                            if (!seenNames.count(kw.name)) {
                                                newBlock->copyVars.push_back(kw.name);
                                                seenNames.insert(kw.name);
                                            }
                                            ++i; // 前进
                                            continue;
                                        } else {
                                            // 其它（值字面量等）不应出现，忽略
                                            ++i;
                                        }
                                    }
                                }
                            }
                        }

                        // 若出现 [] 且既不是 isCaptureAll 也没有显式条目 -> copy all
                        if (startedCapture && finishedCapture && !newBlock->isCaptureAll && newBlock->captureVars.empty() && newBlock->copyVars.empty()) {
                            newBlock->isCopyAll = true; // [] 语义：按需拷贝所有用到的外层局部
                        }
                        // [&] 情况下 captureVars/ copyVars 都为空,isCaptureAll 已设

                        // 若该函数直接定义于 CLASS 代码块内,根据类的 using &alias 与 static 修饰,
                        // 在解析阶段为实例方法注入别名为首个参数,确保后续 IR 生成时该别名作为局部已声明。
                        if (!blockStack.empty()) {
                            CodeBlock* parentCb = blockStack.back();
                            if (parentCb && parentCb->type == CodeBlockType::CLASS) {
                                newBlock->isClassMethod = true;
                                bool hasClassAlias = !parentCb->classDefaultAlias.empty();
                                ZEUS_DEBUG_PRINT("[Parser   ] Method '%s' in class, hasAlias=%d alias='%s' isStatic=%d",
                                                 newBlock->funcName.c_str(), hasClassAlias, 
                                                 parentCb->classDefaultAlias.c_str(), newBlock->isStaticMethod);
                                bool classifyAsInstance = false;
                                if (hasClassAlias) {
                                    // 类声明了 using &alias; 则无 static 修饰的函数视为实例方法
                                    classifyAsInstance = !newBlock->isStaticMethod;
                                } else {
                                    // 类未声明 using:若函数声明显式捕获 [&name] 则视为实例方法(别名为该 name),否则默认 static
                                    classifyAsInstance = !newBlock->isStaticMethod && !newBlock->captureVars.empty();
                                }
                                ZEUS_DEBUG_PRINT("[Parser   ]   -> classifyAsInstance=%d", classifyAsInstance);
                                if (classifyAsInstance) {
                                    std::string aliasToInsert = hasClassAlias ? parentCb->classDefaultAlias : (newBlock->captureVars.empty() ? std::string() : newBlock->captureVars[0]);
                                    newBlock->inferredInstanceAlias = aliasToInsert;
                                    ZEUS_DEBUG_PRINT("[Parser   ]   -> Injecting alias '%s' to method '%s', paramNames.size()=%zu", 
                                                     aliasToInsert.c_str(), newBlock->funcName.c_str(), newBlock->paramNames.size());
                                    if (!aliasToInsert.empty()) {
                                        if (newBlock->paramNames.empty() || newBlock->paramNames.front() != aliasToInsert) {
                                            newBlock->paramNames.insert(newBlock->paramNames.begin(), aliasToInsert);
                                            newBlock->paramTypeNames.insert(newBlock->paramTypeNames.begin(), std::string());
                                            // 同时为 this 参数插入一个空的默认值，保持与 paramNames 对齐
                                            newBlock->paramDefaults.insert(newBlock->paramDefaults.begin(), std::vector<Token>());
                                            ZEUS_DEBUG_PRINT("[Parser   ]   -> Injected! paramNames.size=%zu", newBlock->paramNames.size());
                                        } else {
                                            ZEUS_DEBUG_PRINT("[Parser   ]   -> Already present, skipping injection");
                                        }
                                    } else {
                                        ZEUS_DEBUG_PRINT("[Parser   ]   -> aliasToInsert is empty!");
                                    }
                                }
                            }
                        }
                    }

                    // 如果是类代码块，尝试从 stack 中提取类名（紧随 class 之后的第一个关键字）
                    // 支持 shared class ClassName 或 class shared ClassName
                    if (cdbType == CodeBlockType::CLASS) {
                        bool seenClass = false;
                        bool seenShared = false;
                        
                        for (const auto& tk : stack) {
                            if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                const auto& kw = std::get<LexerType::Keyword>(tk);
                                
                                if (kw.name == "shared") {
                                    seenShared = true;
                                    newBlock->isSharedClass = true;
                                    continue;
                                }
                                
                                if (kw.name == "class") {
                                    seenClass = true;
                                    continue;
                                }
                                
                                // 类名：第一个非 shared/class 的关键字
                                if (seenClass && newBlock->className.empty()) {
                                    newBlock->className = kw.name;
                                    break;
                                }
                            }
                        }
                    }

                    // 检测是否 else 块：当前收集的 stack 中包含关键字 else 且上一兄弟是 IF
                    bool isElseBlock = false;
                    for (auto const& tk : stack) {
                        if (std::holds_alternative<LexerType::Keyword>(tk)) {
                            if (std::get<LexerType::Keyword>(tk).name == "else") { isElseBlock = true; break; }
                        }
                    }
                    newBlock->isElseBranch = isElseBlock;

                    // 检测是否 catch 块：当前收集的 stack 中包含关键字 catch 且上一兄弟是 TRY
                    bool isCatchBlock = false;
                    for (auto const& tk : stack) {
                        if (std::holds_alternative<LexerType::Keyword>(tk)) {
                            if (std::get<LexerType::Keyword>(tk).name == "catch") { isCatchBlock = true; break; }
                        }
                    }

                    CodeBlock* ptr = newBlock.get();

                    // ===== catch 块处理 =====
                    // catch 应该附加到最近的 TRY 块
                    // 语法: catch (&varName) { ... } 或 catch () { ... }
                    if (isCatchBlock && !blockStack.back()->nodes.empty()) {
                        auto &lastVariant = blockStack.back()->nodes.back();
                        if (std::holds_alternative<std::unique_ptr<CodeBlock>>(lastVariant)) {
                            auto *tryBlock = std::get<std::unique_ptr<CodeBlock>>(lastVariant).get();
                            
                            if (tryBlock && tryBlock->type == CodeBlockType::TRY) {
                                // 设置 catch 块类型
                                newBlock->type = CodeBlockType::CATCH;
                                
                                // 解析 catch (...) 中的变量名
                                // 格式: catch (&varName) 或 catch ()
                                bool seenCatch = false;
                                bool seenOpenParen = false;
                                bool isRef = false;
                                std::string catchVarName;
                                
                                for (const auto& tk : stack) {
                                    if (std::holds_alternative<LexerType::Keyword>(tk)) {
                                        const auto& kw = std::get<LexerType::Keyword>(tk);
                                        if (kw.name == "catch") {
                                            seenCatch = true;
                                            continue;
                                        }
                                        if (seenCatch && seenOpenParen && catchVarName.empty()) {
                                            catchVarName = kw.name;
                                        }
                                    }
                                    if (std::holds_alternative<LexerType::Operator>(tk)) {
                                        const auto& op = std::get<LexerType::Operator>(tk);
                                        if (seenCatch && op.symbol == "(") {
                                            seenOpenParen = true;
                                            continue;
                                        }
                                        if (seenCatch && seenOpenParen && op.symbol == "&") {
                                            isRef = true;
                                            continue;
                                        }
                                        if (seenCatch && seenOpenParen && op.symbol == ")") {
                                            break;
                                        }
                                    }
                                }
                                
                                newBlock->catchVarName = catchVarName;
                                newBlock->catchVarIsRef = isRef;
                                
                                ZEUS_DEBUG_PRINT("[Parser   ] CATCH block: varName='%s', isRef=%d", catchVarName.c_str(), isRef);
                                
                                // 将 catch 块附加到 try 块
                                tryBlock->nodes.push_back(std::move(newBlock));
                                auto &catchVariant = tryBlock->nodes.back();
                                ptr = std::get<std::unique_ptr<CodeBlock>>(catchVariant).get();
                                blockStack.push_back(ptr);
                                stack.clear();
                                ndtype = NodeType::WAIT;
                                counter = 0;
                                continue;
                            }
                        }
                    }
                    // ===== end catch 块处理 =====

                    if (isElseBlock && !blockStack.back()->nodes.empty()) {
                        auto &lastVariant = blockStack.back()->nodes.back();
                        if (std::holds_alternative<std::unique_ptr<CodeBlock>>(lastVariant)) {
                            auto *ifBlock = std::get<std::unique_ptr<CodeBlock>>(lastVariant).get();
                            // 修复：检查 IF 块是否已经有 else 分支
                            // else 分支必须是最后一个节点，且是 CodeBlock 类型
                            bool alreadyHasElse = false;
                            if (ifBlock && ifBlock->type == CodeBlockType::IF && !ifBlock->nodes.empty()) {
                                const auto& lastNode = ifBlock->nodes.back();
                                if (std::holds_alternative<std::unique_ptr<CodeBlock>>(lastNode)) {
                                    const auto* elseBranch =
                                        std::get<std::unique_ptr<CodeBlock>>(lastNode).get();
                                    alreadyHasElse = elseBranch != nullptr && elseBranch->isElseBranch;
                                }
                            }
                            
                            if (ifBlock && ifBlock->type == CodeBlockType::IF && !alreadyHasElse) {
                                ifBlock->nodes.push_back(std::move(newBlock));
                                // else 块成为 IF 的子块，新的当前块是刚附加的 else
                                auto &elseVariant = ifBlock->nodes.back();
                                ptr = std::get<std::unique_ptr<CodeBlock>>(elseVariant).get();
                                blockStack.push_back(ptr);
                                stack.clear();
                                ndtype = NodeType::WAIT;
                                counter = 0;
                                continue;
                            }
                        }
                    }
                    blockStack.back()->nodes.push_back(std::move(newBlock));
                    blockStack.push_back(ptr);

                    stack.clear();

                    ndtype = NodeType::WAIT;
                    counter = 0;
                    continue;
                }

                if (opStr == "}") {
                    if (exprBraceDepth > 0) {
                        exprBraceDepth--;
                        stack.push_back(token);
                        counter = 0; // 继续语句内 token 计数复位（与普通符号一致）
                        ndtype = (ndtype == NodeType::WAIT ? NodeType::WAIT : ndtype);
                        continue;
                    }
                    if (!blockStack.empty()) 
                        blockStack.pop_back();

                    stack.clear();
                    counter = 0;
                    ndtype = NodeType::WAIT;
                    continue;
                }

                stack.push_back(token);
            }

            if (std::holds_alternative<LexerType::Value>(token)) {
                auto const& value = std::get<LexerType::Value>(token);
                auto const& valueStr = value.toString();

                ZEUS_DEBUG_PRINT("[%d] Value: %s | Stat: %s", counter, valueStr.c_str(), Debug::toString(ndtype));
                
                stack.push_back(token);
            }

            if (std::holds_alternative<LexerType::BytesLiteral>(token)) {
                auto const& bytes = std::get<LexerType::BytesLiteral>(token);
                auto const& bytesStr = bytes.toString();

                ZEUS_DEBUG_PRINT("[%d] BytesLiteral: %s | Stat: %s", counter, bytesStr.c_str(), Debug::toString(ndtype));
                
                stack.push_back(token);
            }
        }

        return result;
    }

    void printAST(const std::vector<NodeAndBlock>& ast, int indent = 0) {
        std::string pad(indent, ' ');
        for (const auto& elem : ast) {
            if (std::holds_alternative<Node>(elem)) {
                const auto& node = std::get<Node>(elem);
                        ZEUS_DEBUG_PRINT("%sNode(%s)", pad.c_str(), Debug::toString(node.type));
                for (const auto& token : node.tokens) {
                    if (std::holds_alternative<LexerType::Keyword>(token)) {
                            ZEUS_DEBUG_PRINT("%s  Keyword: %s", pad.c_str(), std::get<LexerType::Keyword>(token).toString().c_str());
                    } else if (std::holds_alternative<LexerType::Operator>(token)) {
                            ZEUS_DEBUG_PRINT("%s  Operator: %s", pad.c_str(), std::get<LexerType::Operator>(token).toString().c_str());
                    } else if (std::holds_alternative<LexerType::Value>(token)) {
                            ZEUS_DEBUG_PRINT("%s  Value: %s", pad.c_str(), std::get<LexerType::Value>(token).toString().c_str());
                    } else if (std::holds_alternative<LexerType::BytesLiteral>(token)) {
                            ZEUS_DEBUG_PRINT("%s  BytesLiteral: %s", pad.c_str(), std::get<LexerType::BytesLiteral>(token).toString().c_str());
                    }
                }
            } else {
                const auto& block = std::get<std::unique_ptr<CodeBlock>>(elem);
                ZEUS_DEBUG_PRINT("%sCodeBlock(%s)", pad.c_str(), Debug::toString(block->type));
                if (block->type == CodeBlockType::FUNC) {
                    ZEUS_DEBUG_PRINT("%s  FuncName: %s", pad.c_str(), block->funcName.c_str());
                    ZEUS_DEBUG_PRINT("%s  Params:", pad.c_str());
                    for (const auto& p : block->paramNames) {
                        ZEUS_DEBUG_PRINT("%s    %s", pad.c_str(), p.c_str());
                    }
                    ZEUS_DEBUG_PRINT("");

                    if (block->isCaptureAll) {
                        ZEUS_DEBUG_PRINT("%s  Capture: [&] (all by ref)", pad.c_str());
                    } else if (block->isCopyAll) {
                        ZEUS_DEBUG_PRINT("%s  Capture: [] (all by copy)", pad.c_str());
                    } else {
                        if (!block->captureVars.empty()) {
                            ZEUS_DEBUG_PRINT("%s  Capture by ref:", pad.c_str());
                            for (const auto& v : block->captureVars) {
                                ZEUS_DEBUG_PRINT("%s    %s", pad.c_str(), v.c_str());
                            }
                            ZEUS_DEBUG_PRINT("");
                        }
                        if (!block->copyVars.empty()) {
                            ZEUS_DEBUG_PRINT("%s  Capture by copy:", pad.c_str());
                            for (const auto& v : block->copyVars) {
                                ZEUS_DEBUG_PRINT("%s    %s", pad.c_str(), v.c_str());
                            }
                            ZEUS_DEBUG_PRINT("");
                        }
                    }
                }
                printAST(block->nodes, indent + 2);
            }
        }
    }

}

#endif
