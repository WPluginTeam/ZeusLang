# Zeus错误处理改进对比

## 改进概述

Zeus错误处理系统已从简单的错误消息升级为结构化、信息丰富的错误报告系统。

---

## 1. Const常量修改错误

### 改进前
```
[VM Error] Line 8: Assign to const global index 15
  (at IR instruction: ASSIGN_VALUE_GLOBAL 15)
```

### 改进后
```
[Runtime Error E1301] Line 8: Cannot assign to constant 'pi'
  Details: The variable 'pi' was declared as 'const' and cannot be modified

Suggestion: If you need to modify this variable, declare it with 'let' instead of 'const'
```

**改进点**:
- ✅ 显示变量名而非索引号
- ✅ 错误分类（Runtime Error）
- ✅ 错误码（E1301）
- ✅ 详细说明
- ✅ 修复建议

---

## 2. For循环步长错误

### 改进前
```
（未实现检测，会导致无限循环或未定义行为）
```

### 改进后
```
[Runtime Error E1904] Line 5: For loop step cannot be zero
  Details: A zero step would cause an infinite loop

Suggestion: Use a non-zero step value in for (start..end..step)
```

**改进点**:
- ✅ 新增步长为0的检测
- ✅ 清晰的错误说明
- ✅ 防止无限循环

---

## 3. For循环负数结束值

### 改进前
```
[VM Error] Line X: COUNT_CHECK_NEG negative end
```

### 改进后
```
[Runtime Error E1904] Line 5: For loop end value cannot be negative
  Details: For loop range must have a non-negative end value

Suggestion: Check the loop range: for (start..end..step) requires end >= 0
```

**改进点**:
- ✅ 更清晰的错误描述
- ✅ 错误码分类
- ✅ 使用建议

---

## 4. 类成员访问错误

### 改进前
```
[VM Error] Line 41: OBJ_LOAD_ATTR needs ObjectHandle
  (at IR instruction: OBJ_LOAD_ATTR test)
```

### 改进后
```
[Attribute Error E1600] Line 41: Cannot access instance member 'test' on class 'JSON'
  Details: You are trying to access an instance member through the class itself

Suggestion: To access instance members, create an instance first: 'let obj = new JSON(...); obj.test'
Or if 'test' should be a static member, Zeus doesn't support static members yet
```

**改进点**:
- ✅ 识别类vs实例的区别
- ✅ 显示类名和成员名
- ✅ 详细的上下文说明
- ✅ 提供具体的修复方案
- ✅ 说明语言限制（静态成员未实现）

---

## 改进特性总结

### 错误消息结构
```
[错误类别 错误码] Line X: 主要错误信息
  Details: 详细说明
  
Suggestion: 修复建议
```

### 新增功能
1. **错误分类**: Runtime, Type, Attribute, Index等
2. **错误编号**: E1000-E1999系统化编码
3. **变量名解析**: 显示实际变量名而非内部索引
4. **类型识别**: 区分类、实例、函数等不同类型
5. **智能建议**: 根据错误类型提供针对性建议
6. **上下文信息**: 显示相关的代码和状态

### 错误码体系

| 范围 | 类别 | 示例 |
|------|------|------|
| 1000-1099 | 通用错误 | E1000_UNKNOWN_ERROR |
| 1300-1399 | 编译/IR错误 | E1301_CONST_ASSIGNMENT |
| 1400-1499 | 类型错误 | E1404_EXPECTED_OBJECT |
| 1500-1599 | 名称错误 | E1500_UNDEFINED_NAME |
| 1600-1699 | 属性错误 | E1600_ATTRIBUTE_NOT_FOUND |
| 1700-1799 | 索引错误 | E1700_INDEX_OUT_OF_RANGE |
| 1900-1999 | VM运行时错误 | E1904_INVALID_OPERATION |

### 实现文件
- `ZeusRunner/ZeusVMException.hpp`: VM运行时错误框架
- `ZeusIR/IRException.hpp`: IR编译期错误框架
- `ZeusRunner/ZeusVM.hpp`: 具体错误检测实现
- `ZeusIR/IRFor.hpp`: For循环错误检测

### 未来扩展方向
1. **调用栈追踪**: 显示完整的函数调用链（框架已就绪）
2. **变量状态快照**: 错误发生时的变量值
3. **相似名称建议**: "Did you mean 'userName'?"
4. **错误恢复**: 编译器收集多个错误而非首次失败
5. **源码高亮**: 终端中高亮错误位置

---

## 使用示例

### 测试文件
见以下测试文件：
- `test_error_const.zeu` - Const修改错误
- `test_for_zero_step.zeu` - 步长为0错误  
- `test_error_summary.zeu` - 错误处理总结
- `demo_error_handling.zeu` - 综合演示

### 文档
- `docs/error_handling_usage_guide.md` - 详细使用指南
- `docs/error_handling_improvement_plan.md` - 完整改进计划
