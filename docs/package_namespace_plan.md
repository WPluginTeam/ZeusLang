# 包命名空间系统实现计划

## 需求
1. `import package.name [&];` - 导入包结构,使用 `package.name.xxx` 调用
2. `import package.name;` - 默认导入包全局(当前已实现)
3. `import package.name [&alias];` - 导入包结构并用别名,使用 `alias.xxx` 调用

## 设计

### 1. 语法解析 (TreeParser)
- `[&]` - 标记为命名空间导入,无别名
- `[&alias]` - 标记为命名空间导入,有别名

### 2. IR 生成 (IRGenerator)
当前已支持解析别名,需要:
- 区分"全局导入"和"命名空间导入"
- 生成不同的 IMPORT 指令标记

### 3. 包命名空间表示
在 VM 中,包命名空间可以表示为一个特殊的对象/字典:
```
{
  "varName": <global_index>,
  "funcName": <global_index>,
  "ClassName": <global_index>
}
```

### 4. 包加载器 (ZeusPackageLoader)
需要记录:
- 包的导出项列表
- 命名空间模式还是全局模式

### 5. VM 运行时
- 处理 IMPORT 指令时,根据模式:
  - 全局模式: 当前行为(不变)
  - 命名空间模式: 创建包对象,存入全局变量

### 6. 符号解析
- `package.name.xxx` 需要被解析为:
  - LOAD_VALUE package.name (加载包对象)
  - DICT_LOAD_VALUE "xxx" (从包对象获取成员)

## 实现步骤

### Step 1: 修改 IMPORT 指令格式
在 IMPORT 指令中添加第三个参数标记导入模式:
- args[0]: packageName (string)
- args[1]: alias (string, 可选)
- args[2]: importMode (int: 0=global, 1=namespace)

### Step 2: 修改 IRGenerator
解析 `[&]` 或 `[&alias]` 时,设置 importMode=1

### Step 3: 修改 CompiledPackage
添加字段记录包的导出项:
```cpp
struct CompiledPackage {
    // ...existing fields...
    std::vector<std::string> exports;  // 包导出的全局变量名列表
};
```

### Step 4: 修改 VM 的 IMPORT 处理
- 检查 importMode
- 如果是命名空间模式:创建字典对象,填充导出项,存入全局变量

### Step 5: 测试用例
```zeu
// test_namespace.zeu
import pkg1 [&];           // 命名空间导入,使用 pkg1.xxx
import pkg2 [&p2];         // 命名空间导入,别名 p2,使用 p2.xxx
import pkg3;               // 全局导入(当前行为)

print(pkg1.varName);
print(p2.funcName());
print(globalFromPkg3);
```
