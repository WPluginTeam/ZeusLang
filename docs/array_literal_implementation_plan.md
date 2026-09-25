# Zeus 嵌入式Array修改方案

## 目标
将Zeus中的所有列表字面量`[...]`在IR生成阶段直接转换为Array对象创建,而不是等到运行时。

## 当前状态分析

### 1. 现有的Array类 (ZeusType.hpp)
- `ZeusType::Array` 已经存在
- `ZeusType::ArrayImpl` 包含 `std::vector<UniversalVariant> data`
- Array已经可以作为Variant的一部分使用

### 2. 现有的列表字面量处理 (TreeParser.hpp)
```cpp
// 当前: [1, 2, 3] 被解析为 TreeType::List
// TreeType::List 包含 std::vector<TreeType::Node> items
```

### 3. 现有的IR生成 (IRGenerator.hpp)
```cpp
// 当前可能的处理方式:
// - PUSH 每个元素
// - 调用某个builtin函数组装成列表?
// 或者: 还没有完整实现列表字面量的IR生成?
```

## 修改方案

### 阶段1: 添加IR指令 `MAKE_ARRAY`

**文件**: `ZeusIR/IRType.hpp`

添加新的IR操作码:
```cpp
enum class IROperator {
    // ... 现有操作码 ...
    MAKE_ARRAY,      // 从栈顶N个元素创建Array对象
    // ...
};
```

### 阶段2: IRGenerator中处理List节点

**文件**: `ZeusIR/IRGenerator.hpp`

在 `IRGenerator::generate()` 中添加对 `TreeType::List` 的处理:

```cpp
case TreeType::NodeType::List: {
    const auto& list = std::get<TreeType::List>(node.data);
    
    // 1. 为每个列表元素生成IR
    for (const auto& item : list.items) {
        generate(item, env, instr_list);
    }
    
    // 2. 生成 MAKE_ARRAY 指令
    // 参数为元素个数
    IRType::Instruction makeArray;
    makeArray.op = IROperator::MAKE_ARRAY;
    makeArray.args.push_back(LexerType::Value((int)list.items.size()));
    makeArray.line = node.line;
    instr_list.push_back(makeArray);
    
    break;
}
```

**详细逻辑**:
1. 遍历列表中的每个元素,为每个元素生成IR(压入栈)
2. 生成`MAKE_ARRAY n`指令,其中n是元素个数
3. VM执行时会从栈顶弹出n个元素,创建Array对象并压回栈

### 阶段3: VM中实现MAKE_ARRAY指令

**文件**: `ZeusRunner/ZeusVM.hpp`

在VM的execute循环中添加MAKE_ARRAY的处理:

```cpp
case VMOperator::MAKE_ARRAY: {
    // 1. 从指令参数中获取元素个数
    if (instr.args.empty() || !std::holds_alternative<LexerType::Value>(instr.args[0])) {
        VMException::VMException exp("MAKE_ARRAY missing element count", __line, instr);
        exp._throw(vmstat, currentChunk);
        break;
    }
    
    int count = VMHelpers::getIntFromValue(std::get<LexerType::Value>(instr.args[0]));
    
    // 2. 检查栈深度
    if ((int)stack.size() < count) {
        VMException::VMException exp(
            "MAKE_ARRAY expects " + std::to_string(count) + " elements on stack, but only " + 
            std::to_string(stack.size()) + " available",
            __line, instr
        );
        exp._throw(vmstat, currentChunk);
        break;
    }
    
    // 3. 从栈中弹出元素(注意顺序: 最后压入的是最后一个元素)
    std::vector<ZeusType::UniversalVariant> elements;
    elements.reserve(count);
    
    for (int i = 0; i < count; ++i) {
        elements.push_back(stack.back());
        stack.pop_back();
    }
    
    // 4. 反转元素顺序(因为栈是LIFO)
    std::reverse(elements.begin(), elements.end());
    
    // 5. 创建Array对象
    ZeusType::Array arr;
    arr.impl = std::make_shared<ZeusType::ArrayImpl>();
    arr.impl->data = std::move(elements);
    
    // 6. 包装为Variant并压入栈
    ZeusType::Variant variantArray = arr;
    stack.push_back(ZeusType::UniversalVariant(variantArray));
    
    printStack();
    break;
}
```

### 阶段4: 更新IROperator到VMOperator的映射

**文件**: `ZeusIRReader/IRRead.hpp`

在 `IROperatorToVMOperator()` 函数中添加映射:

```cpp
inline VMOperator IROperatorToVMOperator(IROperator op) {
    switch (op) {
        // ... 现有映射 ...
        case IROperator::MAKE_ARRAY: return VMOperator::MAKE_ARRAY;
        // ...
    }
}
```

同时在 `VMOperator` 枚举中添加:

```cpp
enum class VMOperator {
    // ... 现有操作码 ...
    MAKE_ARRAY,
    // ...
};
```

### 阶段5: 移除旧的Array构造方式(可选)

如果之前使用 `new Array([...])` 的方式,可以考虑:
1. 保留 `new Array([])` 用于空数组
2. `new Array([1, 2, 3])` 可以在IR阶段优化为 `MAKE_ARRAY`
3. 或者保留两种方式并存

## 实现顺序

1. ✅ **步骤1**: 在 `IRType.hpp` 中添加 `IROperator::MAKE_ARRAY`
2. ✅ **步骤2**: 在 `ZeusVM.hpp` 中添加 `VMOperator::MAKE_ARRAY` 
3. ✅ **步骤3**: 在 `IRRead.hpp` 中添加映射
4. ✅ **步骤4**: 在 `IRGenerator.hpp` 中实现List节点的IR生成
5. ✅ **步骤5**: 在 `ZeusVM.hpp` 中实现MAKE_ARRAY的执行逻辑
6. ✅ **步骤6**: 测试基本用例
7. ✅ **步骤7**: 测试嵌套数组 `[[1, 2], [3, 4]]`
8. ✅ **步骤8**: 测试混合类型 `[1, "hello", true, [2, 3]]`

## 测试用例

### 测试1: 基本数组字面量
```zeus
let arr = [1, 2, 3, 4, 5];
print(arr);  // 应该输出: [1, 2, 3, 4, 5]
print(len(arr));  // 应该输出: 5
```

### 测试2: 空数组
```zeus
let empty = [];
print(empty);  // 应该输出: []
print(len(empty));  // 应该输出: 0
```

### 测试3: 混合类型
```zeus
let mixed = [1, "hello", true, 3.14];
print(mixed);  // 应该输出: [1, "hello", true, 3.14]
```

### 测试4: 嵌套数组
```zeus
let nested = [[1, 2], [3, 4], [5, 6]];
print(nested);  // 应该输出: [[1, 2], [3, 4], [5, 6]]
print(nested[0]);  // 需要array_get支持
```

### 测试5: 表达式元素
```zeus
let x = 10;
let arr = [x + 1, x * 2, x - 5];
print(arr);  // 应该输出: [11, 20, 5]
```

## 注意事项

### 1. 与现有Array类的兼容性
- 确保 `new Array([1, 2, 3])` 和 `[1, 2, 3]` 创建的是**相同类型**的对象
- 所有Array方法(append, get, length等)都应该正常工作

### 2. 性能考虑
- `MAKE_ARRAY` 指令应该一次性分配足够的空间(reserve)
- 避免多次内存重分配

### 3. 错误处理
- 栈深度不足时的错误提示
- 元素类型检查(如果需要)

### 4. 调试支持
- `--debug-channels=IR` 应该能看到 `MAKE_ARRAY n` 指令
- printStack 应该正确显示Array对象

## 优势

1. **性能提升**: 直接在IR阶段确定数组结构,减少运行时开销
2. **类型统一**: 所有数组都是 `ZeusType::Array`,不需要特殊的"列表"类型
3. **简化语法**: `[1, 2, 3]` 比 `new Array([1, 2, 3])` 更简洁
4. **兼容性好**: 与现有的Array方法完全兼容

## 可能的问题

### 问题1: TreeParser是否已经支持List?
**检查**: 查看 `TreeParser.hpp` 中是否有对 `[...]` 语法的解析
**解决**: 如果没有,需要先在Parser中添加List节点的解析

### 问题2: 当前new Array([...])的实现
**检查**: 查看当前 `new Array([...])` 是如何处理的
**解决**: 确保不冲突,或者将其重构为使用 `MAKE_ARRAY`

### 问题3: Array索引访问
**检查**: `arr[0]` 语法是否已经支持?
**解决**: 可能需要添加 `ARRAY_INDEX` 指令或使用现有的 `DICT_LOAD_VALUE`

## 文件清单

需要修改的文件:
1. ✅ `ZeusIR/IRType.hpp` - 添加 IROperator::MAKE_ARRAY
2. ✅ `ZeusRunner/ZeusVM.hpp` - 添加 VMOperator::MAKE_ARRAY + 执行逻辑
3. ✅ `ZeusIRReader/IRRead.hpp` - 添加操作码映射
4. ✅ `ZeusIR/IRGenerator.hpp` - 实现List节点的IR生成
5. ⚠️ `ZeusParser/TreeParser.hpp` - 确认List解析是否存在(可能需要添加)

## 预期效果

修改完成后:
```zeus
// 旧方式(仍然支持)
let arr1 = new Array([1, 2, 3]);

// 新方式(更简洁)
let arr2 = [1, 2, 3];

// 两者完全等价
print(arr1);  // [1, 2, 3]
print(arr2);  // [1, 2, 3]
```

生成的IR:
```
PUSH 1
PUSH 2
PUSH 3
MAKE_ARRAY 3
STORE_VALUE_LOCAL 0
```

---

**总结**: 这个方案将把Zeus的列表字面量从运行时构造改为编译时确定的IR指令,使语法更简洁,性能更好,同时保持与现有Array类型的完全兼容。
