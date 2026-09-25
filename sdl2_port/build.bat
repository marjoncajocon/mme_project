@echo off
rem build.bat - mme in a window of its own (SDL2): sdl2_port\bin\mme-sdl.exe
rem
rem   build [zig]      zig cc (the default), 64 bit
rem   build gcc        MinGW-w64 gcc, 64 bit
rem   build tcc        Tiny C Compiler, 64 bit
rem   build msvc       Visual C++ (cl), 64 bit: run it from a "x64 Native Tools" prompt,
rem                    or let it find Visual Studio with vswhere
rem   build clean
rem
rem Every .c file of mme is compiled as it is, but eterm.c and edraw.c: esdl.c
rem stands in for them (it includes edraw.c itself), and mos.c comes in through
rem emos.c (console programs started without a console window). SDL2 comes from deps\ (see
rem README.md). bin\ is the program, on its own: mme-sdl.exe, SDL2.dll, the
rem fonts (JetBrains Mono Nerd Font, from mmc) and, once it runs, its own
rem mme-data. It is not put in the mmc shell's folder with mme.exe.

setlocal enabledelayedexpansion
cd /d "%~dp0"

set SDL=deps\SDL2-2.32.10
if not exist "%SDL%\include\SDL.h" (
  echo SDL2 is not in %SDL%: see README.md
  exit /b 1
)
set FONTS=..\..\mmc
if not exist "%FONTS%\JetBrainsMonoNerdFontMono-Regular.ttf" set FONTS=D:\mmc-shell\usr\share\fonts

set SRC=
for %%F in (..\*.c) do (
  if /i not "%%~nxF"=="eterm.c" if /i not "%%~nxF"=="edraw.c" if /i not "%%~nxF"=="mos.c" set SRC=!SRC! %%F
)
set SRC=%SRC% esdl.c emos.c tfont.c tshape.c
if not exist bin mkdir bin
rem a mme-sdl.exe that is running cannot be written over, but it can be renamed
for %%F in (bin\mme-sdl.exe.old*) do del "%%F" >nul 2>nul
if exist bin\mme-sdl.exe del bin\mme-sdl.exe >nul 2>nul
if exist bin\mme-sdl.exe ren bin\mme-sdl.exe mme-sdl.exe.old%RANDOM%
if not exist obj mkdir obj

set CC=%1
if "%CC%"=="" set CC=zig
if "%CC%"=="clean" goto clean
if "%CC%"=="zig" goto zig
if "%CC%"=="gcc" goto gcc
if "%CC%"=="tcc" goto tcc
if "%CC%"=="msvc" goto msvc
echo usage: build [zig ^| gcc ^| tcc ^| msvc ^| clean]
exit /b 2

:zig
set ZIG=zig
where zig >nul 2>nul || set ZIG=D:\env\zig\zig.exe
%ZIG% cc -std=c11 -O2 -s -Wall -Wextra -pedantic -target x86_64-windows-gnu -I.. -I%SDL%\include ^
  -o bin\mme-sdl.exe %SRC% ..\mme.rc %SDL%\x86_64-w64-mingw32\lib\libSDL2.dll.a -lshell32 -lws2_32 -lgdi32 ^
  -Wl,--subsystem,windows || exit /b 1
if exist bin\mme-sdl.pdb del bin\mme-sdl.pdb
copy /y %SDL%\x86_64-w64-mingw32\bin\SDL2.dll bin\ >nul
goto done

:gcc
set GCC=gcc
where gcc >nul 2>nul || set GCC=D:\env\mingw\MinGW\bin\gcc.exe
set WINDRES=windres
where windres >nul 2>nul || set WINDRES=D:\env\mingw\MinGW\bin\windres.exe
pushd ..
"%WINDRES%" mme.rc -O coff -o sdl2_port\bin\mme.res.o || (popd & exit /b 1)
popd
"%GCC%" -std=c11 -O2 -s -Wall -Wextra -pedantic -I.. -I%SDL%\include -o bin\mme-sdl.exe %SRC% bin\mme.res.o ^
  %SDL%\x86_64-w64-mingw32\lib\libSDL2.dll.a -lshell32 -lws2_32 -lgdi32 -lm -mwindows || exit /b 1
del bin\mme.res.o
copy /y %SDL%\x86_64-w64-mingw32\bin\SDL2.dll bin\ >nul
goto done

:tcc
set TCC=tcc
where tcc >nul 2>nul || set TCC=D:\env\tcc\tcc.exe
rem tcc 0.9.27 lacks some Windows headers and import libraries: tcc\ has the
rem headers mme needs, and the .def files are made from the dlls (tcc -impdef)
if not exist obj\SDL2.def "%TCC%" -impdef %SDL%\x86_64-w64-mingw32\bin\SDL2.dll -o obj\SDL2.def || exit /b 1
for %%D in (kernel32 user32 gdi32 shell32 ws2_32 advapi32) do (
  if not exist obj\%%D.def "%TCC%" -impdef %SystemRoot%\System32\%%D.dll -o obj\%%D.def || exit /b 1
)
"%TCC%" -O2 -Itcc -I.. -I%SDL%\include -DSDLCALL= -o bin\mme-sdl.exe %SRC% ^
  obj\SDL2.def obj\kernel32.def obj\user32.def obj\gdi32.def obj\shell32.def obj\ws2_32.def obj\advapi32.def ^
  -Wl,-subsystem=gui || exit /b 1
copy /y %SDL%\x86_64-w64-mingw32\bin\SDL2.dll bin\ >nul
copy /y ..\mme.ico bin\ >nul
goto done

:msvc
where cl >nul 2>nul || call :vcvars
where cl >nul 2>nul || (
  echo cl was not found: run this from a "x64 Native Tools Command Prompt for VS", or install Visual Studio
  exit /b 1
)
rc /nologo /fo bin\mme.res ..\mme.rc || exit /b 1
cl /nologo /std:c11 /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I.. /I%SDL%\include /Fobin\ /Febin\mme-sdl.exe ^
  %SRC% bin\mme.res /link /LIBPATH:%SDL%\lib\x64 SDL2.lib shell32.lib ws2_32.lib gdi32.lib user32.lib ^
  /SUBSYSTEM:WINDOWS || exit /b 1
del bin\*.obj bin\mme.res >nul 2>nul
copy /y %SDL%\lib\x64\SDL2.dll bin\ >nul
goto done

:vcvars
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%I
if not defined VSDIR exit /b 0
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
exit /b 0

:done
if not exist bin\fonts mkdir bin\fonts
for %%F in ("%FONTS%\JetBrainsMonoNerdFontMono-*.ttf" "%FONTS%\JetBrainsMonoNerdFont-OFL.txt") do (
  if not exist "bin\fonts\%%~nxF" copy /y "%%F" bin\fonts\ >nul
)
echo built sdl2_port\bin\mme-sdl.exe (%CC%)
exit /b 0

:clean
if exist bin rmdir /s /q bin
if exist obj rmdir /s /q obj
exit /b 0
