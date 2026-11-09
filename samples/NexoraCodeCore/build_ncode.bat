@echo off
setlocal
set "REPO_ROOT=%~dp0..\.."
pushd "%REPO_ROOT%"
if not exist dist mkdir dist
call zeusc.bat samples\NexoraCodeCore\backend_main.zeu --opt-level 2 -o dist\NexoraCodeCoreBackend.exe
if errorlevel 1 goto :build_failed
call zeusc.bat samples\NexoraCodeCore\ncode_main.zeu --opt-level 2 -o dist\ncode.exe
if errorlevel 1 goto :build_failed
if not exist dist\ncode.config.json (
    if exist ncode.config.json (
        copy /Y ncode.config.json dist\ncode.config.json >nul
    ) else (
        copy /Y samples\NexoraCodeCore\ncode.config.template dist\ncode.config.json >nul
    )
    if errorlevel 1 goto :build_failed
)
popd
exit /b 0

:build_failed
set "BUILD_RESULT=%ERRORLEVEL%"
if "%BUILD_RESULT%"=="0" set "BUILD_RESULT=1"
popd
exit /b %BUILD_RESULT%
