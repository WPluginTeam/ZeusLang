# Zeus 内存泄漏分析与修复方案

## 问题描述

在 test.zeu 的 HTTP 服务器场景下，每次客户端连接都会导致约 0.2MB 的内存泄漏，且内存持续累积不释放。

## 问题根源

### 1. 对象生命周期管理缺失

**现状：**
- `Cell` 有引用计数机制（`refCount`），当计数为 0 时自动 `delete`
- `ObjectInstance` **没有引用计数**，存储在 `VM::objects` 向量中
- 对象一旦创建，永远不会被删除

**问题代码位置：**
```cpp
// ZeusRunner/ZeusVM.hpp:1524
int objId = (int)objects.size();
objects.push_back(std::move(obj));  // 永久存储，不会删除
```

### 2. 线程资源未释放

**现状：**
- `ThreadInfo` 存储在 `ThreadManager::threads` 映射中
- 线程结束后，`ThreadInfo` 对象仍保留在内存中
- 只有在 `ThreadManager` 析构时才清理（程序退出时）

**问题代码位置：**
```cpp
// ZeusRunner/ZeusThread.hpp:92-100
~ThreadManager() {
    for (auto& kv : threads) {
        if (kv.second->thread && kv.second->thread->joinable()) {
            kv.second->thread->join();
        }
    }
    threads.clear();  // 仅在析构时清理
}
```

### 3. test.zeu 的内存泄漏场景

```zeus
for (true) {
    let client = server.accept();
    let res = new ClientRes(client);  // 创建对象，永不释放
    // res 包含：
    //   - client (Socket对象)
    //   - thread (Thread对象)
    // 每次循环都创建新对象，旧对象不释放
}
```

每次连接创建的对象：
1. `ClientRes` 对象实例 → 存入 `VM::objects`
2. `Socket` 对象（client）→ 存入 `VM::objects`
3. `Thread` 对象 → 存入 `VM::objects`
4. `ThreadInfo` → 存入 `ThreadManager::threads`

## 内存占用估算

每次连接约 0.2MB 包括：
- `ObjectInstance` 结构（约 1KB）
- `ThreadInfo` 结构（约 1KB）
- 线程栈空间（Windows 默认 1MB）
- Cell 和其他辅助数据（约数 KB）
- **主要开销：线程栈（1MB）+ 线程对象持有的程序副本**

## 解决方案

### 方案 A：对象引用计数（推荐）

为 `ObjectInstance` 添加引用计数机制，类似 `Cell`。

**实现步骤：**

1. **修改 ObjectHandle 结构**
```cpp
// ZeusRunner/ZeusType.hpp
struct ObjectInstance {
    int classId = -1;
    std::unordered_map<std::string, UniversalVariant> values;
    std::unordered_map<std::string, uint8_t> flags;
    
    int refCount = 1;  // 新增引用计数
    
    void retain() { ++refCount; }
    void release() {
        if (--refCount == 0) {
            // 清理对象持有的资源
            values.clear();
            flags.clear();
        }
    }
};
```

2. **在 VM 中管理对象生命周期**
```cpp
// ZeusRunner/ZeusVM.hpp
// 对象创建时：refCount = 1
// 赋值给变量时：retain()
// 变量离开作用域时：release()
// refCount = 0 时：从 objects 向量中移除或标记为可回收
```

3. **实现对象回收机制**
   - 惰性回收：标记删除的对象槽位，后续复用
   - 定期压缩：定期清理 objects 向量中的空槽
   - 引用追踪：扫描 Cell/Stack 中的 ObjectHandle 引用

### 方案 B：作用域析构（简化版）

为局部变量块结束时自动释放对象。

**实现步骤：**

1. **在栈帧弹出时清理对象**
```cpp
// ZeusRunner/ZeusVM.hpp:5700（RETURN 指令处理）
for (auto* cell : local) {
    if (cell) {
        // 检查 Cell 是否持有 ObjectHandle
        if (std::holds_alternative<ZeusType::ObjectHandle>(cell->value)) {
            auto objHandle = std::get<ZeusType::ObjectHandle>(cell->value);
            // 减少对象引用计数或标记删除
            markObjectForDeletion(objHandle.id);
        }
        cell->release();
    }
}
```

2. **实现对象标记删除**
```cpp
void markObjectForDeletion(int objId) {
    if (objId >= 0 && objId < objects.size()) {
        // 简单方案：直接清空对象数据
        objects[objId].values.clear();
        objects[objId].flags.clear();
        objects[objId].classId = -1;  // 标记为已删除
    }
}
```

### 方案 C：线程资源主动回收

在线程结束后主动清理 ThreadInfo。

**实现步骤：**

1. **线程结束时通知 ThreadManager**
```cpp
// ZeusRunner/ZeusThread.hpp
void ThreadManager::onThreadFinished(int id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    
    auto it = threads.find(id);
    if (it != threads.end()) {
        auto& info = it->second;
        
        // Join 线程
        if (info->thread && info->thread->joinable()) {
            info->thread->join();
        }
        
        // 清理资源
        info->program.clear();
        info->globalNames.clear();
        info->args.clear();
        
        // 从映射中移除
        threads.erase(it);
        
        ZEUS_DEBUG_PRINT("[ThreadManager] Cleaned up thread %d", id);
    }
}
```

2. **在线程入口函数中调用清理**
```cpp
void threadEntry(std::shared_ptr<ThreadInfo> info) {
    // ... 执行线程逻辑 ...
    
    // 线程结束时通知管理器
    ThreadManager::instance().onThreadFinished(info->id);
}
```

3. **在 Thread.join() 中触发清理**
```cpp
// ZeusBuiltins/zeus/thread.zeu
func join () {
    if (this._Thread_ID >= 0) {
        __thread_join(this._Thread_ID);  // 这里应该触发清理
        this._is_alive = false;
    }
}
```

## 推荐实施顺序

1. **立即实施：方案 C（线程资源清理）**
   - 影响范围小
   - 可快速见效
   - 解决主要内存开销（线程栈）

2. **中期实施：方案 B（作用域析构）**
   - 逻辑简单
   - 适合当前架构
   - 解决对象累积问题

3. **长期计划：方案 A（完整引用计数）**
   - 需要重构较多代码
   - 提供最完善的内存管理
   - 支持循环引用检测（需扩展）

## 测试验证

修复后，使用以下方式验证：

1. **内存监控**
```powershell
# Windows
Get-Process Zeus | Select-Object WorkingSet64
# 循环运行，观察内存是否稳定
```

2. **压力测试**
```bash
# 模拟 1000 次连接
for i in {1..1000}; do
    curl http://localhost:80
done
```

3. **调试输出**
```cpp
// 启用 MEM 调试通道
Zeus.exe test.zeu --debug-channels=MEM
```

## 注意事项

1. **避免过早释放**：确保对象在所有引用失效后才释放
2. **线程安全**：对象回收需要考虑多线程访问
3. **循环引用**：当前方案无法处理循环引用，需要标记-清除 GC
4. **性能影响**：频繁分配/释放可能影响性能，考虑对象池

## 相关文件

- `ZeusRunner/ZeusCell.hpp` - Cell 引用计数实现（参考）
- `ZeusRunner/ZeusVM.hpp` - VM 对象管理
- `ZeusRunner/ZeusThread.hpp` - 线程管理器
- `ZeusRunner/ZeusType.hpp` - 对象类型定义
- `test.zeu` - 问题场景

