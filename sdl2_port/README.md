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

It is a program of its own, apart from mme in the mmc shell: `bin\` has
`mme-sdl.exe`, `SDL2.dll` and its fonts, and `mme-sdl.exe` keeps its settings
in its own `mme-data` next to it (portable, like `mme.exe`'s). Copy `bin\`
anywhere; nothing of it goes into the shell's folder.

The painting is mmc-term's: `tfont.c`, `tshape.c` and `stb_truetype.h` are
copied from mmc as they are (the fonts: stb_truetype, and GDI's ClearType on
Windows; fallback fonts for missing glyphs), and `esdl.c` has `tdraw.c`'s
blending, box drawing, block elements and underlines. So it looks like mme in
mmc-term, with JetBrains Mono Nerd Font from mmc's `usr/share/fonts` (or the
`fonts` folder next to the program, or the system's).

| | mme in mmc-term | mme-sdl |
| --- | --- | --- |
| programs | mme + OpenConsole + mmc-term | mme-sdl |
| memory (one file open, Copilot left out) | 63 MB | 45 MB |
| idle CPU | 1.3-1.5 % of a core | 1.0 % |

## Building

SDL2 2.32.10 is not in the repository: unpack its development packages from
<https://github.com/libsdl-org/SDL/releases/tag/release-2.32.10> into `deps`
(`SDL2-devel-2.32.10-mingw.zip` for zig, gcc and tcc,
`SDL2-devel-2.32.10-VC.zip` for Visual C++), so that
`deps\SDL2-2.32.10\include\SDL.h` is there.

```
build            zig cc (the default)
build gcc        MinGW-w64 gcc
build tcc        Tiny C Compiler 0.9.27
build msvc       Visual C++ (from a "x64 Native Tools" prompt, or found with vswhere)
build clean
```

`bin\mme-sdl.exe` is made with `SDL2.dll` and `fonts\` (JetBrains Mono Nerd
Font, taken from the mmc checkout beside this one, or from the shell's
`usr\share\fonts`) beside it. tcc's `.def` files go in `obj\`.

tcc 0.9.27 has only some of Windows' headers and import libraries, so its build
uses `tcc\`: the headers mme needs that tcc lacks (Winsock's TCP calls,
`shellapi.h`, `tlhelp32.h`, winnls's UTF-8 conversions, the pseudo console's
start-up info), C99's `snprintf` (msvcrt.dll's leaves a cut text
unterminated) and `strtoll`; the `.def` files of the system's dlls are made
with `tcc -impdef`. The tcc build has no icon (tcc does not compile `.rc`).

On Linux and macOS, with SDL2 installed (`libsdl2-dev`, `brew install sdl2`):

```
make
```

## Keys and the window

The keys come as the same codes `eterm.c` makes of a terminal's, with the kitty
keyboard's modifiers, so every shortcut is VS Code's and Ctrl+Shift+P is not
Ctrl+P. Ctrl+V and Shift+Insert paste the system's clipboard; what mme copies
goes to it too. The window's x is File > Exit (Ctrl+Q): mme asks about files
not saved. Zoom In / Out / Reset change the font; `editor.fontFamily` and
`editor.fontSize` are VS Code's settings.

## Not yet

- Pictures (the image preview): mme sends them to a terminal as sixel; the
  window does not show them yet.
- Windows XP: SDL2 lists XP; the build needs a 32 bit MinGW-w64 with msvcrt
  (zig and the MinGW here link the Universal CRT, which XP does not have), and
  the terminal panel (ConPTY, Windows 10 1809) needs another way on XP.
- Visual C++: the script is there, but no Visual C++ was on the machine it was
  written on to try it.
