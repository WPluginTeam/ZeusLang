"""Build a temporary Windows DLL and verify VM native scalar ABI imports."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from run_native_decl_test import compile_aot


ROOT = Path(__file__).resolve().parents[1]
SOURCES = Path(__file__).resolve().parent
VCVARS = Path(r"F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=ROOT / "Zeus.exe")
    parser.add_argument("--aot-compiler", type=Path, help="also compile and run the fixture with AOT")
    args = parser.parse_args()
    executable = args.executable.absolute()
    aot_compiler = args.aot_compiler.absolute() if args.aot_compiler else None
    if os.name != "nt":
        parser.error("this test builds a Windows x64 DLL")
    if not executable.is_file():
        parser.error(f"Zeus VM executable not found: {executable}")
    if not VCVARS.is_file():
        parser.error(f"MSVC environment script not found: {VCVARS}")

    with tempfile.TemporaryDirectory(prefix="zeus-native-double-", dir=SOURCES) as directory:
        temporary = Path(directory)
        library = temporary / "native_import_test.dll"
        fixture = temporary / "native_import_scalar_test.zeu"
        source = (SOURCES / "native_import_double_test.zeu").read_text(encoding="utf-8")
        source = source.replace(
            'from="native_import_test.dll"',
            f'from="{library.as_posix()}"',
        )
        fixture.write_text(source, encoding="utf-8")
        build_script = temporary / "build_test_dll.cmd"
        build_script.write_text(
            "\r\n".join((
                "@echo off",
                f'call "{VCVARS}" >nul',
                "if errorlevel 1 exit /b %ERRORLEVEL%",
                "cl.exe /nologo /LD /MT /EHsc /std:c++17 "
                f'"{SOURCES / "native_import_test.cpp"}" '
                f'/Fo"{temporary / "native_import_test.obj"}" '
                f'/Fe"{library}" /link /IMPLIB:"{temporary / "native_import_test.lib"}"',
                "exit /b %ERRORLEVEL%",
            )),
            encoding="utf-8",
        )
        built = subprocess.run(
            ["cmd.exe", "/d", "/c", str(build_script)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if built.returncode != 0:
            sys.stderr.write(built.stdout + built.stderr)
            return built.returncode

        result = subprocess.run(
            [str(executable), str(fixture)],
            cwd=ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != 0:
            return result.returncode
        if "PASS dynamic native scalar imports" not in result.stdout:
            print("native scalar import test did not print its pass marker", file=sys.stderr)
            return 1

        if aot_compiler:
            aot_binary = temporary / "native_import_double_aot.exe"
            compile_aot(aot_compiler, fixture, aot_binary)
            aot_result = subprocess.run(
                [str(aot_binary)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            sys.stdout.write(aot_result.stdout)
            sys.stderr.write(aot_result.stderr)
            if aot_result.returncode != 0:
                return aot_result.returncode
            if "PASS dynamic native scalar imports" not in aot_result.stdout:
                print("AOT native scalar import test did not print its pass marker", file=sys.stderr)
                return 1

    result_backend = "VM and AOT" if aot_compiler else "VM"
    print(f"PASS: {result_backend} dynamic DLL scalar ABI conversion and mixed arguments")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
