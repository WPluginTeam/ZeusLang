"""Verify zeus.file opens, writes, reads, appends, seeks and refuses."""
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(__file__).with_name("file_test.zeu")
MARKER = "FILE_TEST_PASS"
GROUND_TOKEN = "__GROUND__"


def check(exe, timeout):
    ground = Path(tempfile.mkdtemp(prefix="file_test_"))
    (ground / "GroundFileTest").mkdir(parents=True, exist_ok=True)
    try:
        work = Path(tempfile.mkdtemp(prefix="file_test_run_"))
        try:
            # The runner hands the module a separate ground directory that is
            # removed after the test, so repeated runs cannot read stale files.
            source = work / "file_test_runner.zeu"
            source.write_text(
                SOURCE.read_text(encoding="utf-8-sig").replace(
                    GROUND_TOKEN, (ground / "GroundFileTest").as_posix()),
                encoding="utf-8",
            )
            try:
                result = subprocess.run(
                    [str(exe.absolute()), str(source)],
                    cwd=work, capture_output=True, timeout=timeout,
                )
            except subprocess.TimeoutExpired as error:
                output = ((error.stdout or b"") + (error.stderr or b""))
                raise RuntimeError(
                    f"file test timed out: {exe}\n{output.decode('utf-8', errors='replace')}"
                ) from error
            output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
            if result.returncode != 0 or MARKER not in output:
                raise RuntimeError(
                    f"file test failed: {exe}\nexit={result.returncode}\n{output}"
                )
            print(f"PASS: {Path(str(exe)).name} / {MARKER}")
        finally:
            shutil.rmtree(work, ignore_errors=True)
    finally:
        shutil.rmtree(ground, ignore_errors=True)


def main():
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "Zeus.exe")
    parser.add_argument("--timeout", type=float, default=30)
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    check(args.exe, args.timeout)


if __name__ == "__main__":
    main()
