@echo off
rem Play in a window: tools\play.cmd C:\path\to\eltalemonsters.jp.z64 [more eltale-p1 options]
rem   e.g. tools\play.cmd rom.z64 --renderer native   (RT64 draws unless --renderer native)
rem Builds nothing; run tools\build_rt64.cmd, python tools\build_host.py and
rem python tools\build_windows.py --p1-os --build-only first.
setlocal
cd /d "%~dp0.."
if "%~1"=="" (echo usage: tools\play.cmd rom.z64 [options] & exit /b 1)
copy /y out\host\*.dll out\p1-windows\ >nul || (echo out\host has no DLLs: python tools\build_host.py & exit /b 1)
if exist out\rt64\eltale_rt64.dll copy /y out\rt64\*.dll out\p1-windows\ >nul
out\p1-windows\eltale-p1.exe --window --rom %*
