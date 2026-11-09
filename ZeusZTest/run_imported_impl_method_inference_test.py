"""Compile and run an AOT call to an imported impl method."""

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "ZeusZTest" / "imported_impl_method_inference_test.zeu"
ZEUSC_BATCH = os.environ.get("ZEUSC_BATCH", str(ROOT / "zeusc.bat"))


def main():
    if os.name != "nt":
        raise SystemExit("This AOT regression test requires the Windows compiler toolchain.")

    with tempfile.TemporaryDirectory(
        prefix="zeus-imported-impl-", dir=SOURCE.parent
    ) as temp_dir:
        executable = Path(temp_dir) / "imported_impl_method_inference_test.exe"
        compile_command = f'"{ZEUSC_BATCH}" -o "{executable}" "{SOURCE}"'
        compiled = subprocess.run(
            compile_command,
            cwd=ROOT,
            shell=True,
            capture_output=True,
            timeout=300,
        )
        stdout = compiled.stdout.decode("mbcs", errors="replace")
        stderr = compiled.stderr.decode("mbcs", errors="replace")
        if compiled.returncode != 0:
            raise AssertionError(f"zeusc failed ({compiled.returncode})\n{stdout}\n{stderr}")

        result = subprocess.run(
            [str(executable)],
            cwd=ROOT,
            capture_output=True,
            timeout=20,
        )
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        if result.returncode != 0 or "IMPORTED_IMPL_METHOD_INFERENCE_PASS" not in output:
            raise AssertionError(
                f"AOT regression failed (exit {result.returncode})\n{output}"
            )

    print("PASS: imported impl method result is a native AOT integer")


if __name__ == "__main__":
    main()
