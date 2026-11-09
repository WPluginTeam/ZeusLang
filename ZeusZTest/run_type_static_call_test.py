"""Check that a type implementation's static method is callable by type name.

impl int { static func twice(value: int) -> int { ... } } is called as
int.twice(3). The VM resolves the method through the type implementation
table and the AOT lowers it to a direct call of the qualified name.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "type_static_call_test.zeu"
ZEUS = ROOT / "Zeus.exe"
EXPECTED_OUTPUT = ["6", "8", "0", "8", "1"]


def decode(data):
    return data.decode("mbcs", errors="replace")


def run_vm():
    result = subprocess.run(
        [str(ZEUS), str(SOURCE)],
        cwd=ROOT,
        capture_output=True,
        timeout=120,
    )
    return result.returncode, decode(result.stdout) + decode(result.stderr)


def run_aot():
    with tempfile.TemporaryDirectory(prefix="zeus-type-static-") as temp_dir:
        llvm_file = Path(temp_dir) / "program.ll"
        result = subprocess.run(
            [str(ZEUS), "--emit-llvm", str(SOURCE), str(llvm_file)],
            cwd=ROOT,
            capture_output=True,
            timeout=300,
        )
        output = decode(result.stdout) + decode(result.stderr)
        emitted = ""
        if llvm_file.exists():
            emitted = llvm_file.read_text(encoding="utf-8", errors="replace")
        return result.returncode, output, emitted


def check(label, returncode, output):
    if returncode != 0:
        raise AssertionError(f"{label} failed ({returncode})\n{output}")
    printed = [line.strip() for line in output.splitlines() if line.strip()]
    missing = [value for value in EXPECTED_OUTPUT if value not in printed]
    if missing:
        raise AssertionError(f"{label} did not print {missing}\n{output}")
    print(f"{label} printed the expected values")


def check_aot(returncode, output, emitted):
    if returncode != 0:
        raise AssertionError(f"AOT compile failed ({returncode})\n{output}")
    if "zeus_int__twice" not in emitted:
        raise AssertionError("AOT output does not call the type static method\n" + output)
    if "zeus_bytes__describe" not in emitted:
        raise AssertionError("AOT output does not call the bytes static method\n" + output)
    print("AOT compile emitted direct calls to the type static methods")


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    check("VM run", *run_vm())
    check_aot(*run_aot())
    print("type static call test passed")


if __name__ == "__main__":
    main()
