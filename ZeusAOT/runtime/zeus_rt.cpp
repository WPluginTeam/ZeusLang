// Zeus AOT runtime 入口（MSVC 编进 libzeus_rt.lib，一次编译到处链接）。
//
// main 常驻这里：以后在这里做 Context 初始化与收尾。
// 每个程序的变化量只有一个 .ll 文件（它提供 zeus_entry）。

#include "zeus_rt.h"

int main() {
    try {
        return zeus_entry();
    } catch (...) {
        zeus_exception_report_current();
        return 1;
    }
}
