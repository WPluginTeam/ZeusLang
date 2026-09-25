@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "ZEUSC_ROOT=%~dp0"
set "ZEUSC_INPUT="
set "ZEUSC_OUTPUT="
set "ZEUSC_LINK_INPUTS="
set "ZEUSC_KEEP_TEMPS=0"
set "ZEUSC_WORK="

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="--help" goto :usage
if /i "%~1"=="-h" goto :usage
if /i "%~1"=="--keep-temps" (
  set "ZEUSC_KEEP_TEMPS=1"
  shift
  goto :parse_args
)
if /i "%~1"=="-o" goto :parse_output
if /i "%~1"=="--output" goto :parse_output
if /i "%~1"=="--link" goto :parse_link
if defined ZEUSC_INPUT (
  echo [zeusc] Only one input source file is supported.
  goto :usage_error
)
set "ZEUSC_INPUT=%~1"
shift
goto :parse_args

:parse_output
if "%~2"=="" (
  echo [zeusc] Missing output path after %~1.
  goto :usage_error
)
if defined ZEUSC_OUTPUT (
  echo [zeusc] Output path was specified more than once.
  goto :usage_error
)
set "ZEUSC_OUTPUT=%~2"
shift
shift
goto :parse_args

:parse_link
if "%~2"=="" (
  echo [zeusc] Missing object or import library path after --link.
  goto :usage_error
)
if not exist "%~2" (
  echo [zeusc] Link input not found: "%~2"
  exit /b 2
)
set ZEUSC_LINK_INPUTS=%ZEUSC_LINK_INPUTS% "%~2"
shift
shift
goto :parse_args

:args_done
if not defined ZEUSC_INPUT goto :usage_error
for %%I in ("%ZEUSC_INPUT%") do set "ZEUSC_INPUT=%%~fI"
if not exist "%ZEUSC_INPUT%" (
  echo [zeusc] Source file not found: "%ZEUSC_INPUT%"
  exit /b 2
)
if not defined ZEUSC_OUTPUT for %%I in ("%ZEUSC_INPUT%") do set "ZEUSC_OUTPUT=%%~dpnI.exe"
if defined ZEUSC_OUTPUT for %%I in ("%ZEUSC_OUTPUT%") do set "ZEUSC_OUTPUT=%%~fI"
for %%I in ("%ZEUSC_OUTPUT%") do set "ZEUSC_OUTDIR=%%~dpI"
if not exist "%ZEUSC_OUTDIR%" mkdir "%ZEUSC_OUTDIR%"
if errorlevel 1 (
  echo [zeusc] Could not create output directory: "%ZEUSC_OUTDIR%"
  exit /b 1
)

call :initialize_msvc
if errorlevel 1 exit /b 1

where clang.exe >nul 2>nul
if errorlevel 1 if exist "%ProgramFiles%\LLVM\bin\clang.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang.exe >nul 2>nul
if errorlevel 1 (
  echo [zeusc] clang was not found. Install LLVM or add clang.exe to PATH.
  exit /b 1
)

if exist "%ZEUSC_ROOT%Zeus.exe" goto :compiler_ready
echo [zeusc] Zeus.exe was not found; building the compiler first.
pushd "%ZEUSC_ROOT%"
call build.bat
set "ZEUSC_RC=%ERRORLEVEL%"
popd
if not "%ZEUSC_RC%"=="0" (
  echo [zeusc] Compiler build failed with exit code %ZEUSC_RC%.
  exit /b %ZEUSC_RC%
)
:compiler_ready

set "ZEUSC_TEMP_ROOT=%TEMP%"
for %%I in ("%ZEUSC_TEMP_ROOT%") do set "ZEUSC_TEMP_ROOT=%%~fI"
:make_work_dir
set "ZEUSC_WORK=%ZEUSC_TEMP_ROOT%\zeusc-%RANDOM%-%RANDOM%"
if exist "%ZEUSC_WORK%" goto :make_work_dir
mkdir "%ZEUSC_WORK%"
if errorlevel 1 (
  echo [zeusc] Could not create temporary build directory.
  exit /b 1
)
for %%I in ("%ZEUSC_WORK%") do set "ZEUSC_WORK=%%~fI"

echo [zeusc] Emitting LLVM IR...
"%ZEUSC_ROOT%Zeus.exe" --emit-llvm "%ZEUSC_INPUT%" "%ZEUSC_WORK%\program.ll"
if errorlevel 1 goto :failed

echo [zeusc] Compiling LLVM IR...
clang.exe -Wno-override-module -c "%ZEUSC_WORK%\program.ll" -o "%ZEUSC_WORK%\program.obj"
if errorlevel 1 goto :failed

echo [zeusc] Building the Zeus runtime...
cl.exe /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 "%ZEUSC_ROOT%ZeusAOT\runtime\zeus_rt.cpp" /Fo"%ZEUSC_WORK%\zeus_rt.obj"
if errorlevel 1 goto :failed
cl.exe /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 "%ZEUSC_ROOT%ZeusRuntimeLib\ZeusABI.cpp" /Fo"%ZEUSC_WORK%\zeus_abi.obj"
if errorlevel 1 goto :failed
cl.exe /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 "%ZEUSC_ROOT%ZeusRuntimeLib\ZeusNativeABI.cpp" /Fo"%ZEUSC_WORK%\zeus_native_abi.obj"
if errorlevel 1 goto :failed
cl.exe /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 "%ZEUSC_ROOT%ZeusRuntimeLib\ZeusStringNativeABI.cpp" /Fo"%ZEUSC_WORK%\zeus_string_native_abi.obj"
if errorlevel 1 goto :failed
cl.exe /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 "%ZEUSC_ROOT%ZeusRuntimeLib\ZeusDynamicABI.cpp" /Fo"%ZEUSC_WORK%\zeus_dynamic_abi.obj"
if errorlevel 1 goto :failed
cl.exe /c /MT /O2 /EHsc /nologo /utf-8 /std:c++17 "%ZEUSC_ROOT%ZeusRuntimeLib\ZeusDynamicBinaryABI.cpp" /Fo"%ZEUSC_WORK%\zeus_dynamic_binary_abi.obj"
if errorlevel 1 goto :failed
lib.exe "%ZEUSC_WORK%\zeus_rt.obj" "%ZEUSC_WORK%\zeus_abi.obj" "%ZEUSC_WORK%\zeus_native_abi.obj" "%ZEUSC_WORK%\zeus_string_native_abi.obj" "%ZEUSC_WORK%\zeus_dynamic_abi.obj" "%ZEUSC_WORK%\zeus_dynamic_binary_abi.obj" /OUT:"%ZEUSC_WORK%\libzeus_rt.lib" /NOLOGO
if errorlevel 1 goto :failed

echo [zeusc] Linking executable...
link.exe "%ZEUSC_WORK%\program.obj" %ZEUSC_LINK_INPUTS% "%ZEUSC_WORK%\libzeus_rt.lib" libcmt.lib kernel32.lib /NOLOGO /SUBSYSTEM:CONSOLE /INCREMENTAL:NO /OUT:"%ZEUSC_OUTPUT%"
if errorlevel 1 goto :failed
if not exist "%ZEUSC_OUTPUT%" (
  echo [zeusc] Linker returned success but did not create the output file.
  goto :failed
)

echo [zeusc] Created "%ZEUSC_OUTPUT%"
if "%ZEUSC_KEEP_TEMPS%"=="1" (
  echo [zeusc] Build files kept in "%ZEUSC_WORK%"
  exit /b 0
)
del /q "%ZEUSC_WORK%\program.ll" "%ZEUSC_WORK%\program.obj" "%ZEUSC_WORK%\zeus_rt.obj" "%ZEUSC_WORK%\zeus_abi.obj" "%ZEUSC_WORK%\zeus_native_abi.obj" "%ZEUSC_WORK%\zeus_string_native_abi.obj" "%ZEUSC_WORK%\zeus_dynamic_abi.obj" "%ZEUSC_WORK%\zeus_dynamic_binary_abi.obj" "%ZEUSC_WORK%\libzeus_rt.lib" 2>nul
rd "%ZEUSC_WORK%" 2>nul
exit /b 0

:initialize_msvc
set "ZEUSC_VCVARS="
set "ZEUSC_VSINSTALL="
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" set "ZEUSC_VCVARS=%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat"
if not defined ZEUSC_VCVARS set "ZEUSC_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined ZEUSC_VCVARS if exist "%ZEUSC_VSWHERE%" for /f "usebackq tokens=*" %%V in (`"%ZEUSC_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZEUSC_VSINSTALL=%%V"
if not defined ZEUSC_VCVARS if defined ZEUSC_VSINSTALL set "ZEUSC_VCVARS=%ZEUSC_VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not defined ZEUSC_VCVARS if exist "F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat" set "ZEUSC_VCVARS=F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat"
if defined ZEUSC_VCVARS (
  if not exist "%ZEUSC_VCVARS%" (
    echo [zeusc] MSVC x64 environment script not found: "%ZEUSC_VCVARS%"
    exit /b 1
  )
  call "%ZEUSC_VCVARS%" >nul
  if errorlevel 1 (
    echo [zeusc] Could not initialize the MSVC x64 build environment.
    exit /b 1
  )
) else (
  where cl.exe >nul 2>nul
  if errorlevel 1 (
    echo [zeusc] MSVC was not found. Install Visual Studio C++ build tools.
    exit /b 1
  )
  if defined VSCMD_ARG_TGT_ARCH if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo [zeusc] Open an x64 MSVC developer prompt before running zeusc.
    exit /b 1
  )
)
where cl.exe >nul 2>nul
if errorlevel 1 (
  echo [zeusc] cl.exe was not found after initializing MSVC.
  exit /b 1
)
where lib.exe >nul 2>nul
if errorlevel 1 (
  echo [zeusc] lib.exe was not found after initializing MSVC.
  exit /b 1
)
where link.exe >nul 2>nul
if errorlevel 1 (
  echo [zeusc] link.exe was not found after initializing MSVC.
  exit /b 1
)
exit /b 0

:failed
set "ZEUSC_RC=%ERRORLEVEL%"
echo [zeusc] Build failed with exit code %ZEUSC_RC%.
if defined ZEUSC_WORK echo [zeusc] Intermediate files were kept in "%ZEUSC_WORK%"
exit /b %ZEUSC_RC%

:usage_error
call :print_usage
exit /b 2

:usage
call :print_usage
exit /b 0

:print_usage
echo Usage: zeusc ^<input.zeu^> [-o ^<output.exe^>] [--link ^<file.obj^|file.lib^>]... [--keep-temps]
echo.
echo Compiles a Zeus source file to a standalone Windows console executable.
echo --link adds a native object file or import/static library for @native symbols.
echo Requires Zeus.exe, clang, and the MSVC x64 C++ build tools.
exit /b 0
