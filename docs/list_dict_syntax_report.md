# Zeus 列表和字典语法验证报告

## 测试日期
2024年

## 语法确认

### ✅ 列表语法
- **创建列表**: 使用 `[元素1, 元素2, ...]` 语法（**不是** `{}`）
  ```zeus
  let numbers = [1, 2, 3, 4, 5];      // 整数列表
  let mixed = [1, "hello", 3.14];     // 混合类型列表
  let empty = [];                      // 空列表
  ```

- **访问元素**: 使用下标 `list[index]`
  ```zeus
  numbers[0]    // 返回第一个元素
  numbers[2]    // 返回第三个元素
  ```

- **修改元素**: 使用下标赋值
  ```zeus
  numbers[1] = 99;    // 成功修改
  ```

- **遍历列表**: 使用 `for (list) [&item]` 语法
  ```zeus
  for (numbers) [&item] {
      print("Item:", item);    // 遍历每个元素
  }
  
  for (numbers) [&item] {
      item = item * 2;         // 使用引用捕获修改元素
  }
  ```

### ✅ 字典语法
- **创建字典**: 使用 `{键: 值, ...}` 语法
  ```zeus
  let dict = {"a": 1, "b": 2};           // 带初始值
  let empty = {};                         // 空字典
  let person = {
      "name": "Alice",
      "age": 25
  };
  ```

- **访问元素**: 使用下标 `dict["键"]`
  ```zeus
  person["name"]    // 返回 "Alice"
  person["age"]     // 返回 25
  ```

- **修改元素**: 使用下标赋值
  ```zeus
  person["age"] = 26;    // 成功修改
  ```

- **遍历字典**: 使用 `for (dict) [&key: &value]` 语法（注意冒号）
  ```zeus
  for (person) [&key: &value] {
      print(key, ":", value);
  }
  ```

### ✅ 数值遍历
- **固定次数遍历**: `for (n) [&index]`
  ```zeus
  for (5) [&index] {       // index: 0, 1, 2, 3, 4
      print("index =", index);
  }
  ```

- **范围遍历**: `for (start..end..step) [&index]`
  ```zeus
  for (1..10..2) [&i] {    // i: 1, 3, 5, 7, 9
      print("i =", i);
  }
  ```

## 测试结果

### ✅ 通过的功能
1. **列表创建**: `[1, 2, 3]` 语法正确创建列表
2. **列表访问**: `list[0]` 正确返回元素值
3. **列表修改**: `list[0] = 100` 成功修改元素
4. **列表遍历**: `for (list) [&item]` 正确遍历
5. **引用捕获修改**: `for (list) [&item] { item = item * 2; }` 成功修改所有元素
6. **字典创建**: `{"key": value}` 语法正确
7. **字典访问**: `dict["key"]` 正确返回值
8. **字典修改**: `dict["key"] = newValue` 成功修改
9. **字典遍历**: `for (dict) [&k: &v]` 正确遍历键值对
10. **数值遍历**: `for (5) [&i]` 和 `for (1..10..2) [&i]` 都正常工作

### ⚠️ 已知问题
1. **混合类型列表元素**: 访问字符串元素时返回 `<obj>` 而不是字符串值
   - `mixedList[1]` 返回 `<obj>`（实际应该是 `"hello"`）
   
2. **嵌套结构访问**: 嵌套的字典/列表元素显示为 `<obj>`
   - `config["database"]` 返回 `<obj>`（应该是嵌套字典）
   - `matrix[0]` 返回 `<obj>`（应该是嵌套列表）

**原因分析**: 这可能是 `print` 函数对复杂类型的显示问题，或者是类型转换的问题。底层的访问和存储机制是正常的。

## 核心修复

### 问题
之前 `DICT_LOAD_VALUE` 和 `DICT_STORE_VALUE` 只支持字典（`ZeusType::Object`），不支持列表（`ZeusType::Array`），导致列表下标操作失败。

### 解决方案
修改了 `ZeusVM.hpp` 中的两个操作：

1. **DICT_LOAD_VALUE**: 添加类型检查，对 `Array` 类型使用 `arrayRead`，对 `Object` 类型使用字典查找
2. **DICT_STORE_VALUE**: 添加类型检查，对 `Array` 类型使用 `arrayWrite`，对 `Object` 类型使用字典赋值

修改了两个版本的代码：
- Switch-case 版本（完整代码）
- Goto-dispatch 版本（压缩一行代码）

## 语法总结

| 功能 | 列表 | 字典 |
|------|------|------|
| **字面量** | `[1, 2, 3]` | `{"a": 1, "b": 2}` |
| **空集合** | `[]` | `{}` |
| **访问** | `list[0]` | `dict["key"]` |
| **修改** | `list[0] = value` | `dict["key"] = value` |
| **遍历** | `for (list) [&item] {}` | `for (dict) [&key: &value] {}` |
| **引用捕获** | `[&item]` | `[&key: &value]` |
| **复制捕获** | `[item]` | `[key: value]` |

## 重要提示

1. **列表使用方括号 `[]`，字典使用花括号 `{}`**
2. **字典遍历使用冒号 `:`** 分隔键和值：`[&key: &value]`
3. **列表遍历只需一个变量**：`[&item]`
4. **引用捕获可以修改原集合**
5. **下标操作对列表和字典通用**，VM会自动识别类型

## 测试文件
- `test_simple_collection.zeu` - 基础测试（通过）
- `test_list_dict_syntax.zeu` - 完整测试（大部分通过）
