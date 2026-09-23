/*
** econfig.c - settings.json, VS Code's settings with VS Code's names
**
** settings.json (in mme-data) is made the first time with every setting there is
** and its default. Ctrl+, opens it in a tab; saving it applies it at once.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifdef _WIN32	/* the default terminal profile's setting, as VS Code names it */
#define PROFILE_SETTING	"terminal.integrated.defaultProfile.windows"
#define PROFILE_KEY	"terminal\\.integrated\\.defaultProfile\\.windows"
#elif defined(__APPLE__)
#define PROFILE_SETTING	"terminal.integrated.defaultProfile.osx"
#define PROFILE_KEY	"terminal\\.integrated\\.defaultProfile\\.osx"
#else
#define PROFILE_SETTING	"terminal.integrated.defaultProfile.linux"
#define PROFILE_KEY	"terminal\\.integrated\\.defaultProfile\\.linux"
#endif


Opt opt = {4, 1, 1, 1, 1, 1, 0, 0, 1, 0, "Dark Modern", 1, 1, "", "", 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, {0}, 0, 1, 1, 1, 1, 1, 1, 1, 1,
           0, 1000, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1,
           1, 1, 1, 0, 1, 1, 0, 0, 1000, 1, 0, "", 0, 1, 20000,	/* term_cwd, preview_tabs, textmate, tm_max_line */
           300, 1,	/* hover_delay, hover_sticky */
           1, 1, 1, 1, 1, 0, 1, 0, 1,	/* explorer.* */
           1, "", 0, 0,	/* command_center, win_title, panel_loc, panel_justify */
           1, 1,	/* markdown.preview.scroll* */
           1};	/* merge_editor */

static Json *g_json;	/* the file as read, for what is looked up later */

static const char default_file[] =
  "// mme settings, like VS Code's settings.json: \"name\": value,\n"
  "// Saving this file applies it.\n"
  "{\n"
  "  // Dark Modern, Dark+, Light+, Monokai, Solarized Dark (Ctrl+K Ctrl+T)\n"
  "  \"workbench.colorTheme\": \"Dark Modern\",\n"
  "  // columns a tab takes, and the indent of a new file\n"
  "  \"editor.tabSize\": 4,\n"
  "  // Tab puts spaces (true) or a tab (false)\n"
  "  \"editor.insertSpaces\": true,\n"
  "  // tabs or spaces, and how many, from what the file has\n"
  "  \"editor.detectIndentation\": true,\n"
  "  \"editor.minimap.enabled\": true,\n"
  "  // \"on\" or \"off\" (Alt+Z)\n"
  "  \"editor.wordWrap\": \"off\",\n"
  "  // \"on\", \"off\", \"relative\" or \"interval\"\n"
  "  \"editor.lineNumbers\": \"on\",\n"
  "  // \"always\" or \"never\": ( [ { \" ' ` close themselves\n"
  "  \"editor.autoClosingBrackets\": \"always\",\n"
  "  // brackets colored by how deep they are\n"
  "  \"editor.bracketPairColorization.enabled\": true,\n"
  "  // the scopes the top line is in stay at the top of the editor\n"
  "  \"editor.stickyScroll.enabled\": true,\n"
  "  // how many scopes it keeps at the top at most\n"
  "  \"editor.stickyScroll.maxLineCount\": 5,\n"
  "  // the other places of the symbol at the cursor are lit\n"
  "  \"editor.occurrencesHighlight\": true,\n"
  "  // thin lines at each indent level, the block of the cursor brighter\n"
  "  \"editor.guides.indentation\": true,\n"
  "  \"editor.guides.highlightActiveIndentation\": true,\n"
  "  // vertical lines at these columns, e.g. [80, 120]\n"
  "  \"editor.rulers\": [],\n"
  "  // \"on\" or \"off\": the language server's hints in the text (parameter names, types)\n"
  "  \"editor.inlayHints.enabled\": \"on\",\n"
  "  // the language server's actions over functions (references, run test ...)\n"
  "  \"editor.codeLens\": true,\n"
  "  // the language server colors names by what they are\n"
  "  \"editor.semanticHighlighting.enabled\": true,\n"
  "  // \"onCode\" or \"off\": the lightbulb when there are code actions\n"
  "  \"editor.lightbulb.enabled\": \"onCode\",\n"
  ;
static const char default_file1[] =
  "  // the language server formats the file when it is saved (Shift+Alt+F: now)\n"
  "  \"editor.formatOnSave\": false,\n"
  "  // the pasted text formatted, and the line as ; } are typed (the language server)\n"
  "  \"editor.formatOnPaste\": false,\n"
  "  \"editor.formatOnType\": false,\n"
  "  // {\"source.organizeImports\": \"explicit\"}: the imports sorted when saving\n"
  "  \"editor.codeActionsOnSave\": {},\n"
  "  // an HTML tag's name typed changes its closing tag's too\n"
  "  \"editor.linkedEditing\": false,\n"
  "  // the tests' icons (run, passed, failed) by their lines, like VS Code's Testing\n"
  "  \"testing.gutterEnabled\": true,\n"
  "  // \"on\", \"off\" or \"auto\": while debugging, the values after the lines\n"
  "  \"debug.inlineValues\": \"auto\",\n"
  "  // the words of the file are suggested too\n"
  "  \"editor.wordBasedSuggestions\": true,\n"
  "  // \"inline\" or \"none\": snippets in the suggestions (mme-data/snippets)\n"
  "  \"editor.snippetSuggestions\": \"inline\",\n"
  "  // \"none\", \"selection\" or \"all\": spaces as dots, tabs as arrows\n"
  "  \"editor.renderWhitespace\": \"selection\",\n"
  "  \"files.trimTrailingWhitespace\": false,\n"
  "  \"files.insertFinalNewline\": false,\n"
  "  \"workbench.sideBar.visible\": true,\n"
  "  // a file clicked once opens in a tab that the next one replaces, as VS Code does;\n"
  "  // false (mme's default): every file gets its own tab\n"
  "  \"workbench.editor.enablePreview\": false,\n"
  "  // \"welcomePage\" or \"none\": what shows at startup when no file is opened\n"
  "  \"workbench.startupEditor\": \"welcomePage\",\n"
  "  // \"left\" or \"right\": where the side bar and the activity bar are\n"
  "  \"workbench.sideBar.location\": \"left\",\n"
  "  // \"classic\" or \"hidden\"; \"default\" or \"hidden\"\n"
  "  \"window.menuBarVisibility\": \"classic\",\n"
  "  \"workbench.statusBar.visible\": true,\n"
  "  \"workbench.activityBar.location\": \"default\",\n"
  "  // \"off\", \"afterDelay\", \"onFocusChange\" or \"onWindowChange\" (File > Auto Save)\n"
  "  \"files.autoSave\": \"off\",\n"
  "  \"files.autoSaveDelay\": 1000,\n"
  "  // \"onExit\": quitting keeps unsaved changes for the next time, no questions; \"off\"\n"
  "  \"files.hotExit\": \"onExit\",\n"
  "  // \"all\": the folder's editors come back when it is opened again; \"none\"\n"
  "  \"window.restoreWindows\": \"all\",\n"
  "  // utf8, utf8bom, utf16le, utf16be, windows1252, iso88591: for files without a BOM\n"
  "  \"files.encoding\": \"utf8\",\n"
  "  // line, block, underline, line-thin, block-outline, underline-thin\n"
  "  \"editor.cursorStyle\": \"line\",\n"
  "  // \"blink\" or \"solid\"\n"
  "  \"editor.cursorBlinking\": \"blink\",\n"
  "  // who changed the cursor's line and when, after it (Git: Toggle Git Blame Editor Decoration)\n"
  "  \"git.blame.editorDecoration.enabled\": true,\n"
  "  // and in the status bar\n"
  "  \"git.blame.statusBarItem.enabled\": true,\n"
  "  // commit all the changes when none is staged (Ctrl+Enter)\n"
  "  \"git.enableSmartCommit\": false,\n"
  "  // a file with merge conflicts opens in the merge editor (Incoming, Current, Result)\n"
  "  \"git.mergeEditor\": true,\n"
  "  // \"all\" or \"none\": the gutter's bars of the changes against git's index\n"
  "  \"scm.diffDecorations\": \"all\",\n"
  "  // the terminal's shell; empty: mmc-shell, else cmd / $SHELL\n"
  "  \"terminal.integrated.shell\": \"\",\n"
  "  // the shell of new terminals, by its name (Terminal: Select Default Profile)\n"
  "  \"" PROFILE_SETTING "\": \"\",\n"
  "  // the color themes, snippets and languages of the extensions VS Code has\n"
  "  \"mme.extensions.useVSCodeExtensions\": true,\n"
  "  // VS Code's TextMate grammars (from VS Code and its extensions) color the code\n"
  "  \"editor.textmateGrammars\": true,\n"
  "  // a longer line is left to mme's own highlighter: a minified file stays quick\n"
  "  \"editor.maxTokenizationLineLength\": 20000,\n"
  ;
static const char default_edit[] =	/* the editing settings */
  "  // how far a mouse wheel notch scrolls: 1 is three lines, like VS Code\n"
  "  \"editor.mouseWheelScrollSensitivity\": 1,\n"
  "  // \"none\", \"keep\", \"brackets\", \"advanced\" or \"full\": how Enter and typing indent\n"
  "  \"editor.autoIndent\": \"full\",\n"
  "  // pasted lines are moved to the cursor's indent\n"
  "  \"editor.autoIndentOnPaste\": false,\n"
  "  // \"on\" or \"onlySnippets\": a snippet's prefix, then Tab, puts it in\n"
  "  \"editor.tabCompletion\": \"off\",\n"
  "  // \"on\", \"smart\" (only when it changes the text) or \"off\"\n"
  "  \"editor.acceptSuggestionOnEnter\": \"on\",\n"
  "  // \"first\", \"recentlyUsed\" or \"recentlyUsedByPrefix\"\n"
  "  \"editor.suggestSelection\": \"first\",\n"
  "  \"editor.quickSuggestions\": {\"other\": \"on\", \"comments\": \"off\", \"strings\": \"off\"},\n"
  "  \"editor.suggestOnTriggerCharacters\": true,\n"
  "  // \"alt\" or \"ctrlCmd\": the key that Click adds a cursor with (the other one goes to the definition)\n"
  "  \"editor.multiCursorModifier\": \"alt\",\n"
  "  // \"spread\" (a line to each cursor) or \"full\"\n"
  "  \"editor.multiCursorPaste\": \"spread\",\n"
  "  // Ctrl+C / Ctrl+X without a selection take the line\n"
  "  \"editor.emptySelectionClipboard\": true,\n"
  "  \"editor.renderControlCharacters\": true,\n"
  "  \"editor.unicodeHighlight.ambiguousCharacters\": true,\n"
  "  \"editor.unicodeHighlight.invisibleCharacters\": true,\n"
  "  // \"none\", \"gutter\", \"line\" or \"all\"\n"
  "  \"editor.renderLineHighlight\": \"line\",\n"
  "  // lines kept visible above and below the cursor\n"
  "  \"editor.cursorSurroundingLines\": 0,\n"
  "  \"editor.scrollBeyondLastLine\": true,\n"
  ;
static const char default_view[] =	/* the minimap's, the diff editor's and the tabs' */
  "  // git colors the name of a changed file's tab, and its letter (M, U, A, D)\n"
  "  \"workbench.editor.decorations.colors\": true,\n"
  "  \"workbench.editor.decorations.badges\": true,\n"
  "  // \"right\" or \"left\"; \"mouseover\" or \"always\": the box of what the editor shows\n"
  "  \"editor.minimap.side\": \"right\",\n"
  "  \"editor.minimap.showSlider\": \"mouseover\",\n"
  "  // true: the text in small; false: blocks of color\n"
  "  \"editor.minimap.renderCharacters\": true,\n"
  "  // the columns it shows, and 1, 2 or 3: how big a line is in it\n"
  "  \"editor.minimap.maxColumn\": 120,\n"
  "  \"editor.minimap.scale\": 1,\n"
  "  // the diff editor: spaces at the start and end of lines are not changes\n"
  "  \"diffEditor.ignoreTrimWhitespace\": true,\n"
  "  // long runs of lines with no change fold into one row (a click opens them)\n"
  "  \"diffEditor.hideUnchangedRegions.enabled\": false,\n"
  "  // false: the old lines above the new ones (inline)\n"
  "  \"diffEditor.renderSideBySide\": true,\n"
  ;
static const char default_hover[] =	/* a part of its own (4095 bytes a literal at most) */
  "  // ms the mouse rests on a name before its hover shows\n"
  "  \"editor.hover.delay\": 300,\n"
  "  // the mouse can go into the hover (to scroll it, click its links)\n"
  "  \"editor.hover.sticky\": true,\n"
  ;
static const char default_term[] =	/* the terminal's; a part of its own (4095 bytes a literal at most) */
  "  // the shell marks its prompts and commands: circles by them, Run Recent Command (Ctrl+Alt+R)\n"
  "  \"terminal.integrated.shellIntegration.enabled\": true,\n"
  "  // \"both\" or \"never\": the circles by the commands\n"
  "  \"terminal.integrated.shellIntegration.decorationsEnabled\": \"both\",\n"
  "  \"terminal.integrated.stickyScroll.enabled\": true,\n"
  "  // a selection made with the mouse goes to the clipboard at once\n"
  "  \"terminal.integrated.copyOnSelection\": false,\n"
  "  // \"copyPaste\", \"paste\", \"selectWord\", \"default\" (a menu) or \"nothing\"\n"
  "  \"terminal.integrated.rightClickBehavior\": \"copyPaste\",\n"
  "  // \"auto\", \"always\" or \"never\": ask before pasting more than one line\n"
  "  \"terminal.integrated.enableMultiLinePasteWarning\": \"auto\",\n"
  "  // \"block\", \"line\" or \"underline\"\n"
  "  \"terminal.integrated.cursorStyle\": \"block\",\n"
  "  \"terminal.integrated.cursorBlinking\": false,\n"
  "  \"terminal.integrated.scrollback\": 1000,\n"
  "  // the folder new terminals start in (relative: from the one open)\n"
  "  \"terminal.integrated.cwd\": \"\",\n"
  "  // more environment variables for the shell: {\"NAME\": \"value\"}\n"
  "  \"terminal.integrated.env.windows\": {},\n"
  "  // \"never\", \"editor\", \"panel\" or \"always\": ask before a terminal is killed\n"
  "  \"terminal.integrated.confirmOnKill\": \"editor\",\n"
  "  // \"never\", \"always\" or \"hasChildProcesses\": ask before quitting with terminals\n"
  "  \"terminal.integrated.confirmOnExit\": \"never\",\n"
  ;
static const char default_explorer[] =	/* the Explorer's */
  "  // ask before a drag and drop moves files, and before deleting\n"
  "  \"explorer.confirmDragAndDrop\": true,\n"
  "  \"explorer.confirmDelete\": true,\n"
  "  // files with problems in red / yellow, and their count\n"
  "  \"explorer.decorations.colors\": true,\n"
  "  \"explorer.decorations.badges\": true,\n"
  "  // folders with a single folder in them on one row: \"a/b/c\"\n"
  "  \"explorer.compactFolders\": true,\n"
  "  // related files under one (a.ts: a.js; package.json: package-lock.json ...)\n"
  "  \"explorer.fileNesting.enabled\": false,\n"
  "  \"explorer.fileNesting.expand\": true,\n"
  "  // default, mixed, filesFirst, type, modified\n"
  "  \"explorer.sortOrder\": \"default\",\n"
  "  // the file in front is selected in the Explorer\n"
  "  \"explorer.autoReveal\": true,\n"
  "  // what the Explorer, Go to File and Search leave out\n"
  "  \"files.exclude\": {\"**/.git\": true, \"**/.svn\": true, \"**/.hg\": true, \"**/CVS\": true, \"**/.DS_Store\": true, \"**/Thumbs.db\": true},\n"
  ;
static const char default_window[] =	/* the window's */
  "  // the search box in the title bar (Go to File); back and forward beside it\n"
  "  \"window.commandCenter\": true,\n"
  "  // empty: \"${dirty}${activeEditorShort}${separator}${rootName}${separator}${appName}\"\n"
  "  \"window.title\": \"\",\n"
  "  // \"bottom\", \"right\" or \"left\": where the panel (terminal, problems ...) is\n"
  "  \"workbench.panel.defaultLocation\": \"bottom\",\n"
  "  // \"center\": under the editors; \"justify\": the whole width\n"
  "  \"workbench.panel.alignment\": \"center\",\n"
  ;
static const char default_file2[] =	/* the rest: one literal may not be longer than 4095 */
  "  // the language servers (IntelliSense): a command per language\n"
  "  \"mme.languageServers\": {\n"
  "    \"c\": \"clangd\",\n"
  "    \"cpp\": \"clangd\",\n"
  "    \"go\": \"gopls\",\n"
  "    \"rust\": \"rust-analyzer\",\n"
  "    \"python\": \"pylsp\",\n"
  "    \"javascript\": \"typescript-language-server --stdio\",\n"
  "    \"typescript\": \"typescript-language-server --stdio\",\n"
    "    \"zig\": \"zls\",\n"
  "    \"lua\": \"lua-language-server\",\n"
  "    \"csharp\": \"csharp-ls\",\n"
  "    \"java\": \"jdtls\",\n"
  "    \"php\": \"intelephense --stdio\",\n"
  "    \"ruby\": \"solargraph stdio\",\n"
  "    \"kotlin\": \"kotlin-language-server\",\n"
  "    \"dart\": \"dart language-server\",\n"
  "    \"haskell\": \"haskell-language-server-wrapper --lsp\",\n"
  "    \"elixir\": \"elixir-ls\",\n"
  "    \"html\": \"vscode-html-language-server --stdio\",\n"
  "    \"css\": \"vscode-css-language-server --stdio\",\n"
  "    \"json\": \"vscode-json-language-server --stdio\",\n"
  "    \"shellscript\": \"bash-language-server start\",\n"
  "    \"yaml\": \"yaml-language-server --stdio\",\n"
  "    \"dockerfile\": \"docker-langserver --stdio\"\n"
  "  },\n"
  "\n"
  "  // the Markdown preview follows the editor it is beside, and the editor follows it\n"
  "  \"markdown.preview.scrollPreviewWithEditor\": true,\n"
  "  \"markdown.preview.scrollEditorWithPreview\": true\n"
  "}\n";


/*
** {==================================================================
** The data folder
** ===================================================================
*/

static char *g_data;	/* where mme keeps its files */


static int is_dir (const char *p) {
  OsStat st;
  return os_stat(p, &st) == 0 && st.exists && st.is_dir;
}


/*
** Like VS Code's portable mode: everything mme keeps (settings.json,
** keybindings.json, snippets, extensions, recent folders) is in the folder
** mme-data next to the program (D:\mmc-shell\usr\bin\mme-data), made the
** first time. Moving the program's folder takes its data along.
*/
void data_init (const char *argv0) {
  char *exe = os_exe_path(argv0), *dir;
  dir = exe ? path_dirname(exe) : xstrdup(".");
  g_data = path_join(dir, "mme-data");
  free(dir);
  free(exe);
  if (!is_dir(g_data)) mkdir_p(g_data);
}


/* the data folder */
const char *data_dir (void) {
  return g_data ? g_data : ".";
}


/* a file or folder in it */
char *data_path (const char *name) {
  return path_join(data_dir(), name);
}


/* settings.json, in the data folder */
char *settings_path (void) {
  return data_path("settings.json");
}

/* }================================================================== */


/* makes the file with the defaults when there is none */
void settings_create (void) {
  char *f = settings_path();
  OsStat st;
  if (f && (os_stat(f, &st) != 0 || !st.exists)) {
    int fd = os_open(f, OS_WRITE);
    if (fd >= 0) {
      os_write(fd, default_file, sizeof(default_file) - 1);
      os_write(fd, default_file1, sizeof(default_file1) - 1);
      os_write(fd, default_edit, sizeof(default_edit) - 1);
      os_write(fd, default_view, sizeof(default_view) - 1);
      os_write(fd, default_term, sizeof(default_term) - 1);
      os_write(fd, default_hover, sizeof(default_hover) - 1);
      os_write(fd, default_window, sizeof(default_window) - 1);
      os_write(fd, default_explorer, sizeof(default_explorer) - 1);
      os_write(fd, default_file2, sizeof(default_file2) - 1);
      os_close(fd);
    }
  }
  free(f);
}


static int clamp (int v, int lo, int hi) {
  return v < lo ? lo : v > hi ? hi : v;
}


/* reads the file; 0 ok (or no file), -1 when it is not good JSON */
/* src's members over dst's (both objects): a copy of each, the old one of that name goes */
static void overlay (Json *dst, const Json *src) {
  size_t i, k;
  for (i = 0; src && src->type == J_OBJ && i < src->n; i++) {
    Buf b;
    Json *c;
    buf_init(&b);
    json_write(&b, src->kid[i]);
    c = json_parse(b.s, b.len);
    buf_free(&b);
    if (c == NULL) continue;
    c->key = xstrdup(src->kid[i]->key);
    for (k = 0; k < dst->n && strcmp(dst->kid[k]->key, c->key) != 0; k++) ;
    if (k < dst->n) {
      json_free(dst->kid[k]);
      dst->kid[k] = c;
      continue;
    }
    dst->kid = (Json **)xrealloc(dst->kid, (dst->n + 1) * sizeof(Json *));
    dst->kid[dst->n++] = c;
    dst->cap = dst->n;
  }
}


/*
** The settings over the user's, like VS Code: a workspace's "settings", or
** the folder's .vscode/settings.json.
*/
static void overlay_workspace (Json *j) {
  if (ws_active()) overlay(j, ws_settings());
  else {
    char *d = path_join(side_root(), ".vscode"), *f = path_join(d, "settings.json"), *s;
    size_t len;
    free(d);
    if ((s = read_file(f, &len)) != NULL) {
      Json *w = json_parse(s, len);
      if (w && w->type == J_OBJ) overlay(j, w);
      json_free(w);
      free(s);
    }
    free(f);
  }
}


int settings_load (void) {
  char *f = settings_path(), *s;
  size_t len;
  Json *j;
  const char *ws;
  if (f == NULL) return 0;
  s = read_file(f, &len);
  free(f);
  j = s ? json_parse(s, len) : json_parse("{}", 2);	/* no file: the defaults, and the workspace's */
  free(s);
  if (j == NULL || j->type != J_OBJ) {
    json_free(j);
    return -1;
  }
  overlay_workspace(j);
  json_free(g_json);
  g_json = j;
  opt.tab_size = clamp((int)json_num(json_get(j, "editor\\.tabSize"), 4), 1, 16);
  opt.insert_spaces = json_bool(json_get(j, "editor\\.insertSpaces"), 1);
  opt.detect_indent = json_bool(json_get(j, "editor\\.detectIndentation"), 1);
  opt.minimap = json_bool(json_get(j, "editor\\.minimap\\.enabled"), 1);
  opt.line_numbers = strcmp(json_str(json_get(j, "editor\\.lineNumbers"), "on"), "off") != 0;
  ws = json_str(json_get(j, "editor\\.renderWhitespace"), "selection");
  opt.render_ws = strcmp(ws, "all") == 0 ? 2 : strcmp(ws, "none") == 0 ? 0 : 1;
  opt.trim_ws = json_bool(json_get(j, "files\\.trimTrailingWhitespace"), 0);
  opt.final_newline = json_bool(json_get(j, "files\\.insertFinalNewline"), 0);
  opt.sidebar = json_bool(json_get(j, "workbench\\.sideBar\\.visible"), 1);
  opt.word_wrap = strcmp(json_str(json_get(j, "editor\\.wordWrap"), "off"), "on") == 0;
  snprintf(opt.theme, sizeof(opt.theme), "%s", json_str(json_get(j, "workbench\\.colorTheme"), "Dark Modern"));
  opt.auto_close = strcmp(json_str(json_get(j, "editor\\.autoClosingBrackets"), "always"), "never") != 0;
  opt.pair_colors = json_bool(json_get(j, "editor\\.bracketPairColorization\\.enabled"), 1);
  snprintf(opt.shell, sizeof(opt.shell), "%s", json_str(json_get(j, "terminal\\.integrated\\.shell"), ""));
  snprintf(opt.term_profile, sizeof(opt.term_profile), "%s", json_str(json_get(j, PROFILE_KEY), ""));
  opt.format_on_save = json_bool(json_get(j, "editor\\.formatOnSave"), 0);
  opt.blame_line = json_bool(json_get(j, "git\\.blame\\.editorDecoration\\.enabled"), 1);
  opt.blame_status = json_bool(json_get(j, "git\\.blame\\.statusBarItem\\.enabled"), 1);
  opt.word_suggest = json_bool(json_get(j, "editor\\.wordBasedSuggestions"), 1);
  opt.snippet_suggest = strcmp(json_str(json_get(j, "editor\\.snippetSuggestions"), "inline"), "none") != 0;
  opt.vscode_ext = json_bool(json_get(j, "mme\\.extensions\\.useVSCodeExtensions"), 1);
  opt.sticky = json_bool(json_get(j, "editor\\.stickyScroll\\.enabled"), 1);
  opt.word_hl = json_bool(json_get(j, "editor\\.occurrencesHighlight"), 1);
  opt.emmet_tab = json_bool(json_get(j, "emmet\\.triggerExpansionOnTab"), 1);
  opt.emmet_suggest = strcmp(json_str(json_get(j, "emmet\\.showExpandedAbbreviation"), "always"), "never") != 0;
  opt.close_tags = json_bool(json_get(j, "html\\.autoClosingTags"), 1);
  opt.drag_drop = json_bool(json_get(j, "editor\\.dragAndDrop"), 1);
  opt.startup_welcome = strcmp(json_str(json_get(j, "workbench\\.startupEditor"), "welcomePage"), "welcomePage") == 0;
  {
    static const char *const as[] = {"off", "afterDelay", "onFocusChange", "onWindowChange"};
    static const char *const cs[] = {"line", "block", "underline", "line-thin", "block-outline", "underline-thin"};
    const char *v = json_str(json_get(j, "files\\.autoSave"), "off");
    int i;
    opt.auto_save = AUTO_OFF;
    for (i = 0; i < 4; i++)
      if (strcmp(v, as[i]) == 0) opt.auto_save = i;
    opt.auto_save_delay = clamp((int)json_num(json_get(j, "files\\.autoSaveDelay"), 1000), 100, 600000);
    opt.hot_exit = strcmp(json_str(json_get(j, "files\\.hotExit"), "onExit"), "off") != 0;
    opt.restore = strcmp(json_str(json_get(j, "window\\.restoreWindows"), "all"), "none") != 0;
    i = enc_by_id(json_str(json_get(j, "files\\.encoding"), "utf8"));
    opt.encoding = i < 0 ? ENC_UTF8 : i;
    v = json_str(json_get(j, "editor\\.cursorStyle"), "line");
    opt.cursor_style = CUR_LINE;
    for (i = 0; i < 6; i++)
      if (strcmp(v, cs[i]) == 0) opt.cursor_style = i;
    opt.cursor_blink = strcmp(json_str(json_get(j, "editor\\.cursorBlinking"), "blink"), "solid") != 0;
    opt.side_right = strcmp(json_str(json_get(j, "workbench\\.sideBar\\.location"), "left"), "right") == 0;
    opt.test_gutter = json_bool(json_get(j, "testing\\.gutterEnabled"), 1);
    opt.preview_tabs = json_bool(json_get(j, "workbench\\.editor\\.enablePreview"), 0);	/* VS Code: true */
    opt.md_scroll_preview = json_bool(json_get(j, "markdown\\.preview\\.scrollPreviewWithEditor"), 1);
    opt.md_scroll_editor = json_bool(json_get(j, "markdown\\.preview\\.scrollEditorWithPreview"), 1);
    opt.inline_values = strcmp(json_str(json_get(j, "debug\\.inlineValues"), "auto"), "off") != 0;
    {	/* the terminal */
      const Json *d = json_get(j, "terminal\\.integrated\\.shellIntegration\\.decorationsEnabled");
      static const char *const rc[] = {"default", "copyPaste", "paste", "selectWord", "nothing"};
      static const char *const cs[] = {"block", "line", "underline"};
      static const char *const ck[] = {"never", "editor", "panel", "always"};
      int k;
      opt.term_shell_int = json_bool(json_get(j, "terminal\\.integrated\\.shellIntegration\\.enabled"), 1);
      opt.term_decor = d && d->type == J_BOOL ? d->b : strcmp(json_str(d, "both"), "never") != 0;
      opt.term_sticky = json_bool(json_get(j, "terminal\\.integrated\\.stickyScroll\\.enabled"), 1);
      opt.term_copy_sel = json_bool(json_get(j, "terminal\\.integrated\\.copyOnSelection"), 0);
#ifdef _WIN32
      v = json_str(json_get(j, "terminal\\.integrated\\.rightClickBehavior"), "copyPaste");
#elif defined(__APPLE__)
      v = json_str(json_get(j, "terminal\\.integrated\\.rightClickBehavior"), "selectWord");
#else
      v = json_str(json_get(j, "terminal\\.integrated\\.rightClickBehavior"), "default");
#endif
      opt.term_right_click = RC_COPY_PASTE;
      for (k = 0; k < 5; k++)
        if (strcmp(v, rc[k]) == 0) opt.term_right_click = k;
      d = json_get(j, "terminal\\.integrated\\.enableMultiLinePasteWarning");
      v = d && d->type == J_BOOL ? (d->b ? "auto" : "never") : json_str(d, "auto");
      opt.term_paste_warn = strcmp(v, "never") == 0 ? 0 : strcmp(v, "always") == 0 ? 2 : 1;
      v = json_str(json_get(j, "terminal\\.integrated\\.cursorStyle"), "block");
      opt.term_cursor_style = 0;
      for (k = 0; k < 3; k++)
        if (strcmp(v, cs[k]) == 0) opt.term_cursor_style = k;
      opt.term_cursor_blink = json_bool(json_get(j, "terminal\\.integrated\\.cursorBlinking"), 0);
      opt.term_scrollback = clamp((int)json_num(json_get(j, "terminal\\.integrated\\.scrollback"), 1000), 100, 100000);
      v = json_str(json_get(j, "terminal\\.integrated\\.confirmOnKill"), "editor");
      opt.term_confirm_kill = 1;
      for (k = 0; k < 4; k++)
        if (strcmp(v, ck[k]) == 0) opt.term_confirm_kill = k;
      v = json_str(json_get(j, "terminal\\.integrated\\.confirmOnExit"), "never");
      opt.term_confirm_exit = strcmp(v, "always") == 0 ? 1 : strcmp(v, "hasChildProcesses") == 0 ? 2 : 0;
      snprintf(opt.term_cwd, sizeof(opt.term_cwd), "%s", json_str(json_get(j, "terminal\\.integrated\\.cwd"), ""));
    }
    v = json_str(json_get(j, "window\\.menuBarVisibility"), "classic");
    opt.menubar = strcmp(v, "hidden") != 0 && strcmp(v, "toggle") != 0;
    opt.statusbar = json_bool(json_get(j, "workbench\\.statusBar\\.visible"), 1);
    opt.activitybar = strcmp(json_str(json_get(j, "workbench\\.activityBar\\.location"), "default"), "hidden") != 0;
  }
  opt.format_paste = json_bool(json_get(j, "editor\\.formatOnPaste"), 0);
  opt.format_type = json_bool(json_get(j, "editor\\.formatOnType"), 0);
  opt.linked_edit = json_bool(json_get(j, "editor\\.linkedEditing"), 0);
  {	/* {"source.organizeImports": "explicit"} (or true, "always"), or ["source.organizeImports"] */
    const Json *ca = json_get(j, "editor\\.codeActionsOnSave"), *oi = json_get(ca, "source\\.organizeImports");
    size_t k;
    opt.organize_save = 0;
    if (oi) opt.organize_save = oi->type == J_BOOL ? oi->b : oi->type == J_STR && strcmp(oi->str, "never") != 0 && strcmp(oi->str, "off") != 0;
    for (k = 0; ca && ca->type == J_ARR && k < ca->n; k++)
      if (strcmp(json_str(ca->kid[k], ""), "source.organizeImports") == 0) opt.organize_save = 1;
  }
  opt.smart_commit = json_bool(json_get(j, "git\\.enableSmartCommit"), 0);
  opt.merge_editor = json_bool(json_get(j, "git\\.mergeEditor"), 1);
  opt.suggest_smart_commit = json_bool(json_get(j, "git\\.suggestSmartCommit"), 1);
  opt.scm_decor = strcmp(json_str(json_get(j, "scm\\.diffDecorations"), "all"), "none") != 0;
  opt.guides = json_bool(json_get(j, "editor\\.guides\\.indentation"), 1);
  opt.guides_active = json_bool(json_get(j, "editor\\.guides\\.highlightActiveIndentation"), 1);
  {	/* [80, 120], or [{"column": 80}] */
    const Json *ru = json_get(j, "editor\\.rulers");
    size_t i;
    opt.nrulers = 0;
    for (i = 0; ru && ru->type == J_ARR && i < ru->n && opt.nrulers < 8; i++) {
      const Json *c = ru->kid[i]->type == J_OBJ ? json_get(ru->kid[i], "column") : ru->kid[i];
      if (c && c->type == J_NUM && c->num > 0 && c->num < 10000) opt.rulers[opt.nrulers++] = (int)c->num;
    }
  }
  opt.inlay = strcmp(json_str(json_get(j, "editor\\.inlayHints\\.enabled"), "on"), "off") != 0;
  opt.codelens = json_bool(json_get(j, "editor\\.codeLens"), 1);
  opt.semantic = json_bool(json_get(j, "editor\\.semanticHighlighting\\.enabled"), 1);
  opt.textmate = json_bool(json_get(j, "editor\\.textmateGrammars"), 1);
  opt.tm_max_line = clamp((int)json_num(json_get(j, "editor\\.maxTokenizationLineLength"), 20000), 0, 1000000);
  opt.hover_delay = clamp((int)json_num(json_get(j, "editor\\.hover\\.delay"), 300), 0, 10000);
  opt.hover_sticky = json_bool(json_get(j, "editor\\.hover\\.sticky"), 1);
  opt.command_center = json_bool(json_get(j, "window\\.commandCenter"), 1);
  snprintf(opt.win_title, sizeof(opt.win_title), "%s", json_str(json_get(j, "window\\.title"), ""));
  {
    const char *pl = json_str(json_get(j, "workbench\\.panel\\.defaultLocation"), "bottom");
    opt.panel_loc = strcmp(pl, "right") == 0 ? PANEL_RIGHT : strcmp(pl, "left") == 0 ? PANEL_LEFT : PANEL_BOTTOM;
    opt.panel_justify = strcmp(json_str(json_get(j, "workbench\\.panel\\.alignment"), "center"), "justify") == 0;
  }
  opt.exp_confirm_dnd = json_bool(json_get(j, "explorer\\.confirmDragAndDrop"), 1);
  opt.exp_confirm_del = json_bool(json_get(j, "explorer\\.confirmDelete"), 1);
  opt.exp_dec_colors = json_bool(json_get(j, "explorer\\.decorations\\.colors"), 1);
  opt.exp_dec_badges = json_bool(json_get(j, "explorer\\.decorations\\.badges"), 1);
  opt.exp_compact = json_bool(json_get(j, "explorer\\.compactFolders"), 1);
  opt.exp_nesting = json_bool(json_get(j, "explorer\\.fileNesting\\.enabled"), 0);
  opt.exp_nest_expand = json_bool(json_get(j, "explorer\\.fileNesting\\.expand"), 1);
  opt.exp_reveal = json_get(j, "explorer\\.autoReveal") == NULL || json_get(j, "explorer\\.autoReveal")->type != J_BOOL ||
                   json_get(j, "explorer\\.autoReveal")->b;	/* true, or "focusNoScroll" */
  {
    static const char *const so[] = {"default", "mixed", "filesFirst", "type", "modified"};
    const char *v = json_str(json_get(j, "explorer\\.sortOrder"), "default");
    int i;
    opt.exp_sort = EXS_DEFAULT;
    for (i = 0; i < 5; i++)
      if (strcmp(v, so[i]) == 0) opt.exp_sort = i;
  }
  opt.lightbulb = strcmp(json_str(json_get(j, "editor\\.lightbulb\\.enabled"), "onCode"), "off") != 0;
  edit_settings(j);
  view_settings(j);
  return 0;
}


/*
** One setting written into the file, its comments and the rest kept:
** "key": "value" where the key is, or a new line after the '{'.
*/
void settings_put (const char *key, const char *value) {
  Buf v;
  buf_init(&v);
  if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) buf_puts(&v, value);	/* as they are */
  else json_put_str(&v, value, strlen(value));
  buf_putc(&v, '\0');
  settings_put_raw(key, v.s);
  buf_free(&v);
}


/* a setting's value as the file has it ("editor.tabSize"), NULL: not there */
const Json *settings_value (const char *key) {
  char path[256];
  size_t i, n = 0;
  for (i = 0; key[i] && n + 3 < sizeof(path); i++) {	/* "editor.tabSize" -> "editor\\.tabSize" */
    if (key[i] == '.') path[n++] = '\\';
    path[n++] = key[i];
  }
  path[n] = '\0';
  return json_get(g_json, path);
}


/* where "key" is in s: the start of its line; *line_end after its newline */
static char *find_key (char *s, const char *key, char **line_end) {
  Buf q;
  char *at, *ls, *e;
  buf_init(&q);
  buf_printf(&q, "\"%s\"", key);
  buf_putc(&q, '\0');
  at = strstr(s, q.s);
  buf_free(&q);
  if (at == NULL) return NULL;
  for (ls = at; ls > s && ls[-1] != '\n'; ls--) ;
  for (e = at; *e && *e != '\n'; e++) ;
  if (*e) e++;
  *line_end = e;
  return ls;
}


/* Reset Setting: its line goes from the file, so its default is used */
void settings_reset (const char *key) {
  char *f = settings_path(), *s, *ls, *le;
  size_t len;
  Buf b;
  int fd;
  if (f == NULL || (s = read_file(f, &len)) == NULL) {
    free(f);
    return;
  }
  if ((ls = find_key(s, key, &le)) != NULL) {
    buf_init(&b);
    buf_putn(&b, s, (size_t)(ls - s));
    buf_puts(&b, le);
    if ((fd = os_open(f, OS_WRITE)) >= 0) {
      os_write(fd, b.s, b.len);
      os_close(fd);
    }
    buf_free(&b);
  }
  free(s);
  free(f);
}


/* one setting with any JSON value, written as it is: "true", "4", "\"on\"" */
void settings_put_json (const char *key, const char *json) {
  settings_put_raw(key, json);
}


/* "key": raw, raw is JSON as it is written (true, 12, "text") */
void settings_put_raw (const char *key, const char *value) {
  char *f, *s, *at;
  size_t len;
  Buf b, q;
  int fd;
  settings_create();
  f = settings_path();
  if (f == NULL || (s = read_file(f, &len)) == NULL) {
    free(f);
    return;
  }
  buf_init(&q);
  buf_printf(&q, "\"%s\"", key);
  buf_putc(&q, '\0');
  buf_init(&b);
  at = strstr(s, q.s);
  if (at) {	/* the value after the key's ':' goes */
    char *v = strchr(at + q.len - 1, ':'), *e;
    if (v) {
      v++;
      while (*v == ' ') v++;
      e = v;
      if (*e == '"') {
        e++;
        while (*e && *e != '"') e += (*e == '\\' && e[1]) ? 2 : 1;
        if (*e) e++;
      }
      else while (*e && *e != ',' && *e != '\n' && *e != '}') e++;
      buf_putn(&b, s, (size_t)(v - s));
      buf_puts(&b, value);
      buf_puts(&b, e);
    }
  }
  if (b.len == 0) {	/* not there: a new first setting */
    char *brace = strchr(s, '{');
    size_t k = brace ? (size_t)(brace - s) + 1 : 0;
    buf_putn(&b, s, k);
    if (!brace) buf_putc(&b, '{');
    buf_printf(&b, "\n  %s: ", q.s);
    buf_puts(&b, value);
    buf_putc(&b, ',');
    buf_puts(&b, s + k);
    if (!brace) buf_puts(&b, "\n}\n");
  }
  fd = os_open(f, OS_WRITE);
  if (fd >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  buf_free(&b);
  buf_free(&q);
  free(s);
  free(f);
}


/* the command of the language server for a language ("c", "go" ...), or NULL */
const Json *settings_get (const char *key) {
  return json_get(g_json, key);
}


const char *settings_server (const char *lang) {
  static const struct {
    const char *lang, *cmd;
  } def[] = {
    {"c", "clangd"}, {"cpp", "clangd"}, {"go", "gopls"}, {"rust", "rust-analyzer"},
    {"python", "pylsp"}, {"javascript", "typescript-language-server --stdio"},
    {"typescript", "typescript-language-server --stdio"}, {"zig", "zls"},
    {"lua", "lua-language-server"}, {"csharp", "csharp-ls"}, {"java", "jdtls"},
    {"php", "intelephense --stdio"}, {"ruby", "solargraph stdio"},
    {"kotlin", "kotlin-language-server"}, {"dart", "dart language-server"},
    {"haskell", "haskell-language-server-wrapper --lsp"}, {"elixir", "elixir-ls"},
    {"html", "vscode-html-language-server --stdio"},
    {"css", "vscode-css-language-server --stdio"}, {"scss", "vscode-css-language-server --stdio"},
    {"less", "vscode-css-language-server --stdio"},
    {"json", "vscode-json-language-server --stdio"}, {"jsonc", "vscode-json-language-server --stdio"},
    {"shellscript", "bash-language-server start"}, {"yaml", "yaml-language-server --stdio"},
    {"dockerfile", "docker-langserver --stdio"}, {"sql", "sql-language-server up --method stdio"},
    {"swift", "sourcekit-lsp"}, {"scala", "metals"}, {"nim", "nimlangserver"},
    {"odin", "ols"}, {"v", "v-analyzer"}, {"julia", "julia --startup-file=no -e \"using LanguageServer; runserver()\""},
    {"erlang", "erlang_ls"}, {"r", "R --slave -e languageserver::run()"}, {"perl", "perlnavigator"},
    {"cmake", "cmake-language-server"}, {"graphql", "graphql-lsp server -m stream"},
    {"proto", "buf beta lsp"}, {"terraform", "terraform-ls serve"}, {"vue", "vue-language-server --stdio"},
    {"svelte", "svelteserver --stdio"}, {"latex", "texlab"}, {"objective-c", "clangd"},
    {"powershell", ""}, {"xml", "lemminx"}, {"toml", "taplo lsp stdio"}, {"markdown", "marksman server"}
  };
  const Json *servers = json_get(g_json, "mme\\.languageServers");
  size_t i;
  if (servers) {
    const Json *s = json_get(servers, lang);
    if (s) return json_str(s, NULL);
  }
  for (i = 0; i < sizeof(def) / sizeof(def[0]); i++)
    if (strcmp(def[i].lang, lang) == 0) return def[i].cmd;
  return NULL;
}


/* the name of the default terminal profile's setting on this system */
const char *term_profile_setting (void) {
  return PROFILE_SETTING;
}


/*
** {==================================================================
** The editing settings
** ===================================================================
*/

EdOpt eopt;


/* which of names v is (def when none) */
static int name_of (const char *v, const char *const *names, int n, int def) {
  int i;
  for (i = 0; v && i < n; i++)
    if (strcmp(v, names[i]) == 0) return i;
  return def;
}


/* "on", true: 1; "off", false: 0 (editor.quickSuggestions' parts) */
static int on_off (const Json *v, int def) {
  if (v == NULL) return def;
  if (v->type == J_BOOL) return v->b;
  if (v->type == J_STR) return strcmp(v->str, "off") != 0;
  return def;
}


int wheel_step (int mods) {	/* VS Code: 3 lines a notch, 5 times with Alt */
  int n = eopt.wheel_lines > 0 ? eopt.wheel_lines : 3;
  return (mods & KM_ALT) ? n * 5 : n;
}


void edit_settings (const Json *j) {
  static const char *const ai[] = {"none", "keep", "brackets", "advanced", "full"};
  static const char *const tc[] = {"off", "on", "onlySnippets"};
  static const char *const ae[] = {"off", "on", "smart"};
  static const char *const ss[] = {"first", "recentlyUsed", "recentlyUsedByPrefix"};
  static const char *const lh[] = {"none", "gutter", "line", "all"};
  static const char *const ln[] = {"off", "on", "relative", "interval"};
  const Json *qs = json_get(j, "editor\\.quickSuggestions");
  const char *sep = json_str(json_get(j, "editor\\.wordSeparators"), "`~!@#$%^&*()-=+[{]}\\|;:'\",.<>/?");
  eopt.auto_indent = name_of(json_str(json_get(j, "editor\\.autoIndent"), "full"), ai, 5, 4);
  eopt.indent_paste = json_bool(json_get(j, "editor\\.autoIndentOnPaste"), 0);
  eopt.tab_completion = name_of(json_str(json_get(j, "editor\\.tabCompletion"), "off"), tc, 3, 0);
  eopt.accept_enter = name_of(json_str(json_get(j, "editor\\.acceptSuggestionOnEnter"), "on"), ae, 3, 1);
  eopt.suggest_sel = name_of(json_str(json_get(j, "editor\\.suggestSelection"), "first"), ss, 3, 0);
  if (qs && qs->type == J_OBJ) {
    eopt.qs_other = on_off(json_get(qs, "other"), 1);
    eopt.qs_comments = on_off(json_get(qs, "comments"), 0);
    eopt.qs_strings = on_off(json_get(qs, "strings"), 0);
  }
  else eopt.qs_other = eopt.qs_comments = eopt.qs_strings = on_off(qs, -1) == -1 ? -1 : on_off(qs, 1);
  if (eopt.qs_other == -1) {	/* not said: VS Code's */
    eopt.qs_other = 1;
    eopt.qs_comments = eopt.qs_strings = 0;
  }
  eopt.trigger_chars = json_bool(json_get(j, "editor\\.suggestOnTriggerCharacters"), 1);
  eopt.mc_ctrl = strcmp(json_str(json_get(j, "editor\\.multiCursorModifier"), "alt"), "ctrlCmd") == 0;
  eopt.mc_paste_full = strcmp(json_str(json_get(j, "editor\\.multiCursorPaste"), "spread"), "full") == 0;
  eopt.empty_sel_clip = json_bool(json_get(j, "editor\\.emptySelectionClipboard"), 1);
  eopt.control_chars = json_bool(json_get(j, "editor\\.renderControlCharacters"), 1);
  eopt.uni_ambiguous = json_bool(json_get(j, "editor\\.unicodeHighlight\\.ambiguousCharacters"), 1);
  eopt.uni_invisible = json_bool(json_get(j, "editor\\.unicodeHighlight\\.invisibleCharacters"), 1);
  eopt.line_hl = name_of(json_str(json_get(j, "editor\\.renderLineHighlight"), "line"), lh, 4, 2);
  eopt.surround = clamp((int)json_num(json_get(j, "editor\\.cursorSurroundingLines"), 0), 0, 50);
  eopt.sticky_max = clamp((int)json_num(json_get(j, "editor\\.stickyScroll\\.maxLineCount"), 5), 1, 20);
  eopt.beyond_last = json_bool(json_get(j, "editor\\.scrollBeyondLastLine"), 1);
  eopt.wheel_lines = clamp((int)(json_num(json_get(j, "editor\\.mouseWheelScrollSensitivity"), 1) * 3 + 0.5), 1, 60);
  eopt.line_nums = name_of(json_str(json_get(j, "editor\\.lineNumbers"), "on"), ln, 4, 1);
  memset(eopt.sep, 0, sizeof(eopt.sep));
  for (; *sep; sep++)
    if ((unsigned char)*sep < 128) eopt.sep[(unsigned char)*sep] = 1;
}

VOpt vopt = {0, 0, 1, 120, 1, 1, 0, 1, 1, 1};


void view_settings (const Json *j) {
  vopt.mm_left = strcmp(json_str(json_get(j, "editor\\.minimap\\.side"), "right"), "left") == 0;
  vopt.mm_slider = strcmp(json_str(json_get(j, "editor\\.minimap\\.showSlider"), "mouseover"), "always") == 0;
  vopt.mm_chars = json_bool(json_get(j, "editor\\.minimap\\.renderCharacters"), 1);
  vopt.mm_maxcol = clamp((int)json_num(json_get(j, "editor\\.minimap\\.maxColumn"), 120), 24, 1000);
  vopt.mm_scale = clamp((int)json_num(json_get(j, "editor\\.minimap\\.scale"), 1), 1, 3);
  vopt.diff_trim = json_bool(json_get(j, "diffEditor\\.ignoreTrimWhitespace"), 1);
  vopt.diff_hide = json_bool(json_get(j, "diffEditor\\.hideUnchangedRegions\\.enabled"), 0);
  vopt.diff_side = json_bool(json_get(j, "diffEditor\\.renderSideBySide"), 1);
  vopt.tab_colors = json_bool(json_get(j, "workbench\\.editor\\.decorations\\.colors"), 1);
  vopt.tab_badges = json_bool(json_get(j, "workbench\\.editor\\.decorations\\.badges"), 1);
}

/* }================================================================== */
