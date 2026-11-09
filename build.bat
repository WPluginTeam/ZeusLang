@echo off
setlocal EnableExtensions DisableDelayedExpansion

pushd "%~dp0"
if errorlevel 1 exit /b 1

call "ZeusCLI\build_compiler.bat"
set "ZEUS_BUILD_RESULT=%ERRORLEVEL%"
if not "%ZEUS_BUILD_RESULT%"=="0" goto :finish

call "zeusc.bat" "ZeusCLI\main.zeu" -o "%~dp0Zeus.exe" --onefile
set "ZEUS_BUILD_RESULT=%ERRORLEVEL%"

:finish
popd
endlocal & exit /b %ZEUS_BUILD_RESULT%
