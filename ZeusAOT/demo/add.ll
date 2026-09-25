; Zeus AOT demo：x = 40 + 2; print(x) 的正式 ABI 生成物
;
; 对应 .zeu：
;   var x = 40 + 2;
;   print(x);
;
; 链路：装箱 → zeus_int_add（内含 P0 int64 快路径）→ 打印 → 归还装箱。
; 期望输出：42。

declare ptr @zeus_box_int(i64)
declare ptr @zeus_int_add(ptr, ptr)
declare void @zeus_print(ptr)
declare void @zeus_box_release(ptr)

define i32 @zeus_entry() {
entry:
  %a = call ptr @zeus_box_int(i64 40)
  %b = call ptr @zeus_box_int(i64 2)
  %s = call ptr @zeus_int_add(ptr %a, ptr %b)
  call void @zeus_print(ptr %s)
  call void @zeus_box_release(ptr %a)
  call void @zeus_box_release(ptr %b)
  call void @zeus_box_release(ptr %s)
  ret i32 0
}
