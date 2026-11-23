#!/usr/bin/env python3
"""Compile a Zeus MCU entry file to an object, then build its PlatformIO project."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]


def resolve_executable(
    value: str | None,
    env_name: str,
    default_name: str,
    option_name: str,
) -> str:
    candidate = value or os.environ.get(env_name)
    if candidate:
        path = Path(candidate).expanduser()
        if path.is_file():
            return str(path.resolve())
        located = shutil.which(candidate)
        if located:
            return located
        raise FileNotFoundError(f"Could not find {env_name} executable: {candidate}")

    located = shutil.which(default_name)
    if located:
        return located
    raise FileNotFoundError(
        f"Could not find {default_name}. Pass the path with {option_name} "
        f"or set {env_name}."
    )


def split_paths(value: str) -> list[str]:
    return [part for part in value.split(os.pathsep) if part]


def run(command: list[str], *, cwd: Path, env: dict[str, str]) -> None:
    print("[zeus-platformio]", subprocess.list2cmdline(command))
    subprocess.run(command, cwd=cwd, env=env, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project", nargs="?", default=".", help="PlatformIO project directory")
    parser.add_argument("--source", default="src/app.zeu", help="Zeus entry source, relative to project")
    parser.add_argument("--environment", help="PlatformIO environment name")
    parser.add_argument("--compiler", help="Path to ZeusCompiler.exe; defaults to ZEUS_COMPILER or the Zeus2 checkout")
    parser.add_argument("--clang", help="Path to Xtensa Clang; defaults to ZEUS_MCU_CLANG or PATH")
    parser.add_argument("--pio", help="Path to PlatformIO CLI; defaults to PLATFORMIO_CLI or PATH")
    parser.add_argument("--target", default="xtensa-esp-unknown-elf", help="LLVM target triple")
    parser.add_argument("--cpu", default="esp32s3", help="Xtensa CPU passed to Clang")
    parser.add_argument("--include-path", help="C header search path, also exported as ZEUS_INCLUDE_PATH")
    parser.add_argument(
        "--output-dir",
        help="Build output directory; defaults to <project>/.pio/zeus/<environment>",
    )
    parser.add_argument("--upload", action="store_true", help="Upload the firmware after building")
    parser.add_argument("--clang-arg", action="append", default=[], help="Extra argument passed to Xtensa Clang; may repeat")
    args = parser.parse_args()

    project = Path(args.project).expanduser().resolve()
    config = project / "platformio.ini"
    source = (project / args.source).resolve()
    if not config.is_file():
        parser.error(f"PlatformIO configuration not found: {config}")
    if not source.is_file():
        parser.error(f"Zeus source not found: {source}")

    try:
        compiler_default = REPO_ROOT / "ZeusCompiler.exe"
        compiler_arg = args.compiler or os.environ.get("ZEUS_COMPILER")
        compiler = resolve_executable(
            str(compiler_default) if not compiler_arg and compiler_default.is_file() else compiler_arg,
            "ZEUS_COMPILER",
            "ZeusCompiler.exe",
            "--compiler",
        )
        clang = resolve_executable(args.clang, "ZEUS_MCU_CLANG", "clang", "--clang")
        pio = resolve_executable(args.pio, "PLATFORMIO_CLI", "pio", "--pio")
    except FileNotFoundError as error:
        print(f"[zeus-platformio] ERROR: {error}", file=sys.stderr)
        return 2

    environment_name = args.environment or "default"
    build_dir = project / ".pio" / "zeus" / environment_name
    if args.output_dir:
        output_dir = Path(args.output_dir).expanduser()
        build_dir = output_dir.resolve() if output_dir.is_absolute() else (project / output_dir).resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    llvm_file = build_dir / f"{source.stem}.ll"
    object_file = build_dir / f"{source.stem}.o"

    env = os.environ.copy()
    project_packages = project / "packages"
    current_zeus_path = split_paths(env.get("ZEUS_PATH", ""))
    if project_packages.is_dir():
        env["ZEUS_PATH"] = os.pathsep.join([str(project_packages), *current_zeus_path])
    if args.include_path:
        env["ZEUS_INCLUDE_PATH"] = args.include_path

    try:
        run([compiler, "--mcu", "--emit-llvm", str(source), str(llvm_file)], cwd=project, env=env)
        run(
            [
                clang,
                f"--target={args.target}",
                f"-mcpu={args.cpu}",
                "-c",
                str(llvm_file),
                "-o",
                str(object_file),
                *args.clang_arg,
            ],
            cwd=project,
            env=env,
        )
        env["ZEUS_OBJECT"] = object_file.as_posix()
        command = [pio, "run"]
        if args.environment:
            command.extend(["-e", args.environment])
        if args.upload:
            command.extend(["-t", "upload"])
        run(command, cwd=project, env=env)
    except subprocess.CalledProcessError as error:
        return error.returncode or 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
