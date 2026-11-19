@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem Downloads a complete Fast_Godot toolchain from the original sources into
rem %USERPROFILE%\fast_godot_toolchain (or the folder given as %1).
rem Uses only curl.exe, tar.exe and certutil.exe, which ship with Windows 10/11.
rem Emscripten is the one exception: emsdk is a Python program and installs
rem Emscripten itself with the Python downloaded here.
rem Progress is printed as "GRIT_PROGRESS <percent> <label>" and the result as
rem "GRIT_DONE <folder>" / "GRIT_ERROR <text>", which the editor reads.

set "DEST=%~1"
set "PROG=%~2"
if not defined DEST set "DEST=%USERPROFILE%\fast_godot_toolchain"
if not exist "%DEST%" mkdir "%DEST%"
set "TMP=%DEST%\.download"
if exist "%TMP%" rmdir /s /q "%TMP%"
mkdir "%TMP%"

call :fetch "https://github.com/astral-sh/python-build-standalone/releases/download/20261110/cpython-3.12.16+20261110-x86_64-pc-windows-msvc-install_only.tar.gz" "e23fe5b16926de1d81661b1a97352e89fb167549c55a6825796ccd0f32ec7f1b" "%TMP%\python.tar.gz" "10" "Python" || goto :fail
tar.exe -xzf "%TMP%\python.tar.gz" -C "%DEST%" || goto :fail

call :fetch "https://files.pythonhosted.org/packages/42/b9/b7a5c88f348a0c34594d88100872c55fa1cae863ccb222c1c438341b5503/scons-4.11.1.tar.gz" "4210d1a80a62e986029208117991b6347ccaaaab37b67463a3ff31ee065dc487" "%TMP%\scons.tar.gz" "18" "SCons" || goto :fail
if not exist "%DEST%\scons\4.11.1" mkdir "%DEST%\scons\4.11.1"
tar.exe -xzf "%TMP%\scons.tar.gz" -C "%DEST%\scons\4.11.1" || goto :fail

call :fetch "https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip" "3c69305081e4a647144339a936244fcf7d3d9e558319e1ad7be2cd2886d4b6a1" "%TMP%\mingw.zip" "28" "MinGW-w64" || goto :fail
tar.exe -xf "%TMP%\mingw.zip" -C "%DEST%" || goto :fail

call :fetch "https://repo.msys2.org/mingw/mingw64/mingw-w64-x86_64-libffi-3.8.0-1-any.pkg.tar.zst" "6dad7f3c6c900d3c64a4fe0c2764b405e678b22df3521d2284ffaeff0198b84b" "%TMP%\libffi.pkg.tar.zst" "48" "libffi" || goto :fail
if exist "%TMP%\libffi" rmdir /s /q "%TMP%\libffi"
mkdir "%TMP%\libffi"
tar.exe -xf "%TMP%\libffi.pkg.tar.zst" -C "%TMP%\libffi" || goto :fail
mkdir "%DEST%\libffi\3.8.0\mingw64\include" 2>nul
mkdir "%DEST%\libffi\3.8.0\mingw64\lib" 2>nul
copy /y "%TMP%\libffi\mingw64\include\*.h" "%DEST%\libffi\3.8.0\mingw64\include\" >nul || goto :fail
copy /y "%TMP%\libffi\mingw64\lib\libffi*.a" "%DEST%\libffi\3.8.0\mingw64\lib\" >nul || goto :fail
rem libffi's dlopen/dlclose/dl* symbols collide with SDL's dlmalloc: rename them.
set "FFI=%DEST%\libffi\3.8.0\mingw64\lib"
powershell -NoProfile -Command "$nm='%DEST%\mingw64\bin\nm.exe'; $ar='%DEST%\mingw64\bin\objcopy.exe'; $f='%FFI%\libffi.a'; $r='%TMP%\redefines.txt'; & $nm --defined-only $f | Where-Object { $_ -match '^[0-9a-f]+ [A-Za-z] dl' } | ForEach-Object { $p = $_ -split '\s+'; $p[2] + ' ffi_' + $p[2] } | Set-Content -Encoding ascii $r; if (Test-Path $r) { & $ar --redefine-syms=$r $f }"
if errorlevel 1 goto :fail

call :fetch "https://github.com/godotengine/godot-angle-static/releases/download/chromium/7219/godot-angle-static-x86_64-gcc-release.zip" "ebf94e32334b04fa76b3907727c7a69c35adb713cf0a5d194c83c7ad955217bd" "%TMP%\angle.zip" "58" "ANGLE" || goto :fail
if not exist "%DEST%\angle" mkdir "%DEST%\angle"
tar.exe -xf "%TMP%\angle.zip" -C "%DEST%\angle" || goto :fail

call :fetch "https://github.com/godotengine/godot-nir-static/releases/download/25.3.1-3/godot-nir-static-x86_64-gcc-release.zip" "dc98a962ac90613a4fccf29082d34b12fcb97bf3819517125bc3d1c86ed31afc" "%TMP%\mesa.zip" "66" "Mesa NIR" || goto :fail
if not exist "%DEST%\mesa-x86_64-gcc" mkdir "%DEST%\mesa-x86_64-gcc"
tar.exe -xf "%TMP%\mesa.zip" -C "%DEST%\mesa-x86_64-gcc" || goto :fail

call :fetch "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/1.618.5" "0027fc24f947c48dbded13ada7d280be221eb651644e23a8a476f0f1f0a079dd" "%TMP%\agility.nupkg" "74" "Agility SDK" || goto :fail
if not exist "%DEST%\agility_sdk" mkdir "%DEST%\agility_sdk"
tar.exe -xf "%TMP%\agility.nupkg" -C "%DEST%\agility_sdk" || goto :fail

call :fetch "https://github.com/godotengine/winrt-mingw/releases/download/72/winrt-headers.zip" "9310868d0a82d11b7ae1943c4f35c202c6c9a692ee2deec4bde241fbe2639961" "%TMP%\winrt.zip" "82" "WinRT headers" || goto :fail
if not exist "%DEST%\winrt" mkdir "%DEST%\winrt"
tar.exe -xf "%TMP%\winrt.zip" -C "%DEST%\winrt" || goto :fail

call :fetch "https://github.com/emscripten-core/emsdk/archive/refs/tags/4.0.11.tar.gz" "058f97f3d408e6438c0128de63fc83b085a1a1c8f8e2ef992edee01281af9546" "%TMP%\emsdk.tar.gz" "88" "emsdk" || goto :fail
if exist "%DEST%\emsdk" rmdir /s /q "%DEST%\emsdk"
mkdir "%DEST%\emsdk"
tar.exe -xzf "%TMP%\emsdk.tar.gz" -C "%DEST%\emsdk" --strip-components=1 || goto :fail
pushd "%DEST%\emsdk"
"%DEST%\python\python.exe" emsdk.py install 4.0.11 >nul || goto :fail
"%DEST%\python\python.exe" emsdk.py activate 4.0.11 >nul || goto :fail
popd

call :progress 90 "Engine source"
rem The toolchain carries the engine source tree. The editor unpacks it straight
rem into engine\<revision> and runs this script from there, so there is nothing
rem to copy; a manual run from a checkout copies the tree it runs from instead.
set "ENGINE_ROOT=%~dp0..\.."
set "ENGINE_NAME=4.7.2-stable"
where git >nul 2>nul
if not errorlevel 1 (
  for /f "delims=" %%C in ('git -C "%ENGINE_ROOT%" rev-parse --short HEAD 2^>nul') do set "ENGINE_NAME=%%C"
)
rem A tree unpacked from a GitHub archive has no .git; the editor passes the
rem revision explicitly and has already placed the tree in engine\<revision>.
if "%ENGINE_NAME%"=="4.7.2-stable" if defined ENGINE_REV set "ENGINE_NAME=%ENGINE_REV%"
set "ENGINE_DEST=%DEST%\engine\%ENGINE_NAME%"
if exist "%ENGINE_DEST%\SConstruct" goto :engine_ready
if not exist "%ENGINE_ROOT%\SConstruct" goto :fail
if not exist "%ENGINE_ROOT%\tools\grit_build.py" goto :fail
if exist "%ENGINE_DEST%" rmdir /s /q "%ENGINE_DEST%"
mkdir "%ENGINE_DEST%"
tar.exe -cf "%TMP%\engine.tar" -C "%ENGINE_ROOT%" --exclude=.git --exclude=./bin --exclude=./out --exclude=./.godot --exclude=./compile_commands.json --exclude=./.sconsign.dblite --exclude=./.sconsign5.dblite --exclude=./.scons_env.json --exclude=./.scons_node_count --exclude=__pycache__ --exclude=.DS_Store . || goto :fail
tar.exe -xf "%TMP%\engine.tar" -C "%ENGINE_DEST%" || goto :fail
del /q "%TMP%\engine.tar" 2>nul
if not exist "%ENGINE_DEST%\SConstruct" goto :fail
:engine_ready

call :progress 96 "verifying"
for %%F in (
  "%DEST%\python\python.exe"
  "%DEST%\scons\4.11.1\scons-4.11.1\SCons\__init__.py"
  "%DEST%\mingw64\bin\gcc.exe"
  "%DEST%\libffi\3.8.0\mingw64\lib\libffi.a"
  "%DEST%\angle\libEGL.windows.x86_64.a"
  "%DEST%\mesa-x86_64-gcc\bin\libNIR.windows.x86_64.a"
  "%DEST%\agility_sdk\build\native\bin\x64\D3D12Core.dll"
  "%DEST%\winrt\winrt\Windows.Foundation.h"
  "%DEST%\emsdk\upstream\emscripten\emcc.bat"
  "%ENGINE_DEST%\SConstruct"
) do (
  if not exist %%F ( echo GRIT_ERROR missing %%F & goto :fail )
)

> "%DEST%\toolchain.json" (
  echo {
  echo   "name": "Fast_Godot_Toolchain_win64",
  echo   "format": 1,
  echo   "engineVersion": "4.7.2-stable",
  echo   "platform": "windows",
  echo   "arch": "x86_64",
  echo   "engine": "engine/%ENGINE_NAME%",
  echo   "python": "python",
  echo   "scons": "scons/4.11.1/scons-4.11.1",
  echo   "mingw": "mingw64",
  echo   "libffi": "libffi/3.8.0/mingw64",
  echo   "angle": "angle",
  echo   "mesa": "mesa-x86_64-gcc",
  echo   "agility_sdk": "agility_sdk",
  echo   "winrt": "winrt",
  echo   "emsdk": "emsdk"
  echo }
)

rmdir /s /q "%TMP%"
call :progress 100 "toolchain ready"
echo GRIT_DONE %DEST%
if defined PROG echo GRIT_DONE %DEST%>>"%PROG%"
endlocal & exit /b 0

:fetch
rem %1 url, %2 sha256, %3 file, %4 percent, %5 label
call :progress %~4 "%~5"
curl.exe -sSfL --retry 3 -o "%~3" "%~1" || ( echo GRIT_ERROR could not download %~1 & if defined PROG echo GRIT_ERROR could not download %~1>>"%PROG%" & exit /b 1 )
set "GOT="
for /f "delims=" %%H in ('certutil -hashfile "%~3" SHA256 ^| findstr /r "^[0-9a-fA-F][0-9a-fA-F]*$"') do (
  set "H=%%H"
  set "H=!H: =!"
  if not defined GOT set "GOT=!H!"
)
set "WANT=%~2"
if /i not "!GOT!"=="!WANT!" ( echo GRIT_ERROR checksum mismatch for %~1 & if defined PROG echo GRIT_ERROR checksum mismatch for %~1>>"%PROG%" & exit /b 1 )
exit /b 0

:fail
echo GRIT_ERROR the download failed
if defined PROG echo GRIT_ERROR the download failed>>"%PROG%"
rmdir /s /q "%TMP%" 2>nul
endlocal & exit /b 1

:progress
rem %1 percent, %2 label
echo GRIT_PROGRESS %~1 %~2
if defined PROG echo GRIT_PROGRESS %~1 %~2>>"%PROG%"
exit /b 0
