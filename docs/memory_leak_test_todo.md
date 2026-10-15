# Zeus 编译程序内存泄漏测试 TODO（暂不含循环引用）

## 主线：编译版 socket/thread 服务

- [x] 基于 SocketThreadTest.zeu 准备 loopback 压测版本，绑定 127.0.0.1，不直接运行原脚本的 0.0.0.0:8081。
- [x] 压测版覆盖真实 socket 生命周期：accept、发送响应、关闭 socket、join worker，并显式释放请求对象。
- [x] 为避免引入循环引用，线程目标使用仅持有 accepted socket 的 ClientContext 绑定方法；ClientContext 不拥有 Thread。
- [x] 已检查 LLVM standalone runtime：当前未注册 socket ABI，也没有 thread create/start/join；另有 LLVM 发射错误阻止该程序生成 exe。
- [ ] 用 `zeusc.bat` 编译为独立 exe；准备只访问 `127.0.0.1` 的客户端负载程序。
- [ ] 分阶段运行预热、1,000、10,000 次请求，并覆盖串行和受控并发；每批结束后等工作线程和 socket 全部退出，再记录稳定值。
- [ ] VM 与 LLVM 分别运行同一服务场景；记录成功响应数、活动线程/socket/句柄、进程工作集或 Private Bytes，以及 VM 可用的 `__gc_stats`、`__closure_stats`、`__vm_stats`。
- [ ] 比较每批处理后的稳定内存是否随请求数线性增长；重复运行以区分真实存活对象与分配器保留的内存。
- [ ] 若指标持续增长，再按对象、ThreadInfo、SocketData、闭包和动态值逐项追查创建与释放位置。

当前 `SocketThreadTest.zeu` 是历史问题的参考样例，不是最终压测器：它绑定所有网卡；`delete res` 会在析构中立即 `join()`，因此 accept 循环基本按请求串行等待。新压测版应保留真实 socket/thread 生命周期，同时显式控制并发和线程收尾。

当前 AOT 阻塞来自运行库接口：`ZeusRuntimeLib/ZeusNativeABI.cpp` 注册的 standalone 线程函数不含实际线程启动/等待，socket 模块也没有 standalone C ABI。完成这块前，只能在 VM 跑 socket/thread 端到端场景，不能声称已经测过 LLVM 编译版服务。

## 辅助检查范围

- 用局部对象、闭包、string、bytes、array、dict 的无环 churn 检查引用计数和槽位复用。
- 循环引用回收单独安排，本轮不据此判断通过或失败。
- 进程工作集只作为趋势参考；优先结合存活对象、Cell、闭包节点和线程/socket 计数判断。

## 检查清单

- [x] VM：反复创建并丢弃闭包、捕获变量和动态值；比较循环前后的 Cell 与闭包计数。
- [x] LLVM：重复创建并丢弃无环闭包；确认共享运行时的存活节点数在收集后回到稳定基线。
- [x] VM 与 LLVM：重复创建类实例，并执行 `delete`；检查析构次数及删除后访问行为。
- [x] VM 与 LLVM：对 string、bytes、array、dict 做反复构造和离开作用域；确认两端可执行且没有运行时错误。
- [x] 记录 VM 在 1,000 次与 10,000 次分配后的计数和工作集趋势。
- [x] 比较同一无环分配场景的 VM/LLVM 析构时机。

## 本轮执行结果

此前结果以局部 churn 为主；本轮已完成 VM loopback socket/thread 的 1,000 请求串行试跑，LLVM AOT 端尚未运行。

- VM 无环 churn：10,000 次对象与闭包创建/离开作用域后，Cell `Current` 仍为 22；对象存活数为 0、对象槽总数为 1，闭包槽有 2 个可复用槽。工作集在 1,000 次后为 12.8281 MB，10,000 次后仍为 12.8281 MB。
- LLVM 无环 churn：10,000 次分配 string、bytes、array、dict 和类实例后，析构计数为 10,000。
- 闭包回收：`gc_native_test.zeu` 在 VM 与 LLVM 均为 `PASS = 24 TOTAL = 24`。VM 的原生节点数始终为 0（VM 使用自身闭包表）；LLVM 在 3,000 次 churn 后收集，节点数从 2 到 2，保留的两个节点属于仍存活的闭包与捕获环境。
- 显式删除：VM `delete_source_test.zeu` 和 LLVM `delete_alias_llvm_test.zeu` 均通过；删除后别名失效，析构钩子执行。
- bytes：`bytes_impl_test.zeu` 在 VM 与 LLVM 均输出 `PASS bytes impl`。
- 生命周期差异：同一 10,000 次普通局部作用域测试中，VM 析构计数为 0、LLVM 为 10,000；VM 对象槽仍回收并复用，所以这项差异目前是析构时机不一致的证据，不是对象槽持续增长的证据。
- 探针限制：容器压力用例覆盖 string/bytes/array/dict 的构造和作用域退出，没有验证 dict 项读取；VM 的 `len(dict)` 不支持，组合动态条件的 LLVM 版本报无效条件值，需单独检查这条编译路径。
- 限制：LLVM 侧没有统一的 class/box/string/bytes/array/dict 存活分配计数。析构计数、VM 对象/Cell 计数和工作集趋势不能证明所有 C++ 分配均无泄漏。

## 后续待办

- 优先完成 loopback socket/thread 独立程序和客户端压测；VM 与 LLVM 都跑完后再判断真实应用场景是否仍有泄漏。
- 明确 VM 与 LLVM 的 `operator[~]` 规则：普通局部作用域退出时，VM 不调用而 LLVM 会调用；按语言期望统一析构时机。
- 为 LLVM 运行时增加可查询的对象/box 创建与释放计数，为 VM 的容器值补充相同口径；不要仅依赖工作集。
- 扩展测试至多轮重复运行和更大分配量，继续跟踪函数返回、变量覆盖和 `delete`。
- 完成无环对象生命周期测试后，再单独设计循环引用测试与收集触发策略。

## Socket/thread 回归实测（2026-10-02）

- VM 端运行 ZeusZTest/acyclic_socket_thread_memory_test.zeu，监听 127.0.0.1:53127；本机客户端顺序发送 1,000 个请求，1,000 个都收到 HTTP/1.1 200 OK 和 OK，耗时约 5.25 秒。
- 每个请求创建 ClientContext 和 Thread，执行 start()、join()，然后显式 delete Thread、context 和 client socket。context 只持有 socket，不反向持有 Thread；本轮没有刻意制造循环引用。
- VM 采样内存从第 100 个请求的 20.2109 MB 增至 200–700 请求期间的 20.25 MB，800–1,000 请求期间为 20.2578 MB；采样时 Thread 数为 0，Cell Current 为 48，VM 对象槽位为 9。结束并关闭 server 后，Cell Current 为 45，存活 VM 对象为 3（Array 和两个 Namespace），socket 均断开。观察到少量启动期增长后趋稳；这只说明本次 VM 场景没有线性增长，不能排除原生分配泄漏。
- AOT 只尝试到编译阶段：LLVM 发射在 ThreadPool::start 处因访问不存在的 _workerLoop 字段失败，未生成并运行 exe。另从 runtime 注册代码确认 standalone runtime 当前没有 socket ABI 和 thread create/start/join，因此尚无 LLVM 端泄漏结果。
- 本次仅覆盖 VM 串行 1,000 请求；尚未覆盖 AOT 执行、10,000 请求或并发客户端。成功场景是串行 accept / start / join，不代表并行负载下的结论。

## 本轮后续测试计划

- [x] 修复 LLVM 发射 ThreadPool::start 时访问 _workerLoop 的根因，并让该程序通过 AOT 编译。
- [x] 补齐或明确 standalone runtime 的 socket 与 thread 生命周期接口，再运行同一 loopback 场景；记录 exe 的 Private Bytes 和运行前后对象/句柄指标。
- [ ] VM 与 LLVM 各跑 10,000 次串行请求，再加入受控并发客户端；每轮结束确认 worker 已 join、socket 已关闭。
- [ ] 在无循环引用场景下重复多轮，确认进程内存和活动资源计数不会随请求数持续上升。

## LLVM AOT 内存回归（2026-10-02）

- 根因：条件分支进入多前驱合流时，LLVM 发射器在分支源块为每条边分别克隆借用的动态局部值；克隆指令实际无条件执行，未选择分支上的闭包副本因此丢失。
- 修复：条件跳转前只把借用的动态局部值提升为一份拥有所有权的副本，两条路径共享此副本；覆盖默认参数分支和普通真假条件分支。
- ZeusZTest/bound_method_lifetime_llvm_probe.zeu：AOT 运行 1,000 次绑定方法 + Thread start/join/delete；每 100 次显式 GC 后 __gc_native_nodes() 均为 0，进程退出码 0。
- ZeusZTest/acyclic_socket_thread_aot_memory_test.zeu：AOT 完成 10,000 次 loopback HTTP 请求，全部收到 HTTP 200/OK；每 1,000 次 GC 节点和活动线程数均为 0，句柄数 63、操作系统线程数 3。
- Private Bytes 在 1,000 次时约 1.21 MB、9,000 次时约 4.39 MB；10,000 次检查点后进程正常退出。GC 节点归零证明本次跟踪到的闭包/捕获单元不再累积，但这次测试尚不能区分 Windows 堆保留空闲页与未纳入 GC 计数的分配。
- 下一步：保留 10,000 次 AOT 回归；单独测量更长运行的 Private Bytes，查明其增长是否平台化。循环引用按当前范围暂不测。


## C++ 运行时对象追踪与 AOT 回归（2026-10-03）

- [x] 新增分层调试模块：默认只做低开销原子计数；设置 `ZEUS_RUNTIME_DEBUG_DETAILS=1` 时记录仍存活对象的类别、地址和创建位置。
- [x] 修复 LLVM 在循环回边及分支合流处丢弃局部变量时漏掉拥有引用的类对象：按资源类型生成清理路径。
- [x] 详细模式运行 `cpp_runtime_debug_aot_probe.zeu` 10,000 轮：`ZeusBox` 创建/销毁 160,003/160,003，`native-class-object` 20,001/20,001；两者 `live=0`，`unmatched_destroy=0`，`detail_failures=0`，探针析构回调 10,001 次。
- [x] 条件分支探针验证修复前 41/40（`live=1`），修复后 41/41（`live=0`），析构回调 21 次。
- [x] 使用同一调试模块重跑 10,000 次 AOT socket/thread 负载，对照对象计数与 Private Bytes，继续区分 Zeus 跟踪对象和其他 C++/系统分配。
- 这次结果证明探针覆盖到的 ZeusBox 与注册类对象已平衡；它不代表未纳入模块的 STL、任意 C++ 分配或系统内存均无泄漏。

## VM / LLVM 10,000 轮 socket/thread 对照（2026-10-03）

- [x] 修复顶层 LLVM 字面量所有权标记：生成的 ZeusValue 临时值之前被标为动态值，但 ownsDynamic 留在默认 false；已为 nil、bytes、bigint、float、string 字面量标记所有权。
- [x] AOT IR 复核：Socket.bind("127.0.0.1", 10232) 调用返回后释放主机地址字符串。10,000 个 loopback 请求全部收到 HTTP 200/OK，进程退出码 0。
- [x] AOT 详细对象计数：ZeusBox 1,120,132 创建 / 1,120,131 销毁，live=1；native class 40,001 / 40,001；closure、capture cell、thread record 均 10,000 / 10,000；unmatched destroy 和 detail failures 均为 0。
- [x] AOT 第二轮（含退出报告）Private Bytes 每 1,000 次采样在 909,312 到 974,848 bytes 间波动，10,000 次后为 950,272 bytes，没有随请求数持续上涨。
- [x] VM 同场景 10,000 个请求全部收到 HTTP 200/OK，进程退出码 0；Threads 始终为 0，VM Objects 保持 12，循环期间 Active Closures 保持 21，关闭服务后为 20。
- [x] VM Private Bytes 每 1,000 次采样为 15,912,960 到 16,678,912 bytes 间波动，10,000 次后为 15,994,880 bytes；__vm_stats 工作集约 19.87 到 20.00 MB。
- [x] AOT 收尾报告验证模块全局的生命周期：源程序结束时的即时报告仍有 1 个 rooted-value-clone:boxed；LLVM 清理全局、执行 GC 后，zeus_runtime_debug_dump_at_exit() 报告 ZeusBox 1,120,132/1,120,132、所有资源 live=0、detail_failures=0。因此这个固定值随全局清理释放，不是进程退出后的泄漏。
- 范围说明：本轮只证明无循环引用的 socket/thread 压测没有逐请求增长；不能据此排除循环引用或未纳入运行时对象计数的 C++ / 系统分配泄漏。