#!/bin/sh
# build.sh - builds mme with zig (https://ziglang.org), used as a C compiler;
# build.bat's twin for a bash-like shell: mmc (D:\mmc-shell), git-bash, Linux, macOS
#
#   ./build.sh                 mme for this PC (Windows: mme.exe, copied into $MME_DEST)
#   ./build.sh cross           every platform, into dist/
#   ./build.sh install DIR     copy mme into DIR (an mmc folder: DIR/usr/bin)
#   ./build.sh clean
#
# CC picks another C11 compiler for the native build (CC="gcc" ./build.sh).

cd "$(dirname "$0")" || exit 1

ZIG=zig
command -v zig >/dev/null 2>&1 || ZIG=/d/env/zig/zig.exe

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows*) WIN=1 ;;
  *) WIN=0 ;;
esac

# where every build is copied, to try it at once (MME_DEST= to not copy)
if [ "$WIN" = 1 ]; then
  MME_DEST=${MME_DEST-/d/mmc-shell/usr/bin}
  EXE=mme.exe
else
  MME_DEST=${MME_DEST-}
  EXE=mme
fi

CFLAGS="-std=c11 -O2 -s -Wall -Wextra -pedantic"
BASE="mutil.c mpath.c mos.c"
TSRC="tpty.c tvt.c tgrid.c"
SRC="mme.c ethread.c ejson.c econfig.c elsp.c etheme.c ebuf.c eterm.c edraw.c emenu.c eside.c esearch.c esearched.c emdiff.c egit.c esyntax.c epanel.c ekeys.c esnip.c eext.c egitlog.c equick.c emerge.c eregex.c esettings.c ewelcome.c efiles.c eemmet.c edebug.c etask.c eout.c ehistory.c emd.c ehex.c eimage.c eworkspace.c eimport.c evscode.c etest.c eonig.c etm.c eeditorconfig.c echat.c evim.c enb.c ewindows.c egithub.c eremote.c esync.c eports.c eaccess.c ehost.c etree.c eicons.c esvg.c erefactor.c $TSRC $BASE"

# copies $1 to $2; a program that is running can't be overwritten on
# Windows, but it can be renamed: it keeps the old one until it restarts
put () {
  cp -f "$1" "$2" 2>/dev/null && return 0
  mv -f "$2" "$2.old$$" || return 1
  cp -f "$1" "$2" || return 1
  echo "  ($(basename "$2") is running: it keeps the old version until you restart it)"
}

# the extension host script, as C strings for ehost.c: made again when node is there (else the one checked in)
emit_header () {
  if command -v node >/dev/null 2>&1 && node mme-exthost.js --emit-header > ehost_js.tmp; then
    mv -f ehost_js.tmp ehost_js.h
  fi
  rm -f ehost_js.tmp
}

native () {
  emit_header
  if [ "$WIN" = 1 ]; then
    ${CC:-$ZIG cc} $CFLAGS -target x86_64-windows-gnu -o mme.exe $SRC mme.rc -lshell32 -lws2_32 || exit 1
  else
    ${CC:-$ZIG cc} $CFLAGS -o mme $SRC -lm -lpthread || exit 1
  fi
  rm -f mme.pdb
  echo "built $EXE"
  [ -n "$MME_DEST" ] && [ -d "$MME_DEST" ] || return 0
  rm -f "$MME_DEST/$EXE".old* 2>/dev/null
  put "$EXE" "$MME_DEST/$EXE" || exit 1
  echo "copied to $MME_DEST/$EXE"
}

cross () {
  mkdir -p dist
  for t in x86_64 aarch64; do
    echo "$t-windows"
    $ZIG cc $CFLAGS -target $t-windows-gnu -o dist/mme-$t-windows.exe $SRC mme.rc -lshell32 -lws2_32 || exit 1
    echo "$t-linux"
    # static musl: the same program runs on any Linux, and on Android (Termux)
    $ZIG cc $CFLAGS -target $t-linux-musl -static -o dist/mme-$t-linux $SRC || exit 1
    echo "$t-macos"
    $ZIG cc $CFLAGS -target $t-macos -o dist/mme-$t-macos $SRC || exit 1
  done
  echo "arm-linux (older 32 bit Android phones)"
  $ZIG cc $CFLAGS -target arm-linux-musleabihf -static -o dist/mme-arm-linux $SRC || exit 1
  rm -f dist/*.pdb
  echo "done, see dist/"
  # Remote-SSH copies these to a host that has no mme: next to the installed one
  [ -n "$MME_DEST" ] && [ -d "$MME_DEST" ] || return 0
  mkdir -p "$MME_DEST/mme-server"
  cp -f dist/mme-*-linux dist/mme-*-macos "$MME_DEST/mme-server/" || exit 1
  echo "copied the Linux and macOS builds to $MME_DEST/mme-server (Remote-SSH)"
}

case "$1" in
  "") native ;;
  cross) cross ;;
  install)
    if [ -z "$2" ]; then
      echo "usage: ./build.sh install DIR"
      exit 2
    fi
    if [ ! -f "$EXE" ]; then
      MME_DEST=
      native
    fi
    mkdir -p "$2/usr/bin" || exit 1
    put "$EXE" "$2/usr/bin/$EXE" || exit 1
    echo "installed $EXE in $2/usr/bin"
    ;;
  clean) rm -rf mme mme.exe mme.pdb dist ;;
  *)
    echo "usage: ./build.sh [cross | install DIR | clean]"
    exit 2
    ;;
esac
exit 0
