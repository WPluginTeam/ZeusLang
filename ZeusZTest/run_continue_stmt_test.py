"""Check that `continue` skips to the next iteration in every loop form.

The parser already had a CONTINUE_STMT node but nothing produced it, so the
keyword fell through to the expression path and the compiler reported a syntax
error. The regression pins the VM and the AOT build on the four loop kinds and
on a nested loop, where a continue must stay inside its own loop.
"""

from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "continue_stmt_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "CONTINUE_STMT_PASS"


def decode(data):
    return data.decode("mbcs", errors="replace")


def run(command, label, timeout, marker=None):
    try:
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"") + (error.stderr or b"")
        raise AssertionError(f"{label} timed out after {timeout}s\n{decode(output)}")
    output = decode(result.stdout) + decode(result.stderr)
    missing = marker is not None and marker not in output
    if result.returncode or missing:
        raise AssertionError(f"{label} failed ({result.returncode})\n{output[-8000:]}")
    print(f"{label} passed")
    return output


def run_vm():
    run([str(ZEUS), str(SOURCE)], "VM run", 120, MARKER)


def run_aot():
    with tempfile.TemporaryDirectory(prefix="zeus-continue-") as temp_dir:
        executable = Path(temp_dir) / "continue.exe"
        command = f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{SOURCE}"'
        run(command, "AOT build", 300)
        run([str(executable)], "AOT run", 120, MARKER)


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")
    run_vm()
    run_aot()
    print("continue statement test passed")


if __name__ == "__main__":
    main()
