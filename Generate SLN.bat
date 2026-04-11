@echo off
setlocal

echo Starting build...
sighmake project.buildscript || pause

:: Copy runtime dependencies to bin/
if not exist bin mkdir bin
if exist Engine\Thirdparty\SDL3-3.4.4\lib\x64\SDL3.dll (
    copy /Y Engine\Thirdparty\SDL3-3.4.4\lib\x64\SDL3.dll bin\SDL3.dll >nul
    echo Copied SDL3.dll to bin/
)

echo Build completed successfully.
pause
