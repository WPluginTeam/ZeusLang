; Zeus AOT demo：print("hello") 的正式 ABI 生成物
;
; 对应 .zeu：
;   print("hello")
;
; 链路：常量字符串装箱（zeus_box_cstr）→ 打印（zeus_print）。
; 注意：这正是 Emitter 以后要自动生成的模式，不再是直调 puts 的 toy 版。

; 注：不写 target triple，clang 驱动按宿主（x86_64-pc-windows-msvc）推断。

@.str = private unnamed_addr constant [6 x i8] c"hello\00", align 1

declare ptr @zeus_box_cstr(ptr)
declare void @zeus_print(ptr)
declare void @zeus_box_release(ptr)

define i32 @zeus_entry() {
entry:
  %b = call ptr @zeus_box_cstr(ptr @.str)
  call void @zeus_print(ptr %b)
  call void @zeus_box_release(ptr %b)
  ret i32 0
}
