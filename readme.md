# mme

**mme** is VS Code for the terminal, written from scratch in pure C11.
No dependencies, no installer: one executable. `mme .` opens this folder,
like `code .` does.

```
 File  Edit  Selection  View  Go  Help            main.c - repo - mme
 ▎  EXPLORER                 │  main.c ×
    ⌄ REPO                   │ src › main.c
      ⌄ src                ● │   1  int main (void) {
         main.c            M │   2    int x = 2;
       notes.txt           U │   3    int y = 3;
       readme.md             │   4    return x + y;
  ⎇ master*                          Ln 1, Col 1   Spaces: 2   UTF-8   LF   C
```

The icons are codicons and Seti's, from a Nerd Font: mmc-term's JetBrains
Mono Nerd Font has them (it must be in `usr/share/fonts` of the mmc folder),
other terminals need a Nerd Font too. Without one the icons show as boxes.

## Build

The only tool needed is [zig](https://ziglang.org), used as a C compiler
(`zig cc`). Like Lua and mmc, all the C code is in the root folder.

```
build                 Windows: mme.exe, copied into %MME_DEST% (D:\mmc-shell\usr\bin)
build cross           every platform, into dist\
build install D:\mmc  copy mme.exe into D:\mmc\usr\bin
build clean
```

```
make                  Linux / macOS (make CC=gcc works too: it is plain C11)
make cross
sudo make install     /usr/local/bin/mme (make install PREFIX=/usr for /usr/bin)
```

| File | What is in it |
|---|---|
| `mme.h` | the one shared header |
| `mme.c` | `main`, the layout, the editor, the commands, the find widget, the dialogs for files |
| `ebuf.c` | the text: lines, load and save (LF / CRLF kept), undo and redo, indent detection |
| `eeditorconfig.c` | `.editorconfig`: a file's indent, line ends, charset and whitespace, as VS Code's EditorConfig extension |
| `eterm.c` | the terminal: raw mode, keys (kitty protocol too), mouse, bracketed paste |
| `edraw.c` | the screen: a cell grid, only changed lines are sent; VS Code's Dark+ colors |
| `emenu.c` | the menu bar and menus, the quick input box, the modal dialog, notifications |
| `eside.c` | the activity bar, the sidebar, the Explorer (folder tree, file icons, git colors) |
| `esearch.c` | the Search view: find and replace in files, include / exclude globs |
| `esearched.c` | the Search Editor: a search in an editor tab, its results as a `.code-search` file |
| `ehistory.c` | Local History: a copy of a file at each save, for the Timeline |
| `emd.c` | the Markdown preview |
| `echat.c` | Chat and Inline Chat: Claude, Anthropic's API, through curl, the answer streamed |
| `ehex.c` | the hex viewer for binary files (read-only) |
| `eimage.c` | the picture preview: PNG, BMP, GIF and ICO read here, drawn as sixel |
| `egit.c` | the Source Control view (stage, unstage, commit) and the diff editor |
| `egitlog.c` | the Source Control Graph, branches, pull / push / sync, stash, blame |
| `equick.c` | quick diff (the gutter's changes against the index) and merge conflicts |
| `emerge.c` | the merge editor: Incoming and Current over the Result, three panes |
| `esyntax.c` | syntax highlighting in VS Code's Dark+ colors, for about sixty languages, with VS Code's language ids |
| `eemmet.c` | Emmet: HTML and CSS abbreviations, as VS Code expands them |
| `epanel.c` | the panel: the integrated terminals, split, links, find, profiles |
| `eout.c` | the OUTPUT view: a channel each for Git, the language servers, Extensions |
| `elsp.c` | IntelliSense: a Language Server Protocol client (completion, definition, problems) |
| `ejson.c` | JSON: settings.json and the language servers' messages |
| `econfig.c` | settings.json: VS Code's settings, by VS Code's names |
| `eext.c` | extensions: the Extensions view, Open VSX, VS Code's; their themes, snippets, languages |
| `eregex.c` | regular expressions for Find, Replace and Search (`.*`) |
| `etm.c` | TextMate grammars: VS Code's own highlighting, from VS Code and its extensions |
| `eonig.c` | regular expressions as the grammars write them (Oniguruma's: lookbehind, `\G`, `(?x)` ...) |
| `esnip.c` | snippets: `~/.mme/snippets`, VS Code's format, and mme's own |
| `etest.c` | the Testing view: tests of Go, Python, Rust and npm found and run, results in the tree, gutter and Problems |
| `edebug.c` | Run and Debug: a Debug Adapter Protocol client (dlv, debugpy, lldb-dap, gdb), its view, the Debug Console |
| `etask.c` | tasks: `.vscode/tasks.json`, found tasks (make, go, npm, cargo), problem matchers |
| `ekeys.c` | keyboard shortcuts: `keybindings.json`, VS Code's format, with when clauses |
| `evim.c` | Vim mode: VSCodeVim's keys, modes, registers, macros and the `:` line in the text editor |
| `eworkspace.c` | multi-root workspaces: `.code-workspace` files |
| `eimport.c` | Preferences: Import VS Code Settings |
| `evscode.c` | where VS Code is (a portable one's `data` folder too): its settings, its extensions, read only |
| `esettings.c` | the Settings editor: every setting, its description and control |
| `ewelcome.c` | the Welcome page |
| `efiles.c` | the Explorer's file operations: rename, copy, delete to the Recycle Bin / Trash, reveal |
| `etheme.c` | the color themes: Dark Modern, Dark+, Light+, Monokai, Solarized Dark |
| `mme.rc`, `mme.ico` | Windows only: the program's icon and version details |
| `ethread.c` | the worker threads Search and Go to File walk a folder on, and the lock between them |
| `mmc.h`, `mutil.c` `mos.c` `mpath.c` | copied from [mmc](https://github.com/marjoncajocon/mmc-shell) as they are |
| `mterm.h`, `tpty.c` `tvt.c` `tgrid.c` | mmc-term's terminal core, copied as it is: the pseudo terminal, the escape sequences, the screen |

## The window

Everything VS Code has where VS Code has it: the menu bar, the activity bar
(Explorer, Search, Source Control), the sidebar, whose edge you drag to make
it wider, the editor with its tab and breadcrumbs, and the status bar with
the branch, the position, the indentation, the line ends and the language.
With no file open the editor shows the keys to start with.

- **Tabs** — every file opens in a tab of its own, also when it is clicked
  once in the Explorer. VS Code instead reuses one preview tab (in italics)
  until the file is edited: `"workbench.editor.enablePreview": true` does the
  same here. The x or a middle click closes a tab, a dot says it is not saved.
- **Colors** — C, C++, JavaScript, TypeScript, Go, Python, Rust, Zig, Java,
  Lua, shell, batch, PowerShell, JSON, Markdown, Makefile, YAML, TOML, CSS,
  HTML: keywords, types, functions, strings, numbers and comments in Dark+'s
  colors, in the editor and in the diff editor.
- **Split editor** — Ctrl+\\ opens the file in a new group on the right;
  both show the same text, an edit in one is in the other at once. A click,
  or Ctrl+1 .. 4, picks the group; its last tab closed, it goes.
- **IntelliSense** — a language server per language (settings.json,
  `mme.languageServers`: clangd, gopls, rust-analyzer, pylsp,
  typescript-language-server, zls, lua-language-server). Suggestions come as
  you type (Ctrl+Space asks), Enter or Tab takes one; F12 or Ctrl+Click goes
  to the definition; problems are squiggles, counted in the status bar, and
  F8 / Shift+F8 go through them. The server must be in the PATH. The text
  goes to the server when the typing stops (a fifth of a second), so a key
  does not send a big file again and again; anything asked for (a suggestion,
  a hover) sends it at once first. A server that has its problems asked for
  (pull diagnostics, LSP 3.17: rust-analyzer, typescript's ...) is asked when
  the file opens, after each change it is told, and when it asks for a
  refresh; "unchanged" keeps the ones shown, and the problems it reports for
  related files go to PROBLEMS too.
- **Hover** — the mouse resting on a name (or Ctrl+K Ctrl+I) shows what the
  server knows of it, its code in its language's colors. Resting on a squiggle
  shows its problem first, with a "Quick Fix... (Ctrl+.)" link. Links in it
  (`[text](url)`) are underlined: a click opens the file or the browser. It
  scrolls with the wheel, and with Up / Down / PgUp / PgDn after Ctrl+K
  Ctrl+I; the mouse may go into it (editor.hover.sticky), and it comes after
  editor.hover.delay ms.
- **Parameter hints** — the signature over the line with the parameter the
  cursor is in bold and underlined, its documentation under it. With
  overloads it shows "1/3" and Up / Down (or its arrows) go through them.
- **What the server does** — its progress ($/progress) is a spinner in the
  status bar and a bar in the notification center ("gopls: Setting up
  workspace (42%)"); its questions (showMessageRequest) are notifications
  with buttons: click one, or Ctrl+Shift+A for the first, or
  "Notifications: Focus Notification Toast" and then Left / Right and Enter.
  A notification says what it came from ("Source: gopls").
- **Language status** — "{ }" by the language in the status bar (a spinner
  while the server starts or works, red when it stopped, yellow when the
  program was not found). A click, or "mme: Show Language Status", shows the
  server and offers Restart Language Server, Show Output and the setting to
  change. A server that crashes is started again, and after five times in
  three minutes mme says so and stops, as VS Code does.
- **Inlay hints** — the server's hints in the text, dim and in italics:
  parameter names (`add(a: 1, b: 2)`), the types of `:=` variables ...
  (editor.inlayHints.enabled; gopls gets them turned on). Not with word wrap.
- **Color decorators** — a ■ in its color before each color the server finds
  (CSS, a theme's JSON ...), drawn like an inlay hint. A click on it, or Show
  or Focus Standalone Color Picker at a color, lists the server's ways to
  write it (`#ff0000`, `rgb(255, 0, 0)`): pick one, or type a hex color and
  it is written the way the color was (editor.colorDecorators).
- **Links** — the server's document links (an `#include`, an import, a URL):
  Ctrl+hover underlines the whole link, Ctrl+Click opens it - a file at the
  line its `#L10` says, a URL in the browser (editor.links).
- **VS Code's highlighting** — when VS Code is on the machine (`code` on
  the PATH, a portable install included, or the usual folders), mme reads
  the TextMate grammars of its built-in extensions and of the extensions
  installed (~/.vscode/extensions, a portable install's data folder,
  mme-data/extensions) and colors the code exactly as VS Code does: the
  theme's tokenColors for each scope (Dark Modern, Dark+, Light+, Monokai
  and Solarized Dark take VS Code's own theme files; an extension's theme
  its own), italic and bold included. Languages mme has no highlighter for
  (Clojure, F#, Groovy, Razor ...) come from the extensions too, with their
  comments for Ctrl+/. Long files are tokenized a little at a time so the
  screen stays alive; Developer: Inspect Editor Tokens and Scopes shows the
  scopes at the cursor. Without VS Code (or with editor.textmateGrammars
  off) mme's own highlighter does it.
- **Semantic highlighting** — the server says what each name is (a type, a
  parameter, a package, a constant ...) and it gets that color
  (editor.semanticHighlighting.enabled).
- **Code lens** — the server's actions over functions ("3 references | run
  test | debug test") on a dim row of their own over the line, the lines
  below pushed down; a click runs one, Code Lens: Run... the ones on the
  cursor's line (editor.codeLens).
- **Lightbulb** — in the gutter when the cursor's line has code actions (with
  a spark when one is a quick fix); a click opens them (editor.lightbulb.enabled).
- **Signature help** — typing a call, its parameters show over the line,
  the one the cursor is in brighter.
- **Rename** — F2 renames the name under the cursor everywhere the server
  finds it; files that were not open open, with the changes.
- **Update imports on rename** — renaming or moving a file or folder in the
  Explorer asks the servers that want it first (workspace/willRenameFiles, as
  VS Code does, waited for up to 5 seconds): their edits (the imports that
  name it) are made, the files that were not open open with them, then the
  file moves and the servers are told (didRenameFiles).
- **Quick Fix** — Ctrl+. (or the lightbulb in the gutter on a line with a
  problem) lists the server's code actions.
- **Problems** — the panel's PROBLEMS tab (Ctrl+Shift+M, or a click on the
  counts in the status bar): every file's problems; Enter goes there, a file's
  row folds (Left / Right, or a click), Ctrl+C copies the message. The filter
  box in its title (Ctrl+F, or just type) takes comma separated terms like
  VS Code's: text in the message or the file's name, a glob of the path
  (`**/*.go`), `!**/vendor/**` to leave files out; "Showing 3 of 10". The
  funnel: Show Errors / Warnings / Infos, Show Active File Only; the icon next
  to it collapses all.
- **Word wrap** — Alt+Z (editor.wordWrap): long lines go on in the rows under
  them, cut after a word; Up and Down go by rows.
- **Themes** — Ctrl+K Ctrl+T: Dark Modern (the default, like VS Code's),
  Dark+, Light+, Monokai, Solarized Dark, each shown while it is selected.
  Kept in settings.json (workbench.colorTheme).
- **Zen Mode** — Ctrl+K Z: the text alone; Esc Esc brings the rest back.
- **Editor groups** — up to 8, in a grid like VS Code's: Ctrl+\\ splits
  right, Ctrl+K Ctrl+\\ down (and left / up from the palette); Ctrl+1 .. 4 and
  Ctrl+K Ctrl+arrows focus them, Ctrl+Alt+Right / Left move the editor into
  the next / previous group. Drag a tab to reorder it, onto another group to
  move it, or to an edge of a group to split there (the drop place is
  tinted). Drag the lines between groups to size them; Ctrl+K Ctrl+M makes
  the group in front big and back. View > Editor Layout...: single, two or
  three columns, two rows, a 2x2 grid, join groups, reset sizes. Closing a
  group's last editor closes the group.
- **Appearance** — View > Appearance...: menu bar (window.menuBarVisibility),
  status bar (workbench.statusBar.visible), activity bar
  (workbench.activityBar.location "hidden"), Centered Layout.
- **Folding** — a chevron in the gutter by each block (by indentation, VS
  Code's own way when it has no language's ranges); click it, or Ctrl+Shift+[ / ] folds and unfolds, Ctrl+K Ctrl+0
  / Ctrl+K Ctrl+J all of them. A folded block shows `⋯` after its first line.
  The language server's folding ranges are used when it has them;
  `#region` / `// #region` / `#pragma region` markers and block comments fold
  too. Ctrl+K Ctrl+1 ... 7 fold a level, Ctrl+K Ctrl+8 / 9 fold / unfold the
  regions, Ctrl+K Ctrl+/ the block comments.
- **Brackets** — pairs colored by depth (editor.bracketPairColorization.enabled),
  the pair of the one at the cursor lit; ( [ { " ' ` close themselves and
  typing the closing one steps over it (editor.autoClosingBrackets).
- **Find and Replace** — Ctrl+F finds, Ctrl+H (or the chevron) opens the
  replace box: Enter replaces one, Ctrl+Alt+Enter all of them, in one undo.
  Alt+C match case, Alt+W whole word, Alt+R regular expression (`$1` in the
  replace text), Alt+L find in the selection. Alt+P (AB in the replace box)
  preserves case: `foo` replaced with `bar` gives `Bar` for `Foo` and `BAR`
  for `FOO`, and `Foo-Bar` / `FOO_BAR` go part by part, as in VS Code. The
  Search view has `.*` and AB too.
- **EditorConfig** — the `.editorconfig` files from the file's folder up (to
  the one with `root = true`) are read when it opens, as VS Code's
  EditorConfig extension does: indent_style, indent_size and tab_width win
  over the detected indent and the settings (the status bar shows them),
  charset reads and writes the file (utf-8, utf-8-bom, latin1, utf-16le /
  be), and on save end_of_line, trim_trailing_whitespace and
  insert_final_newline are done (a new file gets its end_of_line at once).
- **References and peek** — Shift+F12 lists every place a name is used in a
  box under the line, like VS Code's peek: the file's text on the left, the
  places by file on the right (Up / Down, Enter goes there, Esc closes). The
  box does not cover the text: the lines under it move down while it is open,
  as they do in VS Code (the dirty diff peek and the debugger's exception peek
  too).
  Ctrl+F12 Go to Implementations, Go to Type Definition, Alt+F12 Peek
  Definition; Ctrl+T (or `#` in Go to File) finds a symbol in the whole
  project. The other places of the name at the cursor are lit, and marked in
  the scrollbar (editor.occurrencesHighlight).
- **Sticky Scroll** — the headers of the functions, classes and namespaces
  the top line is inside stay at the top of the editor, from the symbols of
  the file (the server's, else mme's own; by indentation when there are
  none), with word wrap too; a click goes there
  (editor.stickyScroll.enabled, editor.stickyScroll.maxLineCount).
- **Indent guides and rulers** — a thin line at each indent level, the one of
  the cursor's block brighter (editor.guides.indentation,
  editor.guides.highlightActiveIndentation); lines at the columns of
  editor.rulers (`[80, 120]`).
- **Breadcrumbs** — a click on a folder or the file lists what is next to it
  (a folder goes into it); on a symbol, the symbols next to it, each shown
  while it is selected. Ctrl+Shift+. opens the last one.
- **Column selection** — Shift+Alt+drag, or Ctrl+Shift+Alt+arrows: a cursor
  on every line of the box.
- **Text** — Transform to Upper / Lower / Title Case, Sort Lines, Delete
  Duplicate Lines, Join Lines, Trim Trailing Whitespace (Ctrl+K Ctrl+X),
  Duplicate Selection, Go to Bracket (Ctrl+Shift+\\), Select to Bracket,
  Reopen Closed Editor (Ctrl+Shift+T). The status bar's language (Ctrl+K M),
  line end and indent ("Spaces: 4": convert, detect, tab size) change with a
  click.
- **Outline and breadcrumbs** — the Explorer's OUTLINE lists the file's
  symbols (the language server's, or found by mme when there is none), with
  VS Code's icon for each kind; Enter or a click goes there, a symbol with
  others in it folds (its chevron, Left / Right). Typing filters it (Filter on
  Type; Esc clears), the selection follows the cursor, and its "..." sorts by
  position, name or category; the icon next to it collapses all. Its title's
  chevron folds the whole section, as OPEN EDITORS' and TIMELINE's do, and
  the folded sections come back with the folder next time. The breadcrumbs
  over the text show the symbol the cursor is in.
- **Testing** — the TESTING view (the beaker in the activity bar): the tests of
  the folder, found in their files — Go (`func TestXxx` in `*_test.go`, run by
  `go test -json`), Python (`def test_` in `test_*.py`; pytest when it is
  installed, else unittest for TestCase methods), Rust (`#[test]` fns, `cargo
  test`) and a package.json `test` script. Each file and test shows ○ not run,
  ✓ passed, ✗ failed (with why, under it), a spinner while it runs; the ▷ on
  the selected row (or `r`, Ctrl+Enter) runs it, the title runs all, refreshes,
  collapses. The editor's gutter has ▷ / ✓ / ✗ by each test's line
  (testing.gutterEnabled): a click runs it. A failure goes to the Problems at
  the line that failed; what the tools print goes to OUTPUT, "Test Results".
  Keys, like VS Code's: Ctrl+; A all, Ctrl+; C the test at the cursor, Ctrl+; F
  the file's, Ctrl+; L the last run again, Ctrl+; Ctrl+C debug the test at the
  cursor (Go's dlv, Python's debugpy), Ctrl+; Ctrl+O the output, Ctrl+; Ctrl+X
  cancel. Files are saved before a run.
- **Test coverage** — "Test: Run All Tests with Coverage" (or in Current File,
  at Cursor) runs them with the tool's own coverage: `go test -coverprofile`,
  coverage.py (`pip install coverage`), `cargo llvm-cov`; an npm script is
  read from the `coverage/lcov.info` it writes (jest --coverage, c8). The line
  numbers turn green where the tests ran and red where they did not, as VS
  Code's gutter; "Test: Toggle Inline Coverage" colors the lines too. TESTING
  gets a TEST COVERAGE part: every file with the part of its lines covered
  (red under 60%, yellow under 90%, green), a click opens it, its x (or "Test:
  Close Coverage") clears it. What the runs write stays in mme-data.
- **Snippets** — typing `for`, `if`, `main`, `#ifndef` ... offers the
  language's snippets with the other suggestions; Enter puts one in, Tab and
  Shift+Tab go from field to field (a field used twice changes in both
  places), Esc or leaving it ends it. Edit > Insert Snippet lists them. Your own go in
  `~/.mme/snippets/<language>.json` (`c.json`, `go.json` ...) or any
  `*.code-snippets` there, written like VS Code's, so VS Code's snippet files
  work: `$1`, `${1:default}`, `${1|one,two|}`, `$0`, and variables like
  `$TM_FILENAME`, `$CURRENT_YEAR`, `$TM_SELECTED_TEXT`. File > Configure
  Snippets opens the file of the language in front; saving it applies it.
  Without a language server the file's words are suggested too
  (editor.wordBasedSuggestions).
- **Format Document** — Shift+Alt+F (or Ctrl+Shift+I): the language server
  formats the file; with editor.formatOnSave it does before every save.
  Ctrl+K Ctrl+F formats the selection; editor.formatOnPaste and
  editor.formatOnType do it as you paste and type (when the server can).
  Shift+Alt+O organizes the imports, and editor.codeActionsOnSave
  (`{"source.organizeImports": "explicit"}`) does it on save; Source Action...
  lists the rest.
- **Suggestions, more** — accepting a suggestion adds its import
  (additionalTextEdits, as gopls sends for `strings.ToUpper`); the selected
  one's documentation shows beside the list (Ctrl+Space again hides it); a
  snippet's `${1|a,b|}` shows its choices.
- **Smart Select** — Shift+Alt+Right / Left grow and shrink the selection:
  the server's selection ranges, else word, string, brackets, line, block.
- **Call Hierarchy** — Shift+Alt+H: the calls of the function at the cursor
  in the peek (Show Outgoing Calls, Show Supertypes / Subtypes in the palette).
  A rename that changes several files asks first, listing them.
- **Linked editing** — with editor.linkedEditing, an HTML tag's name typed
  changes its closing tag's too.
- **Comments** — Ctrl+/ comments or uncomments the lines (every cursor's),
  Ctrl+K Ctrl+C only adds, Ctrl+K Ctrl+U only removes; Shift+Alt+A puts a
  block comment around the selection or takes it away.
- **More cursors** — Shift+Alt+I puts a cursor at the end of each selected
  line, Ctrl+F2 selects every place of the word, Ctrl+K Ctrl+D moves the last
  selection to the next match, Alt+Enter in Find selects every match, Ctrl+U
  (Cursor Undo) brings the cursors back as they were (each tab keeps its
  own history, so switching tabs does not lose it).
  editor.multiCursorModifier "ctrlCmd" swaps Alt+Click and Ctrl+Click;
  editor.multiCursorPaste "full" pastes all of it at every cursor.
- **Vim mode** — vim.enable (or Vim: Toggle Vim Mode), VSCodeVim's keys in
  the text editor: Normal, Insert, Visual, Visual Line, Visual Block (a cursor
  per line) and Replace modes, `-- NORMAL --` in the status bar and a block
  cursor; counts, motions (w b e f t % { } gg G H M L ...), operators with
  text objects (d c y > < = gu gU gc; iw a" i( it ip ...), `.`, registers
  (vim.useSystemClipboard), marks, macros, `/` `?` `*` `n` with the matches lit
  as they are typed, Ctrl+D Ctrl+U zz, za zc zo, gd gh, and the `:` line: :w :q
  :wq :x :e :s with ranges :noh :set nu :sp :vs :tabn. Ctrl keys stay VS
  Code's (Ctrl+S, Ctrl+P ...) but for Vim's own (vim.useCtrlKeys,
  vim.handleKeys).
- **Auto indent** (editor.autoIndent "full") — Enter after `{`, `(`, `[`, a
  Python `:`, Lua's `then` / `do`, Ruby's `def` ... indents one more; typing
  `}`, `end`, `else:` ... puts the line back to its block's indent. Reindent
  Lines / Reindent Selected Lines re-do the indent of a file; with
  editor.autoIndentOnPaste pasted lines follow the cursor's indent. Enter
  follows the language's rules as well: one indent less after Python's
  `return`, `pass`, `raise`, `break` and `continue`, ` * ` inside a
  `/* ... */` comment, the next bullet or number of a markdown list (on an
  empty one the list ends).
- **Suggestions** — editor.tabCompletion ("on": a snippet's prefix, then Tab),
  editor.acceptSuggestionOnEnter ("on", "smart", "off"),
  editor.suggestSelection ("recentlyUsed"), editor.quickSuggestions (by
  default not in comments and strings), editor.suggestOnTriggerCharacters.
- **Editing** — Delete All Left / Right, Transpose Letters, Cursor / Delete
  Word Part Left / Right (camelCase, snake_case), Alt+PageUp / PageDown scroll
  a page, Ctrl+M toggles Tab moving the focus, editor.emptySelectionClipboard,
  editor.wordSeparators.
- **Rendering** — control characters as their pictures (␀ ␛,
  editor.renderControlCharacters), characters easy to take for ASCII ones
  (Cyrillic а, “ ”, fullwidth ...) tinted, invisible ones (U+200B, U+FEFF
  ...) as a tinted box of their own although they take no column, the hover
  says what they are (editor.unicodeHighlight.*), editor.renderLineHighlight,
  editor.cursorSurroundingLines, editor.scrollBeyondLastLine, and
  editor.lineNumbers "relative" / "interval".
- **Go Back / Go Forward** — Alt+Left / Alt+Right: to where the cursor was
  before a jump (another file, Go to Definition, Go to Line, a symbol ...).
- **Go to Symbol** — Ctrl+Shift+O: the file's functions and types, each
  shown while it is selected.
- **Run and Debug** — Ctrl+Shift+D, or F5: a debug adapter runs the
  program as `.vscode/launch.json` says, in VS Code's words (Run > Add
  Configuration makes one; without it a Go or Python file is debugged as
  it is). F9 or a click left of the line numbers sets a breakpoint (the red
  dot); paused, the line is yellow with an arrow, and the view shows
  VARIABLES (Enter or Right opens a value), WATCH (+ adds one), CALL STACK
  (Enter goes to a frame) and BREAKPOINTS (Space turns one off, Delete
  removes it). F10 / F11 / Shift+F11 step over / into / out, F5 goes on, F6
  pauses, Shift+F5 stops, Ctrl+Shift+F5 restarts, Ctrl+F5 runs without
  debugging; the same icons float over the editor, and the status bar is
  orange while it runs. The mouse on a name shows its value. The panel's
  DEBUG CONSOLE (Ctrl+Shift+Y) has the program's output and evaluates what
  is typed. Adapters by "type": `go` runs `dlv dap` (Delve, from PATH or
  `go install`), `debugpy` / `python` runs the debugpy of VS Code's Python
  Debugger extension when it is there (else `python -m debugpy.adapter`),
  `lldb-dap` runs lldb-dap, `gdb` / `cppdbg` runs `gdb -i dap`;
  `mme.debugAdapters` in settings.json names others
  (`{"go": "dlv dap --listen=127.0.0.1:0"}`).
  Right-click left of the line numbers: Add Conditional Breakpoint... (an
  expression, or a hit count), Add Logpoint... (`x is {x}`: printed in the
  DEBUG CONSOLE, no stop), Edit, Disable, Run to Line; F2 on a breakpoint
  edits it. BREAKPOINTS has the adapter's exception filters (Raised /
  Uncaught Exceptions) to tick. Paused: the values show dim after the lines
  (debug.inlineValues), an exception opens a red peek with its message,
  Debug: Run to Cursor / Jump to Cursor, Debug: Add to Watch (the selection),
  WATCH values open like VARIABLES, F2 on a variable sets its value, Ctrl+C
  copies it. Breakpoints, watches and the filters are kept per folder in
  `mme-data/state`.
- **Tasks** — Terminal > Run Task, Run Build Task (Ctrl+Shift+B): the tasks
  of `.vscode/tasks.json` (VS Code's format: command, args, group,
  problemMatcher), else the ones found: a Makefile's targets, a Go module's
  build and test, package.json's scripts, Cargo's. A task runs in a terminal
  of its own named after it; `$gcc`, `$go`, `$tsc` and `$msCompile` put its
  errors in the Problems panel. Terminal > Configure Tasks makes tasks.json.
- **Auto Save** — File > Auto Save (files.autoSave): afterDelay saves a file
  files.autoSaveDelay ms after the last change (no format on save, the
  cursor's line keeps its spaces), onFocusChange when the editor or the tab
  is left.
- **Hot exit and restore** — quitting (`mme .`) asks nothing: the open
  editors, every group and its place, cursors, the side bar and the panel are kept in
  `mme-data/state`, the texts not saved (Untitled ones too) in
  `mme-data/backups`, and they come back dirty the next time the folder is
  opened (files.hotExit "onExit", window.restoreWindows). The backups are
  also written every few seconds while there are unsaved changes.
- **Files changed on disk** — an open file changed by another program is
  read again when it has no unsaved changes; with changes, Ctrl+S asks like
  VS Code (Compare / Overwrite). A deleted file's tab says "(deleted)".
- **Encodings** — UTF-8, UTF-8 with BOM, UTF-16 LE/BE (by the BOM), Windows
  1252 and ISO 8859-1; files.encoding for files without a BOM; the status
  bar's encoding: Reopen with Encoding / Save with Encoding.
- **Compare** — File: Compare Active File with Saved (Ctrl+K D), with
  Clipboard (Ctrl+K C), With... (a file of the folder), and the Explorer's
  Select for Compare / Compare with Selected, side by side in the diff
  editor. File: Revert File reads the file again.
- **Command Palette** — the recently used commands first.
- **Cursor and side bar** — editor.cursorStyle (line, block, underline) and
  editor.cursorBlinking; workbench.sideBar.location "right" (View: Toggle
  Primary Side Bar Position).
- **Keyboard Shortcuts** — Ctrl+K Ctrl+S (File > Keyboard Shortcuts): every
  command with its keys and its VS Code name; type to search (names, keys or
  ids), Enter and press the new keys (two for a chord, like `Ctrl+K Ctrl+W`),
  Enter again to keep them. They go to `~/.mme/keybindings.json`, written like
  VS Code's, so a binding copied from VS Code works:
  `{"key": "ctrl+k ctrl+w", "command": "workbench.action.toggleZenMode"}`.
  Saving the file applies it; its keys come before mme's. As in VS Code an
  entry can have a `"when"` clause (`editorTextFocus && editorLangId == 'go'`,
  `resourceExtname =~ /\.c$/`, `terminalFocus`, `inDebugMode`,
  `suggestWidgetVisible`, `config.editor.wordWrap == 'on'` ...), `"args"` (for
  `type`, `editor.action.insertSnippet` and
  `workbench.action.terminal.sendSequence`), and `"-command"` takes mme's own
  key away; the last entry that fits wins. The editor shows each key's When
  and Source (Default or User); Enter on one changes its keys or when,
  removes it or resets it.
- **Import VS Code Settings** — Preferences: Import VS Code Settings (and a
  link on the Welcome page while it was not done): VS Code's User folder
  (Code, Code - Insiders, VSCodium; a portable VS Code's
  `data/user-data/User`, found by the `code` in the PATH; or one you pick) is only read; the
  settings mme has, the keybindings of commands mme has and the snippet files
  go into mme-data, after a look at what will come.
- **Workspaces** — `mme x.code-workspace` or File > Open Workspace from
  File...: each folder is a root of the Explorer, Go to File and Search look
  in all of them, the title says "x (Workspace)" and its `"settings"` apply
  over yours. Add Folder to Workspace... turns a folder into one, Save
  Workspace As... writes it. A folder's `.vscode/settings.json` applies over
  your settings too, like VS Code.
- **The wheel** — a notch scrolls three lines, in the text and in every list
  (editor.mouseWheelScrollSensitivity multiplies it); Alt+wheel five times as
  far, Shift+wheel to the side. Terminals that send several events for one
  notch (mmc-term sends three) still scroll one notch's worth.
- **The mouse pointer** — a hand over what a click does something with (the
  menus, tabs, the side bar, the panel's tabs, the status bar, the pages) and
  an I beam over the text, for terminals that understand OSC 22 (xterm,
  kitty, WezTerm; a terminal that does not just keeps its own pointer).
- **Scrollbar** — at the right of the text, with the problems, the find
  matches and the cursor marked in it like VS Code's overview ruler; click or
  drag it. A line wider than the text gives a horizontal one under it (and
  Shift+wheel goes to the side).
- **Settings editor** — Ctrl+, (File > Settings) opens VS Code's Settings
  editor in a tab: a search box ("tab size", or `@modified`), the table of
  contents on the left (Commonly Used, Text Editor, Workbench, Features,
  Extensions) and every setting with its description and a checkbox, a list
  to pick from or a box to type in. A change goes into settings.json at once
  and applies; a setting not at its default has the blue bar, and its gear
  (or Shift+F10) offers Reset Setting, Copy Setting ID and Copy Setting as
  JSON. The icon at the top right, or "Preferences: Open User Settings
  (JSON)", opens settings.json itself (made the first time with every
  setting and its default); saving it applies it too.
- **settings.json and keybindings.json** — as in VS Code, typing a key's
  quote or its first letters suggests the settings not yet in the file
  (`"editor.ta` finds editor.tabSize); taking one puts in `"key": value`
  with the value to pick, and a comma when another key follows. After
  `"key": ` the values it takes are suggested, the default first; `"[`
  suggests the language blocks. The mouse on a key (or Ctrl+K Ctrl+I) shows
  its description and default. A key mme does not have is a hint ("Unknown
  Configuration Setting"), a value of the wrong kind or not in the list a
  warning. In keybindings.json an entry's key, command, when and args are
  suggested, and every command id after `"command": `.
- **Language-specific settings** — `"[python]": { "editor.tabSize": 2 }`
  (or `"[javascript][typescript]": ...`) applies to that language's files
  only: the text editor's settings and the files' ones (editor.*, files.*,
  diffEditor.*, emmet.*, html.*), as VS Code lets a language override. A
  workspace's block goes over the user's key by key.
- **Welcome** — at startup when no file is opened (workbench.startupEditor),
  and from Help > Welcome: Start (New File, Open File, Open Folder, Open
  Project), the recent folders, a walkthrough of the keys worth knowing, and
  "Show welcome page on startup".
- **Notifications** — the bell at the right of the status bar (a dot on it
  when there are new ones) opens the notification center: the notifications
  so far, the newest on top; Delete or its x clears one, the icon on top
  clears them all.
- **Languages** — about sixty, each with VS Code's language id (for
  snippets, language servers and extensions): C, C++, C#, Objective-C, Java,
  Kotlin, Scala, Swift, Dart, JavaScript, TypeScript, Go, Rust, Zig, Odin, V,
  Nim, Julia, Python, Lua, PHP, Ruby, Perl, Elixir, Erlang, Haskell, R, shell,
  Batch, PowerShell, Dockerfile, Makefile, CMake, SQL, JSON and JSON with
  Comments, YAML, TOML, INI, .properties / .env, .gitignore, Markdown, LaTeX,
  CSS, SCSS, Less, HTML, XML / SVG, Vue, Svelte, GraphQL, Protocol Buffers,
  Terraform, assembly, diff / patch and log files. A file without a known
  name goes by its first line (`#!/usr/bin/env python3`, `<?php`, `<?xml`).
  mme.languageServers has VS Code's usual server for most of them (csharp-ls,
  intelephense, solargraph, vscode-html-language-server ...); a server starts
  only when it is installed.
- **Emmet** — in HTML, XML, Vue, Svelte, PHP and JSX, `ul>li.item$*3>a{Item $}`,
  `div#main.box`, `table>tr*2>td*3`, `input:text`, `link:css` or `!` (the HTML5
  page) becomes the elements: Tab after it (emmet.triggerExpansionOnTab), or
  the suggestion "Emmet Abbreviation"; Tab then goes through the empty
  attributes and contents. In CSS: `m10` → `margin: 10px;`, `p10-20`,
  `w100p` → `width: 100%;`, `df`, `posa`, `c#f` → `color: #fff;`, `fz1.5r`.
- **Closing tags** — typing the `>` of `<div class="x">` puts `</div>` after
  the cursor, and `</` gets the name of the tag still open (html.autoClosingTags).
- **Mouse** — a double click selects a word, a triple click the line (dragging
  on goes by words or lines), Shift+click extends the selection, dragging the
  selection moves it (Ctrl: copies; editor.dragAndDrop), Ctrl+hover underlines
  a name and Ctrl+click goes to its definition, the middle button drags a
  column, Alt+wheel scrolls faster.
- **Multiple cursors** — typing, deleting, moving and selecting happen at
  every cursor; a paste with as many lines as cursors gives each its line.
- **Minimap** — the whole file in small (Braille dots, a dot for a few
  letters) on the right of the text, in its colors. The box of what the
  editor shows (its slider) appears with the mouse over it and is dragged;
  a click elsewhere goes there. The git changes are marked on its left edge
  (green added, blue modified, red deleted), and problems, find matches and
  the occurrences of the symbol at the cursor tint where they are.
  editor.minimap.side puts it on the left, showSlider "always" keeps the box,
  renderCharacters false draws blocks of color, maxColumn and scale say how
  much of a line and how many lines a row holds. Its colors come from mme's
  own scanner, not from the TextMate grammar: a dot stands for a few letters,
  and a grammar over the hundreds of lines it shows would cost a frame too
  much. View > Toggle Minimap hides
  it. The scrollbar next to it is VS Code's overview ruler: the cursor, the
  git changes, the occurrences, the find matches, the warnings and the
  errors, each in its color.
- **Terminal** — Ctrl+` or Ctrl+J opens the panel under the editor with a
  shell in the folder that is open: `mmc-shell` when it is in the PATH (else
  cmd / $SHELL; `MME_SHELL` chooses another). It is mmc-term's own terminal
  core, so colors, full screen programs and the scrollback (the wheel) work.
  Drag its title row to make it higher; + (or Ctrl+Shift+`) starts another
  terminal, each with its tab in the title row; the trash icon ends the one in
  front, the x only hides the panel. With the terminal in front the keys are the shell's, except
  F1, Ctrl+Shift+..., Alt+1 / 2 / 3 and Ctrl+PgUp / PgDn.
- **Split terminals** — Ctrl+Shift+5 (or the split icon) puts one more terminal
  next to the one in front; Alt+Left / Alt+Right go between them. With more
  than one terminal the tabs list is on the right (a split group joined by a
  line): click one to show it, its trash icon ends it.
- **Terminal links** — `file.c:12:5`, `x.c(12,5)`, `a/b.go:7` and URLs in the
  output are underlined under the mouse; Ctrl+click opens the file at that
  line (from the shell's folder, else the one open), or the URL in the browser.
- **Selecting in the terminal** — drag to select (the scrollback scrolls at the
  edges), double-click a word, triple-click a line, Alt+drag a column block.
  Ctrl+C copies when there is a selection (else it is the shell's ^C),
  Ctrl+Shift+C too; Ctrl+V / Ctrl+Shift+V paste. The right button copies the
  selection, or pastes (terminal.integrated.rightClickBehavior: copyPaste,
  paste, selectWord, default for a menu, nothing); copyOnSelection copies as
  you select. Pasting more than one line asks first
  (enableMultiLinePasteWarning). A program that asked for the mouse (vim,
  htop ...) gets it; Shift selects anyway.
- **Shell integration** — like VS Code's, PowerShell, bash and cmd are told to
  mark their prompts (OSC 633; FinalTerm's 133 is read too): each command gets
  a circle by its prompt, blue when it went well, red when it failed;
  Ctrl+Up / Ctrl+Down scroll to the previous / next command, the prompt of the
  command scrolled into stays on top (terminal.integrated.stickyScroll),
  Ctrl+Alt+R runs a recent command again, Ctrl+G goes to a recent folder
  (mme-data keeps both lists). terminal.integrated.shellIntegration.enabled
  turns it off. mmc-shell only says its folder (OSC 7).
- **Run in the terminal** — Terminal: Run Selected Text In Active Terminal
  (the cursor's line when nothing is selected) and Run Active File In Active
  Terminal (quoted as the shell wants it). Terminal: Change Color... and
  Change Icon... mark a terminal's tab; Select All, Copy Selection, Focus
  Terminal are commands too.
- **Terminal settings** — cursorStyle (block, line, underline) and
  cursorBlinking (or what the program asks for), scrollback, cwd, env.windows
  / linux / osx (more variables for the shell only), confirmOnKill and
  confirmOnExit ("Do you want to terminate the active terminal session?").
- **Find in the terminal** — Ctrl+F with the terminal in front: the scrollback
  is searched, the matches lit; Enter goes up, Shift+Enter down, Esc closes.
- **Terminal commands** — Terminal: Clear, Rename..., Select Default Profile
  (mmc-shell, PowerShell, Command Prompt, Git Bash, WSL ... found on the
  system; saved as terminal.integrated.defaultProfile.windows / linux / osx),
  Create New Terminal (With Profile) (the ⌄ next to +). Shift+PgUp / PgDn and
  Ctrl+Shift+Up / Down scroll the scrollback.
- **OUTPUT** — Ctrl+Shift+U: the panel's OUTPUT tab, a channel at a time
  (the list in its title row, or Output: Show Output Channels...): Git (every
  git command, and what went wrong), each language server (what it logs, its
  stderr), Extensions (downloads, installs).
- **Maximized panel** — the ^ in the panel's title row (View: Toggle Maximized
  Panel) gives the panel the whole editor area.
- **Chat** — Ctrl+Alt+I (Chat: Open Chat) opens VS Code's Chat view in the
  secondary side bar, at the right of the editors (Ctrl+Alt+B hides it), with
  Claude behind it: Anthropic's Messages API, asked by curl in the background,
  so the answer streams in as it is written. The answers are Markdown (code in
  its language's colors); each code block has Copy, Insert (at the cursor, as
  a paste) and Apply (in place of the selection, shown as a diff to accept) on
  its top row. Enter asks, Shift+Enter is a new line, Esc stops the answer,
  Ctrl+L (or the +) starts a New Chat, Up brings the last question back. The
  selection goes with the question, else the lines the editor shows, like VS
  Code's implicit context: the chip in the box names it (`edit.c:16-19`), a
  click on it leaves it out. The whole talk is sent again with each question.
  **Inline Chat** — Ctrl+I in the editor: a box over the selection (or the
  cursor) to say what to change; Claude's code comes back as a diff in the text,
  the old tinted red, the new under it, with Accept (Ctrl+Enter or Tab) and
  Discard (Esc). The key: `mme.chat.apiKey` in settings.json (Chat: Set API
  Key... puts it there), else `ANTHROPIC_API_KEY` (or `ANTHROPIC_AUTH_TOKEN`)
  from the environment; it goes to curl on its stdin, never on its command
  line, and never into OUTPUT's "Chat" channel. `mme.chat.model`
  (`claude-opus-5`), `mme.chat.baseUrl` (or `ANTHROPIC_BASE_URL`), and
  `mme.chat.fallbacks`: a request the model declines is answered by another
  (the API's server-side fallback). A refusal, an answer cut at its length, a
  refused key, a rate limit or an overloaded API are said in the talk in words.

- **Extensions** — Ctrl+Shift+X (View > Extensions): with nothing typed the
  installed ones, else a search of [Open VSX](https://open-vsx.org) (the
  marketplace VSCodium uses). Enter on one: Install, Set Color Theme, Show
  Details (its page and README in a tab), Uninstall; a click on its button
  installs or uninstalls. They go in `mme-data/extensions`, as VS Code keeps
  them (curl downloads the .vsix, tar or unzip opens it); Install from VSIX...
  takes a .vsix file. The extensions VS Code has (`~/.vscode/extensions`, a
  portable VS Code's `data/extensions`) are used too, read only
  (`mme.extensions.useVSCodeExtensions`).

  What an extension can do in mme: its **color themes** (Ctrl+K Ctrl+T lists
  them; VS Code's colors and token colors are put on mme's), its **snippets**,
  and its **languages** (file extensions, and the comments Ctrl+/ uses). What
  it cannot: a VS Code extension's code is JavaScript that runs in VS Code's
  extension host (Node and the `vscode` API), which mme does not have, so its
  commands, views, debuggers, language features written in JavaScript, file
  icon themes and TextMate grammars do nothing here (its page says which it
  has). A language server an extension downloads can still be used by naming
  it in `mme.languageServers`.
- **Explorer** — the folder tree, with a scrollbar at its right edge (every
  list of the side bar has one). Enter opens (Space only shows), Left /
  Right fold, typing a letter jumps, F5 reads the folder again. Changed files
  are colored like VS Code's git decorations. The folder's row has New File,
  New Folder, Refresh and Collapse Folders; the name is typed in the tree
  ("a/b.c" makes the folder a too). F2 renames (the name without its
  extension selected), Delete moves to the Recycle Bin / Trash (asked
  first), Ctrl+C / Ctrl+X / Ctrl+V copy and move files, and the right button
  gives VS Code's menu: Reveal in File Explorer (Shift+Alt+R), Open in
  Integrated Terminal, Duplicate, Copy Path (Shift+Alt+C), Copy Relative
  Path (Ctrl+K Ctrl+Shift+C). Open tabs follow a rename.
  Ctrl+click / Shift+click (Shift+Up / Down, Ctrl+A) select more rows;
  Delete, Cut / Copy / Paste and dragging work on all of them. Drag rows onto
  a folder to move them (asked first, explorer.confirmDragAndDrop; Ctrl held:
  copied), or into an editor group to open them. Files with problems are red
  / yellow with their count ("2, M" with git's letter), their folders too.
  Folders with one folder in them show as one row ("a/b/c",
  explorer.compactFolders); explorer.fileNesting.enabled puts related files
  under one (a.ts: a.js; package.json: package-lock.json). files.exclude hides
  files in the tree, Go to File and Search; explorer.sortOrder (default, mixed,
  filesFirst, type, modified), explorer.autoReveal. Ctrl+Enter (or Alt+click)
  opens to the side; Find in Folder... (Shift+Alt+F) searches a folder.
  OPEN EDITORS is at the top, OUTLINE and TIMELINE under the folder.
- **Search** — find in files as you type. Alt+C match case, Alt+W whole word,
  Alt+P preserve case in the replace box,
  Enter or a click opens the match, F4 / Shift+F4 goes through them. The
  chevron (or Ctrl+Shift+H, Replace in Files) opens the replace box: the
  results show the change, Replace All (Ctrl+Alt+Enter, or its icon) asks
  "Replace N occurrences across M files?"; Ctrl+Shift+1 or the row's icon
  replaces one match or one file, Delete dismisses a result. "..." (or
  Ctrl+Shift+J) opens "files to include" / "files to exclude" (globs,
  comma separated: `*.c, src, ./lib`); .gitignore, .git and node_modules are
  left out. Files open in the editor are searched and changed there (one undo
  step). Tab goes from box to box; refresh, clear, Open New Search Editor and
  collapse are on the title.
  A big folder is walked on worker threads, one for each core: the results and
  "Searching N files" show while it goes on, and typing again starts it over.
  The editor draws and answers keys throughout; the results settle into the
  folder's order once the walk is over.
- **Search Editor** — "Search Editor: New Search Editor" (the selection is its
  query), or "Open in editor" under the Search view's count (Alt+Enter there,
  or the new-file icon in its title), opens a tab "Search: query": the query
  box with Match Case, Whole Word and `.*` (Alt+C / W / R), the context lines
  next to it (Alt+L, Alt+= / Alt+-; search.searchEditor.defaultNumberOfContextLines,
  1), and "..." (Ctrl+Shift+J) for files to include / exclude. It searches as
  you type (search.searchOnType; else Enter), Ctrl+Shift+R runs it again. The
  results are VS Code's `.code-search` text: "6 results - 2 files", each file's
  path, `  12: ` a match and `  13  ` a line around it, an empty line where
  lines are skipped; paths, line numbers and matches in VS Code's colors.
  Enter, F12 or a double click on a line opens the file there, the match
  selected; Ctrl+Shift+Backspace drops a file's results, Esc goes back to the
  query. Ctrl+S saves it as a `.code-search` file (`# Query:`, `# Flags:`,
  `# Including:`, `# Excluding:`, `# ContextLines:` on top), which opens in
  the Search Editor again, from the Explorer or the command line. A new one
  takes the toggles and files of the last one with
  search.searchEditor.reusePriorSearchConfiguration. The results are read
  only here: select them (Shift+arrows, the mouse, Ctrl+A) and copy them, but
  they are not typed in as VS Code allows.
- **Quick Open** — Ctrl+P lists the files opened lately; typing searches the
  folder's files too, the ones opened lately still first ("recently opened",
  then "file results", as VS Code labels them). The folder is
  walked on worker threads as well, so the list shows at once on a big one and
  "Indexing... N files" counts up while the rest arrives. What is typed
  first switches it, like VS Code: `>` commands, `@` the file's symbols, `:`
  a line (`:12:5`), `?` the list of these; Backspace over the sign goes back to
  files. Ctrl+R (Open Recent) lists recent folders, then recent files.
  What is typed matches the name and the path, as VS Code does: the letters in
  order, letters side by side and at the start of a word or a part of the path
  worth more, `src/ma` finds `src/main.c`, several words each have to match,
  and the letters that matched are lit blue. The folder's files are found once
  and kept: a big one shows "Indexing... N files" and fills while you type
  (100,000 files in about 15 s here), and it is walked again when a file is
  made, renamed or deleted. Each letter typed only looks again at what
  matched before, so a big folder keeps up. `main.c:12` (or `main.c:12:5`)
  finds main.c and opens it at that line; Ctrl+Enter opens the file in the
  group beside.
- **Tabs** — right-click a tab: Close, Close Others, Close to the Right, Close
  Saved, Close All, Copy Path, Reveal in Explorer View, Keep Open, Pin, Split
  Right. Pinned tabs stay first with a pin and survive Close Others / All; the
  middle button closes; the wheel scrolls the tab bar. Ctrl+Tab lists the
  editors by use (Ctrl+Tab again for the next, Enter opens); Ctrl+K W / Ctrl+K
  Ctrl+W / Ctrl+K U close the group's / all / the saved editors, Ctrl+K Enter
  keeps a preview tab, Ctrl+K Shift+Enter pins, Ctrl+K P copies the path.
- **Local History and TIMELINE** — every save keeps a copy in
  `mme-data/history` (50 per file, like VS Code's workbench.localHistory). The
  TIMELINE pane at the bottom of the Explorer lists them with the file's git
  commits, newest first: Enter shows a side-by-side diff (a commit: its change
  of the file); right-click (or Shift+F10) a copy to Compare or Restore
  Contents. "Local History: Find Entry to Restore..." from the palette too.
- **OPEN EDITORS** — a pane of the Explorer (collapsed at first) with every
  group's tabs: Enter opens, Delete or the x closes, a dot marks unsaved ones.
  Tab goes from pane to pane (folders, OPEN EDITORS, OUTLINE, TIMELINE). Here
  the panes are under the folder tree rather than above it.
- **Markdown preview** — Ctrl+Shift+V opens a preview tab of a .md file,
  Ctrl+K V opens it to the side (the file keeps the keys): headings (with
  `===` / `---` under them too), lists nested as far as they go, task boxes,
  quotes with a bar for each `>` deep, code blocks in their language's
  colors, tables in boxes with their `:---:` alignment, **bold**, *italic*,
  ~~struck~~, footnotes, HTML shown as the words it says rather than its
  tags, and pictures as `🖼 alt (120x80 png, 12 KB)`. Links are underlined
  and Ctrl+click follows them: `#a-heading` jumps down the page, `http` and
  `https` open the browser, and a file next to the document opens in its own
  editor. Side by side with the file the two scroll together
  (`markdown.preview.scrollPreviewWithEditor` and
  `markdown.preview.scrollEditorWithPreview`, both on); the page is only
  measured again when the text or the width changes, so a file of a few
  thousand lines stays smooth. It follows the typing; the wheel and the keys
  scroll it.
- **Picture preview** — .png, .jpg, .gif, .bmp, .webp, .ico and .svg open in
  an editor of their own, from the Explorer, Go to File or the command line.
  The picture is drawn in the editor area by terminals that draw pictures
  (sixel), scaled to fit, with `1920x1080 • PNG • 240 KB` under it; `+` and
  `-` zoom, `0` fits it again (Ctrl+wheel too), and the command palette has
  Image Preview: Zoom In / Out / Reset. A terminal without pictures gets the
  same picture in Braille dots instead. mme reads PNG, BMP, GIF and ICO
  itself, with no libraries; a JPEG, WEBP or SVG shows its size and kind
  from the file's header and says why it is not drawn.
- **Hex viewer** — a binary file (one with a zero byte near its start) opens
  as `offset | 16 bytes | the letters`, like VS Code's Hex Editor, instead of
  the noise a text editor makes of it. It is **read-only**: mme does not
  write binaries. The arrows, PgUp / PgDn and Home / End (Ctrl+Home / End for
  the whole file) move through the bytes, a click puts the cursor on one,
  Ctrl+G goes to an offset (`1f40`, `0x1f40` or `#8000` for decimal), Ctrl+F
  looks for bytes or text (`4d 5a` or `MZ`) and F3 finds the next one. The
  status bar shows the offset and the byte there, the line under it the file
  and its size. View: Reopen Editor With Hex Editor opens any file this way.
- **Source Control** — Staged Changes and Changes. Enter or a click opens the
  diff; `s` / `+` stages, `u` / `-` unstages, `o` opens the file. The message
  box on top commits with Ctrl+Enter. The title's buttons commit, refresh and
  open More Actions (pull, push, sync, fetch, branches, stash, undo last
  commit, amend ...).
- **Graph** — under the changes, VS Code's Source Control Graph: the commits
  of every branch (`git log --all`) with their lanes in color, merges joining
  them, the branches, remote branches and tags as badges, the subject, the
  author and when. Enter, a click or Right opens a commit to its files (Left
  closes it); a file opens its change in the diff editor, side by side,
  titled like VS Code's `f.c (abc1234^ ↔ abc1234)`. "Load More..." shows
  older ones. **Git: View File History** lists the commits of the file in
  front; one opens its change.
- **Branches** — the branch in the status bar opens **Git: Checkout to...**:
  Create new branch..., Create new branch from..., Checkout detached..., the
  branches, remote branches (checked out as a local branch that tracks it)
  and tags, the latest first. Next to it the sync item shows how far behind
  ↓ and ahead ↑ the upstream is; a click syncs (pull, then push). The
  palette has Git: Create / Delete / Rename Branch, Merge..., Pull, Push
  (publishes a branch without an upstream), Fetch, Sync, Stash, Pop Latest
  Stash, Apply Stash..., Undo Last Commit, Commit Staged (Amend). Files git
  changes on disk (checkout, pull, stash) are read again when not edited.
- **Blame** — the cursor's line ends with who changed it last and when, dim
  (`Ana Cruz, 3 days ago • subject`), and the status bar says it too.
  git.blame.editorDecoration.enabled and git.blame.statusBarItem.enabled turn
  them off; **Git: Toggle Git Blame Editor Decoration** too.
- **Quick diff** — the gutter shows the lines changed against git's index,
  like VS Code: a green bar for added lines, blue for modified, a red
  triangle where lines were deleted (scm.diffDecorations). A click on a bar,
  or Alt+F3 / Shift+Alt+F3, opens the dirty diff peek under the change: what
  it was (red) and is (green), "n of m changes", with Stage Change, Revert
  Change, previous / next and close. Alt+F5 / Shift+Alt+F5 go to the next /
  previous change. **Git: Stage Selected Ranges** (Ctrl+K Ctrl+Alt+S),
  **Unstage Selected Ranges** (Ctrl+K Ctrl+N) and **Revert Selected Ranges**
  (Ctrl+K Ctrl+R) take the changes the selection touches, in the editor or
  in the diff editor (there: the change with the bar, F7 moves it). Staging
  gives the index the file with only those changes, as VS Code does.
- **Merge conflicts** — the `<<<<<<<` / `=======` / `>>>>>>>` blocks are
  colored like VS Code's (current green, incoming blue), and the first line
  has **Accept Current Change | Accept Incoming Change | Accept Both Changes |
  Compare Changes**. Merge Conflict: Accept All Current / Incoming / Both,
  Next / Previous Conflict are in the palette. Source Control lists the files
  under **Merge Changes**; `+` on one marks it resolved.
- **The merge editor** — opening a conflicted file gives VS Code's three
  panes: **Incoming** (theirs) and **Current** (ours) side by side, with the
  branches from `MERGE_HEAD` in their titles, and the **Result** under them,
  which says how many conflicts are left. The three versions come from git's
  index (`:1:` base, `:2:` ours, `:3:` theirs), so the conflicts are worked
  out the way diff3 does and everything only one side changed is merged at
  once; a file with markers but no index entry is read from the markers.
  Each conflict is tinted in all three panes and is taken with **Accept
  Incoming**, **Accept Current** or **Accept Combination** (a click, or Enter
  in the pane), F7 and Shift+F7 walk them, and Merge Conflict: Accept All
  Current / Incoming / Both do the lot. The Result is ordinary text: type in
  it, Ctrl+Z, Ctrl+Y. **Complete Merge** (the button, or Ctrl+Enter) writes
  the file and stages it; closing the tab with conflicts left asks first.
  `git.mergeEditor` (default true) decides whether a conflicted file goes
  straight there: with it off a **Resolve in Merge Editor** banner offers it,
  and **Git: Open Merge Editor** is always in the palette. A binary file, or
  one deleted on one side, says so instead.
- **Repositories** — in a folder without git, Source Control offers
  **Initialize Repository** and **Clone Repository** (**Git: Clone** asks the
  URL and the folder, then opens the clone). A branch with no upstream gets
  VS Code's **Publish Branch** button, one behind or ahead **Sync Changes**.
  Committing with nothing staged asks to stage everything: Yes, Always
  (git.enableSmartCommit), Never (git.suggestSmartCommit).
- **Diff editor** — the whole file, old on the left and new on the right
  (one above the other when it is narrow, or with `i` / **Toggle Inline
  View**), changed lines red and green, the changed words in them brighter
  (VS Code's character diff). Up and Down move the cursor, Enter opens the
  file at its line, F7 / Shift+F7 go from change to change, Esc closes it.
  The modified side of a working tree diff is the file itself and is typed
  in, like VS Code's: the keys, undo, the tab's dirty dot and Ctrl+S are the
  editor's own, and the diff is made again a moment after the typing stops.
  There `i` and `o` go into the text, so Alt+I toggles inline and Alt+O opens
  the file; Enter on a "⋯ N hidden lines" row still opens it. It selects as
  the editor does: Shift+arrows, Ctrl+Shift+Left / Right by words, Shift+Home
  / End, Ctrl+A, a drag or a Shift+click; Ctrl+C, Ctrl+X, typing and Delete
  take the selection.
  It has a minimap of its own, green where lines came and red where they
  went; a click in it goes there. The tab bar carries its actions, like VS
  Code's editor title: previous and next change, ¶ (**Diff: Toggle Ignore
  Trim Whitespace**: spaces at the ends of lines are not changes,
  diffEditor.ignoreTrimWhitespace), fold (**Diff: Toggle Collapse Unchanged
  Regions**: long runs with no change become one "⋯ N hidden lines" row that
  a click opens, diffEditor.hideUnchangedRegions.enabled), inline or side by
  side (diffEditor.renderSideBySide), and open the file. A → by a change of
  the working tree reverts that block.

- **View All Changes** — the diff icon in Source Control's title, or "Git:
  View All Changes" / "Git: View Staged Changes", opens VS Code's multi-diff
  editor: every changed file in one tab, a header (name, folder, +added
  -removed, its letter) over its diff, side by side when there is room, the
  unchanged lines folded. Enter or a click on a header folds the file (Left
  / Right too); Enter or a double click on a line opens the file there, on a
  fold opens it. The tab follows git: a save shows at once.
- **Title bar** — the command center in the middle (window.commandCenter): the
  folder's name in a box that opens Go to File, with Go Back and Go Forward
  beside it. `window.title` takes VS Code's variables (`${activeEditorShort}`,
  `${rootName}`, `${dirty}`, `${separator}` ...); a part that comes out empty
  goes, with its separator.
- **Manage** — the gear at the foot of the activity bar (VS Code's): the
  Command Palette, Settings, Extensions, Keyboard Shortcuts, Snippets, Tasks,
  Themes and About. The activity bar's icons carry VS Code's badges: how many
  files changed, how many tests failed, a dot while debugging.
- **Profiles** — like VS Code's: the gear's Profiles (or "Profiles: New
  Profile...", "Switch Profile...", "Rename Profile...", "Delete Profile...").
  A profile has its own settings.json, keybindings.json and snippets, in
  mme-data/profiles/<name>; Default is mme-data's own. A new one starts empty
  or as a copy of the one in use. Switching applies it at once (the open
  files' indentation too), and the folder remembers it: opening that folder
  again brings the profile back. The title says which one is in use
  (`${profileName}`, nothing for Default).
- **Status bar** — its items are VS Code's (the branch, the problems, the
  position, the indentation, the encoding, the line ends, the language, the
  notifications). Resting the mouse on one shows what it does, a click runs it,
  and a right-click hides items; what is hidden is kept in mme-data.
- **Panel position** — workbench.panel.defaultLocation "bottom", "right" or
  "left" (View: Move Panel Right ..., or the panel's "..."); at a side it is as
  high as the editors and its edge drags. workbench.panel.alignment "justify"
  puts a bottom panel across the whole width, under the sidebar too.

## Keys

| Keys | |
|---|---|
| Ctrl+Shift+P, F1 | Command Palette |
| Ctrl+P, Ctrl+E | Go to File (`>` `@` `:` `?` switch it) |
| Help > Keyboard Shortcuts Reference | this table, made from your keys |
| Ctrl+N / Ctrl+O / Ctrl+R | New File / Open File / Open Project (recent folders) |
| Ctrl+S / Ctrl+Shift+S | Save / Save As |
| Ctrl+K S | Save All (each file by its language's settings) |
| Ctrl+W / Ctrl+Q | Close Editor / Exit (asks when not saved) |
| Ctrl+PgDn / Ctrl+PgUp | next / previous tab |
| Ctrl+Tab | the editors by use |
| Ctrl+Shift+H | Replace in Files |
| Ctrl+Shift+V, Ctrl+K V | Markdown preview, to the side |
| Ctrl+G, Ctrl+F, F3 | in the hex viewer: go to offset, find bytes or text, find next |
| + / - / 0 | in the picture preview: zoom in, out, fit |
| Ctrl+K W / Ctrl+K Ctrl+W / Ctrl+K U | close the group's / all / the saved editors |
| Ctrl+B | Toggle the sidebar |
| Ctrl+\\, Ctrl+K Ctrl+\\, Ctrl+1 .. 4 | Split Editor right / down, group 1 .. 4 |
| Ctrl+Alt+Right / Left, Ctrl+K Ctrl+M | Move Editor into Next / Previous Group, Toggle Editor Group Sizes |
| Ctrl+Space, F12, F8 / Shift+F8 | Trigger Suggest, Go to Definition, Next / Previous Problem |
| Ctrl+, | Settings (the Settings editor) |
| Shift+Alt+C, Ctrl+K Ctrl+Shift+C | Copy Path, Copy Relative Path |
| Shift+Alt+R | Reveal in File Explorer |
| F2, Delete, Ctrl+C / X / V in the Explorer | Rename, Delete, copy / move files |
| F2, Ctrl+., Ctrl+K Ctrl+I | Rename Symbol, Quick Fix, Show Hover |
| Ctrl+Shift+. | Focus and Select Breadcrumbs |
| Ctrl+Shift+M | Problems |
| Alt+Z, Ctrl+K Z | Word Wrap, Zen Mode |
| Ctrl+K Ctrl+T | Color Theme |
| Ctrl+K Ctrl+S | Keyboard Shortcuts |
| Ctrl+K D / Ctrl+K C | Compare Active File with Saved / with Clipboard |
| Ctrl+/, Shift+Alt+A | Toggle Line Comment, Toggle Block Comment |
| Ctrl+K Ctrl+C / Ctrl+K Ctrl+U | Add / Remove Line Comment |
| Shift+Alt+I, Ctrl+F2, Ctrl+K Ctrl+D | Cursors at Line Ends, Change All Occurrences, Move Selection to Next Match |
| Ctrl+U, Alt+Enter (in Find) | Cursor Undo, Select All Matches |
| Alt+PageUp / PageDown, Ctrl+M | Scroll a page, Tab Moves Focus |
| Shift+F12, Ctrl+F12, Alt+F12 | References, Implementations, Peek Definition |
| Ctrl+T | Go to Symbol in Workspace |
| Ctrl+Shift+Alt+arrows, Shift+Alt+drag | Column selection |
| Ctrl+Shift+\\, Ctrl+Shift+Space | Go to Bracket, Parameter Hints |
| Ctrl+Shift+T, Ctrl+K M | Reopen Closed Editor, Change Language Mode |
| Shift+Alt+F, Ctrl+Shift+I | Format Document |
| Ctrl+K Ctrl+F, Shift+Alt+O | Format Selection, Organize Imports |
| Shift+Alt+Right / Left | Expand / Shrink Selection |
| Shift+Alt+H | Show Call Hierarchy |
| Shift+Alt+Up / Down | Copy Line Up / Down |
| Ctrl+Shift+K, Ctrl+L | Delete Line, Expand Line Selection |
| Ctrl+Enter / Ctrl+Shift+Enter | Insert Line Below / Above |
| Ctrl+] / Ctrl+[ | Indent / Outdent Line |
| Alt+Left / Alt+Right | Go Back / Go Forward |
| Ctrl+Shift+O | Go to Symbol in Editor |
| Ctrl+; A / C / F / L | Test: Run All / at Cursor / Current File / Rerun Last |
| Ctrl+Shift+X | Extensions |
| Ctrl+Alt+I, Ctrl+I | Chat: Open Chat, Inline Chat (Ctrl+Enter accepts, Esc discards) |
| Tab / Shift+Tab in a snippet | the next / previous field |
| Ctrl+H | Replace |
| Ctrl+Shift+[ / ], Ctrl+K Ctrl+0 / J | Fold / Unfold, Fold All / Unfold All |
| Ctrl+` , Ctrl+J | Toggle the terminal (from it: hide it) |
| Ctrl+Shift+5, Alt+Left / Right | Split Terminal, the split next to it |
| Ctrl+F in the terminal | Find in the terminal |
| Ctrl+Shift+U | Toggle Output |
| Ctrl+Shift+E / F / G, or Alt+1 / 2 / 3 | Explorer / Search / Source Control |
| Alt+F, Alt+E ..., F10 | the menus |
| Ctrl+Z / Ctrl+Y | Undo / Redo |
| Ctrl+X / C / V | Cut / Copy / Paste (the whole line without a selection) |
| Ctrl+F, F3 / Shift+F3 | Find, next / previous |
| Ctrl+G | Go to Line (`12` or `12:5`) |
| Alt+Up / Alt+Down | Move Line Up / Down |
| Alt+Click, Ctrl+Alt+Up / Down | another cursor there / on the line above, below |
| Ctrl+D / Ctrl+Shift+L | the next place of the selection gets a cursor too / all of them |
| Esc | back to one cursor |
| Tab / Shift+Tab | indent / outdent |
| Esc, Tab | from the sidebar back to the editor |

Ctrl+Shift+letter needs a terminal with the kitty keyboard protocol (mmc-term,
kitty, WezTerm, foot, Ghostty); elsewhere F1 and Alt+1 / 2 / 3 do the same.
mmc-term keeps Ctrl+Shift+F, Ctrl+Shift+E and Ctrl+Tab for itself: use Alt+2,
Alt+1 and Ctrl+PgDn there.

## What comes next

Git worktrees, data breakpoints, workspace trust and screencast mode.

An extension's JavaScript cannot run here: mme reads what an extension
*describes* (its color themes, snippets, languages and TextMate grammars) and
uses the language servers and debug adapters it ships. Notebooks, remote
development and settings sync are not planned.
