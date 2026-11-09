// Zeus AOT runtime 入口（MSVC 编进 libzeus_rt.lib，一次编译到处链接）。
//
// main 常驻这里：以后在这里做 Context 初始化与收尾。
// 每个程序的变化量只有一个 .ll 文件（它提供 zeus_entry）。

#include "zeus_rt.h"

namespace {

int zeusProcessExitCode = 0;

}

extern "C" void zeus_runtime_set_exit_code(ZeusValue* result, int code) {
    zeus_value_init_nil(result);
    zeusProcessExitCode = code;
}

int main() {
    try {
        zeus_entry();
        return zeusProcessExitCode;
    } catch (...) {
        zeus_exception_report_current();
        return 1;
    }
}
