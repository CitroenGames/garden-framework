@echo off
REM Compile all Slang shaders to DXIL and SPIR-V
REM Uses slangc from Tools\slang-2026.20\bin\ — downloaded automatically
REM from https://github.com/shader-slang/slang/releases/tag/v2026.20
REM on first run if missing. Tools\slang-2026.20\ is git-ignored.
REM Metal output can be added when targeting macOS
REM
REM Note: pbr.slang and common.slang are modules imported by other shaders.
REM They are NOT compiled standalone — slangc resolves them automatically.

setlocal enabledelayedexpansion

set SLANG_VERSION=2026.20
set SLANG_TAG=v2026.20
set SLANG_DIR=%~dp0Tools\slang-%SLANG_VERSION%
set SLANGC=%SLANG_DIR%\bin\slangc.exe
set SHADER_DIR=%~dp0assets\shaders\slang
set OUT_D3D12=%~dp0assets\shaders\compiled\d3d12
set OUT_VK=%~dp0assets\shaders\compiled\vulkan

if not exist "%SLANGC%" (
    call :ensure_slang
)
if not exist "%SLANGC%" (
    echo ERROR: slangc not found at %SLANGC%
    echo Automatic download of Slang %SLANG_VERSION% failed. Check your internet connection,
    echo or manually extract slang-%SLANG_VERSION%-windows-x86_64.zip from
    echo https://github.com/shader-slang/slang/releases/tag/%SLANG_TAG% into %SLANG_DIR%\
    exit /b 1
)

if not exist "%OUT_D3D12%" (
    mkdir "%OUT_D3D12%"
    if errorlevel 1 (
        echo ERROR: Failed to create output directory %OUT_D3D12%
        exit /b 1
    )
)
if not exist "%OUT_VK%" (
    mkdir "%OUT_VK%"
    if errorlevel 1 (
        echo ERROR: Failed to create output directory %OUT_VK%
        exit /b 1
    )
)

set ERRORS=0

REM Suppress binding-overlap warning (E39001): Vulkan engine uses COMBINED_IMAGE_SAMPLER
REM so texture+sampler sharing a vk::binding is intentional, not an error.
set SLANG_WARN=-warnings-disable 39001

REM Helper: compile a shader to DXIL (VS+PS) and SPIR-V (vert+frag)
REM Usage: call :compile <name> <vs_entry> <ps_entry>

echo Compiling shadow...
call :compile shadow vertexMain fragmentMain
echo Compiling sky...
call :compile sky vertexMain fragmentMain
echo Compiling fxaa...
call :compile fxaa vertexMain fragmentMain
echo Compiling basic (with PBR)...
call :compile basic vertexMain fragmentMain
echo Compiling unlit...
call :compile unlit vertexMain fragmentMain
echo Compiling skinned (with PBR)...
call :compile skinned vertexMain fragmentMain
echo Compiling skinned_shadow...
call :compile skinned_shadow vertexMain fragmentMain
echo Compiling shadow_alphatest...
call :compile shadow_alphatest vertexMain fragmentMain
echo Compiling ssao...
call :compile ssao vertexMain fragmentMain
echo Compiling ssao_blur...
call :compile ssao_blur vertexMain fragmentMain
echo Compiling shadow_mask...
call :compile shadow_mask vertexMain fragmentMain

REM rmlui has 3 entry points
echo Compiling rmlui...
if not exist "%SHADER_DIR%\rmlui.slang" (
    echo   ERROR: Source file not found: %SHADER_DIR%\rmlui.slang
    set /a ERRORS+=6
    goto :after_rmlui
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry vertexMain -stage vertex -o "%OUT_D3D12%\rmlui_vs.dxil"
if errorlevel 1 (
    echo   FAILED: rmlui [DXIL vertex]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentTextured -stage fragment -o "%OUT_D3D12%\rmlui_ps_textured.dxil"
if errorlevel 1 (
    echo   FAILED: rmlui [DXIL fragmentTextured]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentColor -stage fragment -o "%OUT_D3D12%\rmlui_ps_color.dxil"
if errorlevel 1 (
    echo   FAILED: rmlui [DXIL fragmentColor]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry vertexMain -stage vertex -o "%OUT_VK%\rmlui.vert.spv"
if errorlevel 1 (
    echo   FAILED: rmlui [SPIR-V vertex]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentTextured -stage fragment -o "%OUT_VK%\rmlui_texture.frag.spv"
if errorlevel 1 (
    echo   FAILED: rmlui [SPIR-V fragmentTextured]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentColor -stage fragment -o "%OUT_VK%\rmlui_color.frag.spv"
if errorlevel 1 (
    echo   FAILED: rmlui [SPIR-V fragmentColor]
    set /a ERRORS+=1
)
:after_rmlui

echo.
if %ERRORS% equ 0 (
    echo All shaders compiled successfully!
) else (
    echo %ERRORS% shader compilation^(s^) failed.
    exit /b 1
)
goto :eof

:compile
set NAME=%~1
set VS_ENTRY=%~2
set PS_ENTRY=%~3
if not exist "%SHADER_DIR%\%NAME%.slang" (
    echo   ERROR: Source file not found: %SHADER_DIR%\%NAME%.slang
    set /a ERRORS+=4
    goto :eof
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\%NAME%.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry %VS_ENTRY% -stage vertex -o "%OUT_D3D12%\%NAME%_vs.dxil"
if errorlevel 1 (
    echo   FAILED: %NAME% [DXIL vertex]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\%NAME%.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry %PS_ENTRY% -stage fragment -o "%OUT_D3D12%\%NAME%_ps.dxil"
if errorlevel 1 (
    echo   FAILED: %NAME% [DXIL fragment]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\%NAME%.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry %VS_ENTRY% -stage vertex -o "%OUT_VK%\%NAME%.vert.spv"
if errorlevel 1 (
    echo   FAILED: %NAME% [SPIR-V vertex]
    set /a ERRORS+=1
)
"%SLANGC%" %SLANG_WARN% "%SHADER_DIR%\%NAME%.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry %PS_ENTRY% -stage fragment -o "%OUT_VK%\%NAME%.frag.spv"
if errorlevel 1 (
    echo   FAILED: %NAME% [SPIR-V fragment]
    set /a ERRORS+=1
)
goto :eof

REM ---------------------------------------------------------------------------
REM Download and extract Slang %SLANG_VERSION% into %SLANG_DIR% if slangc is
REM missing. The release archive is flat (bin\, lib\, include\ at its root),
REM so extracting it into %SLANG_DIR% yields %SLANG_DIR%\bin\slangc.exe.
:ensure_slang
set SLANG_ARCH=x86_64
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set SLANG_ARCH=aarch64
if /i "%PROCESSOR_ARCHITEW6432%"=="ARM64" set SLANG_ARCH=aarch64
set SLANG_ASSET=slang-%SLANG_VERSION%-windows-%SLANG_ARCH%.zip
set SLANG_URL=https://github.com/shader-slang/slang/releases/download/%SLANG_TAG%/%SLANG_ASSET%
set SLANG_TMP=%TEMP%\garden-slang-%SLANG_VERSION%
set SLANG_ARCHIVE=%SLANG_TMP%\%SLANG_ASSET%
set SLANG_STAGE=%SLANG_TMP%\extracted

echo Slang %SLANG_VERSION% not found in %SLANG_DIR%\
echo Downloading %SLANG_URL% ...
if exist "%SLANG_TMP%" rmdir /s /q "%SLANG_TMP%"
mkdir "%SLANG_STAGE%"
if errorlevel 1 (
    echo ERROR: Failed to create temp directory %SLANG_STAGE%
    goto :eof
)

where curl.exe >nul 2>&1
if not errorlevel 1 (
    curl.exe -L --fail --retry 3 -o "%SLANG_ARCHIVE%" "%SLANG_URL%"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%SLANG_URL%' -OutFile '%SLANG_ARCHIVE%'"
)
if errorlevel 1 (
    echo ERROR: Failed to download %SLANG_URL%
    rmdir /s /q "%SLANG_TMP%"
    goto :eof
)
if not exist "%SLANG_ARCHIVE%" (
    echo ERROR: Download did not produce %SLANG_ARCHIVE%
    rmdir /s /q "%SLANG_TMP%"
    goto :eof
)

echo Extracting to %SLANG_DIR%\ ...
where tar.exe >nul 2>&1
if not errorlevel 1 (
    tar.exe -xf "%SLANG_ARCHIVE%" -C "%SLANG_STAGE%"
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%SLANG_ARCHIVE%' -DestinationPath '%SLANG_STAGE%' -Force"
)
if errorlevel 1 (
    echo ERROR: Failed to extract %SLANG_ARCHIVE%
    rmdir /s /q "%SLANG_TMP%"
    goto :eof
)

REM Archive root normally contains bin\slangc.exe directly; tolerate a
REM single nested top-level folder as well.
set SLANG_ROOT=%SLANG_STAGE%
if not exist "%SLANG_ROOT%\bin\slangc.exe" (
    for /d %%D in ("%SLANG_STAGE%\*") do (
        if exist "%%D\bin\slangc.exe" set SLANG_ROOT=%%D
    )
)
if not exist "!SLANG_ROOT!\bin\slangc.exe" (
    echo ERROR: slangc.exe not found in downloaded archive %SLANG_ASSET%
    rmdir /s /q "%SLANG_TMP%"
    goto :eof
)

if not exist "%SLANG_DIR%" mkdir "%SLANG_DIR%"
robocopy "!SLANG_ROOT!" "%SLANG_DIR%" /E /NFL /NDL /NJH /NJS >nul
if !errorlevel! geq 8 (
    echo ERROR: Failed to copy Slang into %SLANG_DIR%\
    rmdir /s /q "%SLANG_TMP%"
    goto :eof
)
rmdir /s /q "%SLANG_TMP%"

if exist "%SLANGC%" (
    echo Slang %SLANG_VERSION% installed to %SLANG_DIR%\
) else (
    echo ERROR: Slang extraction did not produce %SLANGC%
)
goto :eof
