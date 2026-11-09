"""Check that 'static func name()' declarations are recognised as functions.

The 'static' modifier may precede the 'func' keyword. Before the fix the
statement was not classified as a function declaration, so the class member
was silently dropped and 'ClassName.method()' failed at runtime.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "static_method_declaration_test.zeu"
ZEUS = ROOT / "Zeus.exe"
EXPECTED_OUTPUT = ["7", "9", "11"]


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
    with tempfile.TemporaryDirectory(prefix="zeus-static-method-") as temp_dir:
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
    for symbol in ("zeus_Maker__build", "zeus_Maker__make", "zeus_topLevel"):
        if symbol not in emitted:
            raise AssertionError(f"AOT output does not define {symbol}\n{output}")
    print("AOT compile emitted the expected static method symbols")


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    check("VM run", *run_vm())
    check_aot(*run_aot())
    print("static method declaration test passed")


if __name__ == "__main__":
    main()
