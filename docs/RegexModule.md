# `zeus.re`

`zeus.re` provides a Python-style regular-expression API backed by C++ `std::regex`. It does not load a separate DLL. Patterns use the C++ ECMAScript grammar.

The module binds its native functions with `@native`. The VM resolves them through the native injection table, while AOT links the corresponding ZeusValue ABI entry points; it does not use `registerBuiltin`.

```zeu
import zeus.re [&re];

let pattern = re.compile("([a-z]+)-([0-9]+)");
let found = pattern.search("id abc-42");
if (found != nil) {
    print(found.group(1));
    print(found.span(2));
}
```

The module provides `compile`, `search`, `match`, `fullmatch`, `findall`, `finditer`, `split`, `sub`, `subn`, and `escape`. A compiled `Pattern` exposes the corresponding search and replacement methods. `Match` provides `group`, `groups`, `start`, `end`, and `span`.

`finditer` returns an `Array` of `Match` values. `span` returns a two-element `Array`. `subn` returns `[new_text, replacement_count]`. `findall` returns full matches when there are no captures, one string per match when there is one capture, and arrays of capture strings when there are multiple captures.

Supported flags are `IGNORECASE` / `I` (`2`) and `DOTALL` / `S` (`16`). They can be passed as the `flags` argument; AOT code should use these numeric values because AOT does not currently load module-level values through namespace attributes.

The current ECMAScript engine does not support Python named groups, lookbehind, `VERBOSE`, or `MULTILINE`. Match offsets count UTF-8 bytes.
