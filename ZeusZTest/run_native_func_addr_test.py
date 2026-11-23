"""Verify @addr on @native func and @native decl func across LLVM, AOT C callbacks, MCU emit, VM, and diagnostics."""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
COMPILER = ROOT / "ZeusCompiler.exe"
ZEUSC = ROOT / "zeusc.bat"
CPP_HELPER = Path(__file__).with_name("native_func_addr_test.cpp")
SOURCE = Path(__file__).with_name("native_func_addr_test.zeu")
MARKER = "NATIVE_FUNC_ADDR_PASS"


def run(command, *, cwd=ROOT, timeout=180):
    result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout)
    output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
    details = f"{command}\nexit={result.returncode}\n{output[-8000:]}"
    return result, output, details


def build_native_helper_object(output_obj: Path):
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
    with tempfile.TemporaryDirectory(
        prefix="zeus-native-func-addr-", dir=build_tmp_root
    ) as tmp:
        temp_dir = Path(tmp)

        # 1. Emit LLVM IR and inspect lowered symbols & reachability
        llvm_path = temp_dir / "native_func_addr_test.ll"
        result, output, details = run([str(COMPILER), "--emit-llvm", str(SOURCE), str(llvm_path)])
        assert result.returncode == 0 and llvm_path.is_file(), details
        llvm_text = llvm_path.read_text(encoding="utf-8")
        assert 'ptrtoint ptr @"zeus_custom_add_cb" to i64' in llvm_text, details
        assert 'ptrtoint ptr @"forwardMul" to i64' in llvm_text, details
        assert 'ptrtoint ptr @"zeus_test_only_addr_target" to i64' in llvm_text, details
        assert re.search(r'declare i32 @"zeus_test_only_addr_target"\(i32\)', llvm_text), details

        # 2. Emit --mcu LLVM IR for bare-metal callback + external C symbol addresses
        mcu_source = temp_dir / "mcu_cb_test.zeu"
        mcu_llvm = temp_dir / "mcu_cb_test.ll"
        mcu_source.write_text(
            '@native("mcu_ext_cb") decl func mcuExt(arg: i32) -> i32;\n'
            '@native func mcuLocal(arg: i32) -> i32 { return arg + 1; }\n'
            'func main() -> int { return @addr(mcuLocal) + @addr(mcuExt); }\n',
            encoding="utf-8",
        )
        result, output, details = run(
            [str(COMPILER), "--mcu", "--emit-llvm", str(mcu_source), str(mcu_llvm)]
        )
        assert result.returncode == 0 and mcu_llvm.is_file(), details
        mcu_text = mcu_llvm.read_text(encoding="utf-8")
        assert 'ptrtoint ptr @"mcuLocal" to i64' in mcu_text, details
        assert 'ptrtoint ptr @"mcu_ext_cb" to i64' in mcu_text, details
        assert 'declare i32 @"mcu_ext_cb"(i32)' in mcu_text, details

        # 3. Compile and run real AOT executable linked with C++ callbacks
        helper_obj = temp_dir / "native_func_addr_test.obj"
        build_native_helper_object(helper_obj)
        binary = temp_dir / "native_func_addr_test.exe"
        aot_cmd = f'"{ZEUSC}" "{SOURCE}" --link "{helper_obj}" -o "{binary}"'
        aot_res = subprocess.run(aot_cmd, cwd=ROOT, shell=True, capture_output=True, timeout=300)
        aot_out = (aot_res.stdout + aot_res.stderr).decode("utf-8", errors="replace")
        assert aot_res.returncode == 0 and binary.is_file(), aot_out
        run_res, run_out, run_details = run([str(binary)])
        assert run_res.returncode == 0 and MARKER in run_out, run_details

        # 4. VM mode rejects @addr on @native function cleanly
        vm_source = temp_dir / "vm_reject.zeu"
        vm_source.write_text(
            "@native func f() {}\nlet p = @addr(f);\n",
            encoding="utf-8",
        )
        vm_res, vm_out, vm_details = run([str(COMPILER), str(vm_source)])
        assert vm_res.returncode != 0, vm_details
        assert "requires llvm/aot compilation" in vm_out.lower(), vm_details

        # 5. Compile-time negative diagnostics
        negatives = {
            "non_native_func": (
                "func normal(x: int) -> int { return x; }\nlet p = @addr(normal);\n",
                "@native linkage",
            ),
            "capturing_native_func": (
                "func outer() -> int {\n"
                "    let x: i32 = 1;\n"
                "    @native func inner(a: i32) [&x] -> i32 { return a + x; }\n"
                "    return @addr(inner);\n"
                "}\n"
                "let p = outer();\n",
                "capturing closure",
            ),
            "default_param_native_func": (
                "@native func defParam(x: i32 = 1) -> i32 { return x; }\nlet p = @addr(defParam);\n",
                "default or variadic",
            ),
            "dynamic_return_native_func": (
                "@native func dynRet() -> int? { return 1; }\nlet p = @addr(dynRet);\n",
                "c abi return type or void",
            ),
            "reveal_on_native_func": (
                "@native func cb() {}\nlet p = @reveal(cb);\n",
                "requires a local, global, shared, or captured variable",
            ),
        }
        for name, (src_text, expected_err) in negatives.items():
            src = temp_dir / f"{name}.zeu"
            out_ll = temp_dir / f"{name}.ll"
            src.write_text(src_text, encoding="utf-8")
            neg_res, neg_out, neg_details = run(
                [str(COMPILER), "--emit-llvm", str(src), str(out_ll)]
            )
            assert neg_res.returncode != 0, neg_details
            assert expected_err.lower() in neg_out.lower(), neg_details
            assert not out_ll.exists(), neg_details

    print("PASS: @addr(@native func) & @addr(@native decl func) LLVM/AOT/MCU/diagnostics suite")


if __name__ == "__main__":
    main()
