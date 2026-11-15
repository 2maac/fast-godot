@echo off
setlocal EnableExtensions EnableDelayedExpansion
title Fast_Godot editor build

rem The toolchain is the folder this file lives in; the source tree is found below.
set "TC=%~dp0"
if "%TC:~-1%"=="\" set "TC=%TC:~0,-1%"
if not defined GRIT_JOBS set "GRIT_JOBS=16"

set "SRC="
set "EXTRA="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="-Engine" ( set "SRC=%~2" & shift & shift & goto parse )
if /i "%~1"=="--engine" ( set "SRC=%~2" & shift & shift & goto parse )
if /i "%~1"=="-Toolchain" ( set "TC=%~2" & shift & shift & goto parse )
if /i "%~1"=="--toolchain" ( set "TC=%~2" & shift & shift & goto parse )
set "EXTRA=!EXTRA! %1"
shift
goto parse
:parsed

if not defined SRC if defined GRIT_ENGINE set "SRC=%GRIT_ENGINE%"
if not defined SRC if defined GRIT_SRC set "SRC=%GRIT_SRC%"

if defined SRC call :check "%SRC%"
if not defined SRC call :find "%CD%"
if not defined SRC call :find "%~dp0"
if not defined SRC call :find "%~dp0.."
if not defined SRC call :find "%~dp0..\.."
if not defined SRC for /d %%D in ("%USERPROFILE%\*") do call :find "%%~fD"
if not defined SRC for /d %%D in ("%USERPROFILE%\Desktop\*") do call :find "%%~fD"
if not defined SRC for /d %%D in ("%USERPROFILE%\Documents\*") do call :find "%%~fD"
if not defined SRC for /d %%D in ("%USERPROFILE%\Downloads\*") do call :find "%%~fD"
if not defined SRC for /d %%A in ("C:\b\src*") do for /d %%B in ("%%~fA\*") do call :find "%%~fB"
if not defined SRC for /d %%D in ("C:\b\*") do call :find "%%~fD"

if not defined SRC (
  echo [error] No Fast_Godot source tree found ^(it needs tools\grit_build.py^).
  echo         Pass -Engine PATH or set GRIT_ENGINE.
  pause
  exit /b 1
)
if not exist "%TC%\python\python.exe" (
  echo [error] Toolchain not found: %TC%
  pause
  exit /b 1
)
if not exist "%TC%\libffi\3.8.0\mingw64\include" (
  echo [error] The toolchain has no libffi prefix ^(libffi\3.8.0\mingw64^).
  pause
  exit /b 1
)

set "PATH=%TC%\mingw64\bin;%TC%\python;%PATH%"
set "PYTHONPATH=%TC%\scons\4.11.1\scons-4.11.1"
set "MINGW_PREFIX=%TC%\mingw64"

rem The editor's Direct3D 12 driver needs the Agility SDK; install it once.
if not exist "%LOCALAPPDATA%\Godot\build_deps\agility_sdk" (
  echo Grit: installing the Direct3D 12 Agility SDK ^(one time^)...
  pushd "%SRC%"
  "%TC%\python\python.exe" misc\scripts\install_d3d12_sdk_windows.py
  set "DEPS=!ERRORLEVEL!"
  popd
  if not "!DEPS!"=="0" (
    echo [error] The Direct3D 12 dependency installer failed.
    pause
    exit /b 1
  )
)

rem The editor's ANGLE (GLES3) driver needs the static ANGLE drops; install once.
if not exist "%LOCALAPPDATA%\Godot\build_deps\angle-x86_64-gcc" if not exist "%LOCALAPPDATA%\Godot\build_deps\angle" (
  echo Grit: installing the ANGLE rendering driver dependencies ^(one time^)...
  pushd "%SRC%"
  "%TC%\python\python.exe" misc\scripts\install_angle.py
  set "ANGLE_DEPS=!ERRORLEVEL!"
  popd
  if not "!ANGLE_DEPS!"=="0" (
    echo [error] The ANGLE dependency installer failed.
    pause
    exit /b 1
  )
)

echo Toolchain: %TC%
echo Source:    %SRC%
echo.

cd /d "%SRC%"
"%TC%\python\python.exe" -m SCons platform=windows target=editor -j%GRIT_JOBS% lto=none use_mingw=yes use_volk=yes vulkan=yes accesskit=no winrt=no cffi_libffi_path="%TC%\libffi\3.8.0\mingw64" %EXTRA%
set "CODE=!ERRORLEVEL!"

echo.
if "!CODE!"=="0" (
  echo Build succeeded.
  echo Editor: %SRC%\bin\godot.windows.editor.x86_64.exe
) else (
  echo Build failed with exit code !CODE!.
)
echo.
pause
endlocal & exit /b %CODE%

:check
if exist "%~1\tools\grit_build.py" (
  set "SRC=%~1"
) else (
  echo [error] Not a Fast_Godot checkout: %~1
)
goto :eof

:find
set "P=%~1"
if not defined P goto :eof
for /l %%N in (1,1,4) do (
  if not defined SRC if exist "!P!\tools\grit_build.py" set "SRC=!P!"
  if not defined SRC (
    for %%I in ("!P!") do set "P=%%~dpI"
    if "!P:~-1!"=="\" set "P=!P:~0,-1!"
  )
)
goto :eof
