@echo off
setlocal EnableDelayedExpansion

set "GARDEN_ROOT=%~dp0"
:: Remove trailing backslash
if "%GARDEN_ROOT:~-1%"=="\" set "GARDEN_ROOT=%GARDEN_ROOT:~0,-1%"

set "CLI_SRC=%GARDEN_ROOT%\Tools\GardenCLI"
set "INSTALL_DIR=%LOCALAPPDATA%\Garden\bin"

echo ============================================
echo  Garden Engine Setup
echo ============================================
echo.

:: ---- Step 1: Build GardenCLI ----
echo [1/5] Building Garden CLI tool...

:: Check if already built
if exist "%GARDEN_ROOT%\bin\GardenCLI.exe" (
    echo   Found existing build at bin\GardenCLI.exe
    set "BUILT_CLI=%GARDEN_ROOT%\bin\GardenCLI.exe"
    goto :install
)
if exist "%GARDEN_ROOT%\x64\Release\GardenCLI.exe" (
    echo   Found existing build at x64\Release\GardenCLI.exe
    set "BUILT_CLI=%GARDEN_ROOT%\x64\Release\GardenCLI.exe"
    goto :install
)
if exist "%GARDEN_ROOT%\GardenCLI.exe" (
    echo   Found existing build at GardenCLI.exe
    set "BUILT_CLI=%GARDEN_ROOT%\GardenCLI.exe"
    goto :install
)

:: Try Sighmake
where sighmake >nul 2>&1
if %ERRORLEVEL%==0 (
    echo   Using Sighmake...
    pushd "%GARDEN_ROOT%"
    sighmake project.buildscript
    popd
    echo   Build the solution, then re-run this script.
    echo   Or use the fallback compiler below.
)

:: Fallback: direct MSVC compilation
where cl >nul 2>&1
if %ERRORLEVEL%==0 (
    echo   Using direct MSVC compilation...
    pushd "%GARDEN_ROOT%"
    set "IMGUI_DIR=%GARDEN_ROOT%\Engine\Thirdparty\imgui-docking"
    set "SDL2_DIR=%GARDEN_ROOT%\Engine\Thirdparty\SDL2"
    cl /std:c++20 /EHsc /O2 /utf-8 ^
       /I"%CLI_SRC%\src" ^
       /I"%GARDEN_ROOT%\Engine\Thirdparty\tinygltf-2.9.6" ^
       /I"%IMGUI_DIR%" ^
       /I"%IMGUI_DIR%\backends" ^
       /I"%SDL2_DIR%\include" ^
       /Fe:GardenCLI.exe ^
       "%CLI_SRC%\src\main.cpp" ^
       "%CLI_SRC%\src\EngineRegistry.cpp" ^
       "%CLI_SRC%\src\ProjectFile.cpp" ^
       "%CLI_SRC%\src\EnginePicker.cpp" ^
       "%IMGUI_DIR%\imgui.cpp" ^
       "%IMGUI_DIR%\imgui_draw.cpp" ^
       "%IMGUI_DIR%\imgui_widgets.cpp" ^
       "%IMGUI_DIR%\imgui_tables.cpp" ^
       "%IMGUI_DIR%\backends\imgui_impl_sdl2.cpp" ^
       "%IMGUI_DIR%\backends\imgui_impl_sdlrenderer2.cpp" ^
       /link "%SDL2_DIR%\lib\x64\SDL2.lib"
    popd
    if exist "%GARDEN_ROOT%\GardenCLI.exe" (
        set "BUILT_CLI=%GARDEN_ROOT%\GardenCLI.exe"
        goto :install
    )
    echo   ERROR: Compilation failed.
    goto :error
) else (
    echo   ERROR: Neither sighmake nor MSVC (cl.exe) found.
    echo   Please run from a Visual Studio Developer Command Prompt,
    echo   or build the GardenCLI project from the solution.
    goto :error
)

:: ---- Step 2: Install to PATH ----
:install
echo.
echo [2/5] Installing to %INSTALL_DIR%...

if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
if exist "%INSTALL_DIR%\garden.exe" del /F /Q "%INSTALL_DIR%\garden.exe"
copy /Y "%BUILT_CLI%" "%INSTALL_DIR%\garden.exe" >nul
echo   Installed garden.exe

:: ---- Step 3: Add to user PATH ----
echo.
echo [3/5] Checking PATH...

echo %PATH% | findstr /I /C:"%INSTALL_DIR%" >nul
if %ERRORLEVEL% neq 0 (
    :: Add to user PATH via registry (avoids setx 1024 char limit)
    for /f "tokens=2*" %%a in ('reg query "HKCU\Environment" /v Path 2^>nul') do set "USER_PATH=%%b"
    if defined USER_PATH (
        reg add "HKCU\Environment" /v Path /t REG_EXPAND_SZ /d "!USER_PATH!;%INSTALL_DIR%" /f >nul
    ) else (
        reg add "HKCU\Environment" /v Path /t REG_EXPAND_SZ /d "%INSTALL_DIR%" /f >nul
    )
    echo   Added %INSTALL_DIR% to user PATH
    echo   NOTE: Restart your terminal for PATH changes to take effect.
) else (
    echo   Already in PATH.
)

:: ---- Copy runtime dependencies to bin/ ----
if not exist "%GARDEN_ROOT%\bin" mkdir "%GARDEN_ROOT%\bin"
if exist "%GARDEN_ROOT%\Engine\Thirdparty\SDL2\lib\x64\SDL2.dll" (
    copy /Y "%GARDEN_ROOT%\Engine\Thirdparty\SDL2\lib\x64\SDL2.dll" "%GARDEN_ROOT%\bin\SDL2.dll" >nul
    echo   Copied SDL2.dll to bin/
)

:: ---- Step 4: File association ----
echo.
echo [4/5] Setting up .garden file association...

reg add "HKCU\Software\Classes\.garden" /ve /d "GardenProject" /f >nul
reg add "HKCU\Software\Classes\GardenProject" /ve /d "Garden Project File" /f >nul
reg add "HKCU\Software\Classes\GardenProject\shell\open\command" /ve /d "\"%INSTALL_DIR%\garden.exe\" \"%%1\"" /f >nul
reg add "HKCU\Software\Classes\GardenProject\shell\generate" /ve /d "Generate Project Files" /f >nul
reg add "HKCU\Software\Classes\GardenProject\shell\generate\command" /ve /d "\"%INSTALL_DIR%\garden.exe\" generate \"%%1\"" /f >nul
echo   Associated .garden files with garden.exe

:: ---- Step 5: Register this engine ----
echo.
echo [5/5] Registering engine...

"%INSTALL_DIR%\garden.exe" register-engine --path "%GARDEN_ROOT%"

echo.
echo ============================================
echo  Setup complete!
echo ============================================
echo.
echo You may need to restart your terminal for PATH changes.
echo Double-click any .garden file to open it in the editor.
echo.
pause
exit /b 0

:error
echo.
echo Setup failed. See errors above.
pause
exit /b 1
