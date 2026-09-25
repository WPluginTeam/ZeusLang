# Zeus Array字面量语法实现计划

## 当前状态

### ✅ 已有组件
1. **Array类**: `ZeusBuiltins/list.zeu` 中已定义
   - 构造函数: `init(arr)` - 接受一个数组参数
   - 内部存储: `this.data = []` - 使用底层列表
   - 方法: append, pop, length, get 等

2. **底层列表支持**: VM已支持 `[]` 和 `array_push` 等操作

3. **类实例化**: 支持 `new Array([1, 2, 3])`

### ❌ 缺失组件
1. **Parser**: 没有 `[...]` 字面量的解析
2. **TreeType**: 没有 List 节点类型
3. **IRGenerator**: 没有 List → Array 的转换

## 实现方案: 语法糖转换

### 核心理念
`[1, 2, 3]` 是 `new Array([1, 2, 3])` 的语法糖

**问题**: 构造参数本身就是 `[1, 2, 3]`,这是循环定义!

**解决方案**: 底层列表 vs 高层Array对象
- **底层列表**: VM原生的 `[]` ,用于内部存储
- **高层Array**: Zeus类,提供方法接口

所以:
- `[1, 2, 3]` 在Parser阶段生成**底层列表**
- `new Array([1, 2, 3])` 将底层列表包装成**Array对象**

## 详细实现步骤

### 阶段1: 检查底层列表实现

**问题**: VM中是否已经支持底层的 `[]` 列表?

让我检查:
1. `let x = [];` 是否能工作?
2. `x[0]` 索引访问是否能工作?
3. `array_push(x, 1)` 是否能工作?

**测试代码**:
```zeus
let x = [];
array_push(x, 1);
array_push(x, 2);
print(x);  // 期望输出: [1, 2] 或类似
```

### 阶段2: 添加List字面量解析 (如果需要)

如果底层列表已支持,那么Parser可能已经能解析 `[1, 2, 3]`。

**检查方式**: 
```zeus
let x = [1, 2, 3];  // 直接测试能否编译
print(x);
```

如果报错"unexpected [",则需要添加Parser支持。

### 阶段3A: 如果底层列表已存在

那么只需要修改Array的使用方式:

**当前用法**:
```zeus
import ZeusBuiltins.list;
let arr = new Array([1, 2, 3]);  // [1, 2, 3]是底层列表
```

**目标用法**:
```zeus
import ZeusBuiltins.list;
let arr = new Array([1, 2, 3]);  // 同上,但更自然
// 或者直接:
let arr2 = [1, 2, 3];  // 返回底层列表,不是Array对象!
```

**问题**: 这样的话,[1, 2, 3] 返回的是底层列表,没有 `.append()` 等方法!

**解决**: 需要语法糖转换!

### 阶段3B: 添加语法糖转换

在IRGenerator中,遇到List字面量时:

```cpp
// 伪代码
if (node是List字面量 && 在顶层表达式位置) {
    生成: new Array(list)
} else if (node是List字面量 && 在函数调用参数位置) {
    保持: 底层列表
}
```

**例子**:
```zeus
let arr = [1, 2, 3];           // 转换为: new Array([1, 2, 3])
let result = arr.get([0, 1]);  // [0, 1] 保持为底层列表参数
```

**问题**: 如何判断"顶层表达式位置" vs "参数位置"?

### 阶段4: 简化方案 - 统一使用Array

**更简单的方案**: 所有 `[...]` 都转换为 `Array`

```cpp
case TreeType::NodeType::List: {
    // 总是生成 new Array(...)
    const auto& list = std::get<TreeType::List>(node.data);
    
    // 1. 加载Array类
    generateLoadGlobal("Array", env, instr_list);
    
    // 2. 生成底层列表 (假设VM支持列表字面量)
    //    或者逐个PUSH元素
    for (const auto& item : list.items) {
        generate(item, env, instr_list);
    }
    
    // 3. 创建底层列表对象
    generateMakeList(list.items.size(), instr_list);
    
    // 4. 调用Array构造函数
    generateCallNew(1, instr_list);
    
    break;
}
```

**等等**: 这需要VM支持"创建列表对象"的指令!

## 重新审视问题

### 当前VM的列表能力

让我先确认VM中到底有没有"列表"类型:

查看 `ZeusType.hpp`:
- `ZeusType::Array` - 这是高层Array对象
- 是否有原生的 List/Vector 类型?

查看现有的array builtin函数:
- `array_push(arr, item)` - 第一个参数是什么类型?

### 关键问题

**array_push 的第一个参数是什么?**

如果是 `ZeusType::Array`,那么所有列表都必须是Array对象。
如果是原生列表,那么需要区分"底层列表"和"Array对象"。

## 最简方案推荐

基于你说的"不要提前优化",我建议:

### 方案: 所有 [.​..] 都是 Array 对象

1. **Parser**: 识别 `[1, 2, 3]` 语法
2. **TreeType**: 添加 List 节点
3. **IRGenerator**: 
   ```cpp
   遇到 [1, 2, 3] →
   生成:
     LOAD_GLOBAL Array
     PUSH 1
     PUSH 2
     PUSH 3
     // 问题: 如何传递这3个元素?
   ```

**核心问题**: Array.init(arr) 需要一个列表参数,但我们手头只有3个独立的值!

### 解决方案A: 修改Array构造函数

```zeus
class Array {
    func init(...items) {  // 可变参数!
        this.data = [];
        for (0..len(items)) [&i] {
            array_push(this.data, items[i]);
        }
    }
}
```

这样 `new Array(1, 2, 3)` 就可以工作!

IR生成:
```
LOAD_GLOBAL Array
PUSH 1
PUSH 2
PUSH 3
CALL_NEW 3  // 3个参数
```

### 解决方案B: 使用工厂函数

```zeus
func makeArray(...items) {
    let arr = new Array();
    for (0..len(items)) [&i] {
        arr.append(items[i]);
    }
    return arr;
}
```

IR生成:
```
LOAD_GLOBAL makeArray
PUSH 1
PUSH 2
PUSH 3
CALL_CLOSURE 3
```

## 我的最终建议

### 步骤1: 先测试当前能力
创建测试文件检查:
1. Zeus是否支持可变参数?
2. Array.init 是否可以改为可变参数?

### 步骤2: 选择方案
- 如果支持可变参数 → 用方案A (修改Array构造函数)
- 如果不支持 → 用方案B (工厂函数)

### 步骤3: 实现Parser和IRGenerator
- 添加List节点类型
- 添加List解析逻辑
- 添加List→Array转换

### 步骤4: 后续优化
用Hotspot识别Array创建模式,优化为直接的内存分配

---

**下一步**: 我需要你确认:
1. Zeus是否支持可变参数 `func f(...args)`?
2. 你更倾向于方案A还是方案B?
3. 或者有更好的想法?
