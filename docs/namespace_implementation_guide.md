# 包命名空间功能 - 实现指南

## 当前状态 (已完成)
✅ IRGenerator 支持 `[&]` 和 `[&alias]` 语法解析
✅ IMPORT 指令包含 importMode 参数
✅ NamespaceHandle 和 NamespaceData 类型已添加
✅ VM.namespaces 容器已添加  
✅ NAMESPACE_BUILD 操作符已添加
✅ 代码可以编译通过

## 待实现步骤

### Step 1: 修改 extractImports (ZeusPackageLoader.hpp 约188行)

将返回类型从 `std::map<string, string>` 改为 `std::map<string, ImportInfo>`

```cpp
inline std::map<std::string, ImportInfo> extractImports(const std::vector<std::vector<IRType::IRInstr>>& chunks) {
    std::map<std::string, ImportInfo> imports;
    
    for (const auto& chunk : chunks) {
        for (const auto& instr : chunk) {
            if (instr.op == VMOperator::IMPORT && instr.args.size() >= 3) {
                ImportInfo info;
                // args[0]: packageName
                info.packageName = std::get<std::string>(std::get<LexerType::Value>(instr.args[0]).data);
                // args[1]: alias  
                info.alias = std::get<std::string>(std::get<LexerType::Value>(instr.args[1]).data);
                // args[2]: importMode
                info.importMode = (int)std::get<long long>(std::get<LexerType::Value>(instr.args[2]).data);
                
                imports[info.packageName] = info;
            }
        }
    }
    return imports;
}
```

### Step 2: 修改 mergePackageChunks 签名 (约227行)

```cpp
inline void mergePackageChunks(
    std::vector<std::vector<IRType::IRInstr>>& mainChunks,
    std::vector<std::string>& mainGlobalNames,
    const std::vector<CompiledPackage*>& packages,
    const std::map<std::string, ImportInfo>& imports,  // 改为 ImportInfo
    const std::map<std::string, int>& packageGlobalOffsets)
```

### Step 3: 在mergePackageChunks中处理IMPORT指令

在处理IMPORT指令的地方 (约找到 `if (instr.op == VMOperator::IMPORT)`):

```cpp
if (instr.op == VMOperator::IMPORT) {
    auto it = imports.find(packageName);
    if (it != imports.end() && it->second.importMode == 1) {
        // 命名空间导入模式
        // 生成 NAMESPACE_BUILD 指令
        IRType::IRInstr nsInstr;
        nsInstr.op = VMOperator::NAMESPACE_BUILD;
        nsInstr.args.push_back(LexerType::Value(packageName));  // 包名
        nsInstr.args.push_back(LexerType::Value((long long)pkg->globalStartIndex));  // 起始索引
        nsInstr.args.push_back(LexerType::Value((long long)pkg->globalNames.size())); // 数量
        
        // 将命名空间对象存入全局变量(使用别名作为变量名)
        std::string aliasName = it->second.alias.empty() ? packageName : it->second.alias;
        int aliasGlobalIndex = mainGlobalNames.size();
        mainGlobalNames.push_back(aliasName);
        
        nsInstr.args.push_back(LexerType::Value((long long)aliasGlobalIndex));
        
        newChunk.push_back(nsInstr);
        // 跳过原 IMPORT 指令
        continue;
    }
    // 全局导入模式: 删除 IMPORT 指令(当前行为)
    continue;
}
```

### Step 4: 在VM中实现NAMESPACE_BUILD (ZeusVM.hpp 约1500+行,switch case中)

```cpp
case VMOperator::NAMESPACE_BUILD: {
    // args[0]: packageName (string)
    // args[1]: globalStartIndex (int)
    // args[2]: globalCount (int)
    // args[3]: aliasGlobalIndex (int)
    
    std::string pkgName = std::get<std::string>(
        std::get<LexerType::Value>(instr.args[0]).data);
    int startIdx = (int)std::get<long long>(
        std::get<LexerType::Value>(instr.args[1]).data);
    int count = (int)std::get<long long>(
        std::get<LexerType::Value>(instr.args[2]).data);
    int aliasIdx = (int)std::get<long long>(
        std::get<LexerType::Value>(instr.args[3]).data);
    
    // 创建命名空间数据
    ZeusType::NamespaceData nsData;
    nsData.packageName = pkgName;
    
    // 填充导出项映射
    for (int i = 0; i < count; ++i) {
        std::string exportName = globalNames[startIdx + i];
        nsData.exports[exportName] = startIdx + i;
    }
    
    // 添加到 namespaces 容器
    int nsId = (int)namespaces.size();
    namespaces.push_back(nsData);
    
    // 创建命名空间句柄并存入全局变量
    ZeusType::NamespaceHandle handle{nsId};
    Cell* nsCell = newCell(handle, 1);  // const
    
    if (aliasIdx >= (int)global.size()) {
        global.resize(aliasIdx + 1, nullptr);
    }
    if (global[aliasIdx]) {
        global[aliasIdx]->release();
    }
    global[aliasIdx] = nsCell;
    
    ZEUS_DEBUG_PRINT("[VM] Created namespace '%s' with %d exports at global[%d]",
        pkgName.c_str(), count, aliasIdx);
    
    break;
}
```

### Step 5: 修改OBJ_LOAD_ATTR处理NamespaceHandle

在 OBJ_LOAD_ATTR 的 switch case 中添加:

```cpp
case VMOperator::OBJ_LOAD_ATTR: {
    // ... existing code ...
    
    auto& target = stack.back();
    
    // 检查是否为命名空间
    if (std::holds_alternative<ZeusType::NamespaceHandle>(target)) {
        int nsId = std::get<ZeusType::NamespaceHandle>(target).id;
        const auto& nsData = namespaces[nsId];
        
        auto it = nsData.exports.find(attrName);
        if (it != nsData.exports.end()) {
            // 找到导出项,加载对应的全局变量
            int globalIdx = it->second;
            if (globalIdx >= (int)global.size() || !global[globalIdx]) {
                VMException::VMException exp(
                    "Namespace member '" + attrName + "' not initialized",
                    __line, instr);
                exp._throw(vmstat, currentChunk);
                break;
            }
            
            stack.pop_back();  // 弹出命名空间对象
            stack.push_back(global[globalIdx]->value);  // 压入成员值
            break;
        } else {
            VMException::VMException exp(
                "Namespace '" + nsData.packageName + "' has no member '" + attrName + "'",
                __line, instr);
            exp._throw(vmstat, currentChunk);
            break;
        }
    }
    
    // ... existing object/class handling ...
}
```

### Step 6: 更新Zeus.cpp中的调用

```cpp
// 修改 extractImports 调用(约348行)
auto imports = ZeusPackage::extractImports(cr.program);

// mergePackageChunks 的签名已自动匹配
```

## 测试用例

```zeu
// test_pkg1.zeu
let myVar = 100;
func myFunc() {
    print("Hello from pkg1");
}

// test_namespace.zeu  
import test_pkg1 [&];           // 使用 test_pkg1.xxx
import test_pkg1 [&p1];         // 使用 p1.xxx

print(test_pkg1.myVar);         // 输出: 100
test_pkg1.myFunc();             // 输出: Hello from pkg1
print(p1.myVar);                // 输出: 100
```

## 注意事项

1. **命名空间对象是只读的** - 存储的是全局变量的引用(索引)
2. **不支持 `pkg.xxx = value`** - 需要额外实现 OBJ_STORE_ATTR 的命名空间支持
3. **类和函数都可以导出** - 命名空间支持所有全局变量类型
4. **性能考虑** - 通过命名空间访问会有一次额外的索引查找

## 后续优化

- [ ] 支持通过命名空间修改变量
- [ ] 添加命名空间成员补全提示
- [ ] 优化命名空间访问性能(缓存)
- [ ] 支持嵌套命名空间 (pkg.subpkg.xxx)
