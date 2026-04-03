@echo off
setlocal

echo Starting build...
sighmake project.buildscript || pause

:: Copy runtime dependencies to bin/
if not exist bin mkdir bin
if exist Engine\Thirdparty\SDL2\lib\x64\SDL2.dll (
    copy /Y Engine\Thirdparty\SDL2\lib\x64\SDL2.dll bin\SDL2.dll >nul
    echo Copied SDL2.dll to bin/
)

echo Build completed successfully.
pause
