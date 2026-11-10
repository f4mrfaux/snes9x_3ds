@echo off
rem Builds the libretro snes9x core with MSVC x64, for use with wram_host.
rem   build_core_msvc.bat <core source> <outdir>
rem Keep both paths short: cmd fails with "input line too long" otherwise.
rem Apply core-ppu-export.patch to the core first to get PPU state in the host.
setlocal enabledelayedexpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set ROOT=%~f1
set OUT=%~f2
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set COMM=%ROOT%\libretro\libretro-common
set FLAGS=/nologo /O2 /MT /W0 /EHsc /bigobj /D__LIBRETRO__ /DALLOW_CPU_OVERCLOCK /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /Dstrcasecmp=_stricmp /Dstrncasecmp=_strnicmp /D__inline__=__inline /I"%ROOT%\libretro" /I"%COMM%\include" /I"%ROOT%" /I"%ROOT%\apu"
pushd "%OUT%\obj"
set FILES=
for %%f in (c4emu msu1 zipfile srtc obc1 bsflash tile bsx spc7110 fxemu sdd1 seta sa1hw dsp filter\snes_ntsc) do set FILES=!FILES! "%ROOT%\%%f.c"
for %%f in (apu\apu apu\bapu\dsp\sdsp apu\bapu\smp\smp apu\bapu\smp\smp_state cheats2 clip controls cpu cpuexec cpuops crosshairs dma gfx globals loadzip memmap ppu sa1 sa1cpu snapshot sha256 bml fscompat libretro\libretro) do set FILES=!FILES! "%ROOT%\%%f.cpp"
for %%f in (compat\compat_strl compat\compat_posix_string compat\fopen_utf8 compat\compat_strcasestr encodings\encoding_utf encodings\encoding_deflate file\file_path file\file_path_io streams\file_stream streams\file_stream_transforms string\stdstring time\rtime vfs\vfs_implementation vfs\vfs_hybrid file\retro_dirent) do if exist "%COMM%\%%f.c" set FILES=!FILES! "%COMM%\%%f.c"
cl %FLAGS% /MP /c %FILES% || (popd & exit /b 1)
link /nologo /DLL /OUT:..\snes9x_libretro.dll /DEF:"%ROOT%\libretro\libretro.def" *.obj ws2_32.lib "/alternatename:OpenBus=?OpenBus@@3EA" "/alternatename:Settings=?Settings@@3USSettings@@A" "/alternatename:LineMatrixData=?LineMatrixData@@3PAUSLineMatrixData@@A" || (popd & exit /b 1)
popd
echo BUILD_OK
