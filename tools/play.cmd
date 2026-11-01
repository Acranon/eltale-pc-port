@echo off
rem Play in a window: tools\play.cmd C:\path\to\eltalemonsters.jp.z64 [more eltale-p1 options]
rem Builds nothing; run python tools\build_windows.py --p1-os --build-only --rom ROM and
rem python tools\build_host.py first. Copies out\host's DLLs beside the exe, then plays.
setlocal
cd /d "%~dp0.."
if "%~1"=="" (echo usage: tools\play.cmd rom.z64 [options] & exit /b 1)
copy /y out\host\*.dll out\p1-windows\ >nul || (echo out\host has no DLLs: python tools\build_host.py & exit /b 1)
out\p1-windows\eltale-p1.exe --window --rom %*
