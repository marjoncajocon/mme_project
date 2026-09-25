# mme in its own window (SDL2)

`mme-sdl` is mme without a terminal: the editor paints its picture straight
into a window of its own, once. In a terminal the same picture is written as
escape sequences, read by the pseudo console (ConPTY's OpenConsole on
Windows), written again, read by the terminal and painted there: three
programs instead of one.

Everything else is mme as it is: every `.c` file of the project is compiled
unchanged, but `eterm.c` and `edraw.c`. `esdl.c` stands in for them: it
includes `edraw.c` itself (so its grid of cells and its drawing calls are the
same) and paints that grid in the window instead of sending it; the keys, the
mouse, the clipboard and the window's size come from SDL.

It is a program of its own: `bin\` has `mme-sdl.exe`, `SDL2.dll` and its
`mme-fonts`, and `mme-sdl.exe` keeps its settings in the `mme-data` next to it
(portable, like `mme.exe`'s). Copy `bin\` anywhere. A 64 bit build is also
copied into the mmc shell's `usr\bin` (`SDL_DEST`), so `mme-sdl .` opens the
folder you are in; there it shares `mme.exe`'s `mme-data`, its settings too.

The painting is mmc-term's: `tfont.c`, `tshape.c` and `stb_truetype.h` are
copied from mmc as they are (the fonts: stb_truetype, and GDI's ClearType on
Windows; fallback fonts for missing glyphs), and `esdl.c` has `tdraw.c`'s
blending, box drawing, block elements and underlines. So it looks like mme in
mmc-term, with JetBrains Mono Nerd Font from mmc's `usr/share/fonts` (or the
`mme-fonts` folder next to the program, or the system's).

| | mme in mmc-term | mme-sdl |
| --- | --- | --- |
| programs | mme + OpenConsole + mmc-term | mme-sdl |
| memory (a folder open, Copilot left out) | 58 MB | 41 MB |
| idle CPU | 1.2-1.7 % of a core | 1.0 % |
| typing: CPU per 1000 keys (one every 25 ms) | 1.5-2.0 s | 1.2-1.7 s |

The window is shown through SDL's window surface (on Windows GDI's own
bitmap): a key typed sends the rows it changed to the screen, not the whole
window (macOS keeps SDL's renderer, for Retina's pixels).

## Building

SDL2 2.32.10 is not in the repository: unpack its development packages from
<https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10> into `deps`
(`SDL2-devel-2.32.10-mingw.zip` for zig, gcc and tcc,
`SDL2-devel-2.32.10-VC.zip` for Visual C++), so that
`deps\SDL2-2.32.10\include\SDL.h` is there.

```
build [compiler] [32 | 64]      64 when not said

build zig        zig cc (the default)     Windows 7 and later (x64, or x86 with 32)
build gcc        MinGW-w64 gcc            64: gcc on the PATH; 32: a 32 bit MinGW-w64
                                          (i686-w64-mingw32-gcc, or GCC32=path\to\gcc.exe)
build tcc        Tiny C Compiler 0.9.27   64: tcc.exe; 32: i386-win32-tcc.exe
build xp         the same as "tcc 32"     Windows XP and later
build msvc       Visual C++               from a "Native Tools" prompt, or found with vswhere
build clean
```

64 bit goes into `bin\`, 32 bit into `bin32\`. Each is the program on its
own: `mme-sdl.exe` with its `SDL2.dll` (SDL's 64 or 32 bit one), `mme-fonts\`
(JetBrains Mono Nerd Font, taken from the mmc checkout beside this one, or
from the shell's `usr\share\fonts`) and, once it runs, `mme-data\`. The 64 bit
one is copied into `D:\mmc-shell\usr\bin` too (`set SDL_DEST=` to not copy). The
import definitions tcc needs go in `obj\`.

**Windows XP and 7.** A program runs on XP when its C runtime is `msvcrt.dll`
(XP has it; the Universal CRT that zig and MinGW-w64 use by default is Vista
and later) and it calls nothing XP lacks. `build xp` makes such a program:
32 bit, `msvcrt.dll`, marked for Windows 4.0 and later, with no call newer
than XP (SDL2 2.32.10's own 32 bit dll is the same). Two of mme's files are
compiled through small wrappers for that, without a change to them: `mos.c`
through `emos.c` (console programs started without a window) and `tpty.c`
through `etpty.c` (Vista's calls, looked up when the terminal panel starts,
so on XP only the panel is missing: ConPTY is Windows 10's). C11 is only the
language: nothing in it needs a newer Windows.

tcc 0.9.27 has only some of Windows' headers and import libraries, so its build
uses `tcc\`: the headers mme needs that tcc lacks (Winsock's TCP calls,
`shellapi.h`, `tlhelp32.h`, winnls's UTF-8 conversions, the pseudo console's
start-up info), C99's `snprintf` (msvcrt.dll's leaves a cut text
unterminated, and XP's does not know `%lld`: it becomes `%I64d`) and
`strtoll`; the `.def` files of the system's dlls are made with `tcc -impdef`
(from `SysWOW64` for 32 bit). The tcc builds get their icon from `mme.ico`
next to the program (tcc does not compile `.rc`).

On Linux and macOS, with SDL2 installed (`libsdl2-dev`, `brew install sdl2`):

```
make
```

## Keys and the window

The keys come as the same codes `eterm.c` makes of a terminal's, with the kitty
keyboard's modifiers, so every shortcut is VS Code's and Ctrl+Shift+P is not
Ctrl+P. mme's clipboard is the system's: text copied in another program is
what Ctrl+V, Shift+Insert, the terminal's paste, vim's `"+` and the Chat box
paste, and what mme copies goes there too. Ctrl+V reaches mme as a key, so
vim's visual block and Ctrl+K Ctrl+V work. AltGr's characters (@ { [ on a
German keyboard) are typed, not taken for Ctrl+Alt shortcuts; the right Alt
is Alt. The window's x is File > Exit (Ctrl+Q), after closing a question or a
list that is open: mme asks about files not saved.

Ctrl+= / Ctrl+- (the keypad's too) and Ctrl+NumPad0 zoom, as in VS Code;
`editor.fontFamily` and `editor.fontSize` are VS Code's settings, and a change
to them shows at once. Moved to a screen with another DPI, the font keeps its
size. The wheel scrolls a line per notch step (a touchpad's small steps add
up), sideways too. A file dropped on the window pastes its path, as a
terminal does. The image preview shows the picture itself.

## Not yet

- Windows XP on a real XP machine: `build xp` is built for it and checked
  (its header, its runtime, every call it makes), but it was run on Windows 11
  only. The terminal panel has no ConPTY on XP.
- Visual C++: the script is there, but no Visual C++ was on the machine it was
  written on to try it.
