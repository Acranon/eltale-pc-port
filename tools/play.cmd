@echo off
rem Play in a window: tools\play.cmd C:\path\to\eltalemonsters.jp.z64 [more eltale-p1 options]
rem Builds nothing; run tools\build_rt64.cmd and tools\build_windows.py --p1-os --build-only first.
setlocal
cd /d "%~dp0.."
if "%~1"=="" (echo usage: tools\play.cmd rom.z64 [options] & exit /b 1)
copy /y out\rt64\*.dll out\p1-windows\ >nul || exit /b 1
out\p1-windows\eltale-p1.exe --window --rom %*
