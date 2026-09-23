# makefile - builds mme on Linux and macOS (on Windows use build.bat)
#
#   make                    mme, the editor
#   make CC=gcc             any C11 compiler works, zig is only the default
#   make cross              every platform, into dist/ (needs zig)
#   make install            copy mme into $(PREFIX)/bin

CC= zig cc
CFLAGS= -std=c11 -O2 -Wall -Wextra -pedantic
LDFLAGS= -s
PREFIX= /usr/local
ZIG= zig

BASE= mutil.c mpath.c mos.c
TSRC= tpty.c tvt.c tgrid.c
SRC= mme.c ethread.c ejson.c econfig.c elsp.c etheme.c ebuf.c eterm.c edraw.c emenu.c eside.c esearch.c egit.c esyntax.c epanel.c ekeys.c esnip.c eext.c egitlog.c equick.c emerge.c eregex.c esettings.c ewelcome.c efiles.c eemmet.c edebug.c etask.c eout.c ehistory.c emd.c ehex.c eimage.c eworkspace.c eimport.c evscode.c etest.c eonig.c etm.c $(TSRC) $(BASE)

all: mme

mme: $(SRC) mme.h mmc.h mterm.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o mme $(SRC) -lm -lpthread

# static musl: one program that runs on any Linux, Android (Termux) too
cross: $(SRC) mme.h mmc.h
	mkdir -p dist
	for t in x86_64 aarch64; do \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-windows-gnu -o dist/mme-$$t-windows.exe $(SRC) mme.rc -lshell32 -lws2_32 || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-linux-musl -static -o dist/mme-$$t-linux $(SRC) -lm || exit 1; \
	  $(ZIG) cc $(CFLAGS) -s -target $$t-macos -o dist/mme-$$t-macos $(SRC) -lm || exit 1; \
	done
	$(ZIG) cc $(CFLAGS) -s -target arm-linux-musleabihf -static -o dist/mme-arm-linux $(SRC) -lm
	rm -f dist/*.pdb

install: mme
	mkdir -p $(PREFIX)/bin
	cp mme $(PREFIX)/bin/

clean:
	rm -rf mme mme.exe *.pdb dist

.PHONY: all cross install clean
