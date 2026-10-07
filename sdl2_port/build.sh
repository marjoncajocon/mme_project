#!/bin/sh
# build.sh - mme in a window of its own (SDL2); build.bat's twin for a
# bash-like shell: mmc (D:\mmc-shell), git-bash, Linux, macOS
#
# Windows (mmc, git-bash):
#   ./build.sh [zig | gcc | tcc] [32 | 64]   zig and 64 by default; 64 -> bin/, 32 -> bin32/
#   ./build.sh xp                            i386-win32-tcc, 32 bit SDL2: Windows XP
#   ./build.sh cross                         zig: Linux and macOS (x86_64, aarch64) into dist/
#   ./build.sh single [32 | 64]              zig: one stand-alone mme-sdl.exe, nothing beside it
#                                            (SDL2 linked in, the fonts inside) -> dist/
#   ./build.sh clean
# Linux, macOS:
#   ./build.sh                               mme-sdl with the system's SDL2 (sdl2-config), CC or cc
#   ./build.sh cross | clean
#
# On Windows a 64 bit build is copied into $SDL_DEST too (the mmc shell's
# usr/bin, with SDL2.dll and mme-fonts; SDL_DEST= to not copy). msvc: build.bat.
# Every .c file of mme is compiled as it is, but eterm.c and edraw.c: esdl.c
# stands in for them (it includes edraw.c itself); mos.c and tpty.c come in
# through emos.c and etpty.c.

cd "$(dirname "$0")" || exit 1

SDL=deps/SDL2-2.32.10
FONTS=../../mmc
[ -f "$FONTS/JetBrainsMonoNerdFontMono-Regular.ttf" ] || FONTS=/d/mmc-shell/usr/share/fonts
ZIG=zig
command -v zig >/dev/null 2>&1 || ZIG=/d/env/zig/zig.exe

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows*) WIN=1 ;;
  *) WIN=0 ;;
esac

SRC=
for f in ../*.c; do
  case "$f" in
    ../eterm.c|../edraw.c|../mos.c|../tpty.c) ;;
    *) SRC="$SRC $f" ;;
  esac
done
SRC="$SRC esdl.c emos.c etpty.c tfont.c tfont_ed.c tshape.c"

# the extension host script as C strings (../ehost_js.h): made again when node is there
if command -v node >/dev/null 2>&1 && node ../mme-exthost.js --emit-header > ../ehost_js.tmp; then
  mv -f ../ehost_js.tmp ../ehost_js.h
fi
rm -f ../ehost_js.tmp

# copies $1 to $2; a program that is running can't be overwritten on
# Windows, but it can be renamed: it keeps the old one until it restarts
put () {
  cp -f "$1" "$2" 2>/dev/null && return 0
  mv -f "$2" "$2.old$$" || return 1
  cp -f "$1" "$2" || return 1
  echo "  ($(basename "$2") is running: it keeps the old version until you restart it)"
}

fonts_into () {
  mkdir -p "$1"
  for f in "$FONTS"/JetBrainsMonoNerdFontMono-*.ttf "$FONTS"/JetBrainsMonoNerdFont-OFL.txt; do
    [ -f "$f" ] && [ ! -f "$1/$(basename "$f")" ] && cp -f "$f" "$1/"
  done
  return 0
}

cross () {
  CF="-std=c11 -O2 -Wall -Wextra -pedantic -I.. -I$SDL/include -D_REENTRANT"
  mkdir -p dist
  for a in x86_64 aarch64; do
    echo "$a-linux"
    mkdir -p obj/$a-linux
    $ZIG cc -target $a-linux-gnu.2.17 -shared -Wl,-soname,libSDL2-2.0.so.0 -o obj/$a-linux/libSDL2.so sdlstub.c || exit 1
    $ZIG cc $CF -s -target $a-linux-gnu.2.17 -o dist/mme-sdl-$a-linux $SRC -Lobj/$a-linux -lSDL2 -lm -lpthread || exit 1
    echo "$a-macos"
    mkdir -p obj/$a-macos
    $ZIG cc -target $a-macos -shared -Wl,-install_name,@rpath/libSDL2-2.0.0.dylib -o obj/$a-macos/libSDL2.dylib sdlstub.c || exit 1
    $ZIG cc $CF -target $a-macos -o dist/mme-sdl-$a-macos $SRC -Lobj/$a-macos -lSDL2 -lm -lpthread \
      -Wl,-rpath,@executable_path -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,/usr/local/lib || exit 1
  done
  rm -f dist/*.pdb
  fonts_into dist/mme-fonts
  echo "done, see sdl2_port/dist/ (mme-sdl-ARCH-linux, mme-sdl-ARCH-macos, mme-fonts)"
  exit 0
}

# a path as the resource compiler reads it from obj/single*/fonts.rc: /d/x is D:/x, a relative one from there
rc_path () {
  case "$1" in
    /[a-zA-Z]/*) p=${1#/}; echo "${p%%/*}:/${p#*/}" ;;
    /*) echo "$1" ;;
    *) echo "../../$1" ;;
  esac
}

# one program and nothing else: SDL2's static library linked in, the fonts
# (and their licence) as RCDATA resources, unpacked by esdl.c into mme-data
single () {
  if [ "$WIN" = 0 ]; then
    echo "single: Windows only (on Linux and macOS SDL2 comes with the system)"
    exit 2
  fi
  case "${1:-64}" in
    64) TARGET=x86_64-windows-gnu; SDLARCH=x86_64-w64-mingw32; NAME=x86_64 ;;
    32) TARGET=x86-windows-gnu; SDLARCH=i686-w64-mingw32; NAME=x86 ;;
    *) echo "the second word is 32 or 64, not $1"; exit 2 ;;
  esac
  if [ ! -f "$SDL/$SDLARCH/lib/libSDL2.a" ]; then
    echo "SDL2 is not in $SDL: see README.md"
    exit 1
  fi
  OBJ=obj/single${1:-64}
  mkdir -p $OBJ dist
  : > $OBJ/fonts.rc
  for f in "$FONTS"/JetBrainsMonoNerdFontMono-*.ttf "$FONTS"/JetBrainsMonoNerdFont-OFL.txt; do
    [ -f "$f" ] && echo "$(basename "$f") RCDATA \"$(rc_path "$f")\"" >> $OBJ/fonts.rc
  done
  if [ ! -s $OBJ/fonts.rc ]; then
    echo "no fonts in $FONTS (JetBrainsMonoNerdFontMono-*.ttf)"
    exit 1
  fi
  OUTX=dist/mme-sdl-$NAME-windows.exe
  rm -f $OUTX 2>/dev/null || mv -f $OUTX $OUTX.old$$
  $ZIG cc -std=c11 -O2 -s -Wall -Wextra -pedantic -target $TARGET -DMME_SINGLE -I.. -I$SDL/include \
    -o $OUTX $SRC ../mme.rc $OBJ/fonts.rc $SDL/$SDLARCH/lib/libSDL2.a \
    -lshell32 -lws2_32 -lgdi32 -luser32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid -ladvapi32 -lsetupapi \
    -Wl,--subsystem,windows || exit 1
  rm -f dist/*.pdb
  echo "built sdl2_port/$OUTX: stand-alone, no SDL2.dll or mme-fonts needed"
  exit 0
}

case "$1" in
  single) single "$2" ;;
  clean)
    rm -rf dist bin bin32 obj mme-sdl
    exit 0
    ;;
  cross) cross ;;
esac

if [ "$WIN" = 0 ]; then	# Linux, macOS: the system's SDL2, as the makefile does
  case "$1" in
    "") ;;
    *) echo "usage: ./build.sh [cross | clean]"; exit 2 ;;
  esac
  ${CC:-cc} -std=c11 -O2 -Wall -Wextra -pedantic -s -I.. $(sdl2-config --cflags) -o mme-sdl $SRC \
    $(sdl2-config --libs) -lm -lpthread || exit 1
  fonts_into bin/mme-fonts
  echo "built sdl2_port/mme-sdl (fonts: bin/mme-fonts; make install puts both in place)"
  exit 0
fi

if [ ! -f "$SDL/include/SDL.h" ]; then
  echo "SDL2 is not in $SDL: see README.md"
  exit 1
fi
SDL_DEST=${SDL_DEST-/d/mmc-shell/usr/bin}
CC=${1:-zig}
ARCH=${2:-64}
if [ "$CC" = xp ]; then
  CC=tcc
  ARCH=32
fi
case "$ARCH" in
  32) OUT=bin32; SDLARCH=i686-w64-mingw32; OBJ=obj/32 ;;
  64) OUT=bin; SDLARCH=x86_64-w64-mingw32; OBJ=obj/64 ;;
  *) echo "the second word is 32 or 64, not $ARCH"; exit 2 ;;
esac
mkdir -p $OUT $OBJ

# a mme-sdl.exe that is running cannot be written over, but it can be renamed
rm -f $OUT/mme-sdl.exe.old* 2>/dev/null
rm -f $OUT/mme-sdl.exe 2>/dev/null || mv -f $OUT/mme-sdl.exe $OUT/mme-sdl.exe.old$$

case "$CC" in
  zig)
    TARGET=x86_64-windows-gnu
    [ "$ARCH" = 32 ] && TARGET=x86-windows-gnu
    $ZIG cc -std=c11 -O2 -s -Wall -Wextra -pedantic -target $TARGET -I.. -I$SDL/include \
      -o $OUT/mme-sdl.exe $SRC ../mme.rc $SDL/$SDLARCH/lib/libSDL2.dll.a -lshell32 -lws2_32 -lgdi32 \
      -Wl,--subsystem,windows || exit 1
    rm -f $OUT/mme-sdl.pdb
    ;;
  gcc)
    if [ "$ARCH" = 64 ]; then
      GCC=gcc
      command -v gcc >/dev/null 2>&1 || GCC=/d/env/mingw/MinGW/bin/gcc.exe
      WINDRES=windres
      command -v windres >/dev/null 2>&1 || WINDRES=/d/env/mingw/MinGW/bin/windres.exe
    else
      GCC=${GCC32:-i686-w64-mingw32-gcc}
      WINDRES=$(dirname "$GCC")/windres.exe
      [ -f "$WINDRES" ] || WINDRES=i686-w64-mingw32-windres
      if ! command -v "$GCC" >/dev/null 2>&1 && [ ! -f "$GCC" ]; then
        echo "32 bit gcc: no i686-w64-mingw32-gcc on the PATH. Install a 32 bit MinGW-w64"
        echo "(msvcrt for Windows XP) and put it on the PATH, or set GCC32=path/to/gcc.exe"
        exit 1
      fi
    fi
    (cd .. && "$WINDRES" mme.rc -O coff -o sdl2_port/$OBJ/mme.res.o) || exit 1
    "$GCC" -std=c11 -O2 -s -Wall -Wextra -pedantic -I.. -I$SDL/include -o $OUT/mme-sdl.exe $SRC $OBJ/mme.res.o \
      $SDL/$SDLARCH/lib/libSDL2.dll.a -lshell32 -lws2_32 -lgdi32 -lm -mwindows || exit 1
    ;;
  tcc)
    # tcc 0.9.27 lacks some Windows headers and import libraries: tcc/ has the
    # headers mme needs, and the .def files are made from the dlls (tcc -impdef),
    # from the 32 bit ones (SysWOW64 on a 64 bit Windows) for a 32 bit build
    SYSROOT=${SYSTEMROOT:-${SystemRoot:-C:/Windows}}
    if [ "$ARCH" = 64 ]; then
      TCC=tcc
      command -v tcc >/dev/null 2>&1 || TCC=/d/env/tcc/tcc.exe
      SYSDLL=$SYSROOT/System32
    else
      TCC=i386-win32-tcc
      command -v i386-win32-tcc >/dev/null 2>&1 || TCC=/d/env/tcc/i386-win32-tcc.exe
      SYSDLL=$SYSROOT/SysWOW64
      [ -f "$SYSDLL/kernel32.dll" ] || SYSDLL=$SYSROOT/System32
    fi
    [ -f $OBJ/SDL2.def ] || "$TCC" -impdef $SDL/$SDLARCH/bin/SDL2.dll -o $OBJ/SDL2.def || exit 1
    for d in kernel32 user32 gdi32 shell32 ws2_32 advapi32; do
      [ -f $OBJ/$d.def ] || "$TCC" -impdef "$SYSDLL/$d.dll" -o $OBJ/$d.def || exit 1
    done
    "$TCC" -O2 -Itcc -I.. -I$SDL/include -DSDLCALL= -o $OUT/mme-sdl.exe $SRC \
      $OBJ/SDL2.def $OBJ/kernel32.def $OBJ/user32.def $OBJ/gdi32.def $OBJ/shell32.def $OBJ/ws2_32.def \
      $OBJ/advapi32.def -Wl,-subsystem=gui || exit 1
    cp -f ../mme.ico $OUT/
    ;;
  *)
    echo "usage: ./build.sh [zig | gcc | tcc | xp | cross | clean] [32 | 64]"
    exit 2
    ;;
esac
cp -f $SDL/$SDLARCH/bin/SDL2.dll $OUT/

[ -d $OUT/fonts ] && [ ! -d $OUT/mme-fonts ] && mv $OUT/fonts $OUT/mme-fonts
fonts_into $OUT/mme-fonts
echo "built sdl2_port/$OUT/mme-sdl.exe ($CC, $ARCH bit)"
[ "$ARCH" = 64 ] && [ -n "$SDL_DEST" ] && [ -d "$SDL_DEST" ] || exit 0
rm -f "$SDL_DEST"/mme-sdl.exe.old* 2>/dev/null
put $OUT/mme-sdl.exe "$SDL_DEST/mme-sdl.exe" || exit 1
[ -f "$SDL_DEST/SDL2.dll" ] || cp -f $OUT/SDL2.dll "$SDL_DEST/"
[ -f $OUT/mme.ico ] && cp -f $OUT/mme.ico "$SDL_DEST/"
mkdir -p "$SDL_DEST/mme-fonts"
for f in $OUT/mme-fonts/*; do
  [ -f "$SDL_DEST/mme-fonts/$(basename "$f")" ] || cp -f "$f" "$SDL_DEST/mme-fonts/"
done
echo "copied to $SDL_DEST/mme-sdl.exe (with SDL2.dll and mme-fonts)"
exit 0
