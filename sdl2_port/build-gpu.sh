#!/bin/sh
# build-gpu.sh - mme-sdl-gpu: mme-sdl with the picture put on the screen by
# the GPU (SDL's Direct3D, OpenGL or Metal renderer) instead of the window's
# own bitmap; painting is the same. Built beside mme-sdl, never over it, so
# both can be used (and copied into $SDL_DEST, the mmc shell's usr/bin, too)
#
#   ./build-gpu.sh [zig | gcc | tcc] [32 | 64]   as build.sh; zig, 64 bit by default
#   ./build-gpu.sh single [32 | 64]              one stand-alone exe -> dist/mme-sdl-gpu-ARCH-windows.exe
#   ./build-gpu.sh cross                         Linux (OpenGL) and macOS (Metal) -> dist/mme-sdl-gpu-ARCH-*
#
# SDL_RENDER_DRIVER=direct3d11 (direct3d, direct3d12, opengl, software) picks
# another renderer when it runs; MME_FRAMESTATS=file writes how long frames took.

cd "$(dirname "$0")" || exit 1
case "$1" in
  xp) echo "build-gpu.sh: no GPU build for Windows XP (tcc's, SDL's Direct3D 9 at best): build.sh xp"; exit 2 ;;
  clean) echo "build-gpu.sh: build.sh clean removes both"; exit 2 ;;
esac
# exported, not "VAR=x exec": mmc's exec drops assignments put before it
MME_PROG=mme-sdl-gpu
MME_CFLAGS="${MME_CFLAGS-} -DMME_GPU"
export MME_PROG MME_CFLAGS
exec ./build.sh "$@"
