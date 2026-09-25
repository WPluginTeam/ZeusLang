@echo off
setlocal
REM Zeus AOT loop: MSVC runtime lib (zeus_rt + ZeusABI) + clang(IR -c) + link.exe
REM Run this after: winget install LLVM.LLVM   (provides clang)

REM Always enter the x64 toolchain: a stale x86 cl in PATH would silently
REM produce x86 objects, conflicting with clang's x64 output (LNK4272).
call "F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat" >nul

where clang >nul 2>nul
if errorlevel 1 (
  REM Terminal predates the LLVM install: try the well-known location.
  if exist "C:\Program Files\LLVM\bin\clang.exe" (
    set "PATH=C:\Program Files\LLVM\bin;%PATH%"
  ) else (
    echo [AOT] clang not found. Install LLVM first: winget install LLVM.LLVM
    exit /b 1
  )
)

cd /d "%~dp0demo"

REM ---- runtime lib (built once, linked everywhere) ----
REM NOTE: /std:c++17 is mandatory (MSVC defaults to C++14, which hides <variant>).
cl /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 ..\runtime\zeus_rt.cpp /Fozeus_rt.obj
if errorlevel 1 exit /b 1

cl /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 ..\..\ZeusRuntimeLib\ZeusABI.cpp /Fozeus_abi.obj
if errorlevel 1 exit /b 1

cl /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 ..\..\ZeusRuntimeLib\ZeusNativeABI.cpp /Fozeus_native_abi.obj
if errorlevel 1 exit /b 1

cl /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 ..\..\ZeusRuntimeLib\ZeusStringNativeABI.cpp /Fozeus_string_native_abi.obj
if errorlevel 1 exit /b 1

cl /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 ..\..\ZeusRuntimeLib\ZeusDynamicABI.cpp /Fozeus_dynamic_abi.obj
if errorlevel 1 exit /b 1

cl /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 ..\..\ZeusRuntimeLib\ZeusDynamicBinaryABI.cpp /Fozeus_dynamic_binary_abi.obj
if errorlevel 1 exit /b 1

cl /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 ..\..\ZeusRuntimeLib\ZeusDynamicMemberABI.cpp /Fozeus_dynamic_member_abi.obj
if errorlevel 1 exit /b 1

lib zeus_rt.obj zeus_abi.obj zeus_native_abi.obj zeus_string_native_abi.obj zeus_dynamic_abi.obj zeus_dynamic_binary_abi.obj zeus_dynamic_member_abi.obj /OUT:libzeus_rt.lib /NOLOGO
if errorlevel 1 exit /b 1

REM ---- demo 1: print("hello") ----
clang -Wno-override-module -c hello.ll -o hello.obj
if errorlevel 1 exit /b 1

link hello.obj libzeus_rt.lib libcmt.lib kernel32.lib /NOLOGO /SUBSYSTEM:CONSOLE /OUT:hello.exe
if errorlevel 1 exit /b 1

echo [AOT] running hello.exe (expect: hello):
hello.exe
echo [AOT] exit code: %errorlevel%
if errorlevel 1 exit /b 1

REM ---- demo 2: x = 40 + 2; print(x) ----
clang -Wno-override-module -c add.ll -o add.obj
if errorlevel 1 exit /b 1

link add.obj libzeus_rt.lib libcmt.lib kernel32.lib /NOLOGO /SUBSYSTEM:CONSOLE /OUT:add.exe
if errorlevel 1 exit /b 1

echo [AOT] running add.exe (expect: 42):
add.exe
echo [AOT] exit code: %errorlevel%
if errorlevel 1 exit /b 1

REM ---- emitter round-trip: .zeu -> .ll -> .exe (needs fresh Zeus.exe) ----
if not exist ..\..\Zeus.exe (
  echo [AOT] skip emitter round-trip: build Zeus.exe first ^(build.bat^)
  exit /b 0
)

..\..\Zeus.exe --emit-llvm hello.zeu hello_gen.ll
if errorlevel 1 exit /b 1

clang -Wno-override-module -c hello_gen.ll -o hello_gen.obj
if errorlevel 1 exit /b 1

link hello_gen.obj libzeus_rt.lib libcmt.lib kernel32.lib /NOLOGO /SUBSYSTEM:CONSOLE /OUT:hello_gen.exe
if errorlevel 1 exit /b 1

echo [AOT] running hello_gen.exe (expect: hello):
hello_gen.exe
echo [AOT] exit code: %errorlevel%
if errorlevel 1 exit /b 1

..\..\Zeus.exe --emit-llvm add.zeu add_gen.ll
if errorlevel 1 exit /b 1

clang -Wno-override-module -c add_gen.ll -o add_gen.obj
if errorlevel 1 exit /b 1

link add_gen.obj libzeus_rt.lib libcmt.lib kernel32.lib /NOLOGO /SUBSYSTEM:CONSOLE /OUT:add_gen.exe
if errorlevel 1 exit /b 1

echo [AOT] running add_gen.exe (expect: 42):
add_gen.exe
echo [AOT] exit code: %errorlevel%
if errorlevel 1 exit /b 1

REM ---- emitter demo 3: for-loop benchmark, PK vs interpreter (~92ms in ben.exe) ----
..\..\Zeus.exe --emit-llvm loop.zeu loop_gen.ll
if errorlevel 1 exit /b 1

clang -Wno-override-module -c loop_gen.ll -o loop_gen.obj
if errorlevel 1 exit /b 1

link loop_gen.obj libzeus_rt.lib libcmt.lib kernel32.lib /NOLOGO /SUBSYSTEM:CONSOLE /OUT:loop_gen.exe
if errorlevel 1 exit /b 1

echo [AOT] running loop_gen.exe (expect: 100000):
powershell -NoProfile -Command "$sw = [System.Diagnostics.Stopwatch]::StartNew(); $o = .\loop_gen.exe; $sw.Stop(); Write-Host $o; Write-Host ('[AOT] loop_gen wall(ms)=' + $sw.Elapsed.TotalMilliseconds)"
