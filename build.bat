@echo off
rem Builds artview.3dsx (rule34.xxx + Derpibooru + DeviantArt + e621 + pixiv in one app) using devkitPro's MSYS2.
rem Usage: build.bat          builds artview.3dsx
rem        build.bat cia      also builds artview.cia (needs tools\makerom.exe + tools\bannertool.exe)
rem        build.bat clean
rem Single-site apps: build-r34.bat, build-derpi.bat, build-da.bat, build-e6.bat, build-px.bat (same options).
setlocal
set "MSYS_BASH=C:\devkitPro\msys2\usr\bin\bash.exe"
if not exist "%MSYS_BASH%" set "MSYS_BASH=C:\msys64\usr\bin\bash.exe"
if not exist "%MSYS_BASH%" (
  echo Could not find devkitPro MSYS2. Install devkitPro from https://devkitpro.org/wiki/Getting_Started
  pause
  exit /b 1
)
set "CHERE_INVOKING=1"
set "MSYSTEM=MSYS"
"%MSYS_BASH%" -lc "export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}; export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}; export PATH=$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH; cd \"$(cygpath -u '%~dp0')\" && make %*"
set ERR=%ERRORLEVEL%
if %ERR% neq 0 (echo. & echo Build failed. & pause & exit /b %ERR%)
echo. & echo Build finished.
pause
