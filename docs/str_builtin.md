# str() 内置函数文档

## 概述
`str()` 是Zeus语言的内置函数,用于将各种类型的值转换为字符串表示形式。

## 语法
```
str(value)
```

## 参数
- `value`: 任意类型的值

## 返回值
返回值的字符串表示形式(不带引号)

## 支持的类型

### 1. 整数 (BigInteger)
```zeus
str(42)        // "42"
str(-100)      // "-100"
```

### 2. 浮点数 (BigDecimal)
```zeus
str(3.14)      // "3.14"
str(-2.5)      // "-2.5"
```

### 3. 布尔值
```zeus
str(true)      // "true"
str(false)     // "false"
```

### 4. 字符串
```zeus
str("hello")   // "hello"  (原样返回,不添加引号)
```

### 5. 数组
```zeus
var arr = [1, 2, 3];
str(arr)       // "[1, 2, 3]"
```

### 6. nil
```zeus
var x = nil;
str(x)         // "nil"
```

### 7. 对象、闭包等
```zeus
var obj = new MyClass();
str(obj)       // "<object:#id>"
```

## 使用场景

### 1. 字符串拼接
```zeus
var x = 10;
var y = 20;
print("计算: " + str(x) + " + " + str(y) + " = " + str(x + y));
// 输出: "计算: 10 + 20 = 30"
```

### 2. 格式化输出
```zeus
var name = "Alice";
var score = 95;
print(str(name) + ": " + str(score) + "分");
// 输出: "Alice: 95分"
```

### 3. 与运算符重载结合
```zeus
class Vector {
    using &self;
    var x;
    var y;
    
    func toString() {
        return "(" + str(self.x) + ", " + str(self.y) + ")";
    }
}

var v = new Vector();
v.x = 3;
v.y = 4;
print(v.toString());  // "(3, 4)"
```

### 4. 调试输出
```zeus
var data = [1, 2, 3];
print("数组内容: " + str(data));
// 输出: "数组内容: [1, 2, 3]"
```

## 与 print() 的区别

- `print()`: 直接输出值到控制台,可接受多个参数
- `str()`: 将单个值转换为字符串,用于字符串拼接

```zeus
// print 用法
print("x =", 42);           // 输出: "x = 42"

// str 用法
print("x = " + str(42));    // 输出: "x = 42"
```

## 注意事项

1. `str()` 只接受一个参数
2. 对于字符串类型,返回原字符串(不添加引号)
3. 对于复杂对象,返回debug格式的表示形式
4. 适合用于动态构建输出字符串

## 实现位置
- 文件: `ZeusRunner/ZeusBuiltins.hpp`
- 函数: `builtin_str()`
- 注册: `initCoreBuiltins()`

## 相关函数
- `print()`: 输出到控制台
- `raw_print()`: 原始输出(不添加换行)
- `len()`: 获取长度
