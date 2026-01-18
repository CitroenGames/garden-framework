@echo off
REM Compile Vulkan shaders to SPIR-V
REM Requires glslc from Vulkan SDK

setlocal

REM Find glslc - check VULKAN_SDK first, then PATH
if defined VULKAN_SDK (
    set GLSLC=%VULKAN_SDK%\Bin\glslc.exe
) else (
    where glslc >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        set GLSLC=glslc
    ) else (
        echo ERROR: glslc not found. Install Vulkan SDK and ensure VULKAN_SDK is set.
        exit /b 1
    )
)

echo Using glslc: %GLSLC%

set SHADER_DIR=assets\shaders\vulkan
set OUT_DIR=assets\shaders\vulkan

REM Create output directory if it doesn't exist
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

echo Compiling vertex shader...
"%GLSLC%" "%SHADER_DIR%\basic.vert" -o "%OUT_DIR%\basic.vert.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile basic.vert
    exit /b 1
)

echo Compiling fragment shader...
"%GLSLC%" "%SHADER_DIR%\basic.frag" -o "%OUT_DIR%\basic.frag.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile basic.frag
    exit /b 1
)

echo.
echo Shader compilation successful!
echo Output files:
echo   %OUT_DIR%\basic.vert.spv
echo   %OUT_DIR%\basic.frag.spv

endlocal
