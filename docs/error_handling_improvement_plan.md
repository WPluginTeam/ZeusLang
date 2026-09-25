# Zeus 错误处理机制完善方案

## 当前问题分析

### 1. **错误处理不统一**
- **IR阶段**：使用`IRException`改变编译状态，打印到stdout
- **VM阶段**：混用`VMException`和`std::runtime_error`
- **内置函数**：直接`throw std::runtime_error`，没有行号信息
- **主程序**：完全没有try-catch包裹

### 2. **缺少调用栈追踪**
```
[VM Error] Line 41: OBJ_LOAD_ATTR needs ObjectHandle
```
- 只显示错误发生的行号
- **没有调用栈**：不知道是从哪个函数调用链导致的
- **没有变量状态**：不知道当时的变量值

### 3. **错误信息不详细**
```cpp
throw std::runtime_error("hasKey: first argument must be a dictionary");
```
- 没有说明实际收到的是什么类型
- 没有上下文信息（在哪个对象上调用）
- 没有建议修复方案

### 4. **编译期和运行期错误混淆**
- IR错误只改状态不抛异常
- VM错误有的抛异常有的不抛
- 用户无法捕获和处理错误

### 5. **调试信息缺失**
- 没有错误码系统
- 无法根据错误码查询详细文档
- 无法根据错误严重程度分类

---

## 完善方案设计

### 阶段1：统一错误类型系统

#### 1.1 定义错误类型枚举
```cpp
// ZeusError/ErrorTypes.hpp
namespace ZeusError {
    enum class ErrorCategory {
        LEXER,          // 词法错误
        PARSER,         // 语法错误
        IR_COMPILER,    // IR编译错误
        VM_RUNTIME,     // VM运行时错误
        TYPE_ERROR,     // 类型错误
        NAME_ERROR,     // 名称错误
        ATTRIBUTE_ERROR,// 属性错误
        INDEX_ERROR,    // 索引错误
        IMPORT_ERROR,   // 导入错误
        INTERNAL_ERROR  // 内部错误
    };

    enum class ErrorSeverity {
        WARNING,        // 警告（不中断执行）
        ERROR,          // 错误（中断当前执行）
        FATAL           // 致命错误（退出程序）
    };

    // 错误码定义（便于文档化和国际化）
    enum class ErrorCode {
        // 通用错误 (1000-1099)
        E1000_UNKNOWN_ERROR,
        E1001_INTERNAL_ERROR,
        
        // 词法错误 (1100-1199)
        E1100_INVALID_TOKEN,
        E1101_UNTERMINATED_STRING,
        E1102_INVALID_NUMBER,
        
        // 语法错误 (1200-1299)
        E1200_UNEXPECTED_TOKEN,
        E1201_MISSING_PAREN,
        E1202_MISSING_BRACE,
        E1203_INVALID_SYNTAX,
        
        // IR编译错误 (1300-1399)
        E1300_UNDEFINED_VARIABLE,
        E1301_CONST_ASSIGNMENT,
        E1302_INVALID_OPERATOR,
        E1303_FUNCTION_NOT_FOUND,
        
        // 类型错误 (1400-1499)
        E1400_TYPE_MISMATCH,
        E1401_NOT_CALLABLE,
        E1402_NOT_ITERABLE,
        E1403_INVALID_ARGUMENT_TYPE,
        
        // 名称错误 (1500-1599)
        E1500_UNDEFINED_NAME,
        E1501_UNDEFINED_ATTRIBUTE,
        E1502_UNDEFINED_METHOD,
        
        // 属性错误 (1600-1699)
        E1600_ATTRIBUTE_NOT_FOUND,
        E1601_NO_SUCH_ATTRIBUTE,
        E1602_ATTRIBUTE_READ_ONLY,
        
        // 索引错误 (1700-1799)
        E1700_INDEX_OUT_OF_RANGE,
        E1701_INVALID_INDEX_TYPE,
        E1702_KEY_NOT_FOUND,
        
        // 导入错误 (1800-1899)
        E1800_MODULE_NOT_FOUND,
        E1801_IMPORT_CYCLE,
        E1802_PACKAGE_NOT_FOUND,
        
        // VM运行时错误 (1900-1999)
        E1900_STACK_OVERFLOW,
        E1901_STACK_UNDERFLOW,
        E1902_NULL_REFERENCE,
        E1903_DIVISION_BY_ZERO,
        E1904_INVALID_OPERATION,
    };
}
```

#### 1.2 统一错误基类
```cpp
// ZeusError/ZeusException.hpp
namespace ZeusError {
    
    struct SourceLocation {
        std::string filename;
        int line = 0;
        int column = 0;
        std::string functionName;
        
        std::string toString() const {
            std::ostringstream oss;
            if (!filename.empty()) oss << filename << ":";
            if (line > 0) oss << line;
            if (column > 0) oss << ":" << column;
            if (!functionName.empty()) oss << " in " << functionName;
            return oss.str();
        }
    };
    
    struct StackFrame {
        SourceLocation location;
        std::string code;           // 错误发生的代码行
        std::map<std::string, std::string> localVars;  // 局部变量快照
    };
    
    class ZeusException {
    protected:
        ErrorCode code;
        ErrorCategory category;
        ErrorSeverity severity;
        std::string message;
        std::string detailedMessage;
        std::vector<StackFrame> callStack;
        std::string suggestion;     // 修复建议
        
    public:
        ZeusException(
            ErrorCode code,
            ErrorCategory category,
            ErrorSeverity severity,
            const std::string& message
        ) : code(code), category(category), severity(severity), message(message) {}
        
        // 添加调用栈帧
        void addStackFrame(const StackFrame& frame) {
            callStack.push_back(frame);
        }
        
        // 设置详细信息
        void setDetails(const std::string& details) {
            detailedMessage = details;
        }
        
        // 设置修复建议
        void setSuggestion(const std::string& suggest) {
            suggestion = suggest;
        }
        
        // 格式化输出
        std::string format() const {
            std::ostringstream oss;
            
            // 错误头
            oss << "[" << categoryToString(category) << " Error E" 
                << static_cast<int>(code) << "] ";
            
            // 主要位置信息
            if (!callStack.empty()) {
                oss << callStack.back().location.toString() << ": ";
            }
            
            oss << message << "\n";
            
            // 详细信息
            if (!detailedMessage.empty()) {
                oss << "  Details: " << detailedMessage << "\n";
            }
            
            // 错误代码行
            if (!callStack.empty() && !callStack.back().code.empty()) {
                oss << "  | " << callStack.back().code << "\n";
            }
            
            // 调用栈
            if (callStack.size() > 1) {
                oss << "\nCall Stack (most recent first):\n";
                for (int i = callStack.size() - 1; i >= 0; --i) {
                    oss << "  [" << (callStack.size() - i) << "] "
                        << callStack[i].location.toString() << "\n";
                    if (!callStack[i].code.empty()) {
                        oss << "      | " << callStack[i].code << "\n";
                    }
                }
            }
            
            // 局部变量（仅最顶层栈帧）
            if (!callStack.empty() && !callStack.back().localVars.empty()) {
                oss << "\nLocal variables:\n";
                for (const auto& [name, value] : callStack.back().localVars) {
                    oss << "  " << name << " = " << value << "\n";
                }
            }
            
            // 修复建议
            if (!suggestion.empty()) {
                oss << "\nSuggestion: " << suggestion << "\n";
            }
            
            return oss.str();
        }
        
        ErrorCode getCode() const { return code; }
        ErrorCategory getCategory() const { return category; }
        ErrorSeverity getSeverity() const { return severity; }
        
    private:
        static std::string categoryToString(ErrorCategory cat) {
            switch (cat) {
                case ErrorCategory::LEXER: return "Lexer";
                case ErrorCategory::PARSER: return "Parser";
                case ErrorCategory::IR_COMPILER: return "Compiler";
                case ErrorCategory::VM_RUNTIME: return "Runtime";
                case ErrorCategory::TYPE_ERROR: return "Type";
                case ErrorCategory::NAME_ERROR: return "Name";
                case ErrorCategory::ATTRIBUTE_ERROR: return "Attribute";
                case ErrorCategory::INDEX_ERROR: return "Index";
                case ErrorCategory::IMPORT_ERROR: return "Import";
                default: return "Internal";
            }
        }
    };
}
```

### 阶段2：调用栈追踪系统

#### 2.1 VM调用栈追踪
```cpp
// ZeusRunner/ZeusCallStack.hpp
namespace ZeusVM {
    
    struct CallStackEntry {
        int protoId = -1;               // 函数protoId
        std::string functionName;        // 函数名
        int chunkIndex = -1;             // 当前chunk
        int instructionIndex = -1;       // 当前指令
        int line = 0;                    // 源代码行号
        size_t stackBase = 0;            // 栈基址
        std::vector<std::string> localVarNames;  // 局部变量名
    };
    
    class CallStackTracker {
    private:
        std::vector<CallStackEntry> stack;
        std::vector<std::string>* sourceLines = nullptr;
        
    public:
        void setSourceLines(std::vector<std::string>* lines) {
            sourceLines = lines;
        }
        
        // 进入函数调用
        void enter(int protoId, const std::string& name, int chunk, size_t stackBase) {
            CallStackEntry entry;
            entry.protoId = protoId;
            entry.functionName = name;
            entry.chunkIndex = chunk;
            entry.stackBase = stackBase;
            stack.push_back(entry);
        }
        
        // 退出函数调用
        void exit() {
            if (!stack.empty()) stack.pop_back();
        }
        
        // 更新当前指令位置
        void updatePosition(int instrIndex, int line) {
            if (!stack.empty()) {
                stack.back().instructionIndex = instrIndex;
                stack.back().line = line;
            }
        }
        
        // 生成调用栈快照
        std::vector<ZeusError::StackFrame> snapshot(const std::vector<ZeusType::Cell>& varStack) const {
            std::vector<ZeusError::StackFrame> frames;
            
            for (const auto& entry : stack) {
                ZeusError::StackFrame frame;
                frame.location.line = entry.line;
                frame.location.functionName = entry.functionName;
                
                // 获取源代码行
                if (sourceLines && entry.line > 0 && entry.line <= sourceLines->size()) {
                    frame.code = (*sourceLines)[entry.line - 1];
                }
                
                // 获取局部变量值（从varStack）
                for (size_t i = 0; i < entry.localVarNames.size(); ++i) {
                    size_t varIndex = entry.stackBase + i;
                    if (varIndex < varStack.size()) {
                        frame.localVars[entry.localVarNames[i]] = 
                            varStack[varIndex].toDebugString();
                    }
                }
                
                frames.push_back(frame);
            }
            
            return frames;
        }
        
        // 获取当前函数名
        std::string currentFunction() const {
            return stack.empty() ? "<top-level>" : stack.back().functionName;
        }
        
        int depth() const { return stack.size(); }
    };
}
```

#### 2.2 在VM中集成调用栈
```cpp
// 在ZeusVM.hpp的VM类中添加
class VM {
private:
    CallStackTracker callStack;
    
public:
    void run() {
        callStack.setSourceLines(&VMException::sourceLines);
        
        // 在FUNC_BEGIN时
        case VMOperator::FUNC_BEGIN: {
            callStack.enter(currentProtoId, functionName, currentChunk, stackBase);
            break;
        }
        
        // 在FUNC_END或RETURN时
        case VMOperator::FUNC_END:
        case VMOperator::RETURN: {
            callStack.exit();
            break;
        }
        
        // 每条指令执行前更新位置
        callStack.updatePosition(instrIndex, instr.line);
        
        // 错误处理时使用调用栈
        try {
            // ... 执行指令 ...
        } catch (const std::exception& e) {
            auto ex = createRuntimeError(e.what(), instr);
            auto frames = callStack.snapshot(varStack);
            for (const auto& frame : frames) {
                ex.addStackFrame(frame);
            }
            throw ex;
        }
    }
};
```

### 阶段3：增强错误信息

#### 3.1 类型错误增强
```cpp
// 当前：
throw std::runtime_error("hasKey: first argument must be a dictionary");

// 改进后：
auto ex = ZeusException(
    ErrorCode::E1403_INVALID_ARGUMENT_TYPE,
    ErrorCategory::TYPE_ERROR,
    ErrorSeverity::ERROR,
    "hasKey() expects a dictionary as first argument"
);
ex.setDetails(
    "Got: " + getTypeName(args[0]) + 
    "\nExpected: Dict\n" +
    "Argument value: " + args[0].toDebugString()
);
ex.setSuggestion(
    "Make sure you're passing a dictionary object. "
    "You can create one with {key: value} syntax."
);
throw ex;
```

#### 3.2 属性错误增强
```cpp
// 当前：
throw std::runtime_error("getattr: attribute 'xxx' not found");

// 改进后：
auto ex = ZeusException(
    ErrorCode::E1600_ATTRIBUTE_NOT_FOUND,
    ErrorCategory::ATTRIBUTE_ERROR,
    ErrorSeverity::ERROR,
    "Object has no attribute '" + attrName + "'"
);
ex.setDetails(
    "Object type: " + obj.className + "\n" +
    "Available attributes: " + joinAttributeNames(obj)
);

// 智能建议（查找相似名称）
auto similar = findSimilarNames(attrName, obj.getAllAttributes());
if (!similar.empty()) {
    ex.setSuggestion("Did you mean '" + similar + "'?");
}
throw ex;
```

### 阶段4：错误恢复机制

#### 4.1 编译期错误收集（不立即停止）
```cpp
// ZeusError/ErrorCollector.hpp
class ErrorCollector {
private:
    std::vector<ZeusException> errors;
    std::vector<ZeusException> warnings;
    bool stopOnFirstError = false;
    
public:
    void setStopOnFirstError(bool stop) { stopOnFirstError = stop; }
    
    void addError(const ZeusException& ex) {
        if (ex.getSeverity() == ErrorSeverity::WARNING) {
            warnings.push_back(ex);
        } else {
            errors.push_back(ex);
            if (stopOnFirstError) {
                throw CompilationAborted();
            }
        }
    }
    
    bool hasErrors() const { return !errors.empty(); }
    bool hasWarnings() const { return !warnings.empty(); }
    
    void printAll() const {
        for (const auto& warn : warnings) {
            std::cout << warn.format() << "\n";
        }
        for (const auto& err : errors) {
            std::cout << err.format() << "\n";
        }
    }
    
    std::string summary() const {
        std::ostringstream oss;
        if (hasErrors()) {
            oss << errors.size() << " error(s)";
        }
        if (hasWarnings()) {
            if (hasErrors()) oss << ", ";
            oss << warnings.size() << " warning(s)";
        }
        return oss.str();
    }
};
```

#### 4.2 主程序错误处理
```cpp
// Zeus.cpp main() 中
int main(int argc, char* argv[]) {
    try {
        // ... 原有代码 ...
        
        ZeusCompile::Result cr = ZeusCompile::compile(code, strictDecl, enableHotspot);
        
        if (cr.errorCollector && cr.errorCollector->hasErrors()) {
            cr.errorCollector->printAll();
            std::cerr << "\nCompilation failed: " 
                     << cr.errorCollector->summary() << std::endl;
            return 1;
        }
        
        ZeusCompile::run(cr);
        
    } catch (const ZeusError::ZeusException& ex) {
        std::cerr << ex.format() << std::endl;
        return 1;
    } catch (const std::exception& ex) {
        std::cerr << "[Fatal Error] " << ex.what() << std::endl;
        return 2;
    } catch (...) {
        std::cerr << "[Fatal Error] Unknown exception" << std::endl;
        return 3;
    }
    
    return 0;
}
```

### 阶段5：调试辅助功能

#### 5.1 变量状态快照
```cpp
// 在错误发生时自动捕获变量状态
std::string captureVariableState(const VM& vm) {
    std::ostringstream oss;
    
    // 全局变量
    oss << "Global variables:\n";
    for (size_t i = 0; i < vm.global.size(); ++i) {
        if (vm.global[i]) {
            oss << "  g[" << i << "] = " 
                << vm.global[i]->toDebugString() << "\n";
        }
    }
    
    // 栈变量
    oss << "\nStack:\n";
    for (size_t i = 0; i < vm.stack.size(); ++i) {
        oss << "  s[" << i << "] = " 
            << vm.stack[i].toDebugString() << "\n";
    }
    
    return oss.str();
}
```

#### 5.2 断点和单步调试支持
```cpp
// ZeusDebug/Debugger.hpp
class Debugger {
private:
    std::set<int> breakpoints;  // 行号断点
    bool stepping = false;
    
public:
    void addBreakpoint(int line) { breakpoints.insert(line); }
    void removeBreakpoint(int line) { breakpoints.erase(line); }
    
    bool shouldBreak(int line) const {
        return stepping || breakpoints.count(line) > 0;
    }
    
    void stepInto() { stepping = true; }
    void continue_() { stepping = false; }
    
    // 交互式调试REPL
    void debugREPL(VM& vm, int currentLine) {
        std::cout << "\n=== Debugger (line " << currentLine << ") ===\n";
        std::cout << "Commands: (s)tep, (c)ontinue, (p)rint <var>, (q)uit\n";
        
        while (true) {
            std::cout << "debug> ";
            std::string cmd;
            std::getline(std::cin, cmd);
            
            if (cmd == "s" || cmd == "step") {
                stepInto();
                break;
            } else if (cmd == "c" || cmd == "continue") {
                continue_();
                break;
            } else if (cmd.starts_with("p ")) {
                std::string varName = cmd.substr(2);
                // 打印变量值
                printVariable(vm, varName);
            } else if (cmd == "q" || cmd == "quit") {
                exit(0);
            }
        }
    }
};
```

---

## 实施优先级

### 第一阶段（核心）：
1. ✅ 创建统一的`ZeusException`基类
2. ✅ 定义错误码枚举系统
3. ✅ 实现调用栈追踪器
4. ✅ 在VM中集成调用栈
5. ✅ 替换所有`std::runtime_error`为`ZeusException`

### 第二阶段（增强）：
6. 增强错误消息（类型信息、建议）
7. 实现错误收集器（编译期多错误）
8. 添加相似名称建议功能
9. 改进源码行显示（带上下文）

### 第三阶段（高级）：
10. 实现断点调试系统
11. 添加性能分析功能
12. 国际化支持（错误消息多语言）
13. IDE集成（LSP错误格式）

---

## 示例：改进前后对比

### 改进前
```
[VM Error] Line 41: OBJ_LOAD_ATTR needs ObjectHandle
    (at IR instruction: OBJ_LOAD_ATTR anotherTestClass)
```

### 改进后
```
[Runtime Error E1600] test.zeu:41:5 in toString: Object has no attribute 'anotherTestClass'
  Details: Attempting to access 'anotherTestClass' on object of type 'JSON'
  | let test2 = new json.JSON.anotherTestClass("Testing");
                        ^^^^^

Call Stack (most recent first):
  [1] test.zeu:41 in toString
      | let test2 = new json.JSON.anotherTestClass("Testing");
  [2] test.zeu:10 in <top-level>
      | print(testjson.toString());

Local variables:
  this = <JSON object at 0x7f8a...>
  testjson = <JSON object at 0x7f8a...>

Available attributes on JSON:
  - data
  - init
  - toString

Suggestion: Class attributes are not accessible through instances. 
If 'anotherTestClass' is a nested class, use 'json.anotherTestClass' instead.
```

---

## 总结

这套错误处理方案提供了：

1. **统一的错误类型系统**：所有错误都继承自`ZeusException`
2. **完整的调用栈追踪**：清楚地看到错误传播路径
3. **详细的错误信息**：类型、值、建议都清晰可见
4. **智能的修复建议**：基于上下文的自动建议
5. **灵活的错误恢复**：编译期可收集多个错误
6. **强大的调试支持**：断点、单步、变量查看

实施这套方案后，Zeus的开发和调试体验将大幅提升！
