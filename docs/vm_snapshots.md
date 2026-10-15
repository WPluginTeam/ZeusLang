# VM Breakpoint Snapshots

Run a Zeus source file with one or more source line breakpoints:

```text
Zeus.exe --breakpoints 1,3 example.zeu
```

The VM writes one JSON file before the first instruction attributed to each
breakpoint line, then continues execution. Re-entering a line through a loop
creates another file. Files are placed in `example.zeu.snapshots` beside the
source file. The equivalent `--breakpoints=1,3` form is also accepted.

Each snapshot records the current instruction, VM call frames, operand stack,
global slots, and frame local and upvalue slots. Array and dictionary values
are represented by their sizes, and VM object handles by their IDs, so cyclic
values cannot recurse while the snapshot is being written.

Line numbers refer to the main source file. This is a logical VM state dump,
not a restorable process image: it does not copy object heap contents or
capture other threads at one synchronized instant. Snapshot write failures
are reported and stop the VM run. Breakpoints are available for VM execution;
LLVM output and compile-only commands do not run them.
