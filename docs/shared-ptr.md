# SharedPtr

`zeus.shared_ptr` provides a small AOT-only owner for a raw allocation:

```zeus
import zeus.shared_ptr;

let shared = new SharedPtr(8);
shared.save(123);
print(shared.read(0)); // 123; the int argument selects T=int

let alias = shared; // Zeus class aliases share the same allocation owner
```

The capacity is supplied by the caller. The current `save`/`read` implementation
uses `@faddr` directly; the allocation must be large enough for the selected
type. Plain `int` and `bool` slots are suitable. `string`/`bytes` have view and
length rules, ZeusValue-backed types need clone/release handling, and class
values need retain/release handling, so those types are not safe generic
round-trips here.

The last Zeus class alias runs `operator[~]`, which calls `free`. The memory API
is LLVM/AOT-only; running this module in the VM requires a separately registered
VM ABI implementation.

For a hot loop, `address()` exposes the raw allocation address once so the loop
can use `@faddr` directly instead of paying for a class method call on every
read and write. That path keeps the same raw-memory bounds and lifetime
responsibilities as other `@faddr` operations. The benchmark in
`ZeusZTest/shared_ptr_malloc_benchmark.zeu` compares both access styles.
