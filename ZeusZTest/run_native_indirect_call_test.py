"""Verify @native(addr) decl func: indirect C calls through an address argument."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "ZeusCompiler.exe"
ZEUSC = ROOT / "zeusc.bat"
CPP_HELPER = Path(__file__).with_name("native_indirect_call_test.cpp")
SOURCE = Path(__file__).with_name("native_indirect_call_test.zeu")
MARKER = "NATIVE_INDIRECT_CALL_PASS"


def run(command, *, cwd=ROOT, timeout=180):
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    return result, output, f"{command}\nexit={result.returncode}\n{output[-8000:]}"


def build_helper_object(output_obj: Path):
    vcvars = Path(r"F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat")
    assert vcvars.is_file(), f"Missing vcvars64.bat at {vcvars}"
    command = (
        f'call "{vcvars}" >nul && '
        f'cl.exe /nologo /c /O2 /MT /EHsc /std:c++17 "{CPP_HELPER}" /Fo"{output_obj}"'
    )
    result = subprocess.run(command, cwd=ROOT, shell=True, capture_output=True, timeout=120)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    assert result.returncode == 0 and output_obj.is_file(), f"cl.exe failed:\n{output}"


def main():
    build_tmp_root = ROOT / "ZeusCLI" / ".build"
    build_tmp_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zeus-native-indirect-", dir=build_tmp_root) as tmp:
        temp_dir = Path(tmp)

        # 1. IR: private thunk with inttoptr + indirect call, no external symbol.
        llvm_path = temp_dir / "indirect.ll"
        result, output, details = run([str(COMPILER), "--emit-llvm", str(SOURCE), str(llvm_path)])
        assert result.returncode == 0 and llvm_path.is_file(), details
        llvm_text = llvm_path.read_text(encoding="utf-8")
        assert "inttoptr i64 %arg0 to ptr" in llvm_text, details
        assert "define internal i32 " in llvm_text and "%callee(i32 %arg1, i32 %arg2)" in llvm_text, details
        assert "define internal void " in llvm_text, details

        # 2. MCU emit.
        mcu_source = temp_dir / "mcu_indirect.zeu"
        mcu_llvm = temp_dir / "mcu_indirect.ll"
        mcu_source.write_text(
            "@native(addr) decl func callUnary(fn: i64, arg: i32) -> i32;\n"
            "func main() -> int { return callUnary(0, 1); }\n",
            encoding="utf-8",
        )
        result, output, details = run(
            [str(COMPILER), "--mcu", "--emit-llvm", str(mcu_source), str(mcu_llvm)]
        )
        assert result.returncode == 0 and mcu_llvm.is_file(), details
        assert "inttoptr i64 %arg0 to ptr" in mcu_llvm.read_text(encoding="utf-8"), details

        # 3. Real AOT executable calling C function pointers.
        helper_obj = temp_dir / "native_indirect_call_test.obj"
        build_helper_object(helper_obj)
        binary = temp_dir / "native_indirect_call_test.exe"
        aot = subprocess.run(
            f'"{ZEUSC}" "{SOURCE}" --link "{helper_obj}" -o "{binary}"',
            cwd=ROOT, shell=True, capture_output=True, timeout=300,
        )
        aot_out = (aot.stdout + aot.stderr).decode("utf-8", errors="replace")
        assert aot.returncode == 0 and binary.is_file(), aot_out
        run_res, run_out, run_details = run([str(binary)])
        assert run_res.returncode == 0 and MARKER in run_out, run_details

        # 4. Compile-time diagnostics.
        negatives = {
            "definition": (
                "@native(addr) func f(fn: i64) -> i32 { return 1; }\n",
                "only decorates a 'decl func'",
            ),
            "other_identifier": (
                "@native(other) decl func f(fn: i64) -> i32;\n",
                "@native(addr)",
            ),
            "no_address_param": (
                "@native(addr) decl func f() -> i32;\nlet p = f();\n",
                "first parameter",
            ),
            "non_integer_address": (
                "@native(addr) decl func f(fn: i32) -> i32;\nlet p = f(1);\n",
                "first parameter",
            ),
            "dynamic_return": (
                "@native(addr) decl func f(fn: i64);\nf(0);\n",
                "'-> void'",
            ),
            "anon_native_capture": (
                "func outer() -> int {\n"
                "    let x: i32 = 1;\n"
                "    return @addr(@native func(a: i32) [&x] -> i32 { return a + x; });\n"
                "}\n"
                "let p = outer();\n",
                "capturing closure",
            ),
        }
        for name, (text, expected) in negatives.items():
            src = temp_dir / f"{name}.zeu"
            out_ll = temp_dir / f"{name}.ll"
            src.write_text(text, encoding="utf-8")
            res, out, details = run([str(COMPILER), "--emit-llvm", str(src), str(out_ll)])
            assert res.returncode != 0, details
            assert expected.lower() in out.lower(), f"{name}: {details}"

    print("PASS: @native(addr) decl func indirect C call suite")


if __name__ == "__main__":
    main()
