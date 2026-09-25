# makefile - mme in a window of its own (SDL2) on Linux and macOS
# (on Windows use build.bat: zig, gcc, tcc or msvc)
#
#   make                    mme-sdl, with SDL2 from the system (sdl2-config)
#   make CC=clang           any C11 compiler
#   make install            copy mme-sdl into $(PREFIX)/bin
#
# SDL2: apt install libsdl2-dev, dnf install SDL2-devel, pacman -S sdl2,
# brew install sdl2. Every .c file of mme is compiled as it is, but eterm.c
# and edraw.c: esdl.c stands in for them.

CC= cc
CFLAGS= -std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS= -s
PREFIX= /usr/local
SDL2_CONFIG= sdl2-config

SRC= $(filter-out ../eterm.c ../edraw.c ../mos.c ../tpty.c, $(wildcard ../*.c)) esdl.c emos.c etpty.c tfont.c tshape.c

all: mme-sdl

mme-sdl: $(SRC) ../mme.h ../mmc.h ../mterm.h ../edraw.c
	$(CC) $(CFLAGS) $(LDFLAGS) -I.. $$($(SDL2_CONFIG) --cflags) -o mme-sdl $(SRC) $$($(SDL2_CONFIG) --libs) -lm -lpthread

install: mme-sdl
	mkdir -p $(PREFIX)/bin
	cp mme-sdl $(PREFIX)/bin/

clean:
	rm -f mme-sdl
