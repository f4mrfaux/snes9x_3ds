@echo off
rem Builds wram_host.exe with MSVC x64.
rem   build_wram_host.bat <outdir> <folder holding libretro.h>
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%~1" mkdir "%~1"
cl /nologo /O2 /MT /W3 /EHsc /D_CRT_SECURE_NO_WARNINGS /I"%~2" /Fe"%~1\wram_host.exe" /Fo"%~1\wram_host.obj" "%~dp0wram_host.cpp" || exit /b 1
echo BUILD_OK
