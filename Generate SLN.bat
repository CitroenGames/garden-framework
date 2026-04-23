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

:: Generate solutions for the bundled editor plugins. Each plugin lives in a
:: standalone sighmake solution (it links against EngineSDK, not engine source)
:: so we invoke sighmake separately. The main Garden solution must be built
:: first because plugins link ../bin/EngineCore.lib etc.
echo.
echo Generating bundled editor plugins...
if not exist plugins mkdir plugins
for %%P in (EditorPlugins\QuakeImporter\QuakeImporter.buildscript) do (
    if exist %%P (
        echo   - %%P
        sighmake %%P -D ENGINE_PATH=. || pause
    )
)

echo Build completed successfully.
pause
