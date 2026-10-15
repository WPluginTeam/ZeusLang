# Zeus Array内嵌实现方案 (方案1改进版)

## 目标
让 `let a = [1, 2, 3]` 语法糖自动转换为 `new Array([1, 2, 3])`

## 核心思路
**不添加新的VM指令,只在IR生成阶段做语法糖转换**

## 实现步骤

### 步骤1: 默认加载 ZeusBuiltins/zeus/list.zeu

**文件**: `Zeus.cpp` 或启动代码

在程序初始化时自动import:
```cpp
// 在main()开始时,或者在IR生成前
// 自动加载核心库
std::vector<std::string> corePackages = {
    "zeus.list"
};

for (const auto& pkg : corePackages) {
    // 加载到全局环境
    ZeusPackageLoader::loadPackage(pkg);
}
```

**或者更简单**: 在IRGenerator初始化时自动注册Array的全局符号

### 步骤2: Parser已支持List语法

检查 `TreeParser.hpp` 是否已经支持 `[1, 2, 3]` 解析为 `TreeType::List`

如果已支持,直接进入步骤3。

### 步骤3: IRGenerator处理List节点

**文件**: `ZeusIR/IRGenerator.hpp`

在 `generate()` 函数中添加:

```cpp
case TreeType::NodeType::List: {
    const auto& list = std::get<TreeType::List>(node.data);
    
    // 方案: 将 [1, 2, 3] 转换为 new Array([1, 2, 3])
    // 等价于: LOAD Array构造函数, PUSH参数列表, CALL
    
    // 1. 加载Array构造函数
    //    假设Array已经在全局环境中注册
    int arrayGlobalId = env.resolveGlobal("Array");
    if (arrayGlobalId < 0) {
        throw std::runtime_error("Array class not found. Did you import zeus.list?");
    }
    
    IRType::Instruction loadArray;
    loadArray.op = IROperator::LOAD_VALUE;
    loadArray.args.push_back(LexerType::Keyword("global"));
    loadArray.args.push_back(LexerType::Value(arrayGlobalId));
    loadArray.line = node.line;
    instr_list.push_back(loadArray);
    
    // 2. 创建参数列表 (也是一个List,但这次是作为构造参数)
    //    方式A: 将元素直接作为可变参数传递
    //    方式B: 先创建一个临时列表对象
    
    // 这里我们用方式A: 直接将元素push到栈上
    for (const auto& item : list.items) {
        generate(item, env, instr_list);
    }
    
    // 3. 调用Array构造函数
    //    这里假设Array构造函数接受可变参数
    IRType::Instruction callNew;
    callNew.op = IROperator::CALL_NEW;  // 或者 CALL_CLOSURE?
    callNew.args.push_back(LexerType::Value((int)list.items.size())); // 参数个数
    callNew.line = node.line;
    instr_list.push_back(callNew);
    
    break;
}
```

**问题**: Array构造函数当前如何定义?

### 步骤4: 修改Array类构造函数

**文件**: `ZeusBuiltins/zeus/list.zeu`

当前的Array类可能是:
```zeus
class Array {
    // ...
}
```

需要确保构造函数支持:
```zeus
class Array {
    func __init__(items...) {
        // items是可变参数列表
        for (0..len(items)) [&i] {
            this.data.append(items[i]);
        }
    }
}
```

**或者更简单**: 构造函数接受一个列表参数
```zeus
class Array {
    func __init__(itemList) {
        // itemList已经是一个列表
        this.data = itemList;
    }
}
```

### 步骤5: 处理嵌套情况

对于 `[[1, 2], [3, 4]]`:
- 外层List识别到包含两个内层List
- 先递归生成内层List的IR (创建两个Array对象)
- 再用这两个Array对象作为外层Array的参数

递归自然处理!

## 关键点

### 1. List vs Array的语义

**List**: Parser阶段的语法节点,表示 `[...]` 字面量
**Array**: Runtime的对象类型,在Zeus层或C++层实现

`[1, 2, 3]` 是语法糖,编译后变成 `new Array(...)`

### 2. 不需要新的VM指令

所有操作都用现有指令完成:
- `LOAD_VALUE` - 加载Array类
- `PUSH` - 压入参数
- `CALL_NEW` 或 `CALL_CLOSURE` - 调用构造函数

### 3. 默认Import问题

两种方案:
- **方案A**: 在Zeus.cpp启动时自动import list.zeu
- **方案B**: 在IRGenerator初始化时,预先注册Array符号

建议用**方案B**更轻量。

## 实际IR示例

### 源代码:
```zeus
let arr = [1, 2, 3];
```

### 生成的IR:
```
LOAD_VALUE global <Array_id>    // 加载Array类
PUSH 1
PUSH 2  
PUSH 3
CALL_NEW 3                       // 调用构造函数,传入3个参数
STORE_VALUE_LOCAL 0              // 存储到arr
```

### 等价于:
```zeus
let arr = new Array(1, 2, 3);
```

## 待确认的问题

1. **Array构造函数签名**: 当前是什么? 需要修改吗?
2. **CALL_NEW vs CALL_CLOSURE**: Array是class还是function?
3. **默认import机制**: 是否已经存在?

## 下一步

1. 检查 `TreeParser.hpp` 是否支持List解析
2. 检查 `ZeusBuiltins/zeus/list.zeu` 中Array的构造函数
3. 确定Array的加载机制(global? class?)
4. 实现IRGenerator中的List处理
5. 测试基本用例

---

**总结**: 这个方案完全不需要新增VM指令,只是在IR生成阶段做语法糖转换,将 `[...]` 替换为 `new Array(...)` 的调用序列。简单、清晰、可维护!
