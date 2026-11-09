"""Run the re module smoke test in the VM and, optionally, through AOT."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(__file__).resolve().with_name("regex_module_test.zeu")
MARKER = "REGEX_MODULE_PASS"


def run(command, *, shell=False, timeout=300):
    result = subprocess.run(
        command,
        cwd=ROOT,
        capture_output=True,
        shell=shell,
        timeout=timeout,
    )
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    details = f"{command}\nexit={result.returncode}\n{output[-12000:]}"
    return result, output, details


def compile_aot(compiler, source, destination):
    command = [str(compiler), str(source), "-o", str(destination)]
    if os.name == "nt":
        command = subprocess.list2cmdline(command)
        result, output, details = run(command, shell=True)
    else:
        result, output, details = run(command)
    assert result.returncode == 0, details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=ROOT / "Zeus.exe")
    parser.add_argument("--aot-compiler", type=Path)
    args = parser.parse_args()
    executable = args.executable.absolute()
    compiler = args.aot_compiler.absolute() if args.aot_compiler else None
    if not executable.is_file():
        parser.error(f"Zeus executable is not a file: {executable}")
    if compiler is not None and not compiler.is_file():
        parser.error(f"AOT compiler is not a file: {compiler}")

    result, output, details = run([str(executable), str(SOURCE)])
    assert result.returncode == 0 and MARKER in output, details
    print("PASS: regex module VM behavior")

    if compiler is not None:
        with tempfile.TemporaryDirectory(prefix="regex-module-", dir=SOURCE.parent) as directory:
            binary = Path(directory) / "regex_module_test.exe"
            compile_aot(compiler, SOURCE, binary)
            result, output, details = run([str(binary)])
            assert result.returncode == 0 and MARKER in output, details
        print("PASS: regex module AOT behavior")


if __name__ == "__main__":
    main()
