@echo off
setlocal EnableExtensions DisableDelayedExpansion

pushd "%~dp0.."
if errorlevel 1 exit /b 1

call :initialize_msvc
if errorlevel 1 goto :failed

set "ZEUS_RUNTIME_DEBUG_FLAGS="
if /i "%ZEUS_RUNTIME_DEBUG_DETAILS%"=="1" set "ZEUS_RUNTIME_DEBUG_FLAGS=/DZEUS_RUNTIME_DEBUG_DETAILS=1"
call "%~dp0..\ZeusNativeRuntime\build_native_runtime.bat"
if errorlevel 1 goto :failed

set "ZEUS_NATIVE_CALL_ABI_OBJ=%TEMP%\zeus_native_call_%RANDOM%.obj"
ml64.exe /nologo /c /Fo"%ZEUS_NATIVE_CALL_ABI_OBJ%" "ZeusRunner\ZeusBuiltins\ZeusNativeCallABI_x64.asm"
if errorlevel 1 goto :failed

cl.exe /O2 /GL /MT /utf-8 /EHsc /nologo /std:c++17 %ZEUS_RUNTIME_DEBUG_FLAGS% /Fe"./ZeusCompiler.exe" "Zeus.cpp" "%ZEUS_NATIVE_CALL_ABI_OBJ%" "ZeusRuntimeLib\ZeusGCABI.cpp" "ZeusRuntimeLib\ZeusThreadABI.cpp" "ZeusRuntimeLib\ZeusProcessCaptureABI.cpp" "ZeusRuntimeLib\ZeusRuntimeDebug.cpp" "ZeusRuntimeLib\ZeusABI.cpp" "ZeusRuntimeLib\ZeusMemoryABI.cpp" "ZeusRuntimeLib\ZeusLValueABI.cpp" "ZeusRuntimeLib\ZeusDynamicABI.cpp" "ZeusRuntimeLib\ZeusDynamicBinaryABI.cpp" "ZeusRuntimeLib\ZeusDynamicMemberABI.cpp" "ZeusRuntimeLib\ZeusRangeCheckABI.cpp" "ZeusRuntimeLib\ZeusClosureABI.cpp" "ZeusRuntimeLib\ZeusObjectLifetime.cpp" "ZeusRuntimeLib\ZeusThreadClosureABI.cpp" /link /LTCG
set "ZEUS_COMPILER_RESULT=%ERRORLEVEL%"
if defined ZEUS_NATIVE_CALL_ABI_OBJ del /q "%ZEUS_NATIVE_CALL_ABI_OBJ%" >nul 2>nul
if not "%ZEUS_COMPILER_RESULT%"=="0" goto :finish

echo [build] Created ZeusCompiler.exe
goto :finish

:failed
set "ZEUS_COMPILER_RESULT=%ERRORLEVEL%"
if defined ZEUS_NATIVE_CALL_ABI_OBJ del /q "%ZEUS_NATIVE_CALL_ABI_OBJ%" >nul 2>nul
if "%ZEUS_COMPILER_RESULT%"=="0" set "ZEUS_COMPILER_RESULT=1"

:finish
popd
endlocal & exit /b %ZEUS_COMPILER_RESULT%

:initialize_msvc
if defined VSCMD_ARG_TGT_ARCH if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" goto :check_msvc_tools
set "ZEUS_COMPILER_VCVARS="
set "ZEUS_COMPILER_VSINSTALL="
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" set "ZEUS_COMPILER_VCVARS=%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat"
if not defined ZEUS_COMPILER_VCVARS set "ZEUS_COMPILER_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined ZEUS_COMPILER_VCVARS if exist "%ZEUS_COMPILER_VSWHERE%" for /f "usebackq tokens=*" %%V in (`"%ZEUS_COMPILER_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "ZEUS_COMPILER_VSINSTALL=%%V"
if not defined ZEUS_COMPILER_VCVARS if defined ZEUS_COMPILER_VSINSTALL set "ZEUS_COMPILER_VCVARS=%ZEUS_COMPILER_VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat"
if not defined ZEUS_COMPILER_VCVARS if exist "F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat" set "ZEUS_COMPILER_VCVARS=F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat"
if defined ZEUS_COMPILER_VCVARS (
  if not exist "%ZEUS_COMPILER_VCVARS%" (
    echo [build] MSVC x64 environment script not found: "%ZEUS_COMPILER_VCVARS%"
    exit /b 1
  )
  call "%ZEUS_COMPILER_VCVARS%" >nul
  if errorlevel 1 (
    echo [build] Could not initialize the MSVC x64 build environment.
    exit /b 1
  )
) else (
  where cl.exe >nul 2>nul
  if errorlevel 1 (
    echo [build] MSVC was not found. Install Visual Studio C++ build tools.
    exit /b 1
  )
  if defined VSCMD_ARG_TGT_ARCH if /i not "%VSCMD_ARG_TGT_ARCH%"=="x64" (
    echo [build] Open an x64 MSVC developer prompt before building.
    exit /b 1
  )
)

:check_msvc_tools
where cl.exe >nul 2>nul
if errorlevel 1 (
  echo [build] cl.exe was not found after initializing MSVC.
  exit /b 1
)
where ml64.exe >nul 2>nul
if errorlevel 1 (
  echo [build] ml64.exe was not found after initializing MSVC.
  exit /b 1
)
where link.exe >nul 2>nul
if errorlevel 1 (
  echo [build] link.exe was not found after initializing MSVC.
  exit /b 1
)
exit /b 0
