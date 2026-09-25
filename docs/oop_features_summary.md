# Zeus OOP 功能总结

## 概述
本文档总结了 Zeus 语言中实现的面向对象编程特性,包括构造函数参数、运算符重载和下标运算符。

---

## 1. 构造函数参数

### 语法
```zeus
class ClassName {
    using &self;
    
    var field1 = defaultValue1;
    var field2 = defaultValue2;
    
    // 构造函数
    func init(param1, param2) {
        self.field1 = param1;
        self.field2 = param2;
    }
}

// 创建对象并传递参数
var obj = new ClassName(arg1, arg2);
```

### 实现细节
- **IR 生成**: `IRGenerator.hpp` 中的 `tryEmitLeadingNew` lambda 解析构造函数参数
  - 识别 `new ClassName(...)` 模式
  - 递归调用 `emitExprIR` 为每个参数生成 IR
  - 生成 `OBJ_BUILD argc` 指令,其中 argc 为参数数量

- **VM 执行**: `ZeusVM.hpp` 中的 `OBJ_BUILD` 指令
  - 从指令参数读取 argc
  - 从栈中提取构造函数参数
  - 创建对象并填充默认字段值
  - 如果类中定义了 `init()` 方法,则调用它并传递参数

### 调用链
```
emitExprIR → tryEmitLeadingNew → 解析参数 → 生成 LOAD_VALUE + OBJ_BUILD
→ VM执行 OBJ_BUILD → 提取参数 → 调用 init(args...)
```

---

## 2. 运算符重载

### 支持的运算符
Zeus 支持以下运算符重载:

#### 算术运算符
- `operator +` - 加法
- `operator -` - 减法
- `operator *` - 乘法
- `operator /` - 除法
- `operator %` - 取模

#### 比较运算符
- `operator ==` - 相等
- `operator !=` - 不等
- `operator <` - 小于
- `operator >` - 大于
- `operator <=` - 小于等于
- `operator >=` - 大于等于

### 语法
```zeus
class MyClass {
    using &self;
    var value = 0;
    
    func operator[+] (other) {
        return self.value + other.value;
    }
    
    func operator[==] (other) {
        return self.value == other.value;
    }
}

var a = new MyClass();
var b = new MyClass();
var result = a + b;  // 调用 operator[+]
var equal = a == b;  // 调用 operator[==]
```

### 实现细节
- **解析**: `TreeParser.hpp` 识别 `operator` 关键字并收集中括号内的运算符符号
  - 语法格式: `func operator[符号] (params)`
  - 例如: `func operator[+]`, `func operator[==]`, `func operator[<]`
  - TreeParser收集 `[`, 运算符, `]` 三个符号,然后合并成 `"[+]"`, `"[==]"` 等
- **IR 生成**: `IRGenerator.hpp` 将运算符重载标记为实例方法
  - 存储在 `operatorMethods` 映射中,key为完整符号如 `"[+]"`, `"[==]"`
- **VM 执行**: 运算符被编译为普通方法,在执行算术/比较操作时自动查找并调用
  - VM在执行 ADD, SUBTRACT 等指令时,查找 `operatorMethods["[+]"]`, `operatorMethods["[-]"]` 等

---

## 3. 下标运算符

### operator[[]] - 下标读取
允许使用 `obj[key]` 语法读取对象属性。

```zeus
class Container {
    using &self;
    var data = {};
    
    func operator[[]] (key) {
        // 返回指定 key 对应的值
        if (key == "property") {
            return self.data;
        }
        return nil;
    }
}

var c = new Container();
var value = c["property"];  // 调用 operator[[]]
```

### operator[[]=] - 下标赋值
允许使用 `obj[key] = value` 语法设置对象属性。

```zeus
class Container {
    using &self;
    var data = {};
    
    func operator[[]= ] (key, value) {
        // 注意: 符号之间有空格
        if (key == "property") {
            self.data = value;
        }
    }
}

var c = new Container();
c["property"] = 42;  // 调用 operator[[]=]
```

### 实现细节

#### TreeParser 符号合并
- **问题**: `operator[[]]` 被解析为分离的符号: `[`, `]`, `[`, `]`
- **解决方案**: 修改 `TreeParser.hpp` 收集所有连续的运算符符号并合并
  - 添加 `!inOperatorList` 检查防止重复进入
  - 将收集的符号拼接成单个字符串: `"[[]]"`

#### VM 指令扩展
所有算术和比较运算符指令都已修改以支持运算符重载:

**算术运算符**:
- **ADD** (行 916-970): 查找 `operatorMethods["[+]"]`
- **SUBTRACT** (行 1450-1520): 查找 `operatorMethods["[-]"]`
- **MULTIPLY** (行 1520-1590): 查找 `operatorMethods["[*]"]`
- **DIVIDE** (行 1590-1665): 查找 `operatorMethods["[/]"]`
- **MODULUS** (行 1665-1740): 查找 `operatorMethods["[%]"]`

**比较运算符**:
- **EQUAL** (行 2535-2630): 查找 `operatorMethods["[==]"]`
- **NOT_EQUAL** (行 2630-2730): 查找 `operatorMethods["[!=]"]`
- **GREATER_THAN** (行 2730-2805): 查找 `operatorMethods["[>]"]`
- **LESS_THAN** (行 2805-2850): 查找 `operatorMethods["[<]"]`
- **GREATER_EQUAL** (行 2850-2900): 查找 `operatorMethods["[>=]"]`
- **LESS_EQUAL** (行 2900-2950): 查找 `operatorMethods["[<=]"]`

**下标运算符**:
- **DICT_LOAD_VALUE** (行 1145-1195): 查找 `operatorMethods["[[]]"]`
  - 检查 dictVar 是否为 `ObjectHandle`
  - 在类的 `operatorMethods` 映射中查找 `operator[[]]`
  - 创建闭包并调用: `[closure, this, key]` argc=2

- **DICT_STORE_VALUE** (行 1250-1300): 查找 `operatorMethods["[[]=]"]`
  - 检查 dictVar 是否为 `ObjectHandle`
  - 在类的 `operatorMethods` 映射中查找 `operator[[]=]`
  - 创建闭包并调用: `[closure, this, key, value]` argc=3

---

## 4. 逻辑运算符

### and / or
Zeus 支持短路逻辑运算符。

```zeus
var a = true;
var b = false;

var result1 = a and b;  // false
var result2 = a or b;   // true
```

### 实现细节
- **IR 生成**: `IRGenerator.hpp` 生成 `LOGICAL_AND` 和 `LOGICAL_OR` 指令
- **VM 执行**: `ZeusVM.hpp` 实现短路求值逻辑
  - `and`: 如果左操作数为 false,直接返回 false
  - `or`: 如果左操作数为 true,直接返回 true

---

## 5. 内置函数

### str() - 类型转换为字符串
将任意值转换为字符串表示。

```zeus
var num = 42;
var text = str(num);  // "42"

var flag = true;
var text2 = str(flag);  // "true"
```

### toBool() - 类型转换为布尔值
将任意值转换为布尔值(内部使用)。

---

## 6. 完整示例

参见 `object_syntax_demo.zeu` 和 `constructor_test.zeu` 文件获取完整的使用示例。

### 运行演示
```powershell
.\Zeus.exe object_syntax_demo.zeu
```

---

## 7. 实现文件清单

| 文件 | 修改内容 | 行数范围 |
|------|---------|---------|
| `ZeusVM.hpp` | ADD 支持 operator[+] | 916-970 |
| `ZeusVM.hpp` | SUBTRACT 支持 operator[-] | 1450-1520 |
| `ZeusVM.hpp` | MULTIPLY 支持 operator[*] | 1520-1590 |
| `ZeusVM.hpp` | DIVIDE 支持 operator[/] | 1590-1665 |
| `ZeusVM.hpp` | MODULUS 支持 operator[%] | 1665-1740 |
| `ZeusVM.hpp` | EQUAL 支持 operator[==] | 2535-2630 |
| `ZeusVM.hpp` | NOT_EQUAL 支持 operator[!=] | 2630-2730 |
| `ZeusVM.hpp` | GREATER_THAN 支持 operator[>] | 2730-2805 |
| `ZeusVM.hpp` | LESS_THAN 支持 operator[<] | 2805-2850 |
| `ZeusVM.hpp` | GREATER_EQUAL 支持 operator[>=] | 2850-2900 |
| `ZeusVM.hpp` | LESS_EQUAL 支持 operator[<=] | 2900-2950 |
| `ZeusVM.hpp` | DICT_LOAD_VALUE 支持 operator[[]] | 1145-1195 |
| `ZeusVM.hpp` | DICT_STORE_VALUE 支持 operator[[]=] | 1250-1300 |
| `ZeusVM.hpp` | OBJ_BUILD 支持构造函数参数 | 522-625 |
| `TreeParser.hpp` | 运算符符号收集与合并 | 670-710 |
| `IRGenerator.hpp` | tryEmitLeadingNew 解析构造参数 | 1112-1190 |
| `IRGenerator.hpp` | parseVarDecl 路径优化 | 1536-1580 |
| `IRGenerator.hpp` | 运算符重载分类为实例方法 | 2615-2630 |

---

## 8. 测试验证

所有功能已通过以下测试:
- ✅ 构造函数参数传递
- ✅ 11 种算术/比较运算符重载
- ✅ 2 种逻辑运算符 (and/or)
- ✅ 下标读取运算符 operator[[]]
- ✅ 下标赋值运算符 operator[[]=]
- ✅ str() 内置函数
- ✅ 所有功能组合使用

---

## 9. 已知限制

- 运算符重载语法必须使用中括号包裹运算符: `func operator[+]`, `func operator[==]` 等
- 运算符 `operator[[]=]` 的符号之间需要有空格: `operator[[]= ]`
- 构造函数必须命名为 `init`
- 下标运算符当前仅支持单个 key 参数

---

## 10. 下一步计划

可能的功能扩展:
- [ ] 析构函数支持
- [ ] 静态方法/字段
- [ ] 继承和多态
- [ ] 抽象类/接口
- [ ] 属性访问器 (getter/setter)
- [ ] 更多运算符重载 (一元运算符等)

---

**文档版本**: 1.0  
**最后更新**: 2024
