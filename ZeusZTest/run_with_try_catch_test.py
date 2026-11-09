"""Check that with exit hooks agree across VM and AOT exception paths."""

import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "with_try_catch_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "WITH_TRY_CATCH_PASS"
OPT_LEVELS = ("0", "2")


def decode(data):
    return data.decode("mbcs", errors="replace")


def check(label, result):
    output = decode(result.stdout) + decode(result.stderr)
    if result.returncode != 0:
        raise AssertionError(f"{label}: exited with {result.returncode}\n{output}")
    if MARKER not in output:
        raise AssertionError(f"{label}: missing '{MARKER}'\n{output}")
    print(f"PASS: {label}")


def run_vm():
    result = subprocess.run(
        [str(ZEUS), str(SOURCE)], cwd=ROOT, capture_output=True, timeout=180
    )
    check("Zeus VM", result)


def run_aot():
    with tempfile.TemporaryDirectory(prefix="zeus-with-try-catch-") as temp_dir:
        temp_path = Path(temp_dir)
        for level in OPT_LEVELS:
            executable = temp_path / f"with_try_catch_o{level}.exe"
            compiled = subprocess.run(
                f'"{ZEUSC}" --opt-level {level} -o "{executable}" "{SOURCE}"',
                cwd=ROOT,
                shell=True,
                capture_output=True,
                timeout=300,
            )
            if compiled.returncode != 0:
                output = decode(compiled.stdout) + decode(compiled.stderr)
                raise AssertionError(f"AOT O{level} compile failed\n{output}")

            result = subprocess.run(
                [str(executable)], cwd=ROOT, capture_output=True, timeout=120
            )
            check(f"AOT O{level}", result)


def main():
    run_vm()
    run_aot()
    print("WITH_TRY_CATCH_TEST_PASS")


if __name__ == "__main__":
    main()
