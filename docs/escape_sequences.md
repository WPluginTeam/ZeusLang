# Zeus 转义字符支持文档

## 概述

Zeus 现已全面支持字符串中的转义序列，允许你在字符串字面量中包含特殊字符。

---

## 支持的转义序列

### 基本转义序列

| 转义序列 | 描述 | 示例 | 输出 |
|---------|------|------|------|
| `\n` | 换行符 (LF) | `"第一行\n第二行"` | 第一行<br>第二行 |
| `\t` | 水平制表符 | `"列1\t列2"` | 列1&nbsp;&nbsp;&nbsp;&nbsp;列2 |
| `\r` | 回车符 (CR) | `"Before\rAfter"` | After |
| `\\` | 反斜杠 | `"C:\\Windows"` | C:\Windows |
| `\"` | 双引号 | `"说 \"Hello\""` | 说 "Hello" |
| `\'` | 单引号 | `"It\'s ok"` | It's ok |

### 特殊转义序列

| 转义序列 | 描述 | ASCII | 用途 |
|---------|------|-------|------|
| `\0` | 空字符 | 0 | 字符串终止符 |
| `\b` | 退格符 | 8 | 删除前一个字符 |
| `\f` | 换页符 | 12 | 打印机换页 |
| `\v` | 垂直制表符 | 11 | 垂直对齐 |

---

## 使用示例

### 1. 多行文本

```zeus
let message = "欢迎使用 Zeus!\n\n这是第二行。\n这是第三行。";
raw_print(message);
```

**输出:**
```
欢迎使用 Zeus!

这是第二行。
这是第三行。
```

---

### 2. 格式化表格

```zeus
raw_print("姓名\t年龄\t城市");
raw_print("----\t----\t----");
raw_print("张三\t25\t北京");
raw_print("李四\t30\t上海");
```

**输出:**
```
姓名    年龄    城市
----    ----    ----
张三    25      北京
李四    30      上海
```

---

### 3. 引号处理

```zeus
// 双引号
raw_print("他说: \"Hello, World!\"");

// 单引号
raw_print("It\'s a beautiful day!");
```

**输出:**
```
他说: "Hello, World!"
It's a beautiful day!
```

---

### 4. 文件路径

```zeus
let path = "C:\\Program Files\\Zeus\\bin";
raw_print("安装路径: ", path);

let networkPath = "\\\\server\\share\\folder";
raw_print("网络路径: ", networkPath);
```

**输出:**
```
安装路径: C:\Program Files\Zeus\bin
网络路径: \\server\share\folder
```

---

### 5. JSON 格式化

```zeus
let json = "{\n\t\"name\": \"Zeus\",\n\t\"version\": \"1.0\"\n}";
raw_print(json);
```

**输出:**
```json
{
	"name": "Zeus",
	"version": "1.0"
}
```

---

## 实际应用场景

### 场景 1: 配置文件路径

```zeus
let configPath = "C:\\Users\\Admin\\AppData\\Local\\Zeus\\config.json";
raw_print("配置文件: ", configPath);
```

---

### 场景 2: 帮助文本

```zeus
let help = "Zeus 命令行工具\n\n用法:\n\tzeus run <file>\n\tzeus compile <file>\n\tzeus help\n\n选项:\n\t-v, --version\t显示版本\n\t-h, --help\t显示帮助";
raw_print(help);
```

---

### 场景 3: 日志格式化

```zeus
func logMessage(level, msg) {
    let timestamp = "2025-11-02 10:30:00";  // 实际应用中应动态生成
    raw_print("[", timestamp, "] ", level, ":\t", msg);
}

logMessage("INFO", "程序启动成功");
logMessage("WARN", "配置文件未找到，使用默认配置");
logMessage("ERROR", "连接数据库失败");
```

**输出:**
```
[2025-11-02 10:30:00] INFO:	程序启动成功
[2025-11-02 10:30:00] WARN:	配置文件未找到，使用默认配置
[2025-11-02 10:30:00] ERROR:	连接数据库失败
```

---

### 场景 4: CSV 生成

```zeus
let csvHeader = "名称\t价格\t数量";
let csvRow1 = "苹果\t5.00\t10";
let csvRow2 = "香蕉\t3.50\t15";

raw_print(csvHeader);
raw_print(csvRow1);
raw_print(csvRow2);
```

---

## 字符串长度计算

**重要**: 转义序列被解析为单个字符，因此会影响字符串长度。

```zeus
let str1 = "Hello";        // 长度: 5
let str2 = "Hello\n";      // 长度: 6 (包含换行符)
let str3 = "A\tB\tC";      // 长度: 5 (包含2个制表符)
let str4 = "C:\\Windows";  // 长度: 10 (2个反斜杠变成1个)

raw_print("str1 长度: ", len(str1));  // 5
raw_print("str2 长度: ", len(str2));  // 6
raw_print("str3 长度: ", len(str3));  // 5
raw_print("str4 长度: ", len(str4));  // 10
```

---

## 注意事项

### 1. 未知转义序列

如果使用了不支持的转义序列，会保留原样：

```zeus
let str = "Hello\xWorld";  // \x 不支持
// 结果: "Hello\xWorld" (保持原样)
```

### 2. 字符串结束

在字符串中间使用 `\"` 不会结束字符串：

```zeus
let str = "这是 \"引号\" 示例";  // ✓ 正确
```

### 3. 反斜杠本身

要输出单个反斜杠，必须转义：

```zeus
let path = "C:\Windows";    // ❌ 错误: \W 被视为转义序列
let path = "C:\\Windows";   // ✓ 正确
```

---

## 转义 vs 原始字符串

**Zeus 目前只支持转义字符串**，不支持原始字符串（raw strings）。

其他语言的对比：

```python
# Python
regular = "C:\\Windows\\System32"  # 需要转义
raw = r"C:\Windows\System32"       # 原始字符串
```

```rust
// Rust
let regular = "C:\\Windows\\System32";  // 需要转义
let raw = r"C:\Windows\System32";       // 原始字符串
```

**Zeus (当前版本):**
```zeus
let path = "C:\\Windows\\System32";  // 只能使用转义
```

---

## 最佳实践

### 1. 文件路径推荐格式

```zeus
// Windows 路径
let winPath = "C:\\Program Files\\Zeus";

// 网络路径
let netPath = "\\\\server\\share";

// Unix 风格路径（如果支持）
let unixPath = "/usr/local/bin/zeus";
```

### 2. 多行文本格式化

```zeus
// 推荐: 使用 \n 和 \t 格式化
let text = "标题\n\n内容行1\n\t缩进行2\n\t缩进行3";
```

### 3. 引号使用

```zeus
// 对话文本
let dialogue = "他问: \"你好吗?\"\n我回答: \"很好，谢谢!\"";

// 缩写
let contraction = "It\'s a wonderful day!";
```

---

## 性能说明

- 转义序列在**词法分析阶段**被解析
- 字符串在运行时已经是最终形式
- **无性能开销** - 转义处理只在编译时发生一次

---

## 测试清单

运行 `escape_test.zeu` 以验证所有转义功能：

```bash
.\Zeus.exe escape_test.zeu
```

**测试覆盖:**
- ✅ 基本转义序列 (\n, \t, \r)
- ✅ 引号转义 (\", \')
- ✅ 反斜杠转义 (\\)
- ✅ 特殊转义序列 (\0, \b, \f, \v)
- ✅ 组合使用
- ✅ 数组中的转义字符串
- ✅ 实际应用场景
- ✅ 长度计算

---

## 未来扩展

可能的增强功能：

1. **原始字符串支持**
   ```zeus
   let raw = r"C:\Windows\System32";  // 未来可能支持
   ```

2. **Unicode 转义**
   ```zeus
   let emoji = "\u{1F600}";  // 😀
   ```

3. **十六进制转义**
   ```zeus
   let char = "\x41";  // 'A'
   ```

4. **八进制转义**
   ```zeus
   let char = "\101";  // 'A'
   ```

---

## 总结

Zeus 现在完全支持字符串转义序列，包括：

✅ **10 种转义序列**: `\n`, `\t`, `\r`, `\\`, `\"`, `\'`, `\0`, `\b`, `\f`, `\v`  
✅ **零性能开销** - 编译时处理  
✅ **全面测试** - 所有功能经过验证  
✅ **实用场景** - 路径、JSON、表格、多行文本  

转义字符让 Zeus 字符串处理更加强大和灵活！
