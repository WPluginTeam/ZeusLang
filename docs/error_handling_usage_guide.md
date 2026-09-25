# 错误处理系统使用指南

## 概述

Zeus 现在拥有一个统一、强大的错误处理系统，包含以下特性：
- **错误分类**：按类别（Lexer, Parser, Compiler, Runtime等）组织错误
- **错误码**：使用 E1000-E1999 编号系统，便于文档查找
- **调用栈追踪**：显示完整的函数调用链（VM层）
- **详细上下文**：显示源代码、变误位置等
- **智能建议**：提供修复建议

## ✅ 已实现的错误检测

### 运行时错误（VM Runtime）

#### 1. E1301 - Const常量修改
尝试修改const声明的变量时触发。
```zeu
let const pi = 3.14;
pi = 3.15;  // Error: Cannot assign to constant 'pi'
```
**错误输出示例**:
```
[Runtime Error E1301] Line 5: Cannot assign to constant 'pi'
  Details: The variable 'pi' was declared as 'const' and cannot be modified

Suggestion: If you need to modify this variable, declare it with 'let' instead of 'const'
```

#### 2. E1904 - For循环步长为0
for循环步长不能为0，否则会导致无限循环。
```zeu
for (1..10..0) [i] {}  // Error: For loop step cannot be zero
```
**错误输出示例**:
```
[Runtime Error E1904] Line 5: For loop step cannot be zero
  Details: A zero step would cause an infinite loop

Suggestion: Use a non-zero step value in for (start..end..step)
```

#### 3. E1904 - For循环负数结束值
for循环结束值不能为负数。
```zeu
for (1..-5..1) [i] {}  // Error: For loop end value cannot be negative
```
**错误输出示例**:
```
[Runtime Error E1904] Line 5: For loop end value cannot be negative
  Details: For loop range must have a non-negative end value

Suggestion: Check the loop range: for (start..end..step) requires end >= 0
```

### 属性访问错误

#### 4. E1600 - 类上访问实例成员
尝试在类本身而非实例上访问实例成员。
```zeu
let obj = ClassName.instanceMember;  // Error: Cannot access instance member
```
**错误输出示例**:
```
[Attribute Error E1600] Line 10: Cannot access instance member 'test' on class 'JSON'
  Details: You are trying to access an instance member through the class itself

Suggestion: To access instance members, create an instance first: 'let obj = new JSON(...); obj.test'
Or if 'test' should be a static member, Zeus doesn't support static members yet
```

### 类型错误

#### 5. E1404 - 期望对象类型
尝试在非对象类型上访问属性。
```zeu
let num = 123;
print(num.length);  // Error: Expected object
```

## 错误码分类

### 通用错误 (1000-1099)
- `E1000_UNKNOWN_ERROR` - 未知错误
- `E1001_INTERNAL_ERROR` - 内部错误

### 词法错误 (1100-1199)
- `E1100_INVALID_TOKEN` - 非法token
- `E1101_UNTERMINATED_STRING` - 未闭合的字符串
- `E1102_INVALID_NUMBER` - 非法数字

### 语法错误 (1200-1299)
- `E1200_UNEXPECTED_TOKEN` - 意外的token
- `E1201_MISSING_PAREN` - 缺少括号
- `E1202_MISSING_BRACE` - 缺少花括号
- `E1203_INVALID_SYNTAX` - 非法语法

### IR编译错误 (1300-1399)
- `E1300_UNDEFINED_VARIABLE` - 未定义变量
- `E1301_CONST_ASSIGNMENT` - 常量赋值
- `E1302_INVALID_OPERATOR` - 非法操作符
- `E1303_FUNCTION_NOT_FOUND` - 函数未找到
- `E1304_INVALID_EXPRESSION` - 非法表达式
- `E1305_DUPLICATE_DECLARATION` - 重复声明

### 类型错误 (1400-1499)
- `E1400_TYPE_MISMATCH` - 类型不匹配
- `E1401_NOT_CALLABLE` - 不可调用
- `E1402_NOT_ITERABLE` - 不可迭代
- `E1403_INVALID_ARGUMENT_TYPE` - 参数类型错误
- `E1404_EXPECTED_OBJECT` - 期望对象
- `E1405_EXPECTED_DICT` - 期望字典
- `E1406_EXPECTED_LIST` - 期望列表
- `E1407_EXPECTED_STRING` - 期望字符串
- `E1408_EXPECTED_NUMBER` - 期望数字

### 名称错误 (1500-1599)
- `E1500_UNDEFINED_NAME` - 未定义名称
- `E1501_UNDEFINED_ATTRIBUTE` - 未定义属性
- `E1502_UNDEFINED_METHOD` - 未定义方法

### 属性错误 (1600-1699)
- `E1600_ATTRIBUTE_NOT_FOUND` - 属性未找到
- `E1601_NO_SUCH_ATTRIBUTE` - 无此属性
- `E1602_ATTRIBUTE_READ_ONLY` - 只读属性

### 索引错误 (1700-1799)
- `E1700_INDEX_OUT_OF_RANGE` - 索引越界
- `E1701_INVALID_INDEX_TYPE` - 非法索引类型
- `E1702_KEY_NOT_FOUND` - 键不存在

### 导入错误 (1800-1899)
- `E1800_MODULE_NOT_FOUND` - 模块未找到
- `E1801_IMPORT_CYCLE` - 导入循环
- `E1802_PACKAGE_NOT_FOUND` - 包未找到

### VM运行时错误 (1900-1999)
- `E1900_STACK_OVERFLOW` - 栈溢出
- `E1901_STACK_UNDERFLOW` - 栈不足
- `E1902_NULL_REFERENCE` - 空引用
- `E1903_DIVISION_BY_ZERO` - 除零错误
- `E1904_INVALID_OPERATION` - 非法操作
- `E1905_INSUFFICIENT_ARGS` - 参数不足

## 使用方法

### 1. 在IR编译阶段抛出错误

#### 使用便捷宏

```cpp
// 简单错误（使用默认错误码）
IR_THROW_NODE(line, node, "Error message", irCompilerStatus);

// 带错误码的错误
IR_THROW_WITH_CODE(
    IRExceptionNS::ErrorCode::E1304_INVALID_EXPRESSION,
    IRExceptionNS::ErrorCategory::IR_COMPILER,
    line, node, "Invalid expression", irCompilerStatus
);

// 未定义变量（专用宏）
IR_THROW_UNDEFINED_VAR(line, node, "myVar", irCompilerStatus);

// 常量赋值错误（专用宏）
IR_THROW_CONST_ASSIGNMENT(line, node, "myConst", irCompilerStatus);
```

#### 手动构造异常（用于添加详细信息）

```cpp
IRExceptionNS::IRException ex(
    IRExceptionNS::ErrorCode::E1300_UNDEFINED_VARIABLE,
    IRExceptionNS::ErrorCategory::NAME_ERROR,
    "Variable 'x' is not defined",
    line, node
);
ex.setDetails("Variable 'x' is referenced but never declared");
ex.setSuggestion("Did you mean 'y'? Or declare 'x' before using it.");
ex._throw(irCompilerStatus);
```

### 2. 在VM运行时抛出错误

#### 使用便捷函数

```cpp
// 类型错误
auto ex = VMException::typeError_ExpectedType(
    "object", "number", line, instr
);
ex._throw(vmStatus, chunkIndex);

// 属性不存在
auto ex = VMException::attributeError_NotFound(
    "name", "Number", line, instr
);
ex._throw(vmStatus, chunkIndex);

// 索引越界
auto ex = VMException::indexError_OutOfRange(
    index, size, line, instr
);
ex._throw(vmStatus, chunkIndex);

// 键不存在
auto ex = VMException::keyError_NotFound(
    "myKey", line, instr
);
ex._throw(vmStatus, chunkIndex);
```

#### 添加调用栈信息

```cpp
VMException::VMException ex(
    VMException::ErrorCode::E1400_TYPE_MISMATCH,
    VMException::ErrorCategory::TYPE_ERROR,
    "Type mismatch",
    line, instr
);

// 添加栈帧
ex.addStackFrame("myFunction", 10, "let x = someFunc()");
ex.addStackFrame("main", 5, "myFunction()");

// 添加局部变量快照
VMException::StackFrame frame;
frame.functionName = "calculate";
frame.line = 42;
frame.sourceCode = "let result = a / b";
frame.localVars["a"] = "10";
frame.localVars["b"] = "0";  // 导致除零错误
ex.addStackFrame(frame);

ex._throw(vmStatus, chunkIndex);
```

## 错误输出示例

### 编译时错误示例

```
[Compiler Error E1300] Line 15: Undefined variable 'userName'
    | print(userName)
  Suggestion: Check for typos or declare the variable before using it.
```

### 运行时错误示例（带调用栈）

```
[Type Error E1400] Line 42 in calculate: Expected number but got string
  | let result = a / b
  Details: Type mismatch: expected number, actual string

Call Stack (most recent first):
  [1] in calculate (line 42)
      | let result = a / b
  [2] in processData (line 28)
      | let x = calculate(input)
  [3] in <top-level> (line 5)
      | processData(userInput)

Local variables:
  a = "hello"
  b = 10

Suggestion: Check the type of the value being used.
```

### 属性错误示例

```
[Attribute Error E1600] Line 20: Object of type 'Number' has no attribute 'length'
  | print(myNumber.length)
  Details: Attempting to access non-existent attribute 'length'
```

## 最佳实践

### 1. 选择合适的错误类别
- 编译期问题使用 `IRException`
- 运行期问题使用 `VMException`

### 2. 提供有用的建议
```cpp
ex.setSuggestion("Did you mean 'userName' instead of 'user_name'?");
```

### 3. 添加详细上下文
```cpp
ex.setDetails("Expected: number\nGot: string\nValue: \"hello\"");
```

### 4. 使用专门的便捷函数
优先使用 `typeError_ExpectedType()` 等函数，而不是手动构造。

### 5. 维护调用栈
在VM执行关键操作时记录栈帧信息：
```cpp
// 进入函数时
currentStackFrame.functionName = funcName;
currentStackFrame.line = currentLine;

// 发生错误时
ex.addStackFrame(currentStackFrame);
```

## 未来扩展

计划中的功能：
- **ErrorCollector**：收集编译期多个错误
- **相似名称检测**：在名称错误时建议相似的已定义名称
- **变量状态快照**：自动捕获错误发生时的变量值
- **源码高亮**：在终端中高亮错误位置
- **错误恢复**：编译器尝试从错误中恢复继续编译

## 参考

完整的错误处理改进计划见：`docs/error_handling_improvement_plan.md`
