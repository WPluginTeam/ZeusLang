@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "ZEUS_ROOT=%~dp0.."
for %%I in ("%ZEUS_ROOT%") do set "ZEUS_ROOT=%%~fI"
set "ZEUS_WORK=%TEMP%\zeus-vm-reveal-slot-%RANDOM%-%RANDOM%"
mkdir "%ZEUS_WORK%" 2>nul

set "ZEUS_VCVARS="
if defined VSINSTALLDIR if exist "%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat" set "ZEUS_VCVARS=%VSINSTALLDIR%VC\Auxiliary\Build\vcvars64.bat"
if not defined ZEUS_VCVARS for /f "usebackq tokens=*" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "ZEUS_VCVARS=%%V\VC\Auxiliary\Build\vcvars64.bat"
if not defined ZEUS_VCVARS if exist "F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat" set "ZEUS_VCVARS=F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat"
if not defined ZEUS_VCVARS (
  echo [vm-reveal-slot] MSVC x64 tools not found.
  exit /b 1
)
call "%ZEUS_VCVARS%" >nul
if errorlevel 1 exit /b 1

set "ZEUS_RT=%ZEUS_ROOT%\ZeusRuntimeLib"
cl.exe /nologo /MT /O2 /EHsc /utf-8 /std:c++17 /bigobj /I "%ZEUS_ROOT%" /I "%ZEUS_RT%" "%ZEUS_ROOT%\ZeusZTest\vm_revealed_slot_reuse_test.cpp" "%ZEUS_RT%\ZeusABI.cpp" "%ZEUS_RT%\ZeusClosureABI.cpp" "%ZEUS_RT%\ZeusGCABI.cpp" "%ZEUS_RT%\ZeusNativeABI.cpp" "%ZEUS_RT%\ZeusStringNativeABI.cpp" "%ZEUS_RT%\ZeusDynamicABI.cpp" "%ZEUS_RT%\ZeusDynamicBinaryABI.cpp" "%ZEUS_RT%\ZeusDynamicMemberABI.cpp" "%ZEUS_RT%\ZeusRangeCheckABI.cpp" "%ZEUS_RT%\ZeusLValueABI.cpp" "%ZEUS_RT%\ZeusMemoryABI.cpp" "%ZEUS_RT%\ZeusObjectLifetime.cpp" "%ZEUS_RT%\ZeusThreadABI.cpp" "%ZEUS_RT%\ZeusRuntimeDebug.cpp" /Fe:"%ZEUS_WORK%\vm_revealed_slot_reuse_test.exe" /Fo:"%ZEUS_WORK%\\" /link /NOLOGO
if errorlevel 1 exit /b 1
"%ZEUS_WORK%\vm_revealed_slot_reuse_test.exe"
set "ZEUS_RC=%ERRORLEVEL%"
del /q "%ZEUS_WORK%\*.obj" "%ZEUS_WORK%\vm_revealed_slot_reuse_test.exe" 2>nul
rd "%ZEUS_WORK%" 2>nul
exit /b %ZEUS_RC%