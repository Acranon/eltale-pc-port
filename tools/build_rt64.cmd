@echo off
rem Build out\rt64\eltale_rt64.dll (port/rt64) with clang-cl from Visual Studio Build Tools.
setlocal
set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (echo vcvars64.bat not found: install VS 2022 Build Tools with the Clang component & exit /b 1)
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0.."
cmake -S port/rt64 -B out/rt64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl || exit /b 1
cmake --build out/rt64 || exit /b 1
