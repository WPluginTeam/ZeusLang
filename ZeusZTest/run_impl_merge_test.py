"""Check that a later `impl` block merges into an existing type implementation.

Two impl blocks on the same builtin type must add members instead of replacing
the table, and the library's own members have to survive. Instance methods need
`using &this;`; without it the compiler treats the function as static (P0044),
which is exactly what the regression pins apart.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "impl_merge_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "IMPL_MERGE_PASS"


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

    with tempfile.TemporaryDirectory(prefix="zeus-impl-merge-") as temp_dir:
        executable = Path(temp_dir) / "impl_merge.exe"
        command = f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{SOURCE}"'
        run(command, "AOT build", 300)
        run([str(executable)], "AOT run", 120, MARKER)

    print("impl merge test passed")


if __name__ == "__main__":
    main()
