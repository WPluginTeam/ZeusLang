# Socket 迁移与共享实例

`socket.zeu` 直接声明 Windows Winsock 原生接口。旧的 VM socket 内建与单独 Socket ABI 源文件已移除，解释执行与 AOT 使用同一个模块。

## 共享变量的实例语义

```zeus
let original = new Socket();
var shared connection = original;
```

`connection` 和 `original` 指向同一个实例。初始化、重新赋值和读取共享槽都保持对象身份，不再复制字段形成另一个对象。重新赋值只更换共享槽持有的实例；已有别名继续持有原实例。

```zeus
let previous = connection;
connection = new Socket();
// previous 仍然持有原 Socket。
```

`delete connection` 显式销毁当前实例，并使指向该实例的别名失效。将共享槽改为 `nil` 只释放这个槽的持有关系；最后一个持有者释放后才析构。

普通 `let b = a` 的整体复制规则本轮没有修改。

## 回归入口

- `ZeusZTest/run_shared_instance_identity_test.py`：共享槽独占根、别名、自赋值、重新绑定、析构、跨函数 GC 与工作线程访问。
- `ZeusZTest/run_socket_winsock_test.py`：数字地址和 localhost、二进制收发、句柄移交、EOF 与原生错误、关闭取消阻塞、延迟分段收包，独立和同进程组合运行。
- 两个 runner 都支持 `--exe` 指定解释器；socket runner 的 `--aot-exe` 接受已编译的基础 socket 回归程序。

测试只使用本机回环地址，并为阻塞用例设置进程超时。

## 共享槽的退出释放边界

解释执行时，共享槽保留至进程结束。当前主 VM 析构不会执行仅由共享槽持有的对象的 Zeus 析构函数；进程级共享堆销毁槽值时，也没有对应的 VM 对象析构回调。

AOT 在 entry 正常或异常离开时先释放共享槽，再释放局部变量及声明逆序的全局变量。普通别名仍持有实例时，由最后一个持有者触发析构；模块依赖在使用它的对象之后释放。

VM 在全局退出时先按现有顺序释放主程序变量，再释放主程序导入别名；之后按现有顺序释放包全局和包内导入别名。`.zir` 文件会保存这份槽位布局，旧文件没有该信息时沿用原来的单一程序清理顺序。

资源需要确定释放时，应显式调用 `connection.close()`，或用 `delete connection` 销毁实例。多个独立 VM 的共享名称作用域、槽归属与 owner 退出释放规则，需要单独设计。

跨 VM GC 根回归入口为 `ZeusZTest/run_vm_peer_gc_roots_test.py`，覆盖主线程与工作线程互相收集时保留对方局部对象，并验证 native 等待、join 与析构次数。

AOT 线程回归：`run_socket_aot_workers_test.py --compiler <zeusc.bat> --repeat 3 --work-dir <目录>`，保留构建与运行日志；覆盖 accept、recv、延迟分段以及组合场景。`aot_shared_exit_dependency_test.zeu` 与对应 runner 验证正常、异常退出时共享资源与普通全局依赖的析构顺序。

## 本轮验证

- VM socket 全套连续 10 轮通过；AOT 四种 worker 场景连续 3 轮通过，均检查退出码。
- 跨线程 GC、64 轮启动交接、析构重入、共享身份、AOT 删除生命周期及正常/异常退出依赖顺序通过。
- 完整语言回归为 56 项通过、0 项失败；新版编译器的 requests 本机 HTTP 集成通过。
- 对象密集微基准（500 对象、5000 次字段读取）交替 7 次的进程耗时中位数：旧版 0.2121 秒，新版 0.2760 秒；启动线程并 join 后为 0.2497 与 0.3124 秒。根追踪仍有约 25–30% 开销，后续可继续优化持有屏障。

后续设计事项：VM 退出时共享槽的所属程序及释放规则，以及仍有工作线程运行时的统一退出顺序。
