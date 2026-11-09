# Zeus 运算符重载语法参考

## 重要说明

⚠️ **运算符重载必须使用中括号包裹运算符符号!**

- ✅ 正确: `func operator[+] (other)`
- ❌ 错误: `func operator + (other)`

## 支持的运算符

### 1. 算术运算符

```zeus
class Number {
    using &self;
    var value = 0;
    
    // 加法
    func operator[+] (other) {
        return self.value + other.value;
    }
    
    // 减法
    func operator[-] (other) {
        return self.value - other.value;
    }
    
    // 乘法
    func operator[*] (other) {
        return self.value * other.value;
    }
    
    // 除法
    func operator[/] (other) {
        return self.value / other.value;
    }
    
    // 取模
    func operator[%] (other) {
        return self.value % other.value;
    }
}
```

### 2. 比较运算符

```zeus
class Comparable {
    using &self;
    var value = 0;
    
    // 等于
    func operator[==] (other) {
        return self.value == other.value;
    }
    
    // 不等于
    func operator[!=] (other) {
        return self.value != other.value;
    }
    
    // 大于
    func operator[>] (other) {
        return self.value > other.value;
    }
    
    // 小于
    func operator[<] (other) {
        return self.value < other.value;
    }
    
    // 大于等于
    func operator[>=] (other) {
        return self.value >= other.value;
    }
    
    // 小于等于
    func operator[<=] (other) {
        return self.value <= other.value;
    }
}
```

### 3. 下标运算符

```zeus
class Container {
    using &self;
    var data = {};
    
    // 下标读取: obj[key]
    func operator[[]] (key) {
        return self.data[key];
    }
    
    // 下标赋值: obj[key] = value
    // 注意: [[]=] 之间需要有空格!
    func operator[[]= ] (key, value) {
        self.data[key] = value;
    }
}
```

### 4. 绑定/拷贝协议 `operator[@=]`

类的默认语义是**值语义**：`let b = a;`、`a = b`、读字段、读数组下标都会复制对象
（标量字段逐个复制，数组/字典字段共享同一个底层容器）。函数传参与返回则共享。

要改掉这个行为，就在类里定义 `operator[@=]`，它决定“把这个对象绑定到另一个名字
时返回什么”：

```zeus
class Shared {
    using &this;
    var value = 0;

    // 返回引用：绑定别名，两边是同一个对象（多线程状态机必须这样）
    func operator[@=]() -> &Shared {
        return this;
    }
}

class Custom {
    using &this;
    var value = 0;

    // 返回新对象：自定义拷贝（默认行为就是复制一份）
    func operator[@=]() -> Custom {
        let copy = new Custom();
        copy.value = this.value + 1;
        return copy;
    }
}
```

规则：

- `-> &T { return this; }` 给出**引用语义**；不写 `&` 的 `-> T { return this; }` 仍是
  拷贝（返回的对象会再被拷贝一次）。
- 没有 `operator[@=]` 的类保持默认值语义。
- **带 `operator[~]` 的类必须声明 `operator[@=]`。** 默认值语义下连「读字段」都会
  复制一份，每个副本析构都会执行一次 `[~]`，于是同一个资源被释放两次（double
  free，或关掉别的副本还在用的句柄）。`-> &T` 表示绑定同一实例，`-> T` 表示显式
  拷贝。没写时编译器给出 `P0045` 警告，回归见
  `ZeusZTest/run_destructor_copy_warning_test.py`。
- 常见坑：把状态机对象交给工作线程后，`let client = ...` 的复制品上写的状态别处
  看不见——这时就该给类加引用钩子，而不是把状态塞进数组/字典里绕。

参考实现：`ZeusZTest/class_copy_core_test.zeu`、`samples/NexoraCodeCore/connection.zeu`。

## 使用示例

```zeus
var a = new Number();
a.value = 10;

var b = new Number();
b.value = 20;

// 调用 operator[+]
var sum = a + b;          // sum.value == 30

// 调用 operator[==]
var equal = a == b;       // false

// 调用 operator[<]
var less = a < b;         // true

// 下标操作
var c = new Container();
c["name"] = "Zeus";       // 调用 operator[[]=]
var name = c["name"];     // 调用 operator[[]]
```

## 语法规则

1. **中括号是必需的**
   - 格式: `func operator[符号] (参数)`
   - 中括号包裹运算符符号

2. **参数数量**
   - 二元运算符接受一个参数 (other)
   - `operator[[]]` 接受一个参数 (key)
   - `operator[[]=]` 接受两个参数 (key, value)

3. **特殊符号**
   - `operator[[]]` - 两对中括号,表示下标读取
   - `operator[[]= ]` - 注意空格!表示下标赋值

4. **返回值**
   - 算术运算符通常返回计算结果
   - 比较运算符应返回布尔值
   - 下标读取返回对应的值
   - 下标赋值通常不返回值

## 实现原理

### 解析阶段 (TreeParser)
1. 识别 `operator` 关键字
2. 收集中括号内的所有符号: `[`, 运算符, `]`
3. 合并符号: `"[+]"`, `"[==]"`, `"[[]]"` 等

### IR生成阶段 (IRGenerator)
1. 将运算符重载标记为实例方法
2. 存储在 `operatorMethods` 映射中
3. Key为完整符号字符串,如 `"[+]"`, `"[<]"`

### VM执行阶段 (ZeusVM)
1. 执行 ADD/SUBTRACT/EQUAL 等指令时
2. 检查左操作数是否为对象
3. 在对象的类中查找对应的运算符方法
4. 如果找到,调用该方法;否则使用默认实现

## 调试提示

如果运算符重载不工作:
1. 检查语法是否使用了中括号: `operator[+]` 而不是 `operator +`
2. 对于下标赋值,检查是否有空格: `operator[[]= ]`
3. 确保方法在类内部定义,且使用了 `using &self;`
4. 使用 `--debug-channels=IR,VM` 查看详细信息

## 相关文档

- [OOP功能总结](oop_features_summary.md) - 完整的OOP特性文档
- [对象构造](object_construction.md) - 对象创建和构造函数
- [运算符重载实现](operator_overload_implementation.md) - 实现细节

---

**版本**: 1.0  
**最后更新**: 2024
