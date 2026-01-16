@echo off
setlocal

echo Starting build...
sighmake project.buildscript || pause

echo Build completed successfully.
pause
