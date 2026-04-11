@echo off
REM Compile all Slang shaders to DXIL and SPIR-V
REM Requires slangc in Tools\slang-2026.5.2\bin\
REM Metal output can be added when targeting macOS

setlocal enabledelayedexpansion

set SLANGC=%~dp0Tools\slang-2026.5.2\bin\slangc.exe
set SHADER_DIR=%~dp0assets\shaders\slang
set OUT_D3D12=%~dp0assets\shaders\compiled\d3d12
set OUT_VK=%~dp0assets\shaders\compiled\vulkan

if not exist "%SLANGC%" (
    echo ERROR: slangc not found at %SLANGC%
    exit /b 1
)

if not exist "%OUT_D3D12%" mkdir "%OUT_D3D12%"
if not exist "%OUT_VK%" mkdir "%OUT_VK%"

set ERRORS=0

REM Helper: compile a shader to DXBC (VS+PS) and SPIR-V (vert+frag)
REM Usage: call :compile <name> <vs_entry> <ps_entry>

echo Compiling shadow...
call :compile shadow vertexMain fragmentMain
echo Compiling sky...
call :compile sky vertexMain fragmentMain
echo Compiling fxaa...
call :compile fxaa vertexMain fragmentMain
echo Compiling basic...
call :compile basic vertexMain fragmentMain
echo Compiling unlit...
call :compile unlit vertexMain fragmentMain
echo Compiling skinned...
call :compile skinned vertexMain fragmentMain
echo Compiling skinned_shadow...
call :compile skinned_shadow vertexMain fragmentMain

REM rmlui has 3 entry points
echo Compiling rmlui...
"%SLANGC%" "%SHADER_DIR%\rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry vertexMain -stage vertex -o "%OUT_D3D12%\rmlui_vs.dxil" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentTextured -stage fragment -o "%OUT_D3D12%\rmlui_ps_textured.dxil" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\rmlui.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry fragmentColor -stage fragment -o "%OUT_D3D12%\rmlui_ps_color.dxil" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry vertexMain -stage vertex -o "%OUT_VK%\rmlui.vert.spv" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentTextured -stage fragment -o "%OUT_VK%\rmlui_texture.frag.spv" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\rmlui.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry fragmentColor -stage fragment -o "%OUT_VK%\rmlui_color.frag.spv" 2>nul
if errorlevel 1 set /a ERRORS+=1

echo.
if %ERRORS% equ 0 (
    echo All shaders compiled successfully!
) else (
    echo %ERRORS% shader compilation(s) failed.
    exit /b 1
)
goto :eof

:compile
set NAME=%~1
set VS_ENTRY=%~2
set PS_ENTRY=%~3
"%SLANGC%" "%SHADER_DIR%\%NAME%.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry %VS_ENTRY% -stage vertex -o "%OUT_D3D12%\%NAME%_vs.dxil" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\%NAME%.slang" -DTARGET_HLSL -target dxil -profile sm_6_0 -entry %PS_ENTRY% -stage fragment -o "%OUT_D3D12%\%NAME%_ps.dxil" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\%NAME%.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry %VS_ENTRY% -stage vertex -o "%OUT_VK%\%NAME%.vert.spv" 2>nul
if errorlevel 1 set /a ERRORS+=1
"%SLANGC%" "%SHADER_DIR%\%NAME%.slang" -DTARGET_SPIRV -target spirv -profile glsl_450 -entry %PS_ENTRY% -stage fragment -o "%OUT_VK%\%NAME%.frag.spv" 2>nul
if errorlevel 1 set /a ERRORS+=1
goto :eof
