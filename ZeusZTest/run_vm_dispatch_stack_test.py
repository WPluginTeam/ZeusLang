"""Run VM dispatch regressions against an executable with a 1 MB PE stack."""
import argparse
from pathlib import Path
import struct
import subprocess


ROOT = Path(__file__).resolve().parents[1]
PROBES = (
    "[RFL-IN]", "[PF-I]", "[PF-III]", "[EX-IN]", "[EX-A]",
    "[EX-ANN]", "[EX-OUT]", "[EX-ENUM]",
)


def stack_reserve(executable):
    with executable.open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise AssertionError("Executable is not a Windows PE file")
        stream.seek(0x3C)
        pe_offset = struct.unpack("<I", stream.read(4))[0]
        stream.seek(pe_offset)
        if stream.read(4) != b"PE\0\0":
            raise AssertionError("Invalid PE signature")
        optional_header = pe_offset + 24
        stream.seek(optional_header)
        magic = struct.unpack("<H", stream.read(2))[0]
        if magic == 0x20B:
            stack_format = "<Q"
        elif magic == 0x10B:
            stack_format = "<I"
        else:
            raise AssertionError(f"Unsupported PE optional header: {magic:#x}")
        stream.seek(optional_header + 72)
        return struct.unpack(stack_format, stream.read(struct.calcsize(stack_format)))[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path, nargs="?", default=ROOT / "Zeus.exe")
    args = parser.parse_args()
    executable = args.executable.resolve(strict=True)
    reserve = stack_reserve(executable)
    assert reserve == 1024 * 1024, f"Expected 1 MB stack reserve, got {reserve} bytes"
    source = ROOT / "ZeusZTest" / "vm_dispatch_stack_test.zeu"
    result = subprocess.run(
        [str(executable), str(source)], cwd=ROOT, capture_output=True, timeout=60,
    )
    stdout = result.stdout.decode("utf-8", errors="replace")
    stderr = result.stderr.decode("utf-8", errors="replace")
    details = (
        f"exit={result.returncode:#x}\nstdout (last 4000 characters):\n{stdout[-4000:]}"
        f"\nstderr (last 4000 characters):\n{stderr[-4000:]}"
    )
    assert result.returncode == 0, details
    assert "VM_DISPATCH_STACK_TEST_PASS 17" in stdout, details
    assert "FAIL_VM_DISPATCH" not in stdout, details
    assert "[Runtime Error" not in stdout, details
    for probe in PROBES:
        assert probe not in stderr and probe not in stdout, details
    print("VM dispatch regression: 17 assertions passed with 1 MB stack; no probes")


if __name__ == "__main__":
    main()
