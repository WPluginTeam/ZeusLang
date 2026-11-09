"""Verify package globals stay available to VM global destructors."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = Path(__file__).with_name("vm_module_exit_dependency_test.zeu")
SOCKET_FIXTURE = Path(__file__).with_name("vm_socket_exit_dependency_test.zeu")


def run_case(command, expected, label):
    result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=30)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    missing = [marker for marker in expected if marker not in output]
    if result.returncode or missing or "NameError" in output:
        raise RuntimeError(
            f"{label} failed: exit={result.returncode}, missing={missing}\n{output[-8000:]}"
        )
    print(f"PASS: {label}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=ROOT / "Zeus.exe")
    args = parser.parse_args()
    executable = str(args.exe.absolute())
    expected = ["VM_MODULE_EXIT_ENTRY_DONE", "VM_MODULE_EXIT_DEPENDENCY_PASS"]

    run_case([executable, str(FIXTURE)], expected, "source execution")
    run_case([executable, str(SOCKET_FIXTURE)], ["VM_SOCKET_EXIT_ENTRY_DONE"], "socket source execution")
    with tempfile.TemporaryDirectory(prefix="zeus-vm-module-exit-", dir=ROOT) as directory:
        source = Path(directory) / FIXTURE.name
        shutil.copyfile(FIXTURE, source)
        run_case([executable, "-c", str(source)], ["Compile success"], "IR compilation")
        run_case([executable, "-r", str(source) + ".zir"], expected, "serialized IR execution")


if __name__ == "__main__":
    main()
