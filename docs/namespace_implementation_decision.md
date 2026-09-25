# 包命名空间实现 - 详细方案

## 当前状态
- ✅ IRGenerator 已支持解析 `[&]` 和 `[&alias]`
- ✅ IMPORT 指令包含3个参数: packageName, alias, importMode
- ⚠️ extractImports 仍返回旧格式 (需修改但不紧急)
- ❌ mergePackageChunks 不支持命名空间模式
- ❌ 无法使用 `pkg.xxx` 语法访问

## 实现步骤

### Step 1: 修改mergePackageChunks支持命名空间导入

对于每个IMPORT指令:
- 检查importMode
- 如果是namespace模式(mode=1):
  1. 创建一个字典对象
  2. 将包的所有导出项作为键值对添加(键=名称,值=全局索引的引用)
  3. 将字典存入一个全局变量(名称为alias)
  4. 删除IMPORT指令

伪代码:
```cpp
if (importMode == 1) {
    // 生成创建包对象的IR:
    // DICT_BUILD 0
    // STORE_LOCAL 0  // 临时存储
    // 对每个导出项:
    //   LOAD_LOCAL 0
    //   PUSH "exportName"
    //   LOAD_VALUE_GLOBAL <global_index>
    //   DICT_STORE_VALUE
    // LOAD_LOCAL 0
    // STORE_VALUE_GLOBAL <alias_index>
}
```

但这有个问题:**字典存的是值的拷贝,不是引用**!

### 更好的方案:使用函数闭包或特殊对象类型

实际上,我们需要一个特殊的"命名空间对象",它在访问成员时动态查找全局变量。

### 最简方案:使用字典+包装函数

由于当前VM限制,最简单的实现是:
1. 包命名空间对象是一个普通字典
2. 存储的是**全局变量的索引**(作为整数)
3. 使用时需要特殊语法或helper函数

但这不符合用户需求(`pkg.xxx`应该直接可用)

### 实用方案:扩展VM支持命名空间对象

添加新的Cell类型: NAMESPACE
- 存储包名和导出项映射
- OBJ_LOAD_ATTR时特殊处理:查找全局变量

步骤:
1. 在ZeusType.hpp中添加NAMESPACE类型
2. 修改mergePackageChunks生成CREATE_NAMESPACE指令
3. VM执行CREATE_NAMESPACE时创建命名空间对象
4. OBJ_LOAD_ATTR识别命名空间对象并返回全局变量

## 决策

考虑到实现复杂度和时间,我建议:**Phase 1 - 字典方案(当前可行)**

用户代码:
```zeu
import pkg [&];
let value = pkg["varName"];  // 通过字典访问
```

**Phase 2 - 完整命名空间(后续)**:
```zeu  
import pkg [&];
let value = pkg.varName;  // 直接访问
```

先实现Phase 1,让基本功能可用,再优化到Phase 2。
