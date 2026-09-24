@echo off
rem build.bat - builds the regression suite's own programs
rem
rem   hx.exe                the ConPTY harness the scenarios drive mme with
rem   browser\rundll32.exe  a stand-in for Windows' rundll32, so a scenario
rem                         that opens a URL proves which URL, with no window
rem
rem hx.c uses mmc's terminal core (a Grid a program is driven into and read
rem back from), so the mmc checkout has to be beside this one. Point MMC at it
rem if it is somewhere else.

setlocal
cd /d "%~dp0"

set ZIG=zig
where zig >nul 2>nul || set ZIG=D:\env\zig\zig.exe

if not defined MMC set MMC=..\..\mmc
if not exist "%MMC%\mterm.h" (
  echo cannot find mmc at "%MMC%" - set MMC to the checkout
  exit /b 1
)

%ZIG% cc -std=c11 -O2 -I"%MMC%" -o hx.exe hx.c ^
  "%MMC%\tpty.c" "%MMC%\tvt.c" "%MMC%\tgrid.c" ^
  "%MMC%\mutil.c" "%MMC%\mos.c" "%MMC%\mpath.c" || exit /b 1
echo built hx.exe

%ZIG% cc -std=c11 -O2 -o browser\rundll32.exe browser\rundll32.c || exit /b 1
echo built browser\rundll32.exe

if exist *.pdb del *.pdb >nul 2>nul
if exist browser\*.pdb del browser\*.pdb >nul 2>nul
exit /b 0
