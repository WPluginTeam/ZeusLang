"""Check that a post-added attribute works on a class with declared fields.

The VM stores the new attribute dynamically. The AOT keeps the declared
fields inline and lowers the inferred field to a dynamic slot; compiling
used to fail the OBJ_BUILD layout check instead.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "class_inferred_field_test.zeu"
ZEUS = ROOT / "Zeus.exe"
EXPECTED_OUTPUT = ["5", "9"]


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
    with tempfile.TemporaryDirectory(prefix="zeus-inferred-field-") as temp_dir:
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
    if "%zeus_Box = type { i64, %ZeusValue, i1 }" not in emitted:
        raise AssertionError(
            "AOT output does not keep the declared field inline "
            "with a dynamic slot for the inferred field\n" + output
        )
    print("AOT compile kept the declared field inline with a dynamic inferred slot")


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    check("VM run", *run_vm())
    check_aot(*run_aot())
    print("class inferred field test passed")


if __name__ == "__main__":
    main()
