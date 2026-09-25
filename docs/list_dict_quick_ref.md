# Zeus 列表和字典语法快速参考

## 列表 (List)

### 创建
```zeus
let numbers = [1, 2, 3, 4, 5];          // 数字列表
let words = ["hello", "world"];         // 字符串列表  
let mixed = [1, "two", 3.0, true];      // 混合类型列表
let empty = [];                          // 空列表
```

### 访问和修改
```zeus
let first = numbers[0];      // 访问: 1
numbers[1] = 99;              // 修改
```

### 遍历
```zeus
// 只读遍历
for (numbers) [item] {
    print(item);
}

// 引用捕获，可修改
for (numbers) [&item] {
    item = item * 2;    // 修改原列表
}
```

---

## 字典 (Dict)

### 创建
```zeus
let person = {
    "name": "Alice",
    "age": 25,
    "city": "Beijing"
};

let empty = {};     // 空字典
```

### 访问和修改
```zeus
let name = person["name"];     // 访问: "Alice"
person["age"] = 26;             // 修改
person["job"] = "Engineer";     // 添加新键
```

### 遍历
```zeus
// 只读遍历 (注意冒号)
for (person) [key: value] {
    print(key, ":", value);
}

// 引用捕获，可修改值
for (person) [&key: &value] {
    // key 通常只读，value 可修改
}
```

---

## 数值遍历

### 固定次数
```zeus
for (5) [&index] {
    print(index);    // 0, 1, 2, 3, 4
}
```

### 范围遍历
```zeus
for (1..10..2) [&i] {
    print(i);        // 1, 3, 5, 7, 9
}
```

---

## 关键语法对比

| 特性 | 列表 | 字典 |
|------|------|------|
| 字面量 | `[val1, val2]` | `{"key1": val1, "key2": val2}` |
| 访问 | `list[整数]` | `dict["字符串"]` |
| 遍历变量 | `[&item]` | `[&key: &value]` |

## 重要提示

⚠️ **列表用 `[]`，字典用 `{}`** - 这是最容易混淆的地方！

✅ **字典遍历必须用冒号** - `[&key: &value]` 不是 `[&key, &value]`

✅ **引用捕获 `&` 可以修改原集合** - 没有 `&` 则是副本
