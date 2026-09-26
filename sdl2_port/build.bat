@echo off
rem build.bat - mme in a window of its own (SDL2): sdl2_port\bin\mme-sdl.exe
rem
rem   build [compiler] [32 | 64]
rem
rem   zig        zig cc (the default)           64: Windows 7 and later (x64)
rem                                             32: Windows 7 and later (x86)
rem   gcc        MinGW-w64 gcc                  64: gcc on the PATH
rem                                             32: a 32 bit MinGW-w64: i686-w64-mingw32-gcc
rem                                                 on the PATH, or GCC32=path\to\gcc.exe
rem   tcc        Tiny C Compiler 0.9.27         64: tcc.exe
rem                                             32: i386-win32-tcc.exe, msvcrt.dll:
rem                                                 Windows XP and later
rem   xp         the same as "tcc 32"
rem   msvc       Visual C++ (cl): from a "Native Tools" prompt, or found with vswhere
rem   cross      zig: Linux and macOS (x86_64, aarch64) into dist\, see below
rem   clean
rem
rem 64 bit goes into bin\, 32 bit into bin32\: each is the program on its own,
rem mme-sdl.exe with its SDL2.dll, its mme-fonts (JetBrains Mono Nerd Font, from
rem mmc) and, once it runs, its own mme-data. A 64 bit build is copied into
rem %SDL_DEST% too (the mmc shell's usr\bin: "mme-sdl ." there; it shares mme's
rem mme-data); set SDL_DEST= to not copy.
rem
rem Every .c file of mme is compiled as it is, but eterm.c and edraw.c: esdl.c
rem stands in for them (it includes edraw.c itself). mos.c comes in through
rem emos.c (console programs started without a console window) and tpty.c
rem through etpty.c (Vista's calls looked up at run time, so XP starts it).
rem SDL2 2.32.10 comes from deps\ (see README.md).
rem
rem "build cross" makes mme-sdl for Linux and macOS here, without their SDL2:
rem it is linked to sdlstub.c built as a library named as theirs is, and it
rem loads the system's own where it runs: libSDL2-2.0.so.0 on Linux (glibc
rem 2.17 and later; apt install libsdl2-2.0-0, dnf install SDL2, pacman -S
rem sdl2), libSDL2-2.0.0.dylib on macOS (brew install sdl2, or the dylib next
rem to mme-sdl). dist\mme-fonts goes next to them.

setlocal enabledelayedexpansion
cd /d "%~dp0"

set SDL=deps\SDL2-2.32.10
if not exist "%SDL%\include\SDL.h" (
  echo SDL2 is not in %SDL%: see README.md
  exit /b 1
)
if not defined SDL_DEST set SDL_DEST=D:\mmc-shell\usr\bin
set FONTS=..\..\mmc
if not exist "%FONTS%\JetBrainsMonoNerdFontMono-Regular.ttf" set FONTS=D:\mmc-shell\usr\share\fonts

set CC=%1
set ARCH=%2
if "%CC%"=="" set CC=zig
if "%CC%"=="clean" goto clean
if "%CC%"=="xp" (
  set CC=tcc
  set ARCH=32
)
if "%ARCH%"=="" set ARCH=64
if not "%ARCH%"=="32" if not "%ARCH%"=="64" (
  echo the second word is 32 or 64, not %ARCH%
  exit /b 2
)
if "%ARCH%"=="64" (
  set OUT=bin
  set SDLARCH=x86_64-w64-mingw32
  set SDLVC=x64
  set OBJ=obj\64
) else (
  set OUT=bin32
  set SDLARCH=i686-w64-mingw32
  set SDLVC=x86
  set OBJ=obj\32
)

set SRC=
for %%F in (..\*.c) do (
  set F=%%~nxF
  if /i not "!F!"=="eterm.c" if /i not "!F!"=="edraw.c" if /i not "!F!"=="mos.c" if /i not "!F!"=="tpty.c" set SRC=!SRC! %%F
)
set SRC=%SRC% esdl.c emos.c etpty.c tfont.c tfont_ed.c tshape.c
if not exist %OUT% mkdir %OUT%
if not exist %OBJ% mkdir %OBJ%

if "%CC%"=="cross" goto cross
if "%CC%"=="zig" goto zig
if "%CC%"=="gcc" goto gcc
if "%CC%"=="tcc" goto tcc
if "%CC%"=="msvc" goto msvc
echo usage: build [zig ^| gcc ^| tcc ^| xp ^| msvc ^| cross ^| clean] [32 ^| 64]
exit /b 2

:zig
set ZIG=zig
where zig >nul 2>nul || set ZIG=D:\env\zig\zig.exe
set TARGET=x86_64-windows-gnu
if "%ARCH%"=="32" set TARGET=x86-windows-gnu
call :make_room
%ZIG% cc -std=c11 -O2 -s -Wall -Wextra -pedantic -target %TARGET% -I.. -I%SDL%\include ^
  -o %OUT%\mme-sdl.exe %SRC% ..\mme.rc %SDL%\%SDLARCH%\lib\libSDL2.dll.a -lshell32 -lws2_32 -lgdi32 ^
  -Wl,--subsystem,windows || exit /b 1
if exist %OUT%\mme-sdl.pdb del %OUT%\mme-sdl.pdb
copy /y %SDL%\%SDLARCH%\bin\SDL2.dll %OUT%\ >nul
goto done

:cross
set ZIG=zig
where zig >nul 2>nul || set ZIG=D:\env\zig\zig.exe
set CF=-std=c11 -O2 -Wall -Wextra -pedantic -I.. -I%SDL%\include -D_REENTRANT
if not exist dist mkdir dist
for %%A in (x86_64 aarch64) do (
  echo %%A-linux
  if not exist obj\%%A-linux mkdir obj\%%A-linux
  %ZIG% cc -target %%A-linux-gnu.2.17 -shared -Wl,-soname,libSDL2-2.0.so.0 -o obj\%%A-linux\libSDL2.so sdlstub.c || exit /b 1
  %ZIG% cc %CF% -s -target %%A-linux-gnu.2.17 -o dist\mme-sdl-%%A-linux %SRC% -Lobj\%%A-linux -lSDL2 -lm -lpthread || exit /b 1
  echo %%A-macos
  if not exist obj\%%A-macos mkdir obj\%%A-macos
  %ZIG% cc -target %%A-macos -shared -Wl,-install_name,@rpath/libSDL2-2.0.0.dylib -o obj\%%A-macos\libSDL2.dylib sdlstub.c || exit /b 1
  %ZIG% cc %CF% -target %%A-macos -o dist\mme-sdl-%%A-macos %SRC% -Lobj\%%A-macos -lSDL2 -lm -lpthread ^
    -Wl,-rpath,@executable_path -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib || exit /b 1
)
if exist dist\*.pdb del dist\*.pdb
if not exist dist\mme-fonts mkdir dist\mme-fonts
for %%F in ("%FONTS%\JetBrainsMonoNerdFontMono-*.ttf" "%FONTS%\JetBrainsMonoNerdFont-OFL.txt") do (
  if not exist "dist\mme-fonts\%%~nxF" copy /y "%%F" dist\mme-fonts\ >nul
)
echo done, see sdl2_port\dist\ (mme-sdl-ARCH-linux, mme-sdl-ARCH-macos, mme-fonts)
exit /b 0

:gcc
if "%ARCH%"=="64" (
  set GCC=gcc
  where gcc >nul 2>nul || set GCC=D:\env\mingw\MinGW\bin\gcc.exe
  set WINDRES=windres
  where windres >nul 2>nul || set WINDRES=D:\env\mingw\MinGW\bin\windres.exe
) else (
  if defined GCC32 (set GCC=%GCC32%) else set GCC=i686-w64-mingw32-gcc
  for %%G in ("!GCC!") do set WINDRES=%%~dpGwindres.exe
  if not exist "!WINDRES!" set WINDRES=i686-w64-mingw32-windres
  where !GCC! >nul 2>nul || if not exist "!GCC!" (
    echo 32 bit gcc: no i686-w64-mingw32-gcc on the PATH. Install a 32 bit MinGW-w64
    echo ^(msvcrt for Windows XP^) and put it on the PATH, or set GCC32=path\to\gcc.exe
    exit /b 1
  )
)
pushd ..
"%WINDRES%" mme.rc -O coff -o sdl2_port\%OBJ%\mme.res.o || (popd & exit /b 1)
popd
call :make_room
"%GCC%" -std=c11 -O2 -s -Wall -Wextra -pedantic -I.. -I%SDL%\include -o %OUT%\mme-sdl.exe %SRC% %OBJ%\mme.res.o ^
  %SDL%\%SDLARCH%\lib\libSDL2.dll.a -lshell32 -lws2_32 -lgdi32 -lm -mwindows || exit /b 1
copy /y %SDL%\%SDLARCH%\bin\SDL2.dll %OUT%\ >nul
goto done

:tcc
rem tcc 0.9.27 lacks some Windows headers and import libraries: tcc\ has the
rem headers mme needs, and the .def files are made from the dlls (tcc -impdef),
rem from the 32 bit ones (SysWOW64 on a 64 bit Windows) for a 32 bit build
if "%ARCH%"=="64" (
  set TCC=tcc
  where tcc >nul 2>nul || set TCC=D:\env\tcc\tcc.exe
  set SYSDLL=%SystemRoot%\System32
) else (
  set TCC=i386-win32-tcc
  where i386-win32-tcc >nul 2>nul || set TCC=D:\env\tcc\i386-win32-tcc.exe
  set SYSDLL=%SystemRoot%\SysWOW64
  if not exist "%SystemRoot%\SysWOW64\kernel32.dll" set SYSDLL=%SystemRoot%\System32
)
if not exist %OBJ%\SDL2.def "%TCC%" -impdef %SDL%\%SDLARCH%\bin\SDL2.dll -o %OBJ%\SDL2.def || exit /b 1
for %%D in (kernel32 user32 gdi32 shell32 ws2_32 advapi32) do (
  if not exist %OBJ%\%%D.def "%TCC%" -impdef %SYSDLL%\%%D.dll -o %OBJ%\%%D.def || exit /b 1
)
call :make_room
"%TCC%" -O2 -Itcc -I.. -I%SDL%\include -DSDLCALL= -o %OUT%\mme-sdl.exe %SRC% ^
  %OBJ%\SDL2.def %OBJ%\kernel32.def %OBJ%\user32.def %OBJ%\gdi32.def %OBJ%\shell32.def %OBJ%\ws2_32.def ^
  %OBJ%\advapi32.def -Wl,-subsystem=gui || exit /b 1
copy /y %SDL%\%SDLARCH%\bin\SDL2.dll %OUT%\ >nul
copy /y ..\mme.ico %OUT%\ >nul
goto done

:msvc
where cl >nul 2>nul || call :vcvars
where cl >nul 2>nul || (
  echo cl was not found: run this from a "Native Tools Command Prompt for VS", or install Visual Studio
  exit /b 1
)
rc /nologo /fo %OBJ%\mme.res ..\mme.rc || exit /b 1
call :make_room
cl /nologo /std:c11 /O2 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /I.. /I%SDL%\include /Fo%OBJ%\ /Fe%OUT%\mme-sdl.exe ^
  %SRC% %OBJ%\mme.res /link /LIBPATH:%SDL%\lib\%SDLVC% SDL2.lib shell32.lib ws2_32.lib gdi32.lib user32.lib ^
  /SUBSYSTEM:WINDOWS || exit /b 1
copy /y %SDL%\lib\%SDLVC%\SDL2.dll %OUT%\ >nul
goto done

:vcvars
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if not exist "%VSWHERE%" exit /b 0
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%I
if not defined VSDIR exit /b 0
if "%ARCH%"=="64" (call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul) else call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul
exit /b 0

:make_room
rem a mme-sdl.exe that is running cannot be written over, but it can be renamed
for %%F in (%OUT%\mme-sdl.exe.old*) do del "%%F" >nul 2>nul
if exist %OUT%\mme-sdl.exe del %OUT%\mme-sdl.exe >nul 2>nul
if exist %OUT%\mme-sdl.exe ren %OUT%\mme-sdl.exe mme-sdl.exe.old%RANDOM%
exit /b 0

:done
if exist %OUT%\fonts if not exist %OUT%\mme-fonts ren %OUT%\fonts mme-fonts
if not exist %OUT%\mme-fonts mkdir %OUT%\mme-fonts
for %%F in ("%FONTS%\JetBrainsMonoNerdFontMono-*.ttf" "%FONTS%\JetBrainsMonoNerdFont-OFL.txt") do (
  if not exist "%OUT%\mme-fonts\%%~nxF" copy /y "%%F" %OUT%\mme-fonts\ >nul
)
echo built sdl2_port\%OUT%\mme-sdl.exe (%CC%, %ARCH% bit)
if not "%ARCH%"=="64" exit /b 0
if "%SDL_DEST%"=="" exit /b 0
if not exist "%SDL_DEST%" exit /b 0
for %%F in ("%SDL_DEST%\mme-sdl.exe.old*") do del "%%F" >nul 2>nul
call :put %OUT%\mme-sdl.exe "%SDL_DEST%\mme-sdl.exe" || exit /b 1
if not exist "%SDL_DEST%\SDL2.dll" copy /y %OUT%\SDL2.dll "%SDL_DEST%\" >nul
if exist %OUT%\mme.ico copy /y %OUT%\mme.ico "%SDL_DEST%\" >nul
if not exist "%SDL_DEST%\mme-fonts" mkdir "%SDL_DEST%\mme-fonts"
for %%F in (%OUT%\mme-fonts\*) do (
  if not exist "%SDL_DEST%\mme-fonts\%%~nxF" copy /y "%%F" "%SDL_DEST%\mme-fonts\" >nul
)
echo copied to %SDL_DEST%\mme-sdl.exe (with SDL2.dll and mme-fonts)
exit /b 0

rem copies %1 to %2; a program that is running can't be overwritten on
rem Windows, but it can be renamed: it keeps the old one until it restarts
:put
copy /y %1 %2 >nul 2>nul && exit /b 0
ren %2 "%~nx2.old%RANDOM%" || exit /b 1
copy /y %1 %2 >nul || exit /b 1
echo   (%~nx2 is running: it keeps the old version until you restart it)
exit /b 0

:clean
if exist dist rmdir /s /q dist
if exist bin rmdir /s /q bin
if exist bin32 rmdir /s /q bin32
if exist obj rmdir /s /q obj
exit /b 0
