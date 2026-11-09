"""Verify zeus.file directory and path operations.

`mkdir/mkdirs/exists/isDirectory/rename/removeFile/removeDir/listDir` are the
four platform operations SessionJournal needs (create, test, replace, list).
The runner hands the module a throwaway ground directory and deletes it after
the run, so repeated runs cannot read stale entries.
"""

from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "file_path_test.zeu"
ZEUS = ROOT / "Zeus.exe"
ZEUSC = ROOT / "zeusc.bat"
MARKER = "FILE_PATH_TEST_PASS"
GROUND_TOKEN = "__GROUND__"


def decode(data):
    return data.decode("mbcs", errors="replace")


def run(command, label, timeout, marker=None, cwd=None):
    try:
        result = subprocess.run(
            command, cwd=cwd or ROOT, capture_output=True, timeout=timeout
        )
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

    ground = Path(tempfile.mkdtemp(prefix="file-path-"))
    work = Path(tempfile.mkdtemp(prefix="file-path-run-"))
    try:
        source = work / "file_path_test.zeu"
        source.write_text(
            SOURCE.read_text(encoding="utf-8-sig").replace(
                GROUND_TOKEN, ground.as_posix()
            ),
            encoding="utf-8",
        )

        run([str(ZEUS), str(source)], "VM run", 120, MARKER)

        executable = work / "file_path_test.exe"
        run(
            f'"{ZEUSC}" --opt-level 2 -o "{executable}" "{source}"',
            "AOT build",
            300,
        )
        run([str(executable)], "AOT run", 120, MARKER, cwd=work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(ground, ignore_errors=True)

    print("file path test passed")


if __name__ == "__main__":
    main()
