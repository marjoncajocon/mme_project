@echo off
rem build.bat - builds mme with zig (https://ziglang.org), used as a C compiler
rem
rem   build                 mme.exe for this PC, copied into %MME_DEST%
rem   build cross           every platform, into dist\
rem   build install DIR     copy mme.exe into DIR (an mmc folder: DIR\usr\bin)
rem   build clean

setlocal
cd /d "%~dp0"

set ZIG=zig
where zig >nul 2>nul || set ZIG=D:\env\zig\zig.exe

rem where every build is copied, to try it at once (set MME_DEST= to not copy)
if not defined MME_DEST set MME_DEST=D:\mmc-shell\usr\bin

set CFLAGS=-std=c11 -O2 -s -Wall -Wextra -pedantic
set BASE=mutil.c mpath.c mos.c
set TSRC=tpty.c tvt.c tgrid.c
set SRC=mme.c ethread.c ejson.c econfig.c elsp.c etheme.c ebuf.c eterm.c edraw.c emenu.c eside.c esearch.c esearched.c emdiff.c egit.c esyntax.c epanel.c ekeys.c esnip.c eext.c egitlog.c equick.c emerge.c eregex.c esettings.c ewelcome.c efiles.c eemmet.c edebug.c etask.c eout.c ehistory.c emd.c ehex.c eimage.c eworkspace.c eimport.c evscode.c etest.c eonig.c etm.c eeditorconfig.c echat.c evim.c enb.c %TSRC% %BASE%

if "%1"=="" goto native
if "%1"=="cross" goto cross
if "%1"=="install" goto install
if "%1"=="clean" goto clean
echo usage: build [cross ^| install DIR ^| clean]
exit /b 2

:native
%ZIG% cc %CFLAGS% -target x86_64-windows-gnu -o mme.exe %SRC% mme.rc -lshell32 -lws2_32 || exit /b 1
if exist mme.pdb del mme.pdb
echo built mme.exe
if "%MME_DEST%"=="" exit /b 0
if not exist "%MME_DEST%" exit /b 0
for %%F in ("%MME_DEST%\mme.exe.old*") do del "%%F" >nul 2>nul
call :put mme.exe "%MME_DEST%\mme.exe" || exit /b 1
echo copied to %MME_DEST%\mme.exe
exit /b 0

:cross
if not exist dist mkdir dist
for %%T in (x86_64 aarch64) do (
  echo %%T-windows
  %ZIG% cc %CFLAGS% -target %%T-windows-gnu -o dist\mme-%%T-windows.exe %SRC% mme.rc -lshell32 -lws2_32 || exit /b 1
  echo %%T-linux
  rem static musl: the same program runs on any Linux, and on Android (Termux)
  %ZIG% cc %CFLAGS% -target %%T-linux-musl -static -o dist\mme-%%T-linux %SRC% || exit /b 1
  echo %%T-macos
  %ZIG% cc %CFLAGS% -target %%T-macos -o dist\mme-%%T-macos %SRC% || exit /b 1
)
echo arm-linux (older 32 bit Android phones)
%ZIG% cc %CFLAGS% -target arm-linux-musleabihf -static -o dist\mme-arm-linux %SRC% || exit /b 1
if exist dist\*.pdb del dist\*.pdb
echo done, see dist\
exit /b 0

:install
if "%~2"=="" (
  echo usage: build install DIR
  exit /b 2
)
if not exist mme.exe call "%~f0" || exit /b 1
if not exist "%~2\usr\bin" mkdir "%~2\usr\bin"
call :put mme.exe "%~2\usr\bin\mme.exe" || exit /b 1
echo installed mme.exe in "%~2\usr\bin"
exit /b 0

:clean
if exist mme.exe del mme.exe
if exist mme.pdb del mme.pdb
if exist dist rmdir /s /q dist
exit /b 0

rem copies %1 to %2; a program that is running can't be overwritten on
rem Windows, but it can be renamed: it keeps the old one until it restarts
:put
copy /y %1 %2 >nul 2>nul && exit /b 0
ren %2 "%~nx2.old%RANDOM%" || exit /b 1
copy /y %1 %2 >nul || exit /b 1
echo   (%~nx2 is running: it keeps the old version until you restart it)
exit /b 0
