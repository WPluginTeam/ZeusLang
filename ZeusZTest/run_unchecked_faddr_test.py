"""Verify checked and explicitly unchecked @faddr behavior in VM and AOT."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ZEUS = ROOT / "Zeus.exe"
COMPILER = ROOT / "ZeusCompiler.exe"
ZEUSC = ROOT / "zeusc.bat"
SOURCE = Path(__file__).with_name("unchecked_faddr_test.zeu")
MARKER = "UNCHECKED_FADDR_PASS"


def run(command, *, cwd=ROOT, timeout=180):
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    details = f"{command}\nexit={result.returncode}\n{output[-8000:]}"
    return result, output, details


def main():
    vm_result, vm_output, vm_details = run([str(ZEUS), "run", str(SOURCE)])
    assert vm_result.returncode == 0 and MARKER in vm_output, vm_details

    build_tmp_root = ROOT / "ZeusCLI" / ".build"
    build_tmp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="unchecked-faddr-", dir=build_tmp_root) as tmp:
        temp_dir = Path(tmp)
        llvm_path = temp_dir / "unchecked_faddr.ll"
        result, output, details = run(
            [str(COMPILER), "--emit-llvm", str(SOURCE), str(llvm_path)]
        )
        assert result.returncode == 0 and llvm_path.is_file(), details
        llvm_text = llvm_path.read_text(encoding="utf-8")
        entry_match = re.search(
            r"(?ms)^define [^{]*@zeus_entry\([^)]*\)[^{]*\{.*?^}", llvm_text
        )
        assert entry_match, "missing zeus_entry in emitted LLVM IR"
        entry = entry_match.group(0)
        assert not re.search(
            r"\b(?:call|invoke)\b.*@zeus_lvalue_require_raw\(", entry
        ), "unchecked reads and writes still call zeus_lvalue_require_raw"
        for width in (8, 16, 32, 64):
            assert re.search(rf"^\s*store i{width} ", entry, re.MULTILINE), (
                f"missing direct i{width} store in zeus_entry"
            )
            assert re.search(rf"^\s*%\S+ = load i{width}, ptr ", entry, re.MULTILINE), (
                f"missing direct i{width} load in zeus_entry"
            )
        assert re.search(
            r"\b(?:call|invoke)\b.*@zeus_lvalue_require_raw\(", llvm_text
        ), "ordinary checked @faddr no longer emits its raw validation call"

        invalid_sources = {
            "missing_width": (
                "let address: int = 1;\n"
                "let value = @no_safepoint @faddr(address) -> int;\n"
            ),
            "non_integer": (
                "let address: int = 1;\n"
                "let value = @no_safepoint @faddr(address, 8) -> bytes;\n"
            ),
        }
        for name, source_text in invalid_sources.items():
            source_path = temp_dir / f"{name}.zeu"
            output_path = temp_dir / f"{name}.ll"
            source_path.write_text(source_text, encoding="utf-8")
            result, output, details = run(
                [str(COMPILER), "--emit-llvm", str(source_path), str(output_path)]
            )
            assert result.returncode != 0, details
            assert "supports only @faddr(address, width) -> int" in output, details
            assert not output_path.exists(), details

        binary = temp_dir / "unchecked_faddr_test.exe"
        command = f'"{ZEUSC}" "{SOURCE}" -o "{binary}" --opt-level 2'
        result = subprocess.run(
            command, cwd=ROOT, shell=True, capture_output=True, timeout=300
        )
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        assert result.returncode == 0 and binary.is_file(), output[-12000:]
        aot_result, aot_output, aot_details = run([str(binary)], cwd=temp_dir)
        assert aot_result.returncode == 0 and MARKER in aot_output, aot_details

    print("PASS: unchecked @faddr uses direct LLVM loads/stores; checked validation remains active")


if __name__ == "__main__":
    main()
