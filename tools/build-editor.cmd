@echo off
setlocal EnableExtensions
title Fast_Godot editor build
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-editor.ps1" %*
set "CODE=%ERRORLEVEL%"
echo.
pause
endlocal & exit /b %CODE%
