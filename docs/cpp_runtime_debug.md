# C++ runtime diagnostics

`ZeusRuntimeLib/ZeusRuntimeDebug.hpp` provides counters for Zeus-owned runtime
resources. The counters are always active and use atomic integer updates.

Tracked resources in the first version:

- Heap allocated `ZeusBox` values.
- Native closure records and capture cells.
- LLVM native class objects registered with the lifetime manager.
- Native thread records.
- Dynamic field sidecars attached to native objects.

Each category reports total creations, destructions, current live count, peak
live count, and unmatched destruction calls. C++ code can call
`ZeusRuntimeDebug::counter()` and `ZeusRuntimeDebug::writeReport()`. AOT code
can call the C ABI report function:

```zeus
@native("zeus_runtime_debug_dump")
decl func runtime_debug_dump() -> int;

runtime_debug_dump();
```

`runtime_debug_dump()` reports the current state. To also report after the
generated program has cleared its global slots, register an exit-time report:

```zeus
@native("zeus_runtime_debug_dump_at_exit")
decl func runtime_debug_dump_at_exit() -> int;

runtime_debug_dump_at_exit();
```

This makes it possible to distinguish a value still held by a module global at
the end of the Zeus program from a value that remains after global cleanup.

To include addresses and creation locations for resources still alive, set
`ZEUS_RUNTIME_DEBUG_DETAILS=1` before invoking `zeusc.bat`. The runtime then
prints one `live_object` line per tracked resource. This mode allocates a
diagnostic table and is intended for debugging, not performance measurements.

The module does not intercept arbitrary C++ `new`/`malloc`, STL container
buffers, or operating-system allocations. Compare these object counts with
Private Bytes or the platform heap tools; Private Bytes alone cannot identify
which resource category retained memory.
