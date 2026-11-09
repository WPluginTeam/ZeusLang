"""Check that `return` accepts a dictionary literal.

The parser only treated declarations, assignments and expression statements as
dictionary contexts, so `return {"a": 1};` opened a code block and reported
P0015. Nested braces were worse: the inner `{` opened a second block. The
regression pins single-line, multi-line, empty and nested literals in both the
VM and the AOT build.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "return_dict_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "RETURN_DICT_PASS"


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


def main():
    if not ZEUS.exists():
        raise SystemExit("Zeus.exe was not found; build the compiler first.")

    run([str(ZEUS), str(SOURCE)], "VM run", 120, MARKER)

    with tempfile.TemporaryDirectory(prefix="zeus-return-dict-") as temp_dir:
        executable = Path(temp_dir) / "return_dict.exe"
        command = f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{SOURCE}"'
        run(command, "AOT build", 300)
        run([str(executable)], "AOT run", 120, MARKER)

    print("return dictionary test passed")


if __name__ == "__main__":
    main()
