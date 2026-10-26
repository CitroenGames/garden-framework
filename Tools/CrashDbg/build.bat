@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++17 /Fe:"%~dp0crashdbg.exe" /Fo:"%~dp0crashdbg.obj" "%~dp0crashdbg.cpp" /link dbghelp.lib
