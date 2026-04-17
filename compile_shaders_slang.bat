@echo off
REM Compile all Slang shaders to DXIL and SPIR-V
REM Requires slangc in Tools\slang-2026.5.2\bin\
REM Metal output can be added when targeting macOS
REM
REM Note: pbr.slang and common.slang are modules imported by other shaders.
REM They are NOT compiled standalone — slangc resolves them automatically.

setlocal enabledelayedexpansion

set SLANGC=%~dp0Tools\slang-2026.5.2\bin\slangc.exe
set SHADER_DIR=%~dp0assets\shaders\slang
set OUT_D3D12=%~dp0assets\shaders\compiled\d3d12
set OUT_VK=%~dp0assets\shaders\compiled\vulkan

if not exist "%SLANGC%" (
    echo ERROR: slangc not found at %SLANGC%
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
echo Compiling gbuffer...
call :compile gbuffer vertexMain fragmentMain
echo Compiling deferred_lighting...
call :compile deferred_lighting vertexMain fragmentMain

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
