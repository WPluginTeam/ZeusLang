# Zeus 数组操作 API 文档

## 概述

Zeus 现已支持通过内置函数（builtins）进行数组操作。虽然不是完全的面向对象接口（如 `arr.push()`），但提供了功能完整的数组操作函数。

---

## 数组基础

### 创建数组

```zeus
let arr = [];           // 空数组
let nums = [1, 2, 3];   // 包含元素的数组
```

---

## 数组操作函数

### `len(arr)` - 获取长度

返回数组的元素数量。

```zeus
let arr = [10, 20, 30];
raw_print(len(arr));  // 输出: 3
```

**参数:**
- `arr`: 数组对象

**返回:** 整数，数组长度

---

### `array_push(arr, item)` - 追加元素

向数组末尾添加一个元素，返回新的数组长度。

```zeus
let arr = [];
array_push(arr, 10);
array_push(arr, 20);
// arr 现在是 [10, 20]
```

**参数:**
- `arr`: 数组对象
- `item`: 要添加的元素

**返回:** 整数，添加后的数组长度

---

### `array_pop(arr)` - 弹出元素

移除并返回数组末尾的元素。

```zeus
let arr = [10, 20, 30];
let last = array_pop(arr);
raw_print(last);  // 输出: 30
// arr 现在是 [10, 20]
```

**参数:**
- `arr`: 数组对象

**返回:** 被移除的元素

**注意:** 如果数组为空会抛出运行时错误

---

### `array_get(arr, index)` - 获取元素

获取指定索引位置的元素（索引从 0 开始）。

```zeus
let arr = [10, 20, 30];
let x = array_get(arr, 1);
raw_print(x);  // 输出: 20
```

**参数:**
- `arr`: 数组对象
- `index`: 整数索引（0-based）

**返回:** 该位置的元素

**注意:** 索引越界会抛出运行时错误

---

### `array_set(arr, index, value)` - 设置元素

设置指定索引位置的元素值。

```zeus
let arr = [10, 20, 30];
array_set(arr, 1, 99);
// arr 现在是 [10, 99, 30]
```

**参数:**
- `arr`: 数组对象
- `index`: 整数索引（0-based）
- `value`: 新的元素值

**返回:** 设置的值

**注意:** 索引越界会抛出运行时错误

---

### `array_clear(arr)` - 清空数组

移除数组中的所有元素。

```zeus
let arr = [10, 20, 30];
array_clear(arr);
raw_print(len(arr));  // 输出: 0
```

**参数:**
- `arr`: 数组对象

**返回:** 整数 0

---

### `array_slice(arr, start, end)` - 切片

返回数组的子集，范围是 `[start, end)`（不包含 end）。

```zeus
let arr = [10, 20, 30, 40, 50];
let sub = array_slice(arr, 1, 4);
// sub 是 [20, 30, 40]
```

**参数:**
- `arr`: 数组对象
- `start`: 起始索引（包含）
- `end`: 结束索引（不包含）

**返回:** 新的数组对象

**特性:**
- 负数索引会被调整为 0
- 超出范围的索引会被调整为数组大小
- 如果 `start > end`，返回空数组

---

## 完整示例

### 示例 1: 栈操作

```zeus
// 使用数组模拟栈
let stack = [];

// 入栈
array_push(stack, "A");
array_push(stack, "B");
array_push(stack, "C");

raw_print("栈深度:", len(stack));  // 3

// 出栈
while (len(stack) > 0) [_] {
    let item = array_pop(stack);
    raw_print("弹出:", item);
}
```

### 示例 2: 数组遍历与修改

```zeus
let nums = [1, 2, 3, 4, 5];

// 将所有元素乘以 2
for (0..len(nums)-1) [i] {
    let val = array_get(nums, i);
    array_set(nums, i, val * 2);
}

// 输出结果
for (0..len(nums)-1) [i] {
    raw_print(array_get(nums, i));
}
// 输出: 2, 4, 6, 8, 10
```

### 示例 3: 数组过滤（创建新数组）

```zeus
let nums = [1, 5, 10, 15, 20, 25];
let filtered = [];

// 过滤出大于 10 的元素
for (0..len(nums)-1) [i] {
    let val = array_get(nums, i);
    if (val > 10) {
        array_push(filtered, val);
    }
}

raw_print("过滤后:", len(filtered), "个元素");
// filtered 是 [15, 20, 25]
```

---

## 实现方式说明

### 当前状态

✅ **已实现:**
- 数组数据结构（`ZeusType::Array`, `ArrayData`）
- VM 指令支持（`LIST_BUILD`, `LIST_LOAD_ELEM`, `LIST_STORE_ELEM`）
- 内置函数接口（上述所有函数）

❌ **未实现:**
- 面向对象的方法调用（如 `arr.push(x)`、`arr.length()`）
- 运算符重载（如 `arr + [1, 2]`、`arr[i]` 语法糖）

### 为什么不是面向对象？

Zeus 的数组操作目前通过**内置函数**而非**对象方法**实现，原因：

1. **简单快速**: 内置函数直接暴露底层操作，无需额外的类型系统包装
2. **性能**: 避免方法查找和对象绑定的开销
3. **兼容性**: 不需要修改现有的 OOP 框架和 IR 生成器

### 未来规划

如果需要面向对象接口，可以实现以下两个方向之一：

**方案 A: 扩展 OBJ_CALL_METHOD**
- 在 VM 中检测 Array 类型
- 路由到内置方法处理器
- 语法: `arr.push(x)`, `arr.length()`

**方案 B: 纯 Zeus 包装类**
- 创建 Zeus 类封装数组操作
- 利用现有内置函数
- 语法: `list.push(x)`, `list.get(i)`

---

## 性能提示

1. **避免频繁的 get/set**: 如果需要遍历大数组，考虑批量操作
2. **切片成本**: `array_slice` 会创建新数组，对大数组有性能影响
3. **索引检查**: 所有操作都会进行边界检查，确保安全但有轻微开销

---

## 错误处理

所有数组操作在以下情况会抛出运行时错误：

- 参数数量不匹配
- 参数类型错误（期望数组但传入其他类型）
- 索引越界
- 空数组上的 `array_pop` 操作

建议在关键操作前进行长度检查：

```zeus
if (len(arr) > 0) {
    let item = array_pop(arr);
    // 安全使用 item
}
```

---

## 总结

Zeus 现在拥有完整的数组操作能力，虽然接口不是面向对象风格，但功能齐全且性能良好。通过这些内置函数，你可以实现：

- ✅ 动态数组操作
- ✅ 栈/队列数据结构
- ✅ 数组遍历与修改
- ✅ 数组过滤与映射
- ✅ 子数组提取

如有需要面向对象接口，可随时扩展！
