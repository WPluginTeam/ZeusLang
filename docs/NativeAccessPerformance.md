# Native access and JSON parser performance

This note records the measured cost of the raw memory intrinsics (`@addr`,
`@reveal`, `@faddr`) and of the `zeus.json` scanner, so that a change in
either area can be judged against numbers instead of intuition.

All numbers come from the benchmarks in `ZeusZTest/` on an AMD Ryzen 9
8940HX with MSVC 2022 x64. The VM numbers use `Zeus.exe`, the AOT numbers
use `zeusc.bat --opt-level 2`. The two backends differ by roughly two orders
of magnitude, so a number is only comparable within one backend.

## `@faddr` cost model

`@faddr` has three forms and they do not cost the same.

| Form | What it returns | AOT cost per access |
| --- | --- | --- |
| `@faddr(addr, W) -> int` (`W` is 1, 2, 4 or 8) | a raw integer read | 136-161 ns |
| `@faddr(addr) -> T` where `addr` came from `@addr(x)` | a boxed Zeus value | 760-780 ns |
| `@faddr(addr, L) -> &bytes` | a `{ptr, i64}` view | free to create |

The raw width form is the cheap one. It compiles to a single call that
returns immediately plus an inline `load`, with no box, no allocation, no
lock, no registry lookup and no retain/release pair. The typed slot form goes
through `zeus_lvalue_load`, which takes a process-wide mutex, does an
`unordered_map` lookup, validates the type name (four `std::string` builds
and several registry lookups) and, for a string, bytes, class or shared
value, allocates and clones. That makes it about five times slower than the
raw form for the same memory.

A `&bytes` view is the right primitive for bulk raw access: it is a plain
`{ptr, i64}` by value, it never touches the slot or ownership registries, and
its index operation is a bounds-checked byte load. In AOT, `view[i]` and
`@faddr(a, 1)` measure at parity (156-163 ns versus 152-161 ns per
operation). The existing `ZeusZTest/raw_bytes_range_faddr_benchmark.zeu`
confirms that, but it only exercises the cheapest configuration: no `try`
region, a registered address, and the raw width form.

### The `try` region cliff

Outside a `try` region the raw form costs a call that returns immediately.
Inside one, every access pays:

1. a global mutex,
2. a hash lookup over the slot table,
3. a linear scan over the slot table,
4. a linear scan over the registered owned raw ranges,
5. and, when nothing matched, a second global mutex plus a linear scan over
   every tracked Zeus allocation.

The last step is the expensive one because it grows with the number of live
allocations. Measured in AOT with 20 000 reads:

| Tracked allocations | Raw read inside `try` | Raw read outside `try` |
| --- | --- | --- |
| 100 | 329 ns | ~157 ns |
| 4 000 | 7 050 ns | ~162 ns |

The VM always validates, because it has no equivalent of the `try` gate. Its
`validateRaw` walks the whole slot table and, for every revealed entry,
takes the cell registry lock, does a hash lookup, copies the value out of
the cell and looks up its type name. That makes a raw read cost 5.2 us when
the address belongs to a registered range and 14.9 us when it does not. The
cost also grows with the number of revealed slots, so a program that reveals
many values pays for each of them on every raw read.

## `zeus.json` cost model

The scanner walks the input one byte at a time. Every byte costs a native
call (`__bytes_get`) plus a read of the `this.sourceBytes` and `this.index`
fields.

A field read is not cheap. In the VM a class field read costs a map lookup,
a copy of the value and a liveness check, which measures 1.2 us against a
4.7 us native call. In AOT the same read compiles to
`zeus_class_ref_require_live`, which takes two mutexes and a hash lookup,
and measures 165 ns against 3 ns for the same value in a local.

The scanner therefore keeps the cursor in a local and writes it back at token
boundaries. That removes one field read per input byte and, in the AOT
backend, takes the emitted `zeus_class_ref_require_live` calls in
`JSONParser::parseString` from 52 down to 38 and in `JSONParser::parseNumber`
from 69 down to 36.

### The AOT quadratic clone

The AOT backend clones the whole source buffer for every byte it reads. The
emitted IR for the scanner shows the pattern:

```llvm
  %n41 = load ptr, ptr %n40
  %n42 = invoke ptr @zeus_bytes_clone(ptr %n41)   ; the entire source buffer
  %n44 = load i64, ptr %n43
  %n45 = invoke i64 @"zeus_bytes_get"(ptr %n42, i64 %n44)
  invoke void @zeus_bytes_release(ptr %n42)
```

The clone exists because a bytes value handed to a native function has to
outlive the call, and the emitter cannot tell that `zeus_bytes_get` only
reads it. The cost grows with the document size, so the AOT scanner is
quadratic in the input length. A `bytes` value read through a local does not
help: the emitter clones a local read as well.

Two changes would remove it:

* scan the source through a `&bytes` view instead of through the bytes
  value,
* or move the scan into one native call per string, the way `zeus.re` and
  `zeus.crypto` do their scanning.

The view is the better one, because it is already the zero copy primitive
of the language: `@faddr(addr, L) -> &bytes` yields a `{pointer, length}`
pair by value, and indexing it is a plain bounds checked byte read that
never touches the buffer owner. Measured over a 640 byte buffer:

| Scan shape | VM | AOT |
| --- | --- | --- |
| `__bytes_get(bytesValue, i)` | 5 267 ns/byte | 59 ns/byte |
| `&bytes` view and `range[i]` | 4 600 ns/byte | 5 ns/byte |

The AOT cost of the view is independent of the document size, so it removes
the quadratic term outright.

A `&bytes` view could not cross a dynamic method boundary before, because
the parameter conversion chain in the dynamic member bridge had no case for
it: a `&bytes` fell through to the native pointer branch and produced a
`ptr` where the callee expected a `%ZeusRawBytesRange`, which made the LLVM
module invalid. The bridge now reads the range out of the boxed slot
instead, in the same shape the direct call path already used.

Turning the parser itself over to the view is a larger change and did not
pay off as a whole. The view removes the per byte clone, but it also moves
the cost elsewhere: the parser has to build the bytes object and the view
once per document, the byte read has to be selected per backend because the
VM implements the view index as the generic dynamic index operator, and the
resulting code was slower for several document shapes. Measured on the JSON
benchmark, VM and AOT:

| Shape | AOT before | AOT with the view |
| --- | --- | --- |
| plain strings | 420 ns/byte | 220 ns/byte |
| numbers | 887 ns/byte | 718 ns/byte |
| objects | 856 ns/byte | 1 143 ns/byte |
| escaped strings | 633 ns/byte | 2 588 ns/byte |
| stringify, 200 objects | 37.1 ms | 2 053 ms |

So the copy is the dominant per byte cost only for large documents, and the
remaining work belongs in the emitter rather than in the parser.

The emitter now borrows the buffer for the read only bytes builtins. A bytes
value that reaches a native function has to stay alive for the duration of
the call, so the emitter used to hand over an owned copy of the whole buffer
and free it right after the call. `zeus_bytes_get` and `zeus_bytes_len` only
read the buffer and never retain the pointer, so for them the copy is
unnecessary.

The emitter proves that the borrow is safe before it applies it. The loaded
value has to reach a call to a read only builtin as its first argument, the
scan only accepts simple value pushes on the way, and the callee is resolved
from the native load that precedes the arguments. The whitelist is on the
native symbol rather than on the Zeus name, because a program may bind its
own declaration to a symbol.

The effect on the JSON benchmark, AOT, measured on the same machine:

| Shape | Before | After |
| --- | --- | --- |
| plain strings | 339 ns/byte | 198 ns/byte |
| numbers | 860 ns/byte | 679 ns/byte |
| objects | 961 ns/byte | 690 ns/byte |
| escaped strings | 614 ns/byte | 407 ns/byte |
| 50 items | 885 ns/byte | 732 ns/byte |
| 100 items | 1 129 ns/byte | 741 ns/byte |
| 200 items | 852 ns/byte | 745 ns/byte |
| 400 items | 914 ns/byte | 707 ns/byte |
| 800 items | 960 ns/byte | 723 ns/byte |

The per byte cost is now flat across the scaling series, so the scan is
linear in the document size. The VM is unaffected, because it never used the
emitter's copy in the first place.

### The exponent expansion

`expandExponent` collected the mantissa one character at a time, with a
slice and a concatenation per digit. That is quadratic in the length of the
mantissa. The mantissa is now taken in one slice, and the decimal point is
removed with a single `__str_replace`.

## Benchmarks

`ZeusZTest/json_parse_benchmark.zeu` reports the parse and stringify cost
per shape, plus a scaling series whose per-byte cost must stay flat. It has
no pass marker beyond `JSON_PARSE_BENCH_PASS`; the numbers are the result.

```
Zeus.exe ZeusZTest\json_parse_benchmark.zeu
zeusc.bat ZeusZTest\json_parse_benchmark.zeu --opt-level 2
```

`ZeusZTest/raw_bytes_range_faddr_benchmark.zeu` compares a `&bytes` view
against a raw `@faddr` read and write, alternating the order of the samples
so that cache and branch prediction bias cancel out.

## Measured effect of the scanner change

| Shape | VM before | VM after | AOT before | AOT after |
| --- | --- | --- | --- | --- |
| plain strings | 18 881 ns/byte | 15 016 ns/byte | 420 ns/byte | 300 ns/byte |
| numbers | 38 491 ns/byte | 36 415 ns/byte | 887 ns/byte | 758 ns/byte |
| objects | 36 210 ns/byte | 35 145 ns/byte | 856 ns/byte | 913 ns/byte |
| escaped strings | 21 957 ns/byte | 17 733 ns/byte | 633 ns/byte | 531 ns/byte |
| stringify, 200 objects | 1 477 ms | 1 452 ms | 37.1 ms | 37.6 ms |

The stringify path stays roughly where it was. It remains about five times
slower than the parse path because it calls `type()` and `__builtin_str()`
for every value it writes.
