/*
** mme.h - mme: VS Code for the terminal
**
** A text editor for the terminal that looks and works like VS Code. One
** program, no dependencies:
**   mme.c     main, the layout, the editor, the commands
**   ebuf.c    the text: lines, load and save, undo
**   eterm.c   the terminal: raw mode, keys, mouse, paste
**   edraw.c   the screen: cells, colors, only the changed lines are sent
**   emenu.c   the menu bar, its menus, the quick input box (pickers, dialogs)
**   eside.c   the activity bar and the sidebar; the Explorer (folder tree)
**   esearch.c the Search view: find in files
**   egit.c    the Source Control view and the diff editor (git)
**   egitlog.c the Source Control Graph, branches, the git commands, blame
**   esyntax.c syntax highlighting
**   epanel.c  the panel: integrated terminals (mmc-term's tpty.c, tvt.c, tgrid.c)
**   elsp.c    IntelliSense: language servers
**   ejson.c   JSON          econfig.c settings.json   etheme.c color themes
**   ekeys.c   keybindings.json          esnip.c   snippets
**   eext.c    extensions: Open VSX, VS Code's; their themes, snippets, languages
**   ehistory.c Local History (the Timeline)   emd.c     the Markdown preview
** mutil.c, mos.c and mpath.c come from the mmc shell (see mmc.h).
*/

#ifndef mme_h
#define mme_h

#include "mmc.h"

#define MME_NAME	"mme"
#define MME_VERSION	"0.9.0"	/* also in mme.rc */

#define TABW		(opt.tab_size)	/* columns a tab goes to */


/*
** {==================================================================
** ejson.c, econfig.c - JSON, settings.json
** ===================================================================
*/

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct Json {
  int type;
  int b;	/* J_BOOL */
  double num;	/* J_NUM */
  char *str;	/* J_STR: UTF-8, NUL terminated */
  size_t len;
  char *key;	/* the member's name, in an object */
  struct Json **kid;	/* J_ARR, J_OBJ */
  size_t n, cap;
} Json;

Json *json_parse (const char *s, size_t n);	/* NULL: not JSON */
void json_free (Json *j);
const Json *json_get (const Json *j, const char *path);	/* "a.b", "editor\\.tabSize" */
const char *json_str (const Json *j, const char *def);
double json_num (const Json *j, double def);
int json_bool (const Json *j, int def);
void json_put_str (Buf *b, const char *s, size_t n);	/* "quoted", with escapes */
void json_write (Buf *b, const Json *j);	/* the tree as text */

typedef struct Opt {
  int tab_size;	/* editor.tabSize */
  int insert_spaces;	/* editor.insertSpaces */
  int detect_indent;	/* editor.detectIndentation */
  int minimap;	/* editor.minimap.enabled */
  int line_numbers;	/* editor.lineNumbers */
  int render_ws;	/* editor.renderWhitespace: 0 none, 1 selection, 2 all */
  int trim_ws;	/* files.trimTrailingWhitespace */
  int final_newline;	/* files.insertFinalNewline */
  int sidebar;	/* workbench.sideBar.visible */
  int word_wrap;	/* editor.wordWrap: "on" */
  char theme[64];	/* workbench.colorTheme */
  int auto_close;	/* editor.autoClosingBrackets */
  int pair_colors;	/* editor.bracketPairColorization.enabled */
  char shell[512];	/* terminal.integrated.shell */
  char term_profile[64];	/* terminal.integrated.defaultProfile.windows (linux, osx) */
  int format_on_save;	/* editor.formatOnSave */
  int word_suggest;	/* editor.wordBasedSuggestions */
  int snippet_suggest;	/* editor.snippetSuggestions: not "none" */
  int vscode_ext;	/* mme.extensions.useVSCodeExtensions */
  int blame_line;	/* git.blame.editorDecoration.enabled */
  int blame_status;	/* git.blame.statusBarItem.enabled */
  int sticky;	/* editor.stickyScroll.enabled */
  int word_hl;	/* editor.occurrencesHighlight */
  int startup_welcome;	/* workbench.startupEditor: "welcomePage" */
  int guides;	/* editor.guides.indentation */
  int guides_active;	/* editor.guides.highlightActiveIndentation */
  int rulers[8], nrulers;	/* editor.rulers: columns */
  int inlay;	/* editor.inlayHints.enabled: not "off" */
  int codelens;	/* editor.codeLens */
  int semantic;	/* editor.semanticHighlighting.enabled */
  int lightbulb;	/* editor.lightbulb.enabled: not "off" */
  int emmet_tab;	/* emmet.triggerExpansionOnTab */
  int emmet_suggest;	/* emmet.showExpandedAbbreviation: not "never" */
  int close_tags;	/* html.autoClosingTags */
  int drag_drop;	/* editor.dragAndDrop */
  int auto_save;	/* files.autoSave: AUTO_* */
  int auto_save_delay;	/* files.autoSaveDelay, ms */
  int hot_exit;	/* files.hotExit: not "off" */
  int restore;	/* window.restoreWindows: not "none" */
  int encoding;	/* files.encoding: ENC_* */
  int cursor_style;	/* editor.cursorStyle: CUR_* */
  int cursor_blink;	/* editor.cursorBlinking: not "solid" */
  int side_right;	/* workbench.sideBar.location: "right" */
  int menubar;	/* window.menuBarVisibility: not "hidden" */
  int statusbar;	/* workbench.statusBar.visible */
  int activitybar;	/* workbench.activityBar.location: not "hidden" */
  int format_paste;	/* editor.formatOnPaste */
  int format_type;	/* editor.formatOnType */
  int organize_save;	/* editor.codeActionsOnSave: "source.organizeImports" */
  int linked_edit;	/* editor.linkedEditing */
  int smart_commit;	/* git.enableSmartCommit */
  int suggest_smart_commit;	/* git.suggestSmartCommit */
  int scm_decor;	/* scm.diffDecorations: not "none" */
  int test_gutter;	/* testing.gutterEnabled */
  int inline_values;	/* debug.inlineValues: not "off" */
  int term_shell_int;	/* terminal.integrated.shellIntegration.enabled */
  int term_decor;	/* terminal.integrated.shellIntegration.decorationsEnabled: not "never" */
  int term_sticky;	/* terminal.integrated.stickyScroll.enabled */
  int term_copy_sel;	/* terminal.integrated.copyOnSelection */
  int term_right_click;	/* terminal.integrated.rightClickBehavior: RC_* */
  int term_paste_warn;	/* terminal.integrated.enableMultiLinePasteWarning: 0 never, 1 auto, 2 always */
  int term_cursor_style;	/* terminal.integrated.cursorStyle: 0 block, 1 line, 2 underline */
  int term_cursor_blink;	/* terminal.integrated.cursorBlinking */
  int term_scrollback;	/* terminal.integrated.scrollback */
  int term_confirm_kill;	/* terminal.integrated.confirmOnKill: 0 never, 1 editor, 2 panel, 3 always */
  int term_confirm_exit;	/* terminal.integrated.confirmOnExit: 0 never, 1 always, 2 hasChildProcesses */
  char term_cwd[512];	/* terminal.integrated.cwd */
  int preview_tabs;	/* workbench.editor.enablePreview: a click replaces the tab */
  int textmate;	/* editor.textmateGrammars: VS Code's grammars color the code */
  int tm_max_line;	/* editor.maxTokenizationLineLength: a longer line the grammar leaves alone */
  int hover_delay;	/* editor.hover.delay, ms */
  int hover_sticky;	/* editor.hover.sticky: the mouse can go into the hover */
  int exp_confirm_dnd;	/* explorer.confirmDragAndDrop */
  int exp_confirm_del;	/* explorer.confirmDelete */
  int exp_dec_colors;	/* explorer.decorations.colors */
  int exp_dec_badges;	/* explorer.decorations.badges */
  int exp_compact;	/* explorer.compactFolders */
  int exp_nesting;	/* explorer.fileNesting.enabled */
  int exp_nest_expand;	/* explorer.fileNesting.expand */
  int exp_sort;	/* explorer.sortOrder: EXS_* */
  int exp_reveal;	/* explorer.autoReveal */
  int command_center;	/* window.commandCenter */
  char win_title[256];	/* window.title: empty for VS Code's default */
  int panel_loc;	/* workbench.panel.defaultLocation: PANEL_* */
  int panel_justify;	/* workbench.panel.alignment: "justify" */
  int md_scroll_preview;	/* markdown.preview.scrollPreviewWithEditor */
  int md_scroll_editor;	/* markdown.preview.scrollEditorWithPreview */
  int merge_editor;	/* git.mergeEditor: a conflicted file opens in the merge editor */
  int inline_suggest;	/* editor.inlineSuggest.enabled: the server's ghost text at the cursor */
  int inline_toolbar;	/* editor.inlineSuggest.showToolbar: not "never" (the toolbar itself is not drawn) */
  int next_edit;	/* github.copilot.nextEditSuggestions.enabled: the edit offered away from the cursor */
} Opt;

enum { PANEL_BOTTOM, PANEL_RIGHT, PANEL_LEFT };

enum { EXS_DEFAULT, EXS_MIXED, EXS_FILES_FIRST, EXS_TYPE, EXS_MODIFIED };	/* explorer.sortOrder */

enum { RC_DEFAULT, RC_COPY_PASTE, RC_PASTE, RC_SELECT_WORD, RC_NOTHING };	/* rightClickBehavior */

enum { AUTO_OFF, AUTO_DELAY, AUTO_FOCUS, AUTO_WINDOW };
enum { CUR_LINE, CUR_BLOCK, CUR_UNDERLINE, CUR_LINE_THIN, CUR_BLOCK_OUTLINE, CUR_UNDERLINE_THIN };

extern Opt opt;

typedef struct EdOpt {	/* the editing settings: edit_settings reads them */
  int auto_indent;	/* editor.autoIndent: 0 none, 1 keep, 2 brackets, 3 advanced, 4 full */
  int indent_paste;	/* editor.autoIndentOnPaste */
  int tab_completion;	/* editor.tabCompletion: 0 off, 1 on, 2 onlySnippets */
  int accept_enter;	/* editor.acceptSuggestionOnEnter: 0 off, 1 on, 2 smart */
  int suggest_sel;	/* editor.suggestSelection: 0 first, 1 recentlyUsed, 2 recentlyUsedByPrefix */
  int qs_other, qs_comments, qs_strings;	/* editor.quickSuggestions */
  int trigger_chars;	/* editor.suggestOnTriggerCharacters */
  int mc_ctrl;	/* editor.multiCursorModifier: "ctrlCmd" */
  int mc_paste_full;	/* editor.multiCursorPaste: "full" */
  int empty_sel_clip;	/* editor.emptySelectionClipboard */
  int control_chars;	/* editor.renderControlCharacters */
  int uni_ambiguous, uni_invisible;	/* editor.unicodeHighlight.ambiguousCharacters, .invisibleCharacters */
  int line_hl;	/* editor.renderLineHighlight: 0 none, 1 gutter, 2 line, 3 all */
  int surround;	/* editor.cursorSurroundingLines */
  int beyond_last;	/* editor.scrollBeyondLastLine */
  int line_nums;	/* editor.lineNumbers: 0 off, 1 on, 2 relative, 3 interval */
  int wheel_lines;	/* editor.mouseWheelScrollSensitivity: lines a notch scrolls */
  int sticky_max;	/* editor.stickyScroll.maxLineCount */
  unsigned char sep[128];	/* editor.wordSeparators: 1 for each of them */
} EdOpt;

extern EdOpt eopt;
int wheel_step (int mods);	/* the lines a wheel notch scrolls (Alt: faster) */
void edit_settings (const Json *j);	/* eopt from settings.json (settings_load) */

typedef struct VOpt {	/* the minimap's and the diff editor's settings: view_settings reads them */
  int mm_left;	/* editor.minimap.side: "left" */
  int mm_slider;	/* editor.minimap.showSlider: 0 mouseover, 1 always */
  int mm_chars;	/* editor.minimap.renderCharacters */
  int mm_maxcol;	/* editor.minimap.maxColumn */
  int mm_scale;	/* editor.minimap.scale: 1, 2 or 3 */
  int diff_trim;	/* diffEditor.ignoreTrimWhitespace */
  int diff_hide;	/* diffEditor.hideUnchangedRegions.enabled */
  int diff_side;	/* diffEditor.renderSideBySide */
  int tab_colors;	/* workbench.editor.decorations.colors: git colors the tab's name */
  int tab_badges;	/* workbench.editor.decorations.badges: its letter (M, U, A) */
} VOpt;

extern VOpt vopt;
void view_settings (const Json *j);	/* vopt from settings.json (settings_load) */

void data_init (const char *argv0);	/* mme-data, next to the program */
const char *data_dir (void);
char *data_path (const char *name);	/* a file in it */
char *settings_path (void);
void settings_create (void);
int settings_load (void);	/* -1: the file is not good JSON */
const Json *settings_get (const char *key);	/* "mme\\.debugAdapters"; NULL */
#define INLINE_LANG	"*inline"	/* the reserved key of the inline-completion server */
const char *settings_server (const char *lang);	/* the language server's command */
void settings_put (const char *key, const char *value);	/* "key": "value" in the file */
const char *term_profile_setting (void);	/* "terminal.integrated.defaultProfile.windows" ... */
void settings_put_json (const char *key, const char *json);	/* "key": true, 4 ... */
void settings_reset (const char *key);	/* its line goes: the default again */
const Json *settings_value (const char *key);	/* as the file says, NULL: not set */
void settings_put_raw (const char *key, const char *value);	/* "key": value, JSON as it is */

/* }================================================================== */


/*
** {==================================================================
** ebuf.c - the text
** ===================================================================
*/

typedef struct Pos {
  size_t y, x;	/* line, byte in the line */
} Pos;

typedef struct Row {
  char *s;	/* no newline, not NUL terminated */
  size_t len, cap;
} Row;

typedef struct Undo {
  int ins;	/* 1: text was put at a, 0: text was taken from a */
  Pos a;
  char *text;
  size_t len;
  long group;	/* one step of undo takes a whole group */
} Undo;

typedef struct UndoList {
  Undo *v;
  size_t n, cap;
} UndoList;

typedef struct Doc {
  Row *row;	/* always at least one */
  size_t n, cap;
  char *path;	/* native, NULL: not saved yet */
  int crlf;	/* the file had CR LF: it is saved the same way */
  int tabs;	/* indent with tabs, else with 'indent' spaces */
  int indent;
  UndoList undo, redo;
  long group;
  long changes, saved;	/* dirty when they differ */
  size_t hl_from;	/* the first line changed since the highlighter looked */
  size_t br_from;	/* and since the bracket depths were counted */
  size_t tm_from;	/* and since the grammar tokenized it */
  size_t *depth;	/* the bracket depth each line starts with */
  size_t depth_cap, depth_n;	/* its size, and the lines counted: 0 .. depth_n - 1 */
  unsigned char *hl;	/* the highlighter's state at the start of each line */
  size_t hl_n, hl_cap;	/* known for lines 0 .. hl_n - 1 */
  const void *hl_sx;	/* the language they are for */
  int refs;	/* the tabs that show it (split editors share it) */
  unsigned long edits;	/* every change counts: the language server is told */
  int enc;	/* ENC_*: how the file's bytes are written */
  long long disk_mtime, disk_size;	/* the file when it was read or saved */
  int disk_state;	/* DISK_*: what the file on disk did since */
  long long changed_at;	/* os_now_us() of the last change: auto save waits for a pause */
  void *tm;	/* etm.c: the TextMate stacks its lines start with */
} Doc;

/* files.encoding: the text is UTF-8 inside; these are the file's bytes */
enum { ENC_UTF8, ENC_UTF8BOM, ENC_UTF16LE, ENC_UTF16BE, ENC_1252, ENC_LATIN1, ENC_N };

/* the file on disk, as the watcher last saw it */
enum { DISK_SAME, DISK_NEWER, DISK_GONE };

int pos_cmp (Pos a, Pos b);
Pos pos_after (Pos a, const char *s, size_t n);	/* the end of s put at a */

void doc_init (Doc *d);
void doc_free (Doc *d);
int doc_load (Doc *d, const char *native);	/* 0 read, 1 new file, -1 error */
int doc_load_enc (Doc *d, const char *native, int enc);	/* enc -1: from its BOM, else files.encoding */
void doc_set_text (Doc *d, const char *s, size_t len);	/* the lines of s, LF ends them */
char *doc_disk_text (const Doc *d, size_t *len);	/* the file as it is now, UTF-8, LF; NULL: none */
void doc_stamp (Doc *d);	/* the file's time and size, now */
int doc_disk_check (Doc *d);	/* 0 as it was, 1 changed, 2 gone, 3 back again */
const char *enc_name (int enc);	/* "UTF-8 with BOM" */
const char *enc_id (int enc);	/* "utf8bom", files.encoding's names */
int enc_by_id (const char *id);	/* -1: not one */
int doc_save (Doc *d);	/* 0 ok, -1 error */
int doc_dirty (const Doc *d);
Pos doc_clamp (const Doc *d, Pos p);
Pos doc_end (const Doc *d);
Pos doc_insert (Doc *d, Pos at, const char *s, size_t n);	/* the end */
void doc_delete (Doc *d, Pos a, Pos b);	/* a before b */
char *doc_text (const Doc *d, Pos a, Pos b, size_t *len);
void doc_group (Doc *d);	/* the next change is a new undo step */
int doc_undo (Doc *d, Pos *cur);	/* 0: nothing to undo */
int doc_redo (Doc *d, Pos *cur);
void doc_detect_indent (Doc *d);	/* tabs or spaces, how many: from its lines */

/* }================================================================== */


/*
** {==================================================================
** eterm.c - the terminal
** ===================================================================
*/

/* a key is a code point or one of K_*, with KM_* bits for the modifiers */
#define KM_SHIFT	0x01000000
#define KM_ALT		0x02000000
#define KM_CTRL		0x04000000
#define KEY_CODE(k)	((k) & 0x00FFFFFF)
#define CTRL(c)		((c) & 0x1F)

enum {
  K_NONE = -1, K_TAB = 9, K_ENTER = 13, K_ESC = 27, K_BS = 127,
  K_UP = 0x110000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
  K_INS, K_DEL, K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9,
  K_F10, K_F11, K_F12,
  K_PASTE,	/* read the text with term_paste */
  K_MOUSE	/* see term_mouse */
};

typedef struct Mouse {
  int x, y;	/* cell, from 0 */
  int button;	/* 0 left, 1 middle, 2 right, 3 none */
  int press;	/* 1 down, 0 up */
  int drag;	/* moved with a button down */
  int wheel;	/* -1 up, 1 down, 0 not the wheel */
  int mods;	/* KM_* */
} Mouse;

extern Mouse term_mouse;

int term_open (void);	/* -1: not a terminal */
void term_close (void);
void term_size (int *cols, int *rows);
int term_key (int ms);	/* K_NONE when nothing came in ms (-1: wait) */
void term_paste (Buf *b);	/* after K_PASTE: the pasted text, LF ends */
void term_write (const char *s, size_t n);
void term_font (int delta);	/* +1 bigger, -1 smaller, 0 the configured size */
void term_ask_pixels (void);	/* XTSMGRAPHICS: how many pixels a cell is (the answer comes with the keys) */
int term_cell_px (int *w, int *h);	/* 0: the terminal has not said */

/* a typed character that goes into text: not a control key, no Ctrl or Alt */
#define IS_TEXT(k)	((k) >= 32 && (k) < K_UP && (k) != 127)

/* }================================================================== */


/*
** {==================================================================
** edraw.c - the screen
** ===================================================================
*/

enum {
  S_TEXT, S_LINE, S_GUTTER, S_GUTTER_CUR, S_SEL, S_MATCH, S_CTRL,
  S_STATUS, S_STATUS_ITEM, S_STATUS_DIM,
  S_MENUBAR, S_MENUBAR_ON, S_MENU, S_MENU_SEL, S_MENU_KEY, S_MENU_KEY_SEL, S_MENU_LINE,
  S_ACT, S_ACT_ON, S_ACT_BAR,
  S_SIDE, S_SIDE_HEAD, S_SIDE_TITLE, S_SIDE_DIR, S_SIDE_SEL, S_SIDE_CUR,
  S_SIDE_ACTIVE, S_SIDE_DIM, S_SIDE_HIT, S_BORDER,
  S_INPUT, S_INPUT_ON, S_INPUT_HINT, S_TOGGLE_ON,
  S_BOX, S_BOX_TITLE, S_BOX_SEL, S_BOX_DIM, S_BOX_HIT, S_BOX_HIT_SEL,
  S_TOAST, S_TOAST_WARN,
  S_GIT_M, S_GIT_A, S_GIT_D, S_GIT_U,
  S_ICON_BLUE, S_ICON_YELLOW, S_ICON_GREEN, S_ICON_RED, S_ICON_PURPLE,
  S_ICON_ORANGE, S_ICON_GREY,
  S_DIFF_ADD, S_DIFF_ADD_HI, S_DIFF_DEL, S_DIFF_DEL_HI, S_DIFF_FILL,
  S_DIFF_HUNK, S_DIFF_NUM,
  S_TABS, S_TAB, S_TAB_ON, S_CRUMB, S_TAB_PREVIEW, S_APPICON,
  S_PANEL, S_PANEL_TAB, S_PANEL_TAB_ON, S_MCURSOR,
  S_N,
  S_RGB = 0xFFFF	/* a cell with its own colors: scr_put_rgb */
};

#define RGB_BOLD	1
#define RGB_ITALIC	2
#define RGB_UNDER	4
#define RGB_CURLY	8	/* a squiggle, see scr_squiggle */
#define RGB_STRIKE	16	/* a line through it, for ~~this~~ */
#define RGB_DIM		32	/* half bright, for the inline suggestion's ghost text */

/* the colors of code (VS Code's Dark+ tokens) on the backgrounds of the editor */
enum {
  T_TEXT, T_KEYWORD, T_STORAGE, T_TYPE, T_FUNC, T_STRING, T_NUMBER,
  T_COMMENT, T_VAR, T_CONST, T_ESCAPE, T_HEADING, T_WS, T_N
};
enum { B_EDITOR, B_LINE, B_SEL, B_MATCH, B_ADD, B_ADD_HI, B_DEL, B_DEL_HI, B_N };
#define TOK(t, b)	(S_N + (t) * B_N + (b))	/* a style: token t on background b */

/* etheme.c: the colors a theme gives, by VS Code's names for them */
enum {
  C_EDITOR_BG, C_EDITOR_FG, C_LINE_BG, C_SEL_BG, C_MATCH_BG, C_GUTTER, C_GUTTER_ON,
  C_CTRL, C_WS,
  C_STATUS_BG, C_STATUS_FG, C_STATUS_ITEM,
  C_TITLE_BG, C_TITLE_FG, C_TITLE_ON,
  C_MENU_BG, C_MENU_FG, C_MENU_SEL_BG, C_MENU_SEL_FG, C_MENU_DIM, C_MENU_SEP,
  C_ACT_BG, C_ACT_FG, C_ACT_DIM,
  C_SIDE_BG, C_SIDE_FG, C_SIDE_HEAD, C_LIST_SEL_BG, C_LIST_SEL_FG, C_LIST_CUR_BG, C_DIM, C_BORDER,
  C_INPUT_BG, C_INPUT_FG, C_INPUT_HINT, C_TOGGLE_BG,
  C_HIT, C_TOAST_BG, C_ERROR, C_WARNING, C_INFO,
  C_GIT_M, C_GIT_A, C_GIT_D, C_GIT_U,
  C_ICON_BLUE, C_ICON_YELLOW, C_ICON_GREEN, C_ICON_RED, C_ICON_PURPLE, C_ICON_ORANGE, C_ICON_GREY,
  C_DIFF_ADD, C_DIFF_ADD_HI, C_DIFF_DEL, C_DIFF_DEL_HI,
  C_TABS_BG, C_TAB_BG, C_TAB_FG, C_TAB_ON_BG, C_TAB_ON_FG, C_CRUMB,
  C_ACCENT, C_MCURSOR, C_LIGHTBULB, C_MINIMAP_SLIDER, C_THUMB, C_THUMB_ON,
  C_BRACKET1, C_BRACKET2, C_BRACKET3, C_BRACKET_MATCH,
  C_GUIDE, C_GUIDE_ON, C_RULER, C_INLAY_FG, C_INLAY_BG, C_LENS,	/* indent guides, rulers, inlay hints, code lens */
  C_GHOST,	/* editorGhostText.foreground: the inline suggestion drawn in the text */
  C_TERM_FG, C_TERM_BG, C_ANSI,	/* 16 of them */
  C_TOK = C_ANSI + 16,	/* T_N of them */
  C_N = C_TOK + T_N
};

uint32_t ui_color (int slot);	/* 0xRRGGBB */
int theme_set (const char *name);	/* the theme's index; the first one when the name is not known */
const char *theme_sgr (int st);	/* the escape for style st */
void theme_tok (int st, uint32_t *fg, uint32_t *bg);	/* a TOK() style's colors */
int theme_count (void);
const char *theme_name (int i);
int theme_current (void);
typedef int (*ThemeLoad) (void *arg, uint32_t *color);	/* an extension's colors over the base */
void theme_add (const char *name, int light, ThemeLoad load, void *arg);
void theme_customize (uint32_t *color, const char *theme);	/* workbench.colorCustomizations over it */
void tm_theme_again (void);	/* the grammar's rules are built again: the settings changed */
void theme_clear_added (void);

void scr_resize (int cols, int rows);
int scr_cols (void);
int scr_rows (void);
void scr_clear (int st);
int scr_put (int x, int y, uint32_t ch, int st);	/* columns it took */
int scr_puts (int x, int y, const char *s, int st);	/* UTF-8 */
int scr_putsw (int x, int y, int w, const char *s, int st);	/* at most w columns */
int scr_put_rgb (int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, int at);	/* 0xRRGGBB */
uint32_t tok_color (int t);	/* the color of token t, 0xRRGGBB */
void scr_underline (int x, int y, int w, uint32_t color);	/* a link: underlined, in color */
void scr_squiggle (int x, int y, int w, uint32_t color);	/* a curly underline */
void scr_set_fg (int x, int y, uint32_t fg);	/* a cell's color changed, the rest kept */
void scr_set_bg (int x, int y, uint32_t bg);
void scr_glyph (int x, int y, uint32_t ch, uint32_t fg);	/* a guide line: ch in fg, the background kept */
uint32_t scr_ch (int x, int y);	/* what the cell shows */
void scr_fill (int x, int y, int w, int st);
void scr_box (int x, int y, int w, int h, int st);	/* a filled rectangle */
void scr_restyle (int x, int y, int w, int st);	/* the colors of cells, not their text */
void scr_cursor (int x, int y);
void scr_cursor_shape (int decscusr);	/* 1 .. 6: block, underline, bar; blinking or not */
enum { PTR_DEFAULT, PTR_TEXT, PTR_POINTER };	/* the mouse pointer: an arrow, an I beam, a hand */
void scr_pointer (int shape);	/* over what can be clicked: PTR_POINTER */

/*
** A picture for a terminal that draws them (sixel): the bytes go as they
** are, at the cell x, y, after the rows are sent, and only when a row of
** the w by h cells it covers changed (a picture is big; it is not sent
** again for nothing). Its cells must be drawn blank first.
*/
void scr_image (int x, int y, int w, int h, const char *data, size_t n);

void scr_flush (void);
void scr_redraw (void);	/* send everything on the next flush */

/*
** A line of text at x, y in w columns, from column 'left' on: tabs are
** expanded, control characters shown as ^X. Bytes [h0, h1) get style hst.
** Returns the column after the text (from 0, not from left).
*/
size_t scr_text (int x, int y, int w, const char *s, size_t n, size_t left,
                 int st, size_t h0, size_t h1, int hst);

/*
** The same for code: tok[i] is the token of byte i (NULL: all text), on
** background bg; bytes [h0, h1) on background hbg.
*/
size_t scr_code (int x, int y, int w, const char *s, size_t n, size_t left,
                 const unsigned char *tok, int bg, size_t h0, size_t h1, int hbg);

/* UTF-8: the code point at s (at most n bytes), its length in *len */
uint32_t utf8_decode (const char *s, size_t n, size_t *len);
int utf8_encode (uint32_t cp, char *out);	/* out: 4 bytes */
size_t str_cols (const char *s);	/* columns the string takes */

/* }================================================================== */


/*
** {==================================================================
** emenu.c - the menu bar, the quick input box
** ===================================================================
*/

/* the commands: menu items, keys and the command palette run these */
enum {
  CMD_NONE, CMD_NEW, CMD_OPEN_FILE, CMD_OPEN_FOLDER, CMD_OPEN_PROJECT,
  CMD_SAVE, CMD_SAVE_AS, CMD_CLOSE, CMD_QUIT,
  CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_FIND,
  CMD_FIND_FILES, CMD_SELECT_ALL, CMD_LINE_UP, CMD_LINE_DOWN,
  CMD_EXPLORER, CMD_SEARCH, CMD_GIT, CMD_SIDEBAR,
  CMD_GOTO, CMD_NEXT_CHANGE, CMD_PREV_CHANGE, CMD_QUICK_OPEN,
  CMD_PALETTE, CMD_ABOUT, CMD_TERMINAL, CMD_TERMINAL_KILL, CMD_MINIMAP,
  CMD_CURSOR_UP, CMD_CURSOR_DOWN, CMD_NEXT_MATCH, CMD_ALL_MATCHES,
  CMD_SETTINGS, CMD_TERMINAL_NEW, CMD_SPLIT, CMD_GROUP1, CMD_GROUP2,
  CMD_DEFINITION, CMD_SUGGEST, CMD_NEXT_PROBLEM, CMD_PREV_PROBLEM,
  CMD_RENAME, CMD_QUICKFIX, CMD_HOVER, CMD_PROBLEMS, CMD_THEME, CMD_ZEN, CMD_WORDWRAP, CMD_REPLACE,
  CMD_FOLD, CMD_UNFOLD, CMD_FOLD_ALL, CMD_UNFOLD_ALL, CMD_KEYS,
  CMD_COMMENT, CMD_BLOCK_COMMENT, CMD_COPY_UP, CMD_COPY_DOWN, CMD_DELETE_LINE, CMD_SELECT_LINE,
  CMD_NAV_BACK, CMD_NAV_FORWARD, CMD_GOTO_SYMBOL, CMD_FORMAT, CMD_INSERT_SNIPPET, CMD_LINE_BELOW,
  CMD_LINE_ABOVE, CMD_INDENT, CMD_OUTDENT, CMD_SNIPPETS,
  CMD_EXTENSIONS, CMD_EXT_VSIX,
  CMD_GIT_CHECKOUT, CMD_GIT_BRANCH, CMD_GIT_BRANCH_FROM, CMD_GIT_DELETE_BRANCH, CMD_GIT_RENAME_BRANCH,
  CMD_GIT_MERGE, CMD_GIT_PULL, CMD_GIT_PUSH, CMD_GIT_FETCH, CMD_GIT_SYNC, CMD_GIT_STASH,
  CMD_GIT_STASH_POP, CMD_GIT_STASH_APPLY, CMD_GIT_UNDO_COMMIT, CMD_GIT_COMMIT, CMD_GIT_AMEND,
  CMD_GIT_FILE_HISTORY, CMD_GIT_BLAME, CMD_GIT_REFRESH, CMD_GIT_DIFF_INLINE, CMD_GIT_MORE,	/* CMD_GIT_*: git_command */
  CMD_REFERENCES, CMD_IMPLEMENTATION, CMD_TYPE_DEF, CMD_PEEK_DEF, CMD_WORKSPACE_SYMBOL, CMD_UPPER,
  CMD_LOWER, CMD_TITLE, CMD_SORT_ASC, CMD_SORT_DESC, CMD_JOIN, CMD_TRIM, CMD_DEDUP,
  CMD_DUP_SEL, CMD_GOTO_BRACKET, CMD_SELECT_BRACKET, CMD_PARAM_HINTS, CMD_REOPEN, CMD_LANGUAGE, CMD_EOL,
  CMD_INDENTATION, CMD_INDENT_SPACES, CMD_INDENT_TABS, CMD_TO_SPACES, CMD_TO_TABS, CMD_DETECT_INDENT, CMD_RENDER_WS, CMD_STICKY, CMD_COLUMN_SELECT,
  CMD_SETTINGS_JSON, CMD_WELCOME, CMD_NOTIFICATIONS, CMD_EXP_NEW_FILE, CMD_EXP_NEW_FOLDER,
  CMD_EXP_REFRESH, CMD_EXP_COLLAPSE, CMD_COPY_PATH, CMD_COPY_REL_PATH, CMD_REVEAL_OS,
  CMD_OPEN_IN_TERMINAL,
  CMD_DEBUG_VIEW, CMD_DEBUG_START, CMD_DEBUG_RUN, CMD_DEBUG_STOP, CMD_DEBUG_RESTART, CMD_DEBUG_STEP_OVER,
  CMD_DEBUG_STEP_INTO, CMD_DEBUG_STEP_OUT, CMD_DEBUG_PAUSE, CMD_BREAKPOINT, CMD_DEBUG_CONSOLE,
  CMD_DEBUG_CONFIG, CMD_DEBUG_SELECT, CMD_TASK_RUN, CMD_TASK_BUILD, CMD_TASK_CONFIGURE,
  CMD_TERMINAL_SPLIT, CMD_TERMINAL_PREV_PANE, CMD_TERMINAL_NEXT_PANE, CMD_TERMINAL_FIND,
  CMD_TERMINAL_CLEAR, CMD_TERMINAL_RENAME, CMD_TERMINAL_PROFILE, CMD_TERMINAL_NEW_PROFILE,
  CMD_OUTPUT, CMD_OUTPUT_CHANNELS, CMD_OUTPUT_CLEAR, CMD_PANEL_MAX,
  CMD_REPLACE_FILES, CMD_CLOSE_OTHERS, CMD_CLOSE_RIGHT, CMD_CLOSE_SAVED, CMD_CLOSE_GROUP_ALL, CMD_CLOSE_ALL,
  CMD_PIN, CMD_UNPIN, CMD_KEEP_OPEN, CMD_REVEAL, CMD_SWITCH_EDITOR,
  CMD_SHOW_EDITORS, CMD_HISTORY_RESTORE, CMD_MD_PREVIEW, CMD_MD_SIDE, CMD_OPEN_EDITORS, CMD_TIMELINE,
  CMD_HISTORY_COMPARE, CMD_BREADCRUMBS,
  CMD_AUTO_SAVE, CMD_COMPARE_SAVED, CMD_COMPARE_WITH, CMD_COMPARE_CLIP, CMD_SELECT_COMPARE,
  CMD_COMPARE_SELECTED, CMD_ENCODING, CMD_REVERT, CMD_SIDEBAR_POS,
  CMD_TYPE, CMD_TERM_SEND, CMD_IMPORT_VSCODE, CMD_OPEN_WORKSPACE, CMD_ADD_FOLDER, CMD_SAVE_WORKSPACE,
  CMD_CLOSE_WORKSPACE,
  CMD_SPLIT_DOWN, CMD_SPLIT_LEFT, CMD_SPLIT_UP, CMD_GROUP3, CMD_GROUP4, CMD_FOCUS_LEFT_GROUP,
  CMD_FOCUS_RIGHT_GROUP, CMD_FOCUS_UP_GROUP, CMD_FOCUS_DOWN_GROUP, CMD_MOVE_NEXT_GROUP, CMD_MOVE_PREV_GROUP,
  CMD_JOIN_GROUP, CMD_JOIN_ALL, CMD_GROUP_SIZES, CMD_RESET_SIZES, CMD_LAYOUT_SINGLE, CMD_LAYOUT_2COL,
  CMD_LAYOUT_3COL, CMD_LAYOUT_2ROW, CMD_LAYOUT_GRID, CMD_CENTERED, CMD_TOGGLE_MENUBAR, CMD_TOGGLE_STATUSBAR,
  CMD_TOGGLE_ACTIVITYBAR, CMD_LAYOUT_MENU, CMD_APPEARANCE_MENU,
  CMD_FORMAT_SEL, CMD_ORGANIZE_IMPORTS, CMD_SOURCE_ACTION, CMD_EXPAND_SEL, CMD_SHRINK_SEL,
  CMD_FOLD_REGIONS, CMD_UNFOLD_REGIONS, CMD_FOLD_COMMENTS, CMD_FOLD_L1, CMD_FOLD_L2, CMD_FOLD_L3,
  CMD_FOLD_L4, CMD_FOLD_L5, CMD_FOLD_L6, CMD_FOLD_L7, CMD_CALL_HIERARCHY, CMD_CALLS_OUT,
  CMD_SUPERTYPES, CMD_SUBTYPES,
  CMD_DIRTY_NEXT, CMD_DIRTY_PREV, CMD_CHANGE_NEXT, CMD_CHANGE_PREV, CMD_STAGE_CHANGE, CMD_REVERT_CHANGE,
  CMD_STAGE_RANGES, CMD_UNSTAGE_RANGES, CMD_REVERT_RANGES, CMD_MERGE_CURRENT, CMD_MERGE_INCOMING,
  CMD_MERGE_BOTH, CMD_MERGE_ALL_CURRENT, CMD_MERGE_ALL_INCOMING, CMD_MERGE_ALL_BOTH, CMD_MERGE_NEXT,
  CMD_MERGE_PREV, CMD_MERGE_COMPARE, CMD_GIT_CLONE, CMD_GIT_INIT, CMD_GIT_PUBLISH,
  CMD_TEST_VIEW, CMD_TEST_RUN_ALL, CMD_TEST_RUN_CURSOR, CMD_TEST_RUN_FILE, CMD_TEST_RERUN,
  CMD_TEST_DEBUG_CURSOR, CMD_TEST_REFRESH, CMD_TEST_OUTPUT, CMD_TEST_CANCEL, CMD_TEST_COLLAPSE,
  CMD_PROBLEMS_FILTER, CMD_PROBLEMS_COLLAPSE, CMD_PROBLEMS_ACTIVE, CMD_PROBLEMS_COPY,
  CMD_OUTLINE_FOLLOW, CMD_OUTLINE_SORT, CMD_OUTLINE_COLLAPSE,
  CMD_BP_CONDITIONAL, CMD_BP_LOG, CMD_BP_EDIT, CMD_BP_TOGGLE_ENABLE, CMD_BP_REMOVE_ALL, CMD_BP_ENABLE_ALL,
  CMD_BP_DISABLE_ALL, CMD_RUN_TO_CURSOR, CMD_JUMP_TO_CURSOR, CMD_DEBUG_ADD_WATCH,
  CMD_CURSORS_LINE_ENDS, CMD_CURSOR_UNDO, CMD_CURSOR_REDO, CMD_MOVE_SEL_NEXT, CMD_CHANGE_ALL,
  CMD_SELECT_ALL_FIND, CMD_REINDENT, CMD_REINDENT_SEL, CMD_DEL_ALL_LEFT, CMD_DEL_ALL_RIGHT,
  CMD_TRANSPOSE, CMD_PART_LEFT, CMD_PART_RIGHT, CMD_DEL_PART_LEFT, CMD_DEL_PART_RIGHT,
  CMD_TAB_FOCUS, CMD_ADD_COMMENT, CMD_REMOVE_COMMENT, CMD_SCROLL_PAGE_UP, CMD_SCROLL_PAGE_DOWN,
  CMD_TERM_RUN_SEL, CMD_TERM_RUN_FILE, CMD_TERM_FOCUS, CMD_TERM_PREV_CMD, CMD_TERM_NEXT_CMD,
  CMD_TERM_SELECT_ALL, CMD_TERM_COPY, CMD_TERM_PASTE, CMD_TERM_RECENT, CMD_TERM_RECENT_DIR,
  CMD_TERM_COLOR, CMD_TERM_ICON,
  CMD_INSPECT_TOKENS,
  CMD_EXP_OPEN_SIDE, CMD_EXP_FIND_FOLDER,
  CMD_HELP_KEYS, CMD_HELP_TIPS, CMD_HELP_COMMANDS,
  CMD_DIFF_WS, CMD_DIFF_HIDE,
  CMD_ZOOM_IN, CMD_ZOOM_OUT, CMD_ZOOM_RESET,
  CMD_LSP_RESTART, CMD_LSP_STATUS, CMD_NOTIF_FOCUS, CMD_NOTIF_ACCEPT, CMD_NOTIF_CLEAR,
  CMD_MANAGE, CMD_PANEL_RIGHT, CMD_PANEL_LEFT, CMD_PANEL_BOTTOM, CMD_PANEL_CENTER, CMD_PANEL_JUSTIFY,
  CMD_TOGGLE_CC,
  CMD_HEX_OPEN, CMD_IMG_ZOOM_IN, CMD_IMG_ZOOM_OUT, CMD_IMG_ZOOM_RESET,
  CMD_MERGE_EDITOR, CMD_MERGE_COMPLETE, CMD_LENS_RUN,
  CMD_INLINE_TRIGGER, CMD_INLINE_ACCEPT, CMD_INLINE_WORD, CMD_INLINE_HIDE, CMD_INLINE_NEXT,
  CMD_INLINE_PREV, CMD_COPILOT_SIGNIN, CMD_COPILOT_SIGNOUT, CMD_COPILOT_STATUS,
  CMD_NEDIT_JUMP, CMD_NEDIT_TOGGLE,
  CMD_N
};

/* the whole picture without the overlays, drawn by mme.c; not flushed */
extern void (*ui_background) (void);
extern char ui_title[512];	/* in the middle of the title bar */

void menubar_draw (int open);	/* open: the menu shown, -1 none */
int menubar_hit (int x);	/* the menu under column x on row 0, -1 none */
extern char ui_cc[256];	/* the command center's text: the folder's name */
int menubar_cc_hit (int x);	/* the command center: 1 back, 2 forward, 3 its box, 0 none */

/* a little menu at x, y (flags: MF_*); the item picked, -1: none */
enum { MF_CHECK = 1, MF_OFF = 2, MF_LINE = 4, MF_SUB = 8 };
int popup_list (int x, int y, const char *const *label, const int *flags, int n);
int popup_width (const char *const *label, const int *flags, int n);
int menu_run (int which);	/* the menu opened by key or click: the CMD_ picked */
int menu_popup (int x, int y, const int *cmd, const char *const *label);	/* a right-click menu */
const char *cmd_name (int cmd);
const char *cmd_keys (int cmd);
const char *cmd_default_keys (int cmd);	/* mme's own, not the user's */
int cmd_checked (int cmd);	/* mme.c: a menu item with a check (Auto Save) */
const char *cmd_id (int cmd);	/* VS Code's name: "editor.action.rename" */
int cmd_by_id (const char *id);	/* CMD_NONE when there is none */
void cmd_set_keys (int cmd, const char *keys);	/* the user's keys; NULL: mme's again */
int key_capture (const char *title, int *k2);	/* keys pressed, then Enter; 0: Esc */

/* esnip.c - snippets, VS Code's */
typedef struct Snip {
  char *name, *prefix, *body, *desc;
} Snip;

typedef struct SnipStop {	/* a tab stop: $1 is idx 1; a..b its text (byte offsets) */
  int idx;
  size_t a, b;
  char *choices;	/* ${1|one,two|}: "one\ntwo", NULL: none (the caller frees it) */
} SnipStop;

typedef struct SnipCtx {	/* what the variables are */
  const char *indent;	/* the line's indent: lines after the first get it */
  const char *tab;	/* what \t in a body becomes */
  const char *path, *selected, *line, *word, *clipboard;
  const char *line_comment, *block_open, *block_close;
  size_t line_no;
} SnipCtx;

const char *snip_lang (const char *syntax);	/* "Go" -> "go", "C++" -> "cpp" */
const Snip *snip_list (const char *lang, size_t *n);	/* lang: "go", "c" ... */
void snip_reload (void);	/* the files are read again when next asked */
char *snip_dir (void);	/* ~/.mme/snippets */
char *snip_expand (const char *body, const SnipCtx *ctx, SnipStop **stop, size_t *nstop, size_t *len);

/* ethread.c - the workers Search and Go to File walk the folder on */
typedef struct Mutex Mutex;
typedef struct Thread Thread;

Mutex *mx_new (void);
void mx_free (Mutex *m);
void mx_lock (Mutex *m);
void mx_unlock (Mutex *m);
Thread *th_start (void (*fn) (void *), void *ud);	/* NULL: none could start */
void th_join (Thread *t);	/* waits for it, then frees it */
int th_cpus (void);	/* how many workers are worth starting (2 .. 8) */
void th_nap (int ms);	/* a worker with nothing to do */

/* eregex.c - regular expressions (Find, Replace, Search with ".*") */
typedef struct Regex Regex;
Regex *re_compile (const char *pat, int icase, const char **err);	/* NULL: *err says why */
void re_free (Regex *re);
int re_at (const Regex *re, const char *s, size_t n, size_t at, size_t *end, size_t *cap);	/* cap: 20 */
int re_find (const Regex *re, const char *s, size_t n, size_t from, size_t *a, size_t *b);
int re_groups (const Regex *re);
char *re_expand (const char *repl, const char *s, const size_t *cap, size_t *len);	/* $1, $& ... */

/* emd.c - the Markdown preview */
void md_draw (const Doc *d, int x, int y, int w, int h, size_t *top);	/* top: kept inside */
int md_width (void);	/* the width the preview was last drawn at */
size_t md_rows (const Doc *d, int w);	/* how many rows the whole preview has */
size_t md_row_of_line (const Doc *d, int w, size_t line);	/* the preview row a source line is on */
size_t md_line_of_row (const Doc *d, int w, size_t row);	/* and back, for scrolling together */
int md_anchor_row (const Doc *d, int w, const char *name, size_t *row);	/* [jump](#heading) */
int md_link_at (int x, int y, char **link);	/* the link under the mouse; *link to free */

/* ehex.c - the hex viewer for binaries (read-only, like VS Code's Hex Editor) */
void *hex_open (const char *path);	/* NULL: too big, or unreadable */
void hex_close (void *page);
void hex_reload (void *page);
size_t hex_size (void *page);
const char *hex_status (void *page);	/* "0x0000001C of 0x4A20   0x4D (77)" */
void hex_draw (void *page, int x, int y, int w, int h, int focus);
int hex_key (void *page, int k);	/* 1: it was the viewer's */
void hex_wheel (void *page, int d);
int hex_click (void *page, int mx, int my);

/* eimage.c - the picture preview (PNG, BMP, GIF, ICO drawn; the rest told about) */
int img_is_image (const char *path);	/* by its ending */
int img_info (const char *path, char *out, size_t n);	/* "120x80 png, 12 KB" */
void *img_open (const char *path);	/* NULL: not a picture */
void img_close (void *page);
void img_reload (void *page);
const char *img_status (void *page);	/* "1920x1080 - PNG - 240 KB" */
void img_draw (void *page, int x, int y, int w, int h, int focus);
int img_key (void *page, int k);
void img_zoom (void *page, int delta);	/* 0: fit again */
int img_click (void *page, int mx, int my);

/* ehistory.c - Local History: a copy of a file at each save */
typedef struct HistEntry {
  long long time;	/* seconds since 1970 */
  char *file;	/* the copy */
  char *source;	/* "File Saved" ... */
} HistEntry;

void history_add (const char *path, const char *source);	/* after a save */
size_t history_list (const char *path, HistEntry **v);	/* the newest first */
void history_free (HistEntry *v, size_t n);
void history_age (long long when, char *out, size_t n);	/* "3 mins" */

/* eemmet.c - Emmet abbreviations */
enum { EMMET_NONE, EMMET_HTML, EMMET_JSX, EMMET_XML, EMMET_CSS };
int emmet_mode (const char *lang, const char *path);	/* EMMET_* for a file */
size_t emmet_start (const char *s, size_t x, int mode);	/* where the abbreviation before x starts */
char *emmet_expand (const char *abbr, size_t n, int mode, int *worth);	/* a snippet body, NULL: none */

/* ekeys.c - keybindings.json, VS Code's */
int keys_load (void);	/* -1: the file is not good JSON */
int keys_find (int first, int k);	/* the command; -1: the first of a chord; KEYS_BLOCK: removed */
#define KEYS_BLOCK	-2
const Json *keys_args (void);	/* the args of the binding found last, NULL: none */
void keys_args_clear (void);
int keys_count (void);
int keys_entry (int i, int *cmd, int *k1, int *k2, const char **when);	/* 1: a "-command" */
int keys_default (int cmd, int *k2);	/* mme's own key for cmd, 0: none */
void keys_save (void);
void keys_set_when (int i, const char *when);
void keys_add (int cmd, int k1, int k2, int remove, const char *when);
void keys_del (int i);
void keys_reset (int cmd);	/* the user's entries for cmd go */
int keys_import (const Json *list);	/* VS Code's entries; how many mme took */
int when_eval (const char *expr);	/* a when clause, against when_ctx */
const char *when_ctx (const char *key);	/* mme.c: a context key's value, NULL: false */
void keys_set (int cmd, int k1, int k2);	/* and the file is written */
int key_parse (const char *s, int *k2);	/* "ctrl+k ctrl+t" */
void key_name (int k, int pretty, char *out, size_t n);	/* "ctrl+shift+p", "Ctrl+Shift+P" */
char *keys_path (void);

/* the quick input box at the top (VS Code's QuickPick) */
typedef struct PickItem {
  char *label;	/* what is shown and filtered on */
  char *detail;	/* dim text after it, or NULL */
  int icon;	/* a code point before the label, 0 none */
} PickItem;

typedef struct Pick {
  const char *title;	/* dim text in the input when it is empty */
  const char *prefix;	/* shown before the input, or NULL */
  const char *hint;	/* a dim line under the input when the list is empty */
  PickItem *item;
  size_t n, cap;
  int keep_order;	/* do not filter: the list is fixed (dialogs) */
  int fresh;	/* the text is selected: the first key typed takes its place */
  void (*on_move) (int item);	/* the selection moved to item (a preview) */
  int (*on_tick) (struct Pick *p, int changed);	/* after each key and while waiting: 1 the items changed, 2 stop (PICK_SWITCH) */
  int start;	/* the item selected first */
  const char *modes;	/* Go to File: one of these typed first switches (PICK_MODE) */
  int match_detail;	/* the detail is a path: what is typed matches "detail/label" too */
  const char *status;	/* dim, at the input's right end ("Indexing... 1200 files"), or NULL */
  char text[512];	/* what is typed */
} Pick;

#define PICK_CANCEL	-1	/* Esc, or a click outside */
#define PICK_TEXT	-2	/* Enter with nothing matching: see p->text */
#define PICK_UP		-3	/* Backspace with nothing typed */
#define PICK_SWITCH	-4	/* on_tick said so: another picker takes p->text */
#define PICK_MODE	-5	/* the text starts with one of p->modes */

void pick_init (Pick *p, const char *title);
void pick_add (Pick *p, const char *label, const char *detail, int icon);
void pick_clear (Pick *p);	/* items only */
void pick_free (Pick *p);
int pick_run (Pick *p);	/* the item's index, or PICK_* */
char *ask_text (const char *title, const char *init);	/* NULL: Esc */

/* a modal dialog in the middle: the button pressed, -1 for Esc */
int dialog (const char *msg, const char *detail, const char *const *button, int n);

/* the notification in the corner */
void toast (int warn, const char *fmt, ...);
void toast_src (int sev, const char *src, const char *fmt, ...);	/* sev 0 info, 1 warning, 2 error; src "gopls" */
void toast_ask (int sev, const char *src, const char *msg, const char *const *act, int n,
                void (*done) (void *ud, int choice), void *ud);	/* buttons; choice -1: closed */
int toast_click (int x, int y);	/* 1: it was on a toast */
int toast_key (int k);	/* the focused question's keys; 0: not its */
int toast_focus (void);	/* 0: no question */
int toast_focused (void);
int toast_accept (void);	/* the question's primary button */
void toast_clear_all (void);
const char *ui_spinner (void);	/* a Braille frame, by the time */
void toast_draw (void);
int toast_unread (void);	/* notifications not seen in the center yet */
void note_center (void);	/* the notification center (the bell) */

/* a menu at x, y (right-click); label NULL: a line; the item picked or -1 */
int context_menu (int x, int y, const char *const *label, const char *const *keys, int n);

/*
** esettings.c, ewelcome.c - the pages that show in an editor tab: the
** Settings editor and the Welcome page. They tell mme.c what to do.
*/
enum { PAGE_NONE, PAGE_SETTINGS, PAGE_WELCOME, PAGE_IMAGE, PAGE_HEX, PAGE_MERGE };

typedef struct PageAct {
  int what;	/* PA_* */
  int cmd;	/* PA_CMD: the command to run */
  const char *text;	/* PA_COPY: for the clipboard; PA_FOLDER: to open */
} PageAct;

enum { PA_NONE, PA_APPLY, PA_CMD, PA_COPY, PA_FOLDER };	/* APPLY: settings.json changed */

void sui_open (void);	/* the Settings editor comes to the front: the search box has the keys */
void sui_draw (int x, int y, int w, int h, int focus);
void sui_key (int k, PageAct *a);
void sui_mouse (const Mouse *m, PageAct *a);
void sui_search (void);	/* Ctrl+F: to the search box */

void welcome_draw (int x, int y, int w, int h, int focus, const Vec *recent);
void welcome_key (int k, const Vec *recent, PageAct *a);
void welcome_mouse (const Mouse *m, const Vec *recent, PageAct *a);

/*
** emerge.c - the merge editor: Incoming and Current over the Result. One
** file at a time, in a page tab of its own.
*/
int merge_open (const char *path);	/* 0 it opened, -1: see the toast */
int merge_candidate (const char *path);	/* it is unmerged, or has conflict markers */
int merge_active (void);
const char *merge_file (void);	/* the file it is merging; NULL: not open */
const char *merge_tab_name (void);	/* "a.txt (Merging)" */
int merge_left (void);	/* the conflicts still unresolved */
void merge_next (int back);	/* Go to Next/Previous Conflict */
int merge_complete (void);	/* Complete Merge: 1 written, the tab can close */
int merge_may_close (void);	/* 1: the tab may go (it asks when conflicts are left) */
void merge_close (void);
void merge_draw (int x, int y, int w, int h, int focus);
void merge_key (int k, PageAct *a);
void merge_command (int cmd, PageAct *a);	/* the CMD_MERGE_* it takes over */
void merge_mouse (const Mouse *m, PageAct *a);

/* }================================================================== */


/*
** {==================================================================
** eside.c, esearch.c, egit.c - the sidebar and its views
** ===================================================================
*/

enum { VIEW_FILES, VIEW_SEARCH, VIEW_GIT, VIEW_DEBUG, VIEW_EXT, VIEW_TEST, VIEW_N };

#define ACT_W	4	/* the activity bar's width */

/* what a view wants the editor to do */
typedef struct SideAct {
  int what;	/* SA_* */
  const char *path;
  const char *path2;	/* SA_RENAMED: the new path */
  size_t line, col, len;	/* SA_OPEN from Search: the match (line from 1) */
  int staged;	/* SA_DIFF: the index against HEAD, else the tree against the index */
  int go;	/* SA_DIFF: the diff gets the keys */
  int cmd;	/* SA_CMD: the command to run */
} SideAct;

/* GO: open, the editor gets the keys; SHOW_DIFF: diff_open_rev opened it; FOCUS_SCM: the view */
enum { SA_NONE, SA_OPEN, SA_GO, SA_DIFF, SA_SHOW_DIFF, SA_CMD, SA_FOCUS_SCM,
       SA_RENAMED, SA_DELETED,	/* path (to path2) was renamed / deleted: its tabs follow */
       SA_TERMINAL, SA_CLIP,	/* a terminal in folder path; path to the clipboard */
       SA_OPEN_SIDE, SA_FIND_FOLDER };	/* path opened in the group beside; Search in folder path (from the root) */

void side_bar (int x, int y, int w, int h, size_t total, size_t top, size_t shown);	/* a pane's scrollbar */
int files_bar_y (void);	/* the tree's scrollbar: the row it starts at */
int files_bar_rows (void);	/* its rows, 0 when everything fits */
void files_bar_to (int row, int rows);	/* dragged there */
int side_width (void);	/* mme.c: the sidebar's width now */
void act_draw (int x, int y, int h, int view, int shown);
int act_manage_row (int y, int h);	/* the Manage gear's row, -1: no room */
int act_badge (int view, int *dot);	/* mme.c: a view's number (changes, failed tests), or a dot */

/*
** mme.c: one item of the status bar, given while it is drawn: its id
** ("status.x"), its name for the right-click menu, the side (1 right), a
** priority (the lowest go first when it is narrow), the text (codicons as
** UTF-8), the tooltip, the command a click runs.
*/
void status_add (const char *id, const char *name, int right, int prio, const char *text, const char *tip, int cmd);
int act_hit (int y);	/* the view whose icon is on row y of the body, -1 none */

void side_open (const char *native);	/* the folder the Explorer shows */
const char *side_root (void);
void side_open_roots (char *const *paths, char *const *names, int n, const char *title);	/* a workspace */

/* eworkspace.c - .code-workspace files: several folders */
int ws_open (const char *file);	/* -1: not one */
int ws_active (void);
const char *ws_file (void);	/* NULL: untitled */
const char *ws_title (void);	/* "name (Workspace)" */
const Json *ws_settings (void);	/* its "settings", NULL: none */
int ws_count (void);	/* the folders: 1 without a workspace */
const char *ws_folder (int i);
const char *ws_folder_name (int i);
void ws_add_folder (const char *dir);
int ws_save_as (const char *file);
void ws_close (void);	/* the first folder alone */
void ws_forget (void);	/* a plain folder opened */

/* eimport.c - Preferences: Import VS Code Settings */
int import_find (Vec *dirs);	/* VS Code's user folders found */

/* evscode.c - where VS Code is (read only): installs, a portable one's data folder */
int vscode_installs (Vec *out);	/* the install folders, `code` in the PATH first */
int vscode_user_dirs (Vec *out);	/* User folders: data/user-data/User, %APPDATA%\Code\User ... */
int vscode_ext_dirs (Vec *out);	/* extension folders: data/extensions, ~/.vscode/extensions ... */
int vscode_builtin_dirs (Vec *out);	/* the extensions an install comes with */
char *vscode_find_ext (const char *id);	/* "ms-python.debugpy": its newest folder, NULL none */
char *import_preview (const char *dir);	/* what would be taken, as text */
void import_run (const char *dir, int *settings, int *keys, int *snippets);
int import_offered (void);	/* a VS Code folder is there and it was not imported yet */
int settings_known (const char *key);	/* esettings.c: a setting mme has */
void side_reveal (const char *real);
void side_follow (const char *real);	/* side_reveal when explorer.autoReveal */
void files_gap (int rows);	/* rows under "EXPLORER" left for OPEN EDITORS */
void files_mods (int mods);	/* the mouse's KM_* for the next files_click (Ctrl / Shift: select more) */
int files_excluded (const char *rel);	/* files.exclude hides it (a path from the folder) */
char *files_exclude_list (void);	/* files.exclude as globs, for a worker; free it */
int files_drag_start (int row);	/* the mouse pressed on the tree's row: 1 something to drag */
void files_drag_over (int row);	/* -1: not over the tree */
void files_drop (int row, int copy, SideAct *act);	/* dropped on the tree's row */
size_t files_drag_count (void);
const char *files_drag_path (size_t i);
void files_drag_end (void);
void explorer_renamed (const char *from, const char *to);	/* mme.c: the tabs of a moved file follow */
void explorer_deleted (const char *path);	/* mme.c: the tabs of a deleted file */
void search_scope (const char *rel);	/* esearch.c: Find in Folder (files to include) */
void side_refresh (void);
void side_draw (int view, int x, int y, int w, int h, int focus, const char *active);
int side_key (int view, int k, SideAct *act);	/* 0: not used */
void side_click (int view, int row, int col, SideAct *act);
void side_wheel (int view, int d);
int side_idle (int view);	/* 1: something changed, draw again */

uint32_t file_icon (const char *name, int *st);	/* Seti's icon and its color */
void files_draw (int x, int y, int w, int h, int focus, const char *active);
int files_key (int k, SideAct *act);
void files_click (int row, int col, SideAct *act);
void files_menu (int row, int x, int y, SideAct *act);	/* the right button: its context menu */
enum { FC_NEW_FILE, FC_NEW_FOLDER, FC_RENAME, FC_DELETE, FC_COPY, FC_CUT, FC_PASTE, FC_DUPLICATE,
       FC_COPY_PATH, FC_COPY_REL, FC_REVEAL, FC_TERMINAL, FC_COLLAPSE, FC_REFRESH,
       FC_SELECT_ALL, FC_OPEN_SIDE, FC_FIND_FOLDER };
void files_cmd (int what, SideAct *act);	/* on the selection */
const char *compare_selected (void);	/* mme.c: Select for Compare's file, NULL: none */
const char *files_selected (void);	/* its path, NULL: none */
int files_editing (void);	/* the box of a new name has the keys */

/* efiles.c - files for the Explorer */
int fs_exists (const char *path);
int fs_rename (const char *from, const char *to);
int fs_trash (const char *path);	/* the Recycle Bin / the Trash; -1: could not */
int fs_remove (const char *path);	/* for good, a folder with all in it */
int fs_copy (const char *from, const char *to);	/* a folder with all in it */
char *fs_copy_name (const char *dir, const char *name, int is_folder);	/* "a copy.c" */
void fs_reveal (const char *path);	/* in the system's file manager */
void files_wheel (int d);

void search_draw (int x, int y, int w, int h, int focus);
int search_key (int k, SideAct *act);
void search_click (int row, int col, SideAct *act);
void search_wheel (int d);
int search_idle (void);
void files_index_stale (void);	/* mme.c: a file was made, renamed or deleted: Go to File walks again */
void files_index_stop (void);	/* its workers end (another folder, quitting) */
void files_index_idle (void);	/* what they found, while the editor waits for a key */
int search_busy (void);	/* a search is walking the folder: the main loop comes back soon */
void search_stop (void);	/* the workers end (another folder, quitting) */
int search_ignored (const char *rel);	/* for Go to File: .gitignore'd, or a folder search skips */
char *search_ignore_list (void);	/* the folder's .gitignore as globs, for a worker; free it */
int search_globs (const char *list, const char *rel);	/* rel matches one of the globs (no state: threads) */
void search_set (const char *text);	/* Find in Files with the selection */
void search_replace_mode (const char *text);	/* Replace in Files: the replace box open */

void git_draw (int x, int y, int w, int h, int focus);
int git_key (int k, SideAct *act);
void git_click (int row, int col, SideAct *act);
void git_wheel (int d);
void git_refresh (void);
int git_mark (const char *path, int dir);	/* 'M' 'A' 'D' 'U' or 0: for the Explorer */
const char *git_branch (void);	/* "" outside a repository */
int git_count (void);	/* changed files: the badge on the activity bar */
int git_exec (Buf *out, int err, const char *const *args);	/* git -C <top> args (NULL ends) */
const char *git_root (void);	/* the repository's folder; NULL: none */
int git_letter_style (int l);	/* the style of a change's letter M A D U R */
int git_commit (int amend);	/* the message box's text; 0: it must be typed first */
int diff_open_rev (const char *path, const char *rel, const char *old_rel,
                   const char *old, const char *new_, const char *title);
int diff_open_files (const char *path, const char *old_file, const char *new_file, const char *title);

/* egitlog.c - the Source Control Graph, branches, the git commands, blame */
void graph_load (void);
size_t graph_count (void);
int graph_more (void);	/* more commits than shown: "Load More..." */
void graph_load_more (void);
const char *graph_hash (size_t i);
int graph_open (size_t i);	/* its files show */
void graph_toggle (size_t i);
size_t graph_nfiles (size_t i);
const char *graph_file_path (size_t i, size_t f);
int graph_diff (size_t i, size_t f);	/* the diff editor: commit i's change of file f */
void graph_draw (size_t i, int x, int y, int w, int st);
void graph_draw_file (size_t i, size_t f, int x, int y, int w, int st);
const char *git_sync_text (void);	/* "1\xE2\x86\x93 2\xE2\x86\x91" behind / ahead, "" */
int git_has_upstream (void);
void git_command (int cmd, const char *path, SideAct *act);	/* a CMD_GIT_*; act: what to show */
void git_error (const Buf *b, const char *fallback);	/* git's message in a toast */
void git_ago (long long t, char *out, size_t n);	/* "3 days ago" */
const char *git_blame (const char *path, size_t y, int short_form);	/* NULL: none */
void blame_clear (void);
int git_blame_idle (void);	/* the blame git was asked for, read when nothing else is to do */
void on_disk_changed (void);	/* mme.c: git changed files; the open ones are read again */

/* eext.c - the Extensions view, and what extensions contribute */
void ext_init (void);	/* after settings.json is read */
void ext_rescan (void);
char *ext_dir (void);	/* mme-data/extensions */
void ext_draw (int x, int y, int w, int h, int focus);
int ext_key (int k, SideAct *act);
void ext_click (int row, int col, SideAct *act);
void ext_wheel (int d);
int ext_idle (void);	/* 1: something changed */
void ext_show (void);
void ext_install_vsix (const char *path);
const char *const *ext_snippets (const char *lang, size_t *n);	/* snippet files */
const char *ext_lang_for (const char *path);	/* a language id, or NULL */
const char *ext_lang_name (const char *id);
const char *ext_lang_label (const char *path);	/* for the status bar: "Plain Text" when none */
int ext_comment (const char *id, const char **line, const char **open, const char **close);
const char *ext_theme_path (const char *label);	/* an extension's color theme file */
void on_ext_page (const char *path);	/* mme.c: an extension's page to show */

/* the diff editor: shown in place of the editor */
int diff_open (const char *path, int staged);	/* 0 ok, -1: see the toast */
int diff_open_texts (const char *path, const char *a, size_t na, const char *b, size_t nb,
                     const char *title);	/* two texts: a on the left, b on the right */
int diff_active (void);
void diff_close (void);
const char *diff_title (void);
const char *diff_path (void);
size_t diff_line (void);	/* the line of the file at the cursor, from 1 */
int diff_editable (void);	/* the modified side is the working tree's file: it is typed in */
size_t diff_caret (size_t *col);	/* the caret's line (from 1) and byte, 0: nowhere */
void diff_set_caret (size_t line, size_t col);
void diff_new_text (const char *s, size_t n);	/* the modified side's text again: the lines are made from it */
void diff_draw (int x, int y, int w, int h, int wrap);	/* wrap: editor.wordWrap */
int diff_key (int k);	/* DIFF_* */
void diff_wheel (int d, int mods);	/* Shift: to the side */
int diff_bar_press (int mx, int my);	/* a press on its scrollbars: 1 it took it */
int diff_bar_held (void);	/* a scrollbar's thumb is held */
void diff_bar_drag (int mx, int my);
void diff_bar_up (void);
void diff_change (int back);
void diff_toggle_inline (void);	/* side by side, or one above the other */	/* F7 / Shift+F7 */

enum { DIFF_NO, DIFF_YES, DIFF_CLOSE, DIFF_EDIT, DIFF_REVERT };	/* EDIT: open the file at diff_line; REVERT: the block clicked */
int diff_title_draw (int x1, int y);	/* the diff's icons in the tab bar, before x1; where they start */
int diff_title_click (int x);	/* DIFF_* */
int diff_click (int mx, int my);	/* DIFF_* */
void diff_toggle_trim (void);	/* diffEditor.ignoreTrimWhitespace */
void diff_toggle_hide (void);	/* diffEditor.hideUnchangedRegions.enabled */

int diff_range (int act);	/* Stage (0) / Unstage (1) / Revert (2) Selected Ranges: 0 done */

/* git's blobs and index (egit.c) */
char *git_rel_of (const char *path);	/* from the repository's top, '/'; NULL: not in it */
char *git_blob (const char *spec, size_t *len);	/* ":rel", "HEAD:rel"; NULL: none */
int git_index_put (const char *rel, const char *s, size_t n);	/* the index's copy becomes s */
int git_conflicted (const char *path);	/* git status says it is unmerged */
void git_init_repo (void);	/* Git: Initialize Repository */
void git_publish (void);	/* Git: Publish Branch */
char *git_clone (void);	/* Git: Clone; the folder to open, or NULL */

typedef struct QHunk {	/* a change: old lines o0 .. o0 + on became new n0 .. n0 + nn (from 0) */
  size_t o0, on, n0, nn;
} QHunk;

QHunk *text_hunks (const char *a, size_t na, const char *b, size_t nb, size_t *n);	/* free it */

/* equick.c - quick diff (the gutter's changes against the index) and merge conflicts */
enum { QM_ADD = 1, QM_MOD = 2, QM_DEL = 4, QM_DEL_TOP = 8 };	/* DEL: lines went after this one */

void quick_clear (void);	/* git refreshed: the index's copies are read again */
void quick_forget (Doc *d);
int quick_idle (void);	/* git's copy of a file, read when nothing else is to do */
const QHunk *quick_hunks (Doc *d, const char *path, size_t *n);	/* NULL: no quick diff */
int quick_mark (Doc *d, const char *path, size_t y);	/* QM_* */
const char *quick_old (Doc *d, const char *path, size_t i, size_t *len);	/* the index's line i */
int quick_stage (Doc *d, const char *path, const QHunk *h, size_t nh);	/* 0 done */
int quick_unstage (const char *path, size_t lo, size_t hi);	/* 0 done, 1 nothing there */

typedef struct Conflict {	/* <<<<<<< start, ||||||| base ((size_t)-1: none), ======= mid, >>>>>>> end */
  size_t start, base, mid, end;
} Conflict;

const Conflict *conflicts (Doc *d, size_t *n);
long conflict_at (Doc *d, size_t y);	/* -1: none */

/* the programs: git (egit.c) */
int run_capture (char **argv, Buf *out);	/* the exit code; -1: can't start */
char *find_program (const char *name);	/* in PATH, native; NULL */

/* }================================================================== */


/*
** {==================================================================
** esyntax.c - syntax highlighting
** ===================================================================
*/

typedef struct Syntax Syntax;

const Syntax *syntax_for (const char *name);	/* by the file's name; NULL: plain text */
const Syntax *syntax_detect (const char *path, const Doc *d);	/* by the name, else the first line (#!) */
const Syntax *syntax_first_line (const char *s, size_t n);
const Syntax *syntax_by_id (const char *id);	/* by VS Code's language id */
const char *syntax_id (const char *name);	/* "C#" -> "csharp"; NULL: not one of mme's */
const char *syntax_name (const Syntax *sx);	/* for the status bar */
int syntax_count (void);
const Syntax *syntax_nth (int i);
void syntax_comment (const Syntax *sx, const char **line, const char **open, const char **close);
/* the tokens of a line (tok: n bytes), from the state the line starts in; the next state */
int syntax_scan (const Syntax *sx, const char *s, size_t n, int state, unsigned char *tok);
/* the tokens of line y of d, with the states of the lines before kept up to date */
void syntax_line (Doc *d, const Syntax *sx, size_t y, unsigned char *tok);
void syntax_line_quick (Doc *d, const Syntax *sx, size_t y, unsigned char *tok);	/* without the grammar: for bulk work */
void syntax_doc_free (Doc *d);	/* what was scanned for it is dropped */
void syntax_line_head (Doc *d, const Syntax *sx, size_t y, unsigned char *tok, size_t max);	/* its first max bytes */
/* a language mme only knows from a VS Code extension (its grammar colors it) */
const Syntax *syntax_extra (const char *name, const char *id, const char *line, const char *open, const char *close);
const char *syntax_lang (const Syntax *sx);	/* its VS Code language id */

/* eonig.c - regular expressions as TextMate grammars write them (Oniguruma's) */
typedef struct Onig Onig;
Onig *onig_compile (const char *pat, const char **err);	/* NULL: *err says why */
void onig_free (Onig *re);
int onig_groups (const Onig *re);
int onig_uses_g (const Onig *re);	/* \G or \A is in it */
/* the first match from from on: caps[2 * (groups + 1)], (size_t)-1 unset; \G only at g, \A when first */
int onig_search (const Onig *re, const char *s, size_t n, size_t from, size_t g, int first, size_t *caps);

/* etm.c - TextMate grammars: VS Code's own highlighting */
int tm_line (Doc *d, const Syntax *sx, size_t y, unsigned char *tok);	/* 0: esyntax does it */
int tm_colors (const Doc *d, size_t y, const uint32_t **fg, const unsigned char **fs, const unsigned char **cls);
int tm_scopes (Doc *d, const Syntax *sx, size_t y, size_t x, char *out, size_t n);
const Syntax *tm_detect (const char *path, const Doc *d);	/* a language of VS Code's extensions */
void tm_doc_free (Doc *d);
const char *tm_vscode (void);	/* VS Code's built-in extensions, or NULL */

/* }================================================================== */


/*
** {==================================================================
** epanel.c - the integrated terminal
** ===================================================================
*/

int panel_start (int cols, int rows);	/* the shell, in the open folder */
int panel_alive (void);
int panel_poll (void);	/* the shell's output: 1 new, 2 the shell ended */
void panel_draw (int x, int y, int w, int h, int focus);
void panel_key (int k);
void panel_paste (const char *s, size_t n);
void panel_wheel (int d);
void panel_kill (void);	/* the one in front */
void panel_kill_all (void);
const char *panel_title (void);
int panel_new (int cols, int rows);	/* one more, in front */
void panel_cwd (const char *dir);	/* the next one starts there (else the folder open) */
int panel_count (void);
int panel_current (void);
void panel_select (int i);
const char *panel_name (int i);
void panel_send (const char *s, size_t n);	/* raw, to the terminal in front */
int panel_run (int cols, int rows, const char *name, const char *cmd, const char *cwd, int task);	/* a task's terminal */
int panel_new_profile (int cols, int rows, int p);	/* with a shell of panel_profiles */
int panel_split (void);	/* one more next to the one in front, in its group */
int panel_group_size (void);	/* the terminals shown side by side */
void panel_focus_pane (int d);	/* -1 the one on the left, 1 on the right */
void panel_rename (const char *name);
void panel_clear (void);	/* Terminal: Clear */
void panel_scroll (int d, int page);
int panel_profiles (void);	/* the shells found */
const char *panel_profile_name (int i);	/* "Command Prompt", "Git Bash" ... */
const char *panel_profile_path (int i);
void panel_hover (int x, int y);	/* the mouse: a link there is underlined */
void panel_find_open (void);	/* Ctrl+F in the terminal */
int panel_finding (void);
int panel_find_key (int k);	/* 0: not the find widget's */

typedef struct PanelLink {	/* what a link in the terminal opens */
  char *target;	/* a URL, or a file (native) */
  int url;
  long line, col;	/* in the file, from 1; 0: not said */
} PanelLink;

int panel_click (int x, int y, int mods, int focused, PanelLink *lk);	/* 1 used, 2 open *lk, 3 the last went */
int panel_mouse (const Mouse *m, int focused, PanelLink *lk);	/* the same, and 100 + a menu's command */
int panel_dragging (void);	/* a selection is being made: every mouse event is the panel's */
int panel_key_cmd (int k);	/* the terminal's own keys (copy, paste ...): 1 used */
void panel_copy (void);
void panel_paste_clip (void);
void panel_select_all (void);
void panel_scroll_cmd (int d);	/* to the previous / next command's prompt */
void panel_run_text (const char *s, size_t n);	/* typed, and Enter */
void panel_run_file (const char *path);
void panel_recent_command (void);
void panel_recent_dir (void);
void panel_change_color (void);
void panel_change_icon (void);
int panel_cursor_shape (void);	/* DECSCUSR */
int panel_confirm_kill (void);	/* terminal.integrated.confirmOnKill: 1 go on */
int panel_confirm_exit (void);	/* terminal.integrated.confirmOnExit: 1 go on */
void clip_set (const char *s, size_t n);	/* mme.c: the clipboard, and the system's (OSC 52) */
const char *clip_get (size_t *n);

/* eout.c - the OUTPUT view: a channel for each thing that runs */
void out_append (const char *chan, const char *s, size_t n);
void out_log (const char *chan, const char *fmt, ...);	/* a line, after the time */
int out_count (void);
const char *out_name (int i);
int out_current (void);
void out_select (int i);
void out_clear (void);
void out_draw (int x, int y, int w, int h, int focus);
void out_key (int k);
void out_wheel (int d);

/* }================================================================== */


/*
** {==================================================================
** elsp.c - IntelliSense (language servers)
** ===================================================================
*/

typedef struct Diag {	/* a problem the server found */
  Pos a, b;
  int sev;	/* 1 error, 2 warning, 3 information, 4 hint */
  char *msg;
} Diag;

typedef struct CompItem {	/* a completion */
  char *label, *detail, *insert, *filter, *sort;
  int kind;	/* the protocol's CompletionItemKind */
  int has_range;	/* the server said what it replaces: a to b */
  int snippet;	/* insert is a snippet's body ($1, ${2:x} ...) */
  Pos a, b;
  char *doc;	/* its documentation (markdown), NULL: none said */
  struct TextEdit *extra;	/* additionalTextEdits: an import ... (path NULL) */
  size_t nextra;
  char *json;	/* the item as the server sent it, for completionItem/resolve; NULL: nothing to ask */
  int resolved;	/* resolve answered */
} CompItem;

typedef struct Sym {	/* a symbol of a file, for the Outline and the breadcrumbs */
  char *name;
  int kind;	/* the protocol's SymbolKind: 12 function, 23 struct ... */
  size_t line, end;	/* where it starts and ends */
  int depth;	/* inside how many others */
} Sym;

typedef struct InlayHint {	/* a server's hint shown in the text: "a:", ": int" */
  Pos at;	/* before the character there */
  char *label;	/* with its padding */
} InlayHint;

typedef struct SemTok {	/* a name the server knows: bytes x0 .. x1 of line y are token tok (T_*) */
  size_t y, x0, x1;
  int tok;
} SemTok;

typedef struct Lens {	/* a code lens: "3 references", "run test" over line y */
  size_t y;
  char *title;	/* "" until the server resolved it */
} Lens;

typedef struct InlineItem {	/* textDocument/inlineCompletion: a continuation offered at the cursor */
  char *text;	/* what it inserts; its lines separated by \n */
  Pos a, b;	/* the range it replaces; a == b == the cursor when it named none */
  int snippet;	/* its insertText was {"kind":2}: tab stops, like a snippet's body */
} InlineItem;

typedef struct NEditItem {	/* textDocument/copilotInlineEdit: the next edit, somewhere else in the file */
  char *text;	/* what goes in place of the range; its lines separated by 
 */
  Pos a, b;	/* the range it replaces, which is not where the cursor is */
} NEditItem;

typedef struct TextEdit {	/* a change the server asks for: its line and column, as it counts */
  char *path;
  size_t l0, c0, l1, c1;
  char *text;
  int utf16;	/* the columns are UTF-16 units, else bytes */
} TextEdit;

const char *lsp_lang (const char *syntax);	/* the protocol's language id, or NULL */
void lsp_open (Doc *d, const char *syntax);	/* its server starts when it must */
void lsp_close (Doc *d);
int lsp_active (const Doc *d);
int lsp_poll (void);	/* the servers' messages; 1: something came */
void lsp_complete (Doc *d, Pos at);	/* the answer comes to on_completion */
void lsp_define (Doc *d, Pos at);	/* the answer comes to on_definition */
void lsp_hover (Doc *d, Pos at);	/* to on_hover */
void lsp_signature (Doc *d, Pos at);	/* to on_signature */
void lsp_rename (Doc *d, Pos at, const char *name);	/* to on_edit */
void lsp_actions (Doc *d, Pos a, Pos b);
void lsp_bulb (Doc *d, size_t y);	/* to on_bulb */
void lsp_inlay (Doc *d, size_t y0, size_t y1);	/* to on_inlay */
int lsp_inline_able (const Doc *d);	/* its server said it answers textDocument/inlineCompletion */
/* editor.inlineSuggest: the continuation at the cursor; invoked: the user asked, else idle. To on_inline */
void lsp_inline (Doc *d, Pos at, int invoked);
void lsp_inline_cancel (void);	/* the answer is not wanted any more ($/cancelRequest) */
void lsp_inline_shown (size_t i);	/* item i is on the screen: textDocument/didShowCompletion */
void lsp_inline_partial (size_t i, size_t len);	/* len bytes of item i taken: didPartiallyAcceptCompletion */
void lsp_inline_accept (size_t i);	/* item i taken whole: its command, if it has one */

/*
** Next edit suggestions: textDocument/copilotInlineEdit, the same
** server's other question. It proposes the edit the change just made
** calls for somewhere else in the file, not a continuation at the
** cursor. No capability announces it, so it is asked of whatever
** answers inlineCompletion and dropped for good when that says the
** method is not there.
*/
int lsp_nedit_able (const Doc *d);	/* its inline server is up and has not refused the method */
void lsp_nedit (Doc *d, Pos at);	/* ask; the answer comes to on_nedit */
void lsp_nedit_cancel (void);	/* the answer is not wanted any more ($/cancelRequest) */
void lsp_nedit_shown (void);	/* it is on the screen: textDocument/didShowInlineEdit */
void lsp_nedit_accept (void);	/* taken: its command, through workspace/executeCommand */
enum { NE_IGNORED, NE_ACCEPTED, NE_REJECTED };	/* what became of a next edit that is going away */
void lsp_nedit_done (int what);	/* tell the server which, and report the one held here */

/*
** The inline-completion server's account and its status: Copilot's
** signIn / signOut / checkStatus, and the didChangeStatus notification
** it sends whenever it starts working, stops, or goes wrong.
*/
enum { CS_OFF, CS_INACTIVE, CS_NORMAL, CS_WARNING, CS_ERROR };	/* didChangeStatus "kind" */

typedef struct InlineStatus {
  int kind;	/* CS_*: the last didChangeStatus, CS_OFF until one comes */
  int busy;	/* it is fetching a suggestion */
  int signing;	/* a device flow is out: code and uri hold it */
  int ready;	/* the server answered initialize and is alive */
  int known;	/* it answered checkStatus: user is worth believing */
  char user[64];	/* who is signed in; "" nobody */
  char msg[200];	/* the last didChangeStatus message */
  char code[32], uri[200];	/* the device code, and the page to type it in */
  char chan[64];	/* its OUTPUT channel */
} InlineStatus;

int lsp_inline_configured (void);	/* mme.inlineCompletionServer names a program */
void lsp_inline_status (InlineStatus *out);	/* what that server last said about itself */
void lsp_inline_signin (void);	/* signIn, then its device flow; the answers come as toasts */
void lsp_inline_signout (void);	/* signOut */
void lsp_inline_check (void);	/* checkStatus (sent by itself when the server comes up) */

void lsp_semantic (Doc *d);	/* to on_semantic */
void lsp_lens (Doc *d);	/* to on_lens */
void lsp_lens_run (size_t i);	/* the command of on_lens's lens i */	/* to on_actions; then lsp_action_run */
void lsp_action_run (size_t i);
void lsp_symbols (Doc *d);	/* to on_symbols */

typedef struct Loc {	/* a place the server named */
  char *path;
  Pos a, b;
  int utf16;	/* the columns are the server's UTF-16 units (the file is not open) */
} Loc;

typedef struct WSym {	/* a symbol of the workspace */
  char *name, *detail, *path;
  int kind;
  Pos p;
  int utf16;
} WSym;

enum { LOC_REFS, LOC_IMPL, LOC_TYPE, LOC_PEEK, LOC_CALLS_IN, LOC_CALLS_OUT, LOC_SUPER, LOC_SUB };
void lsp_locations (Doc *d, Pos at, int what);	/* to on_locations */
void lsp_workspace_symbols (Doc *d, const char *query);	/* to on_workspace_symbols */
void lsp_highlights (Doc *d, Pos at);	/* to on_highlights */
int lsp_format (Doc *d);	/* to on_format; 0: no server to ask */
int lsp_format_range (Doc *d, Pos a, Pos b);	/* textDocument/rangeFormatting, to on_format; 0: none */
void lsp_format_type (Doc *d, Pos at, const char *ch);	/* onTypeFormatting, to on_format (quietly) */
const char *lsp_type_chars (const Doc *d);	/* the characters that format as they are typed, "" none */
unsigned lsp_comp_gen (void);	/* which completion answer this is */
void lsp_comp_resolve (Doc *d, size_t i, const char *json);	/* to on_comp_resolve */
void lsp_source_action (Doc *d, const char *kind, int apply);	/* apply: the first one done (2: quietly); else a menu */
int lsp_source_pending (void);	/* an organize imports (for a save) not answered yet */
void lsp_selection_range (Doc *d, Pos at);	/* to on_selection_ranges */
void lsp_folding (Doc *d);	/* to on_folding */
int lsp_hierarchy (Doc *d, Pos at, int what);	/* LOC_CALLS_IN ... LOC_SUB, to on_locations; 0: none */

typedef struct FoldRange {	/* a region the server can fold */
  size_t y0, y1;
  int comment;	/* kind "comment" */
  int region;	/* kind "region" */
} FoldRange;
const Diag *lsp_diags (const Doc *d, size_t *n);
void lsp_counts (int *errors, int *warnings);
size_t lsp_diag_files (void);
const Diag *lsp_diag_file (size_t i, char **path, size_t *n);	/* *path malloc'd */
char *lsp_path (const char *uri);
void lsp_shutdown (void);
enum { LS_NONE, LS_OFF, LS_MISSING, LS_STARTING, LS_READY, LS_BUSY, LS_DEAD };	/* lsp_state */
int lsp_state (const char *lang, char *name, size_t n);	/* LS_*; name: the program ("gopls") */
void lsp_restart (const char *lang);
const char *lsp_channel (const char *lang);	/* its OUTPUT channel, NULL none */
int lsp_progress_count (void);	/* $/progress running now */
int lsp_progress_text (int i, char *buf, size_t n);	/* its text; its percentage, -1 none */
void lsp_task_diags (const char *path, const Diag *v, size_t n);	/* a task's problems in a file */
void lsp_task_clear (void);

/* mme.c: where the answers go */
void on_completion (Doc *d, CompItem *v, size_t n);	/* takes v */
void on_definition (const char *path, Pos p, int utf16);
void on_hover (const char *markdown);	/* NULL: nothing to say */
typedef struct SigInfo {	/* a signature help's overload */
  char *label;
  size_t a0, a1;	/* the active parameter in label */
  char *pdoc;	/* that parameter's documentation, "" none */
  char *doc;	/* the signature's */
} SigInfo;

void on_signatures (SigInfo *v, int n, int active);	/* takes v; n 0: none */
void on_show_document (const char *path, const char *url, long line, long col);	/* window/showDocument */
void on_show_output (const char *chan);	/* the OUTPUT view, on this channel */
void on_edit (const TextEdit *v, size_t n);
void on_actions (const char *const *titles, size_t n);
void on_bulb (Doc *d, size_t y, size_t n, int fix);	/* line y has n code actions */
void on_inlay (Doc *d, size_t y0, size_t y1, InlayHint *v, size_t n);	/* takes v */
void on_inline (Doc *d, unsigned long edits, Pos at, InlineItem *v, size_t n);	/* takes v */
void on_nedit (Doc *d, unsigned long edits, Pos at, NEditItem *v, size_t n);	/* takes v */
void on_semantic (Doc *d, unsigned long edits, SemTok *v, size_t n);	/* takes v */
void on_lens (Doc *d, Lens *v, size_t n);	/* takes v */
void on_symbols (Doc *d, Sym *v, size_t n);	/* takes v */
void on_locations (int what, const Loc *v, size_t n);
void on_workspace_symbols (const WSym *v, size_t n);
void on_highlights (Doc *d, Pos at, const Pos *v, size_t n);	/* v: n pairs, a and b */
void on_format (Doc *d, const TextEdit *v, size_t n, int failed);
void on_comp_resolve (Doc *d, unsigned gen, size_t i, const char *detail, const char *doc, TextEdit *extra, size_t nextra);	/* takes extra */
void on_selection_ranges (Doc *d, Pos at, const Pos *v, size_t n);	/* v: n pairs, the innermost first */
void on_folding (Doc *d, unsigned long edits, FoldRange *v, size_t n);	/* takes v */
void on_edit_confirm (const TextEdit *v, size_t n);	/* a rename's edits: several files are asked about first */
char *open_doc_text (const char *path, size_t *len);	/* mme.c: the editor's text of path, NULL: not open */
void open_docs_list (Vec *out);	/* mme.c: the paths the editor has open (Search reads these itself) */
int open_doc_edit (const char *path, const TextEdit *v, size_t n);	/* mme.c: edited there; 0: not open */

/* }================================================================== */


/*
** {==================================================================
** edebug.c, etask.c - Run and Debug (the Debug Adapter Protocol), tasks
** ===================================================================
*/

enum { DM_BP = 1, DM_UNVERIFIED = 2, DM_DISABLED = 4, DM_TOP = 8, DM_FRAME = 16, DM_COND = 32, DM_LOG = 64 };	/* dbg_mark */
enum { DE_START, DE_STOP, DE_CONT, DE_END, DE_OPEN };	/* on_debug */

int dbg_active (void);	/* a session runs */
int dbg_stopped (void);	/* and it is paused */
int dbg_poll (void);	/* the adapter's messages; 1: something changed */
void dbg_command (int cmd);	/* CMD_DEBUG_* */
void dbg_toggle (const char *path, size_t line);	/* F9: a breakpoint (line from 0) */
int dbg_mark (const char *path, size_t line);	/* DM_*: what the gutter shows */
void dbg_lines (const char *path, size_t first, long delta);	/* lines went in / out at line first */
void dbg_hover (const char *expr);	/* its value, while paused, comes to on_hover */
void dbg_shutdown (void);
void dbg_start_json (const char *config);	/* a session with this launch configuration */
void dbg_edit_bp (const char *path, size_t line, int mode);	/* 0 condition, 1 hit count, 2 log message, -1 ask */
void dbg_enable_bp (const char *path, size_t line);	/* Disable / Enable Breakpoint */
void dbg_run_to (const char *path, size_t line);	/* Run to Cursor */
void dbg_jump_to (const char *path, size_t line);	/* Jump to Cursor */
void dbg_add_watch (const char *expr);	/* Debug: Add to Watch */
int dbg_inline (const char *path, size_t line, const char *s, size_t n, char *out, size_t cap);	/* debug.inlineValues */
int dbg_exception (const char *path, size_t line, const char **title, const char **desc);	/* the exception peek */
void debug_draw (int x, int y, int w, int h, int focus);	/* the Run and Debug view */
int debug_key (int k, SideAct *act);
void debug_click (int row, int col, SideAct *act);
void debug_wheel (int d);
void console_draw (int x, int y, int w, int h, int focus);	/* the DEBUG CONSOLE */
int console_key (int k);
void console_paste (const char *s, size_t n);
void console_wheel (int d);
void dbg_toolbar_draw (int x, int w, int y);	/* the floating toolbar, while debugging */
int dbg_toolbar_hit (int x, int y);	/* the command clicked; -1 on it, 0 not */

char *vs_subst (const char *s);	/* ${workspaceFolder}, ${file} ... put in */
void task_command (int cmd);	/* CMD_TASK_* */
void task_output (int id, const char *s, size_t n);	/* epanel.c: a task's terminal printed */
void task_done (int id, int code);

/* mme.c */
const char *editor_file (void);	/* the file in front, NULL none */
size_t editor_line (void);	/* the cursor's line there, from 0 */
void editor_save_all (void);	/* every file with changes saved (before a run) */
void on_debug (int what, const char *path, size_t line);	/* DE_*: the session started, stopped at path:line ... */
int on_task_terminal (const char *name, const char *cmd, const char *cwd, int id);	/* 0: it runs */

/* }================================================================== */

/*
** {==================================================================
** etest.c - the Testing view
** ===================================================================
*/

enum { TM_NONE, TM_UNSET, TM_PASS, TM_FAIL, TM_RUNNING, TM_QUEUED, TM_SKIP };	/* a test's state */

void test_draw (int x, int y, int w, int h, int focus);
int test_key (int k, SideAct *act);
void test_click (int row, int col, SideAct *act);
void test_wheel (int d);
int test_idle (void);	/* the runs go on; 1: something changed */
void test_show (void);	/* the view shows: the tests are looked for */
int test_mark (const char *path, size_t line);	/* TM_*: the test at that line, for the gutter */
void test_gutter (const char *path, size_t line);	/* its icon clicked: it runs */
void test_saved (const char *path);	/* a file was saved: its tests read again */
void test_command (int cmd);	/* CMD_TEST_* */
void test_shutdown (void);
int test_failed (void);	/* the tests that failed: the badge */

/* }================================================================== */


#endif
