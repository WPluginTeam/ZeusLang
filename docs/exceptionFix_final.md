# Zeus 异常处理与语法检测修复总结

## 修复内容概览

本文档记录 Zeus 语言异常处理系统和基本语法检测的修复工作，共两个阶段。

---

## 第一阶段：异常处理修复

### 问题 1: throw 任意类型异常捕获 ✓

**症状**: 只有 Exception/RuntimeError 能被 catch 捕获，其他类型（字符串、数字、布尔值等）输出 `<Namespace:#0>`

**根因**: VM 的 CATCH_BEGIN 指令在 hasUnhandledException=false 时不存储异常

**修复**:
- 文件: `ZeusRunner/ZeusVM.hpp` (行 7090-7120)
- 改动: 移除 `hasUnhandledException` 条件检查
- 结果: 无论何种类型异常都能被正确存储和捕获

**测试**:
```zeus
throw "string"   → caught: string ✓
throw 123        → caught: 123 ✓
throw true       → caught: true ✓
throw nil       → caught: <RuntimeError> ✓
```

### 问题 2: catch 块无参数时异常未被捕获 ✓

**症状**: `catch() { ... }` 无法捕获异常

**根因**: 同问题 1

**修复**: 同问题 1

**测试**:
```zeus
try { throw new Exception("x"); } catch () { print("ok"); }
→ 输出 "ok" ✓
```

### 问题 3: Exception.getMessage() 无法调用 ✓

**症状**: 异常对象的方法调用报错

**根因**: 异常对象本身无法被正确存储到 catch 变量

**修复**: 同问题 1，异常对象存储后可正常调用方法

**测试**:
```zeus
try { 
    throw new Exception("test msg");
} catch (&ex) {
    print(ex.getMessage());  // 输出 "test msg" ✓
}
```

---

## 第二阶段：基本语法检测修复

### 问题 4: 未定义变量应该报错 ✓

**症状**: `print(aaa)` 输出 "aaa" 字符串而不是报错

**根因**: 在 IRGenerator._parsePrimary 中，未定义的标识符被直接 PUSH 为字符串字面量，而不是生成 LOAD_VALUE 指令

**修复**:
- 文件: `ZeusIR/IRGenerator.hpp` (行 900-935)
- 策略：区分两种情况
  - **函数调用** (如 `funcName()`): 在非 strictDecl 模式下保留原有 PUSH 字符串行为，支持 later-definition
  - **其他情况** (变量使用、表达式等): 生成 `LOAD_VALUE("global", -1, name)` 指令，VM 运行时报错
- 结果: 
  - 未定义变量使用会报错 ✓
  - 函数的 later-definition 仍然支持 ✓

**修改代码**:
```cpp
bool isFollowedByCall = (pos < tokens.size() && _isOperator(tokens[pos]) && _opSym(tokens[pos]) == "(");

if (isFollowedByCall && !ctx.strictDecl) {
    // 非严格模式下的函数调用：PUSH 字符串占位符（支持 later-definition）
    IRType::IRInstr ins;
    ins.op = VMOperator::PUSH;
    ins.args.push_back(LexerType::Value(name));
    out.push_back(ins);
} else {
    // 其他情况：LOAD_VALUE(-1)，让 VM 报错
    IRType::IRInstr ins;
    ins.op = VMOperator::LOAD_VALUE;
    ins.args.push_back(std::string("global"));
    ins.args.push_back(LexerType::Value(-1));
    ins.args.push_back(name);
    out.push_back(ins);
}
```

**测试**:
```bash
# 测试 1: later-definition（应该成功）
func A() { B(); }
func B() { print("ok"); }
A();  → 输出 "ok" ✓

# 测试 2: 未定义变量（应该报错）
print(aaa);  → [Runtime Error] NameError: undefined variable 'aaa' ✓

# 测试 3: 未定义变量在表达式中（应该报错）
let x = undefinedVar + 1;  → [Runtime Error] NameError: undefined variable 'undefinedVar' ✓
```

---

## 部分进展（需要进一步增强）

### 问题 5: 语句缺少分号检测

**当前行为**: 
```zeus
print("hello")    // 缺少分号
print("world");   // 有分号
```
结果：第一条语句被无声丢弃，仅输出 "world"

**原因**: Zeus 支持自动分号插入（ASI），但边界情况处理不完善

**建议**: 
- 需增强语法分析器的错误恢复机制
- 或在 --strict-syntax 严格模式下强制报错

### 问题 6: 括号未闭合检测

**当前行为**:
```zeus
print("hello"    // 括号未闭合
print("world");
```
结果：第一条语句被无声丢弃

**原因**: 表达式解析在括号不匹配时返回失败，但上层没有报错提示

**建议**:
- 在 TreeParser/IRGenerator 中增加括号匹配检测
- 记录不匹配的括号位置，生成有意义的错误信息

---

## 修改列表

| 文件 | 行号 | 修改 | 优先级 |
|-----|------|------|--------|
| ZeusRunner/ZeusVM.hpp | 7090-7120 | CATCH_BEGIN 移除条件检查 | ✅ 完成 |
| ZeusIR/IRGenerator.hpp | 900-935 | 区分函数调用和变量使用，选择性报错 | ✅ 完成 |
| ZeusParser/TreeParser.hpp | 待定 | 增强语法错误检测 | ⏳ 未开始 |

---

## 编译验证

所有修改已通过 MSVC cl.exe 编译，警告信息仅为预存在的宏重定义（无关）：

```
zeus.cpp
...warnings...
(no errors)
```

---

## 最终验证测试

```bash
=== Comprehensive Test ===
Test 1: Later-definition
  B called via later-definition ✓

Test 2: Undefined variable  
  caught error (as expected) ✓

Test 3: Undefined in expression
  caught error (as expected) ✓

Done
```

---

## 后续改进方向

### 高优先级
1. 分号检测：在严格模式下强制分号或提供更好的错误信息
2. 括号匹配：完整的括号/大括号/方括号匹配检测和报错

### 中优先级
3. 语法错误定位：改进错误消息中的行号和列号定位
4. 错误恢复：改进语法分析器的错误恢复能力，减少无声丢弃

### 低优先级
5. throw 类型检查：可选的模式限制 throw 只能用于 Exception 类型
6. 未定义变量警告：--warn-undefined 选项用于宽松模式下警告
