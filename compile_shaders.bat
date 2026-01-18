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

echo Compiling basic shaders...
"%GLSLC%" "%SHADER_DIR%\basic.vert" -o "%OUT_DIR%\basic.vert.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile basic.vert
    exit /b 1
)

"%GLSLC%" "%SHADER_DIR%\basic.frag" -o "%OUT_DIR%\basic.frag.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile basic.frag
    exit /b 1
)

echo Compiling shadow shaders...
"%GLSLC%" "%SHADER_DIR%\shadow.vert" -o "%OUT_DIR%\shadow.vert.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile shadow.vert
    exit /b 1
)

"%GLSLC%" "%SHADER_DIR%\shadow.frag" -o "%OUT_DIR%\shadow.frag.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile shadow.frag
    exit /b 1
)

echo Compiling sky shaders...
"%GLSLC%" "%SHADER_DIR%\sky.vert" -o "%OUT_DIR%\sky.vert.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile sky.vert
    exit /b 1
)

"%GLSLC%" "%SHADER_DIR%\sky.frag" -o "%OUT_DIR%\sky.frag.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile sky.frag
    exit /b 1
)

echo Compiling FXAA shaders...
"%GLSLC%" "%SHADER_DIR%\fxaa.vert" -o "%OUT_DIR%\fxaa.vert.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile fxaa.vert
    exit /b 1
)

"%GLSLC%" "%SHADER_DIR%\fxaa.frag" -o "%OUT_DIR%\fxaa.frag.spv"
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: Failed to compile fxaa.frag
    exit /b 1
)

echo.
echo Shader compilation successful!
echo Output files:
echo   %OUT_DIR%\basic.vert.spv
echo   %OUT_DIR%\basic.frag.spv
echo   %OUT_DIR%\shadow.vert.spv
echo   %OUT_DIR%\shadow.frag.spv
echo   %OUT_DIR%\sky.vert.spv
echo   %OUT_DIR%\sky.frag.spv
echo   %OUT_DIR%\fxaa.vert.spv
echo   %OUT_DIR%\fxaa.frag.spv

endlocal
