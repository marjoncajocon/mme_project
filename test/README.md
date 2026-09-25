# mme regression suite

A repeatable screen-level regression suite for **mme**. Every scenario drives a
real `mme.exe` inside a ConPTY, presses real keys and clicks real pixels, and
compares the resulting terminal screen — character by character, colour by
colour — with a stored golden. A merge that changes what mme draws cannot slip
through silently.

```
python run.py                 run everything, compare with the goldens
python run.py --update        re-bless: this run becomes the golden
python run.py -k explorer     only the scenarios whose name contains "explorer"
python run.py --repeat 3      run the suite N times; any drift between passes fails
python run.py --list          every scenario, what it opens, and what it guards
python run.py --build         rebuild bin/ and pbin/ from tree/ and ptree/
python run.py --jobs 8        how many screen scenarios run at once (default 8)
```

`run.py` exits non-zero on any failure and prints, for each one, the scenario's
name, the one-line description of what it guards, and a unified diff of the
screen.

A full run is about **110 seconds** (62 scenarios): the screen scenarios run
eight at a time, the five performance scenarios run alone and three times each
so their timings mean something.

## What is here

| path | what it is |
| --- | --- |
| `run.py` | the runner and the scenario list — the whole suite is this one file |
| `mkfix.py` | builds `fix/`, the fixture tree, including the scratch git repo |
| `mkprof.py` | makes `ptree/` from `tree/` by adding the tick profiler |
| `settings.seed.json` | the `settings.json` every run starts from |
| `stublsp.py` | a fake language server that always answers the same thing, and stands in for GitHub Copilot's account surface |
| `stubclaude.py` | a fake Anthropic Messages API on a local port: checks the request, streams a canned answer (or a 401, a 529, a refusal) for the `chat-*` scenarios |
| `browser/` | a no-op `rundll32.exe` (and its source) that the sign-in scenarios put in front of the PATH, so opening a URL never opens a browser |
| `try.py` | scratch driver for working out a scenario's steps: `python try.py [-p] <target> <steps...>` |
| `hx.c` / `hx.exe` | `../harness.c` with background and attribute dumps added |
| `tree/` | a copy of the editor sources; `bin/mme.exe` is built from it |
| `ptree/` | the same sources plus an event-loop tick profiler; `pbin/mme.exe` |
| `fix/` | the fixtures (generated — never edit by hand, edit `mkfix.py`) |
| `golden/` | the expected output, one `<scenario>.txt` per scenario |
| `golden/perf-baseline.json` | today's event-loop tick numbers |
| `work/` | scratch: one folder per scenario, with its own exe and `mme-data` |

Nothing here writes to `E:\w\editor`. The suite builds its own copies of mme
with `MME_DEST` pointed inside `work/`, so the installed editor in
`D:\mmc-shell\usr\bin` is never touched either.

## Where things live

This folder is committed: the runner, the scenarios, the goldens, and the
sources of the two small programs the suite needs. Everything else is made:

| made by | what |
| --- | --- |
| `build.bat` | `hx.exe` (the ConPTY harness) and `browser
undll32.exe` |
| `run.py --build` | `tree/`, `ptree/`, `bin/`, `pbin/` - the editor's sources copied from `..` and built, plain and with the tick profiler |
| `mkfix.py` | the fixtures |
| a run | one work folder per scenario |

The fixtures and the work folders are deliberately **outside the checkout**,
under `%TEMP%\mme-test` (set `MME_TEST_WORK` to move them). They have to be:
inside the checkout every scenario would see the editor's own git repository,
mme would draw its branch in the SCM view and the shell would draw it in the
terminal panel's prompt. Masking that away would have hidden real git
behaviour, so the fixtures live where there is no repository to find - except
the three the suite creates on purpose (`gitrepo`, `dashrepo`, `wordrepo`).

`hx.c` uses mmc's terminal core, so the mmc checkout has to be beside this
one, or `MMC` has to point at it.

## First run on a fresh checkout

```
build                      # hx.exe and browserundll32.exe (needs zig and mmc)
python mkfix.py            # build the fixtures (needs git on the PATH)
python run.py --build      # copy ../*.c here and build bin/ and pbin/ with zig
python run.py              # compare with the goldens
```

`run.py --build` takes the editor's sources from the folder above every time,
so the suite can never be measuring a tree someone forgot to copy in.

After that, `python run.py` is the regression check.

## Testing a change to mme

```
copy the changed sources over tree/
python mkprof.py           # re-makes ptree/ from tree/, profiler and all
python run.py --build
python run.py
```

Every diff it prints is a change in what mme draws. Read each one: if the
change is the point of your edit, `python run.py --update` re-blesses it, and
the diff of `golden/` in your commit is a readable record of what the merge
changed on screen.

## How a scenario is written

A scenario is one `Scen(...)` in the list at the top of `run.py`:

```python
Scen("view-folding", "lang/fold.c",
     ["w:1000", "k:" + CTRL_K, "k:" + CTRL_0, "w:700", "d",
      "k:" + CTRL_K, "k:" + csiu("j", ctrl=True), "w:700", "d"],
     "Ctrl+K Ctrl+0 folds every block to its first line with the ... marker"),
```

* **name** — also the golden's file name. Keep the group prefix (`edit-`,
  `colour-`, `git-`, `view-` …) so `-k` can pick a group.
* **target** — a path under `fix/`, a file or a folder, or an absolute path.
* **steps** — what `hx.exe` does, in order:
  * `k:<keys>` — type. Backtick is ESC, `|` is Enter, `^X` is Ctrl-X. Keys with
    modifiers go in as kitty CSI u: use the `csiu()` helper, or one of the
    `CTRL_*` / `ALT_*` constants. A mouse click is `click(col, row)`
    (1 based); Alt+Click is `k:` with `` `[<8;col;rowM `` and `m`.
  * `w:<ms>` — wait. Every key already waits 250 ms.
  * `d` — dump the whole screen as text.
  * `c:<row>` — the row with a `{rrggbb}` marker wherever the **text colour**
    changes. This is the syntax-highlighting guard.
  * `b:<row>` — the same for the **background** colour: selections, the
    current line, find matches, the diff's added and removed lines.
  * `a:<row>` — the same for **attributes**: `<bold>`, `<italic>`,
    `<dim,under,ulstyle2>` (the curly underline of a diagnostic).
  * Row 0 is the menu bar, 1 the tab bar, 2 the breadcrumbs, 3 the first line
    of text, 29 the status bar. `COLOUR_ROWS` is `c:` over rows 3..20.
* **guards** — one sentence, printed when the scenario fails. Write what a
  failure would mean, not what the steps do.
* **settings=** — values written over `settings.seed.json` for this scenario
  (`tabs-preview` turns `workbench.editor.enablePreview` on).
* **stub_lsp=("c",)** — run `stublsp.py` as the language server for those
  languages, which is how the code lens, inlay hint, diagnostic and hover
  surfaces are tested without a real clangd.
* **inline_lsp="out"** — run `stublsp.py` as `mme.inlineCompletionServer`,
  which is where GitHub Copilot goes, with `--auth=` set to the account state
  it should pretend to be in: `out` (nobody signed in), `in` (signed in as
  `stubuser`), `error` (signed in, but the server is unhappy), `busy` (signed
  in and fetching) or `v2` (signed in, said only in the newer
  `didChangeStatus/v2` shape). `signIn` always answers with the device flow,
  and the command it names answers 1.2 s later **from a thread of its own** —
  the server keeps serving everything else meanwhile, so a scenario that
  drives the sign-in would show an editor that blocked on it as a screen that
  never changed.
* **fake_browser=True** — put `browser/` at the front of the PATH. mme opens a
  URL with `rundll32 url.dll,FileProtocolHandler <url>` and `find_program`
  takes the first `rundll32` on the PATH, so the sign-in scenarios hand the
  device-flow page to a program that writes it to `$MME_URLLOG` and exits
  instead of to a real browser window.
* **stub_claude="ok"** — run `stubclaude.py --mode=` (`ok`, `401`, `529`,
  `refusal`) on a free port and point `mme.chat.baseUrl` at it, with a dummy
  `mme.chat.apiKey`. Every run drops `ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN`
  and `ANTHROPIC_BASE_URL` from the environment, so no scenario can ever reach
  the real API with the user's key.
* **perf=True** — also record and assert the event-loop tick profile.
* **private=True** — give the scenario its own copy of the target, for a
  scenario that saves.

Then `python run.py --update -k <name>`, **read the golden it wrote**, and
check it really shows the thing the scenario claims to guard. A scenario whose
keys did nothing still produces a stable golden and a green tick; only reading
it catches that.

New fixture files go in `mkfix.py`, never straight into `fix/`.

## Determinism

A scenario has to give the same bytes on every run, or the suite is noise.
What is done about it:

* **A fresh, empty data folder per run.** mme keeps everything in `mme-data`
  next to the exe, so each scenario gets its own directory under `work/` with
  its own copy of `mme.exe` and a `mme-data` that is deleted and re-seeded
  before every run. The user's own settings, recent files, history and
  extensions can never leak in, and two scenarios running at once cannot
  collide.
* **A pinned `settings.json`.** `settings.seed.json` sets every setting mme
  has. The ones that matter for determinism:
  * `mme.languageServers` maps **every** language to `""`. A real language
    server answers differently on every machine and at every version, and its
    replies arrive whenever they arrive. (An empty `{}` is not enough: mme
    falls back to its built-in table.) Scenarios that want a server ask for
    `stublsp.py` instead.
  * `mme.extensions.useVSCodeExtensions: false` — the user's VS Code
    extensions are not part of this tree.
  * `workbench.startupEditor: "none"`, `files.hotExit: "off"`,
    `window.restoreWindows: "none"` — no session is carried in.
  * `editor.cursorBlinking: "solid"`, `editor.quickSuggestions` off — nothing
    appears or disappears on a timer.
* **Masks.** `build_masks()` in `run.py` replaces what cannot be pinned:
  any path naming this checkout (`<PATH>`), the user name and host name
  (`<USER>`, `<HOST>`), `"2 years ago"` in git blame (`<AGO>`), the OUTPUT
  channel's timestamps (`<DATETIME>`) and command timings (`[<MS>]`), and the
  version of the shell the terminal panel starts (`mmc <VER>`).
* **Toasts are read early, not waited out.** A notification stays 4 seconds
  (10 for an error), so a dump 600 ms after the action that raised it is well
  inside the window and stable. Do not put a dump near the 4 second mark.
* **A blank screen is a timing failure, not a diff.** Under load mme can still
  be starting when the first step runs, and the scenario then captures an empty
  grid. `looks_blank()` notices that and runs the scenario once more before
  reporting anything; a screen that is blank twice is reported as a real
  difference. This is why a scenario whose first step is a colour dump waits
  2500 ms rather than 1200: it has no screen dump to hide behind.
* **Proof.** `python run.py --repeat 3` runs everything three times and fails
  if any pass differs from the first. That is the check to run after adding a
  scenario, not just `run.py`.

## The performance guard

`ptree/` is `tree/` with a few lines added around the `while (!E.quit)` loop in
`main`: every iteration that costs more than 20 ms appends a line to the file
named by the `MME_TICKLOG` environment variable, splitting the cost into
`polls`, `draw` and the rest. `mkprof.py` puts them there, and refuses to guess
if the loop has moved — if it fails, its `EDITS` need updating. `run.py` gives each run its own log inside
`work/`.

A `perf=True` scenario is run three times, alone, and the **median** of the
runs is compared with `golden/perf-baseline.json`:

* `max_us` — the longest single event-loop tick.
* `over_20ms` — how many ticks went over 20 ms at all.

It fails when either goes over `max(4 x baseline, floor)` — 150 ms for the
tick, 8 for the count. One draw of a 500 KB line varies by a factor of two on a
busy machine, so the threshold is deliberately loose: it is there to catch a
40x regression (the tokenizer hang that used to live in the 8000-20000 byte
line window), not to measure milliseconds.

The perf scenarios are `mini-md5` (a 12.7 KB line), `mini-mid` (a generated
~12 KB line, the middle of that window), `mini-boot-css` (190 KB on one line,
past `editor.maxTokenizationLineLength`), `mini-found-js` (531 KB on one line)
and `mini-big-c` (1400 lines, scrolled).

To move the baseline after a deliberate change: `python run.py --update -k mini-`.

## What each group guards

| group | a failure means |
| --- | --- |
| `edit-*` | typing, undo/redo, selection and its colours, clipboard, PgDn/Home/End, multi-cursor by Ctrl+D and by Alt+Click |
| `colour-*` | the syntax highlighter for C, Go, TSX, JS, CSS, Markdown, JSON, Python, and bracket-pair colouring |
| `hl-invalidate-*` | the incremental re-highlight: a `/*` or a backtick recolours every line below it, and undo puts the colours back **exactly** |
| `mini-*` | minified and very long lines still draw, and the event loop still ticks |
| `explorer-*` | the tree, compact folders, expanding, the scrollbar, OPEN EDITORS |
| `tabs-*` | opening, switching by click, closing, pinning, preview replacement and the italics of a preview tab that is not in front |
| `palette-*`, `gotofile*` | the command palette and Go to File, and their filtering |
| `find-*`, `replace-*`, `search-*` | find and replace in the file and across the folder, and the colours of the matches |
| `editorconfig-*` | `.editorconfig`: the indent it says over the detected one, its globs, a closer file winning, `root = true`, charset, and what a save does (trim, final newline, CR LF) |
| `git-*` | the gutter bars, the blame decoration, SOURCE CONTROL, the diff editor and its backgrounds, file history |
| `lsp-*` | code lens rows, inlay hints, PROBLEMS, hover and the diagnostic squiggle, pull diagnostics (`--pull`), color decorators and the color picker (`--colors`), document links (`--links`), the imports a rename in the Explorer updates (`--rename`) — all against `stublsp.py` |
| `copilot-*` | the GitHub Copilot status item (its icon, its dimming, its hover and its menu), the sign-in device flow end to end, sign-out, and the three palette commands — all against `stublsp.py --auth=...`, never a real account |
| `chat-*` | the Chat view and Inline Chat against `stubclaude.py`: the streamed Markdown answer, the implicit context, Insert and Apply, the diff's Accept and Discard, the missing key, HTTP errors and a refusal |
| `panel-*` | the terminal, PROBLEMS and OUTPUT tabs of the panel |
| `page-*` | the Settings editor and the Keyboard Shortcuts list |
| `view-*` | word wrap, the minimap, sticky scroll, folding, the Markdown preview, the side bar |
| `vim-*` | Vim mode (vim.enable): the modes in the status bar, motions and counts, operators and text objects, Visual line and block, `.`, undo, `/` search, `:s`, `:w`, macros |

## What the suite cannot cover

* **A real GitHub account.** `stublsp.py` answers `signIn`, `signOut`,
  `checkStatus` and `didChangeStatus` the way Copilot's server does, so the
  whole sign-in can be driven, but nothing here ever talks to GitHub: the
  device code is never a real one and `signOut` is never sent to the real
  server, which would throw away the user's credentials.
* **Anything that needs a real language server.** Go to Definition, Rename
  Symbol, semantic highlighting, completion and formatting all come from
  clangd/gopls/tsserver. `stublsp.py` covers the *rendering* of lenses, hints,
  diagnostics and hovers; it cannot cover mme's handling of a real server's
  answers, and installing one would make the suite machine dependent.
* **Anything graphical.** Images (`eimage.c`), sixel and the mmc-term window
  are outside a text grid. The harness reads a `Grid`, not pixels.
* **Colours the terminal never sees.** Only the foreground, the background and
  the SGR attributes of each cell are compared. A cursor shape change, a mouse
  hover effect that needs a real pointer, or a change in the escape sequences
  mme emits that renders identically, all pass.
* **The network.** Git push/pull/clone/sync, and downloading extensions.
* **Debugging.** A debug adapter (`dlv`, `debugpy`) is another external
  program, with the same problem as a language server.
* **Real timing under load.** The tick numbers are a smoke alarm, not a
  benchmark: they catch an order-of-magnitude regression, not a 20% one.
* **Other platforms.** `hx.exe` uses ConPTY, so the suite is Windows only,
  although mme itself is not.
