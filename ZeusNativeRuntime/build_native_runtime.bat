@echo off
setlocal

if not exist "%~dp0build" mkdir "%~dp0build"
pushd "%~dp0build"
cl.exe /O2 /MT /utf-8 /EHsc /nologo /std:c++17 /LD "%~dp0ZeusNativeRuntime.cpp" /link /OUT:"%~dp0..\ZeusNativeRuntime.dll" /IMPLIB:"%~dp0build\ZeusNativeRuntime.lib" shell32.lib
set "ZEUS_NATIVE_RUNTIME_RESULT=%ERRORLEVEL%"
popd
exit /b %ZEUS_NATIVE_RUNTIME_RESULT%
