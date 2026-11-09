"""Check nullable types (`T?`) and safe navigation (`?.`).

A nullable binding accepts nil. A bare call on it is a compile error;
`?.` short-circuits to nil instead, including across chains. Arguments
are evaluated eagerly, like `&&` operands.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "nullable_safe_nav_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))
EXPECTED_LINES = [
    "nil", "nil", "1", "1", "42", "41", "42", "nil", "42", "nil",
    "7", "-1", "42", "-1", "5", "-1",
]
BARE_CALL_ON_NULLABLE = """class Node {
    using &this;
    func get() -> int { return 1; }
}
func main() {
    let a: Node? = nil;
    print(a.get());
}
main();
"""
SAFENAV_ON_NONNULL = """class Node {
    using &this;
    func get() -> int { return 1; }
}
func main() {
    let b = new Node();
    print(b?.get());
}
main();
"""
NONNULL_DEFAULT_NIL = """func f(x: int = nil) -> int {
    return x;
}
func main() {
    print(f());
}
main();
"""
PRUNE_CLASSES = "\n".join(
    f"""class Prune{f} {{
    using &this;
    var v: int = {f};
    func operator[[]](k: string) -> int {{ return this.v; }}
    func ping() -> int {{ return {f}; }}
}}"""
    for f in range(8)
)
PRUNE_SOURCE = PRUNE_CLASSES + """
func handler(params: dict? = nil) -> int {
    if (params != nil) {
        return params["x"];
    }
    return -1;
}
func main() {
    let d: dict = {"x": 9};
    print(handler(d));
    print(handler());
}
main();
"""


def decode(data):
    return data.decode("mbcs", errors="replace")


def printed_lines(output):
    return [line.strip() for line in output.splitlines() if line.strip()]


def check_run(label, returncode, output):
    if returncode != 0:
        raise AssertionError(f"{label} failed ({returncode})\n{output}")
    actual = printed_lines(output)
    if actual != EXPECTED_LINES:
        raise AssertionError(
            f"{label} printed {actual}, expected {EXPECTED_LINES}\n{output}"
        )
    print(f"{label} printed the expected values")


def run_vm():
    result = subprocess.run(
        [str(ZEUS), str(SOURCE)],
        cwd=ROOT,
        capture_output=True,
        timeout=120,
    )
    return result.returncode, decode(result.stdout) + decode(result.stderr)


def run_aot_exe():
    if os.name != "nt":
        raise SystemExit("This AOT regression test requires the Windows compiler toolchain.")
    with tempfile.TemporaryDirectory(
        prefix="zeus-nullable-safe-nav-", dir=SOURCE.parent
    ) as temp_dir:
        executable = Path(temp_dir) / "nullable_safe_nav_test.exe"
        compile_command = f'"{ZEUSC_BATCH}" -o "{executable}" "{SOURCE}"'
        compiled = subprocess.run(
            compile_command,
            cwd=ROOT,
            shell=True,
            capture_output=True,
            timeout=900,
        )
        stdout = decode(compiled.stdout) + decode(compiled.stderr)
        if compiled.returncode != 0:
            raise AssertionError(f"zeusc failed ({compiled.returncode})\n{stdout}")
        result = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            capture_output=True,
            timeout=60,
        )
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        return result.returncode, output


def check_pruning():
    compiler = ROOT / "ZeusCompiler.exe"
    if not compiler.exists():
        raise SystemExit("ZeusCompiler.exe was not found; build the compiler first.")
    # ZeusCompiler.exe opens the output path with std::ofstream; some sandboxed
    # directories reject that handle even though Python can write there. The
    # compiler build directory is the location the harness already trusts.
    build_dir = ROOT / "ZeusCLI" / ".build"
    build_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="zeus-nullable-prune-", dir=build_dir
    ) as temp_dir:
        probe = Path(temp_dir) / "probe.zeu"
        probe.write_text(PRUNE_SOURCE, encoding="utf-8")
        llvm_file = Path(temp_dir) / "program.ll"
        result = subprocess.run(
            [str(compiler), "--emit-llvm", str(probe), str(llvm_file)],
            cwd=ROOT,
            capture_output=True,
            timeout=300,
        )
        output = decode(result.stdout) + decode(result.stderr)
        if result.returncode != 0:
            raise AssertionError(f"nullable pruning probe failed\n{output}")
        layouts = re.search(r"reachable class layouts (\d+)/(\d+)", output)
        if layouts is None or int(layouts.group(1)) > 2:
            raise AssertionError(
                "a dict? parameter retained unrelated class layouts\n" + output
            )
    print("dict? parameter reachability stayed monomorphic")


def check_error(label, source_text, needle):
    with tempfile.TemporaryDirectory(prefix="zeus-nullable-neg-") as temp_dir:
        probe = Path(temp_dir) / "probe.zeu"
        probe.write_text(source_text, encoding="utf-8")
        result = subprocess.run(
            [str(ZEUS), str(probe)],
            cwd=ROOT,
            capture_output=True,
            timeout=120,
        )
        output = decode(result.stdout) + decode(result.stderr)
        if result.returncode == 0:
            raise AssertionError(f"{label} unexpectedly compiled\n{output}")
        if needle not in output:
            raise AssertionError(f"{label} missing diagnostic {needle!r}\n{output}")
    print(f"{label} rejected as expected")


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    check_run("VM run", *run_vm())
    check_run("AOT run", *run_aot_exe())
    check_pruning()
    check_error(
        "bare call on nullable",
        BARE_CALL_ON_NULLABLE,
        "use '?.' or check for nil first",
    )
    check_error(
        "safe navigation on non-nullable",
        SAFENAV_ON_NONNULL,
        "'?.' needs a nullable receiver",
    )
    check_error(
        "nil default on non-nullable",
        NONNULL_DEFAULT_NIL,
        "must int, not nil",
    )
    print("nullable safe navigation test passed")


if __name__ == "__main__":
    main()
