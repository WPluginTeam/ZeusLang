@echo off
setlocal
call "F:\Code\Env\VisualStudio2022\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %ERRORLEVEL%
cd /d "%~dp0"
cl.exe /O1 /bigobj /MT /utf-8 /EHsc /nologo /std:c++17 /Fe"%TEMP%\ZeusSemantics.exe" "Zeus.cpp" "ZeusRuntimeLib\ZeusGCABI.cpp" "ZeusRuntimeLib\ZeusThreadABI.cpp" "ZeusRuntimeLib\ZeusSocketABI.cpp" "ZeusRuntimeLib\ZeusRuntimeDebug.cpp"
exit /b %ERRORLEVEL%
