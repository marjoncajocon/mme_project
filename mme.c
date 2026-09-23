/*
** mme.c - mme: VS Code for the terminal. main, the layout, the editor
** and the commands
**
** The window is VS Code's: the menu bar on top, the activity bar and the
** sidebar on the left (its edge drags), the editor with its tab and
** breadcrumbs, and the status bar at the bottom. Its keys are VS Code's
** too; menus, the quick input box and dialogs are in emenu.c.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { F_EDITOR, F_SIDE, F_PANEL };	/* who gets the keys */

/* a cursor, with its selection */
typedef struct Cur {
  Pos cur, anchor;
  int sel;
  size_t want;
} Cur;

/* an open file: its text and where the editor is in it */
typedef struct Tab {
  Doc *doc;	/* shared with the other group's tab of the same file */
  const Syntax *sx;	/* its language */
  char *real;	/* realpath of doc.path: the Explorer marks it */
  Pos cur, anchor;	/* the selection goes from anchor to cur */
  int sel;
  size_t want;	/* the column Up and Down aim for */
  size_t top, left;	/* the first line and column shown */
  size_t sub;	/* word wrap: the first of top's rows shown */
  size_t wmax;	/* the widest line's columns ... */
  unsigned long wmax_at;	/* ... worked out when doc->edits was this - 1 */
  int last_kind;	/* typing merges into one undo step */
  int preview;	/* VS Code's preview tab: the next file opened replaces it */
  size_t *fold;	/* the folded lines (the start of each region), in order */
  size_t nfold, capfold;
  Cur *mc;	/* the other cursors (Alt+Click, Ctrl+D ...); the main one is cur */
  int nmc, capmc;
  int page;	/* PAGE_SETTINGS, PAGE_WELCOME: a page, not a file (its doc stays empty) */
  int pinned;	/* VS Code's pinned tab: first, and Close Others / All leave it */
  int md;	/* the Markdown preview of doc (top counts the page's rows) */
  long used;	/* when it was last in front: Ctrl+Tab's order */
} Tab;

/* an editor group: its tabs; with a split editor there are two */
typedef struct Group {
  Tab **tab;	/* the tabs, in their order */
  int ntab, captab, active;
  int dtab;	/* the diff's tab is in this group */
  int diff;	/* and it is the one in front */
  int first;	/* the leftmost tab shown */
  int last_on;	/* the tab in front when they were drawn: the wheel may scroll away from it */
  int col;	/* its column of the editor grid (groups go column by column, top down) */
  int hw;	/* its height in its column, as a weight */
} Group;

typedef struct Ed {
  int cols, rows;	/* the terminal */
  int follow;	/* after this key, scroll the cursor into view */
  int side, view, side_w;	/* the sidebar: shown, which view, how wide */
  int focus;
  int resizing;	/* dragging the sidebar's edge */
  int bar_drag;	/* the side bar's scrollbar is held */
  int drag_text;	/* selecting with the mouse */
  int drag_unit;	/* 2: a double click drags by words, 3: a triple one by lines */
  Pos drag_a, drag_b;	/* the word or line clicked first */
  int drag_drop;	/* the mouse went down in the selection: it may be dragged elsewhere */
  int drop_moved;
  Pos drop;	/* where it would go */
  int link_y;	/* Ctrl+hover: the word underlined (y from 1, 0 none) */
  size_t link_x0, link_x1;
  int drag_col;	/* Shift+Alt+drag: a column selection */
  int finding;	/* the find widget has the keys */
  int find_open;	/* the find widget shows */
  char find[256];
  int find_case, find_word, find_regex;	/* Alt+C, Alt+W, Alt+R */
  int find_insel;	/* Alt+L: only in fsel_a .. fsel_b */
  Pos fsel_a, fsel_b;
  int replacing;	/* the find widget's second row: replace */
  int in_repl;	/* the keys go to the replace box */
  char repl[256];
  char *clip;	/* what Ctrl+C took */
  size_t cliplen;
  int panel, panel_h;	/* the terminal's panel: shown, how high */
  int resizing_panel;	/* dragging its top */
  int minimap;	/* VS Code's minimap on the right of the text */
  int drag_mm;	/* dragging in it */
  int drag_sb;	/* dragging the scrollbar's thumb: 1 + where in it it was taken */
  int drag_hsb;	/* the same for the horizontal one */
  int panel_view;	/* the panel shows: 0 the terminal, 1 the problems, 2 the output */
  int panel_max;	/* the panel has the whole editor area (View: Toggle Maximized Panel) */
  int chord;	/* Ctrl+K was pressed: the next key finishes it */
  int chord_test;	/* Ctrl+; was pressed: Testing's chord */
  int uchord;	/* the first key of a keybindings.json chord, 0 none */
  int nav_quiet;	/* the last key only moved or typed: no Go Back place */
  int tab_focus;	/* Ctrl+M: Tab moves the focus (Toggle Tab Key Moves Focus) */
  int wrap;	/* word wrap: long lines go on in the rows under them */
  int zen;	/* Zen mode: the text alone */
  int outline_focus;	/* in the Explorer, the keys go to the Outline */
  char tab_unit[16];	/* one indent: a tab, or spaces */
  size_t tab_unit_len;
  long long last_esc;	/* Esc Esc leaves Zen mode */
  const char *ptext;	/* what K_PASTE_TEXT and K_PASTE_LINE put in */
  size_t plen;
  const char **plines;
  size_t *plens;
  int pline;
  int quit;
  int tdrag;	/* a tab pressed (1), dragged (2) */
  int tdrag_g, tdrag_i, tdrag_x, tdrag_y;	/* its group, its index, where it was pressed */
  int fdrag, fdrag_x, fdrag_y;	/* the Explorer's rows pressed (1), dragged (2); where */
  int tdrop_g, tdrop_zone, tdrop_at;	/* where it would go: DROP_*, a tab index in the tab bar */
  int grp_resize;	/* a border between groups dragged: 1000 + g its right one, 2000 + g its bottom */
  int grp_max;	/* View: Toggle Editor Group Sizes: the group in front is big */
  int centered;	/* View: Toggle Centered Layout */
} Ed;

enum { DROP_NONE, DROP_IN, DROP_LEFT, DROP_RIGHT, DROP_UP, DROP_DOWN };

#define SHOW_MENU	(!E.zen && opt.menubar)	/* window.menuBarVisibility */
#define SHOW_STATUS	(!E.zen && opt.statusbar)	/* workbench.statusBar.visible */

static Ed E;
static Doc g_none_doc;
static Tab g_none;	/* what T is when no file is open */
static Tab *T = &g_none;	/* the tab in front */

#define MAX_GRP	8

static Group g_grp[MAX_GRP];	/* the editor groups, column by column, each column top down */
static int g_ngrp = 1, g_gcur;
static int g_colw[MAX_GRP] = {100, 100, 100, 100, 100, 100, 100, 100};	/* the columns' widths, as weights */
static Group *G = &g_grp[0];	/* the group in front */

#define HAS_DOC		(G->ntab > 0)
#define HAS_DIFF	(diff_active() && G->dtab)	/* the diff's tab is in this group */

/* keys of our own: what each cursor does in a paste or a cut */
#define K_PASTE_TEXT	(K_MOUSE + 1)
#define K_PASTE_LINE	(K_MOUSE + 2)
#define K_CUT_SEL	(K_MOUSE + 3)

enum { KIND_OTHER, KIND_TYPE, KIND_DELETE };

/* the pieces of the window, worked out for each picture */
typedef struct Layout {
  int body_y, body_h;	/* between the menu bar and the status bar */
  int act_w;	/* the activity bar's width: ACT_W, or 0 in Zen mode */
  int side_x, side_w;	/* the sidebar, its edge included */
  int ed_x, ed_w;	/* the editor group */
  int text_y, text_h;	/* its text, under the tab and the breadcrumbs */
  int mm_w;	/* the minimap's width, 0: none */
  int mml_w, mm_x;	/* editor.minimap.side "left": its width there (mm_w 0 then); where it is */
  int sb_w;	/* the scrollbar's: 1, or 0 */
  int hsb;	/* the horizontal scrollbar under the text: 1, or 0 */
  int text_h0;	/* the text's rows before it */
  int panel_y, panel_h;	/* the panel, its title row included; 0: none */
  int area_x, area_w;	/* the editor area: the groups side by side */
  int act_x;	/* the activity bar: on the left, or the right (workbench.sideBar.location) */
  int edge_x;	/* the sidebar's edge that drags */
  int ed_y, ed_h;	/* the group in front: its tab row, its height (with the tab row) */
  int gx[MAX_GRP], gw[MAX_GRP], gy[MAX_GRP], gh[MAX_GRP];	/* each group's part of it */
} Layout;

static Layout L;

/* the Outline, at the bottom of the Explorer */
static struct {
  int open;	/* the section is not collapsed (the chevron on its title) */
  size_t sel, top;	/* in vis */
  int y0, h;	/* where it is */
  size_t *vis;	/* the symbols shown, in their order: sorted, collapsed, filtered */
  size_t nvis, capvis;
  int nofollow;	/* Follow Cursor off */
  int sort;	/* 0 by position, 1 by name, 2 by category */
  int nofilter;	/* Filter on Type off */
  char filter[64];	/* typed while the Outline has the keys */
  Vec closed;	/* the symbols collapsed: "name\n depth" */
  const Doc *doc;	/* the file they are for */
} OL;

/* the Explorer's panes under the folders: E.outline_focus says which has the keys */
enum { PANE_TREE, PANE_OUTLINE, PANE_EDITORS, PANE_TIMELINE };

/* OPEN EDITORS: every group's tabs (collapsed at first, like VS Code) */
static struct {
  int open;
  size_t sel, top;
  int y0, h;
} OE;

/* TIMELINE: the file's Local History and its commits, the newest first */
typedef struct TItem {
  long long time;
  char *label;	/* "File Saved", or the commit's subject */
  char *detail;	/* the commit's author */
  char *file;	/* the local copy; NULL: a commit */
  char hash[16];
} TItem;

static struct {
  int open;
  char *path;	/* what the items are for */
  int stale;	/* read them again */
  TItem *v;
  size_t n, sel, top;
  int y0, h;
} TL;

static void run_command (int cmd);
static int editor_shape (void);
static void apply_act (const SideAct *act);
static int text_cols (void);
static void page_draw (int other);
static void fold_shift (int ins, Pos a, Pos b);
static void editor_key (int k);
static int indent_more (const char *s, size_t n);
static void outdent_typed (void);
static void paste_text (const char *s, size_t n);
static uint32_t uni_flag (uint32_t cp, uint32_t *ascii);
static char *uni_explain (Pos p);
static int qs_allowed (void);
static void comp_recent_add (const char *label, const char *pre, size_t n);
static void comp_recent_pick (const char *pre);
static void select_find_matches (void);
static void cursor_record (void);
static int tab_complete (void);
static void edit_extra (int cmd);
static void draw_comp (int gw);
static void plain_md (const char *s, size_t n, Buf *o);
static void draw_outline (int x, int y, int w, int h, int focus);
static int oe_height (void);
static int tl_height (void);
static void draw_open_editors (int x, int y, int w, int h, int focus);
static void draw_timeline (int x, int y, int w, int h, int focus);
static void md_preview (int side);
static void help_page (const char *name, void (*make) (Buf *b));
static void help_keys_md (Buf *b);
static void help_tips_md (Buf *b);
static const Sym *symbols (size_t *n);
static size_t ih_shift (size_t y, size_t x);
static int screen_at (Pos p, int *sx, int *sy);
static size_t sym_path (const Sym *v, size_t n, size_t *out, size_t cap);
static uint32_t sym_icon (int kind, int *st);
static void sig_close (void);
static void draw_signature (int gw);
static void draw_hover (int gw);
static void draw_problems (int x, int y, int w, int h, int focus);
static void problems_title (int y, int x1);
static int problems_title_click (int x);
static void clip_text (const char *s);
static int draw_sticky (int gw, Pos sa, Pos sb);
static void hl_row (int sy, size_t y, int gw, size_t from, size_t to, size_t left);
static const Pos *hl_ranges (size_t *n);
static void draw_peek (void);
static void draw_quick (int sy, size_t y, int gw, int is_cur);
static void draw_dirty_peek (void);
static void conflict_row (int sy, size_t y, int gw, size_t from, int other);
static int dirty_key (int k);
static int dirty_click (Mouse *m);
static int conflict_click (Mouse *m);
static void conflict_reset (void);
static long hunk_at (const QHunk *h, size_t n, size_t y);
static const QHunk *qhunks (size_t *n);
static void dirty_show (long i);
static void quick_cmd (int cmd);
static void merge_cmd (int cmd);
static int peek_is_open (void);
static void closed_push (const Tab *t);
static void palette (const char *init);
static void goto_symbol (const char *init);
static void goto_line (const char *init);
static void workspace_symbols (const char *init);


/*
** {==================================================================
** Columns
** ===================================================================
*/

static Row *row_at (size_t y) {
  if (y >= T->doc->n) y = T->doc->n - 1;	/* a line kept from before the text got shorter: never past the end */
  return &T->doc->row[y];
}


/* the width of the character at s[x] when it starts at column col */
static size_t char_width (const Row *r, size_t x, size_t col, size_t *len) {
  uint32_t cp = utf8_decode(r->s + x, r->len - x, len);
  if (cp == '\t') return TABW - col % TABW;
  if (cp < 32 || cp == 127) return eopt.control_chars ? 1 : 2;	/* shown as the control picture, or ^X */
  return (size_t)uc_width(cp);
}


static size_t col_of (const Row *r, size_t x) {
  size_t i = 0, col = 0, len;
  while (i < x && i < r->len) {
    col += char_width(r, i, col, &len);
    i += len;
  }
  return col;
}


/* the byte in r that is at column col, or the end of the line */
static size_t x_of_col (const Row *r, size_t col) {
  size_t i = 0, c = 0, len;
  while (i < r->len) {
    size_t w = char_width(r, i, c, &len);
    if (c + w > col) break;
    c += w;
    i += len;
  }
  return i;
}


static size_t next_x (const Row *r, size_t x) {
  size_t len;
  if (x >= r->len) return r->len;
  utf8_decode(r->s + x, r->len - x, &len);
  x += len;
  while (x < r->len) {	/* accents and joiners stay with their letter */
    uint32_t cp = utf8_decode(r->s + x, r->len - x, &len);
    if (cp < 0x300 || uc_width(cp) != 0) break;
    x += len;
  }
  return x;
}


static size_t prev_x (const Row *r, size_t x) {
  while (x > 0) {
    size_t len;
    uint32_t cp;
    x--;
    while (x > 0 && ((unsigned char)r->s[x] & 0xC0) == 0x80) x--;
    cp = utf8_decode(r->s + x, r->len - x, &len);
    if (cp < 0x300 || uc_width(cp) != 0) break;
  }
  return x;
}


/* 0 space, 1 word, 2 punctuation */
static int char_class (const Row *r, size_t x) {
  unsigned char c;
  if (x >= r->len) return 0;	/* past the end (an empty row has no text at all) */
  c = (unsigned char)r->s[x];
  if (c == ' ' || c == '\t') return 0;
  if (c >= 0x80) return 1;
  if (c < 32 || eopt.sep[c]) return 2;	/* editor.wordSeparators */
  return 1;
}


static Pos word_right (Pos p) {
  const Row *r = row_at(p.y);
  int k;
  if (p.x >= r->len) {
    if (p.y + 1 < T->doc->n) {
      p.y++;
      p.x = 0;
    }
    return p;
  }
  while (p.x < r->len && char_class(r, p.x) == 0) p.x++;
  if (p.x < r->len) {
    k = char_class(r, p.x);
    while (p.x < r->len && char_class(r, p.x) == k) p.x = next_x(r, p.x);
  }
  return p;
}


static Pos word_left (Pos p) {
  const Row *r = row_at(p.y);
  int k;
  if (p.x == 0) {
    if (p.y > 0) {
      p.y--;
      p.x = row_at(p.y)->len;
    }
    return p;
  }
  while (p.x > 0 && char_class(r, p.x - 1) == 0) p.x--;
  if (p.x > 0) {
    k = char_class(r, prev_x(r, p.x));
    while (p.x > 0 && char_class(r, prev_x(r, p.x)) == k) p.x = prev_x(r, p.x);
  }
  return p;
}


static size_t indent_end (const Row *r) {
  size_t x = 0;
  while (x < r->len && (r->s[x] == ' ' || r->s[x] == '\t')) x++;
  return x;
}

/* }================================================================== */


/*
** {==================================================================
** More than one cursor
** ===================================================================
*/

/* where p goes when the text from a to e is put in / taken out */
static Pos shift_ins (Pos p, Pos a, Pos e) {
  if (pos_cmp(p, a) < 0) return p;
  if (p.y == a.y) {
    p.x = e.x + (p.x - a.x);
    p.y = e.y;
  }
  else p.y += e.y - a.y;
  return p;
}


static Pos shift_del (Pos p, Pos a, Pos b) {
  if (pos_cmp(p, a) <= 0) return p;
  if (pos_cmp(p, b) < 0) return a;
  if (p.y == b.y) {
    p.x = a.x + (p.x - b.x);
    p.y = a.y;
  }
  else p.y -= b.y - a.y;
  return p;
}


/* the other group's view of the same text: its cursors stay with their text */
static void shift_views (int ins, Pos a, Pos b) {
  int g, i, k;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      Tab *t = g_grp[g].tab[i];
      Pos top;
      if (t == T || t->doc != T->doc) continue;
      top.y = t->top;
      top.x = 0;
      t->cur = ins ? shift_ins(t->cur, a, b) : shift_del(t->cur, a, b);
      t->anchor = ins ? shift_ins(t->anchor, a, b) : shift_del(t->anchor, a, b);
      t->top = (ins ? shift_ins(top, a, b) : shift_del(top, a, b)).y;
      for (k = 0; k < t->nmc; k++) {
        t->mc[k].cur = ins ? shift_ins(t->mc[k].cur, a, b) : shift_del(t->mc[k].cur, a, b);
        t->mc[k].anchor = ins ? shift_ins(t->mc[k].anchor, a, b) : shift_del(t->mc[k].anchor, a, b);
      }
    }
}


/*
** A snippet being filled in: Tab and Shift+Tab go from tab stop to tab
** stop; the stops, and the snippet, follow the text as it changes.
*/
typedef struct Stop {
  int idx;
  Pos a, b;
  char *choices;	/* ${1|a,b|}: "a\nb", shown as a list at the stop */
} Stop;

static struct {
  Tab *t;	/* the tab it is in; NULL: none */
  Stop *v;
  size_t n;
  int order[64];	/* the stops' numbers in the order Tab goes: 1, 2 ... then 0 */
  int norder, at;
  Pos a, b;	/* the whole snippet */
} SN;


static void snip_end (void) {
  size_t i;
  for (i = 0; i < SN.n; i++) free(SN.v[i].choices);
  free(SN.v);
  memset(&SN, 0, sizeof(SN));
}


/*
** The text changed at..e (inserted) or at..e went (deleted). The stop being
** filled in grows with what is typed at its end; the others move away.
*/
static void snip_shift (int ins, Pos at, Pos e) {
  size_t i;
  int now;
  if (SN.t == NULL || SN.t != T) return;
  if (!ins) {
    for (i = 0; i < SN.n; i++) {
      SN.v[i].a = shift_del(SN.v[i].a, at, e);
      SN.v[i].b = shift_del(SN.v[i].b, at, e);
    }
    SN.a = shift_del(SN.a, at, e);
    SN.b = shift_del(SN.b, at, e);
    return;
  }
  now = SN.at >= 0 && SN.at < SN.norder ? SN.order[SN.at] : -1;
  for (i = 0; i < SN.n; i++) {
    Stop *st = &SN.v[i];
    if (st->idx == now) {	/* the one typed in: its start stays, its end grows */
      if (pos_cmp(at, st->a) < 0) st->a = shift_ins(st->a, at, e);
      st->b = shift_ins(st->b, at, e);
    }
    else if (pos_cmp(st->a, st->b) == 0) st->a = st->b = shift_ins(st->a, at, e);
    else {
      st->a = shift_ins(st->a, at, e);
      if (pos_cmp(at, st->b) < 0) st->b = shift_ins(st->b, at, e);
    }
  }
  if (pos_cmp(at, SN.a) < 0) SN.a = shift_ins(SN.a, at, e);
  SN.b = shift_ins(SN.b, at, e);
}


/* every change of the text: the other cursors stay where their text is */
static Pos ed_insert (Pos at, const char *s, size_t n) {
  Pos e = doc_insert(T->doc, at, s, n);
  int i;
  if (e.y > at.y) dbg_lines(T->real, at.x == 0 ? at.y : at.y + 1, (long)(e.y - at.y));	/* the breakpoints go with their lines */
  shift_views(1, at, e);
  snip_shift(1, at, e);
  fold_shift(1, at, e);
  for (i = 0; i < T->nmc; i++) {
    T->mc[i].cur = shift_ins(T->mc[i].cur, at, e);
    T->mc[i].anchor = shift_ins(T->mc[i].anchor, at, e);
  }
  return e;
}


static void ed_delete (Pos a, Pos b) {
  int i;
  if (pos_cmp(a, b) >= 0) return;
  doc_delete(T->doc, a, b);
  if (b.y > a.y) dbg_lines(T->real, a.y + 1, -(long)(b.y - a.y));
  shift_views(0, a, b);
  snip_shift(0, a, b);
  fold_shift(0, a, b);
  for (i = 0; i < T->nmc; i++) {
    T->mc[i].cur = shift_del(T->mc[i].cur, a, b);
    T->mc[i].anchor = shift_del(T->mc[i].anchor, a, b);
  }
}


static void cur_get (Cur *c) {
  c->cur = T->cur;
  c->anchor = T->anchor;
  c->sel = T->sel;
  c->want = T->want;
}


static void cur_set (const Cur *c) {
  T->cur = c->cur;
  T->anchor = c->anchor;
  T->sel = c->sel;
  T->want = c->want;
}


/* the main cursor becomes one of the others; the caller moves the main one */
static void mc_push (void) {
  if (T->nmc == T->capmc) {
    T->capmc = T->capmc ? T->capmc * 2 : 8;
    T->mc = (Cur *)xrealloc(T->mc, (size_t)T->capmc * sizeof(Cur));
  }
  cur_get(&T->mc[T->nmc++]);
}


/* cursors that ended up in the same place become one */
static void mc_merge (void) {
  int i, j, n = 0;
  for (i = 0; i < T->nmc; i++) {
    Cur *c = &T->mc[i];
    int dup = pos_cmp(c->cur, T->cur) == 0;
    for (j = 0; j < n && !dup; j++) dup = pos_cmp(T->mc[j].cur, c->cur) == 0;
    if (!dup) T->mc[n++] = *c;
  }
  T->nmc = n;
}


/* is p in the selection of one of the other cursors? */
static int in_other_sel (Pos p) {
  int i;
  for (i = 0; i < T->nmc; i++) {
    const Cur *c = &T->mc[i];
    Pos a = c->anchor, b = c->cur;
    if (!c->sel) continue;
    if (pos_cmp(a, b) > 0) {
      Pos t = a;
      a = b;
      b = t;
    }
    if (pos_cmp(p, a) >= 0 && pos_cmp(p, b) < 0) return 1;
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Brackets: the pair of the one at the cursor, colors by depth
** ===================================================================
*/

static int is_open (int c) {
  return c == '(' || c == '[' || c == '{';
}


static int is_close (int c) {
  return c == ')' || c == ']' || c == '}';
}


static int pair_of (int c) {
  switch (c) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case ')': return '(';
    case ']': return '[';
    case '}': return '{';
  }
  return 0;
}


/* as line_tokens, but never the TextMate grammar: for whole-file work */
static const unsigned char *line_tokens_quick (size_t y) {
  static unsigned char *tok;
  static size_t cap;
  const Row *r = row_at(y);
  if (r->len + 1 > cap) {
    cap = r->len + 256;
    tok = (unsigned char *)xrealloc(tok, cap);
  }
  syntax_line_quick(T->doc, T->sx, y, tok);
  return tok;
}


/* the tokens of line y: a bracket in a string or a comment does not count */
static const unsigned char *line_tokens (size_t y) {
  static unsigned char *tok;
  static size_t cap;
  const Row *r = row_at(y);
  if (r->len + 1 > cap) {
    cap = r->len + 256;
    tok = (unsigned char *)xrealloc(tok, cap);
  }
  syntax_line(T->doc, T->sx, y, tok);
  return tok;
}


static int code_at (const unsigned char *tok, size_t x) {
  return tok[x] != T_STRING && tok[x] != T_COMMENT && tok[x] != T_ESCAPE;
}


/* the bracket that pairs with the one at p; 1 found */
static int match_bracket (Pos p, Pos *m) {
  const Row *r = row_at(p.y);
  int c = (unsigned char)r->s[p.x], want = pair_of(c), depth = 0, dir = is_open(c) ? 1 : -1;
  size_t y = p.y, lines = 0;
  long x = (long)p.x;
  const unsigned char *tok = line_tokens(y);
  if (!code_at(tok, p.x)) return 0;
  for (;;) {
    x += dir;
    while (x < 0 || x >= (long)r->len) {	/* the next line, or the one before */
      if ((dir < 0 && y == 0) || (dir > 0 && y + 1 >= T->doc->n) || ++lines > 3000) return 0;
      y += (size_t)dir;
      r = row_at(y);
      tok = line_tokens(y);
      x = dir > 0 ? 0 : (long)r->len - 1;
      if (r->len == 0) x = -2;
      if (x == -2) continue;
    }
    if (!code_at(tok, (size_t)x)) continue;
    if (r->s[x] == c) depth++;
    else if (r->s[x] == want) {
      if (depth == 0) {
        m->y = y;
        m->x = (size_t)x;
        return 1;
      }
      depth--;
    }
  }
}


/* the brackets to light up: at the cursor, else just before it */
static int cursor_brackets (Pos *a, Pos *b) {
  const Row *r;
  Pos p = T->cur;
  if (!HAS_DOC || T->sel) return 0;
  r = row_at(p.y);
  if (p.x < r->len && (is_open((unsigned char)r->s[p.x]) || is_close((unsigned char)r->s[p.x]))) ;
  else if (p.x > 0 && (is_open((unsigned char)r->s[p.x - 1]) || is_close((unsigned char)r->s[p.x - 1]))) p.x--;
  else return 0;
  *a = p;
  return match_bracket(p, b);
}


/* how deep the brackets are at the start of each line: again after an edit */
static size_t *g_depth;
static size_t g_ndepth;
static const Doc *g_depth_doc;

/*
** The depths are kept between frames: an edit only counts the lines from it
** on (the text above them cannot have changed), and the count stops at the
** last line the editor shows, so a huge file costs no more than a screen.
*/
static size_t g_depth_n;	/* counted for lines 0 .. g_depth_n - 1 */

static size_t depth_at (size_t y) {
  size_t k, d, want = y + 1, last = T->top + (size_t)L.text_h + 1;
  if (g_depth_doc != T->doc || g_ndepth != T->doc->n + 1) {	/* another file, or lines came or went */
    g_ndepth = T->doc->n + 1;
    g_depth = (size_t *)xrealloc(g_depth, g_ndepth * sizeof(size_t));
    g_depth_n = 0;
    g_depth_doc = T->doc;
  }
  if (T->doc->br_from < g_depth_n) g_depth_n = T->doc->br_from;	/* an edit: from its line on again */
  T->doc->br_from = (size_t)-1;
  if (want < last) want = last;	/* what the screen needs now, in one go */
  if (want > T->doc->n) want = T->doc->n;
  if (g_depth_n == 0) g_depth[0] = 0;
  for (k = g_depth_n, d = g_depth[g_depth_n]; k < want; k++) {
    const Row *r = row_at(k);
    const unsigned char *tok = r->len > 0 ? line_tokens_quick(k) : NULL;
    size_t x;
    g_depth[k] = d;
    if (tok == NULL) {
      g_depth[k + 1] = d;
      continue;
    }
    for (x = 0; x < r->len; x++) {
      int c = (unsigned char)r->s[x];
      if (!code_at(tok, x)) continue;
      if (is_open(c)) d++;
      else if (is_close(c) && d > 0) d--;
    }
    g_depth[k + 1] = d;
  }
  if (want > g_depth_n) g_depth_n = want;
  return y < g_depth_n ? g_depth[y] : 0;
}


/* after a row is drawn: its brackets in their depth's color, the pair at the cursor lit */
static void color_brackets (int sy, size_t y, int gw, size_t from, size_t to, size_t left, Pos ba, Pos bb, int lit) {
  const Row *r = row_at(y);
  const unsigned char *tok;
  size_t x, d, col = 0, len;
  static const int level[3] = {C_BRACKET1, C_BRACKET2, C_BRACKET3};
  if (!opt.pair_colors && !lit) return;
  tok = line_tokens(y);
  d = depth_at(y);
  for (x = 0; x < r->len && x < to; x += len) {
    int c = (unsigned char)r->s[x];
    size_t w = char_width(r, x, col, &len), vc = col + ih_shift(y, x);
    int sx = L.ed_x + gw + (int)(vc - left);
    col += w;
    if (!(is_open(c) || is_close(c)) || !code_at(tok, x)) continue;
    if (is_close(c) && d > 0) d--;
    if (x >= from && vc >= left && vc < left + (size_t)text_cols()) {
      if (opt.pair_colors) scr_set_fg(sx, sy, ui_color(level[d % 3]));
      if (lit && ((ba.y == y && ba.x == x) || (bb.y == y && bb.x == x)))
        scr_set_bg(sx, sy, ui_color(C_BRACKET_MATCH));
    }
    if (is_open(c)) d++;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Folding: VS Code's, by indentation. A region is the lines after a line
** that are more indented than it (blank lines inside it too).
** ===================================================================
*/

static int tag_char (int c);


static int blank (size_t y) {
  const Row *r = row_at(y);
  return indent_end(r) == r->len;
}


/* the server's folding ranges of a file (editor.foldingStrategy "auto") */
static struct {
  Doc *d;
  FoldRange *v;	/* by y0 */
  size_t n;
  const Doc *ask_d;
  unsigned long ask_edits, seen;
  long long since;
} FRG;


static int cmp_fold (const void *a, const void *b) {
  const FoldRange *x = (const FoldRange *)a, *y = (const FoldRange *)b;
  return x->y0 < y->y0 ? -1 : x->y0 > y->y0;
}


void on_folding (Doc *d, unsigned long edits, FoldRange *v, size_t n) {
  (void)edits;
  free(FRG.v);
  FRG.d = d;
  FRG.v = v;
  FRG.n = n;
  qsort(FRG.v, n, sizeof(FoldRange), cmp_fold);
}


/* #region / #endregion (// #region, #pragma region, <!-- #region -->): 1 start, 2 end, 0 neither */
static int region_mark (size_t y) {
  const Row *r = row_at(y);
  size_t x = indent_end(r);
  const char *s = r->s + x;
  size_t n = r->len - x, k;
  static const char *const pre[] = {"<!--", "//", "/*", "--", ";", "'"};
  for (k = 0; k < sizeof(pre) / sizeof(pre[0]); k++)
    if (n >= strlen(pre[k]) && memcmp(s, pre[k], strlen(pre[k])) == 0) {
      s += strlen(pre[k]);
      n -= strlen(pre[k]);
      break;
    }
  while (n && *s == ' ') s++, n--;
  if (n >= 8 && memcmp(s, "#pragma ", 8) == 0) {
    s += 8;
    n -= 8;
    if (n >= 6 && memcmp(s, "region", 6) == 0) return 1;
    if (n >= 9 && memcmp(s, "endregion", 9) == 0) return 2;
    return 0;
  }
  if (n >= 7 && memcmp(s, "#region", 7) == 0 && (n == 7 || !tag_char((unsigned char)s[7]))) return 1;
  if (n >= 10 && memcmp(s, "#endregion", 10) == 0) return 2;
  return 0;
}


/* a #region's #endregion line, (size_t)-1 none */
static size_t region_end (size_t y) {
  size_t k, depth = 0;
  if (region_mark(y) != 1) return (size_t)-1;
  for (k = y + 1; k < T->doc->n && k < y + 20000; k++) {
    int m = region_mark(k);
    if (m == 1) depth++;
    else if (m == 2 && depth-- == 0) return k;
  }
  return (size_t)-1;
}


/* a block comment opened (and not closed) on line y: the line it closes on, (size_t)-1 none */
static size_t comment_end (size_t y) {
  const char *lc, *bo, *bc;
  const Row *r = row_at(y);
  size_t x = indent_end(r), ol, cl, k;
  syntax_comment(T->sx, &lc, &bo, &bc);
  if (bo == NULL || bc == NULL) return (size_t)-1;
  ol = strlen(bo);
  cl = strlen(bc);
  if (r->len - x < ol || memcmp(r->s + x, bo, ol) != 0) return (size_t)-1;
  for (k = x + ol; k + cl <= r->len; k++)
    if (memcmp(r->s + k, bc, cl) == 0) return (size_t)-1;	/* closed on it */
  for (k = y + 1; k < T->doc->n && k < y + 5000; k++) {
    const Row *q = row_at(k);
    size_t i;
    for (i = 0; i + cl <= q->len; i++)
      if (memcmp(q->s + i, bc, cl) == 0) return k;
  }
  return (size_t)-1;
}


/* the last line of the region that line y starts; y itself: none */
static size_t fold_end (size_t y) {
  size_t base, k, last = y, n = T->doc->n, e;
  if (y >= n || blank(y)) return y;
  if ((e = region_end(y)) != (size_t)-1 || (e = comment_end(y)) != (size_t)-1) return e;
  if (FRG.d == T->doc && FRG.n) {	/* the server's */
    size_t lo = 0, hi = FRG.n;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (FRG.v[mid].y0 < y) lo = mid + 1;
      else hi = mid;
    }
    for (e = y; lo < FRG.n && FRG.v[lo].y0 == y; lo++)
      if (FRG.v[lo].y1 > e) e = FRG.v[lo].y1;
    return e < n ? e : n - 1;
  }
  base = col_of(row_at(y), indent_end(row_at(y)));
  for (k = y + 1; k < n; k++) {
    const Row *r = row_at(k);
    if (blank(k)) continue;
    if (col_of(r, indent_end(r)) <= base) break;
    last = k;
  }
  return last;
}


static int is_folded (size_t y) {
  size_t lo = 0, hi = T->nfold;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (T->fold[mid] < y) lo = mid + 1;
    else hi = mid;
  }
  return lo < T->nfold && T->fold[lo] == y;
}


static void fold_add (size_t y) {
  size_t i = 0;
  if (is_folded(y) || fold_end(y) == y) return;
  if (T->nfold == T->capfold) {
    T->capfold = T->capfold ? T->capfold * 2 : 16;
    T->fold = (size_t *)xrealloc(T->fold, T->capfold * sizeof(size_t));
  }
  while (i < T->nfold && T->fold[i] < y) i++;
  memmove(T->fold + i + 1, T->fold + i, (T->nfold - i) * sizeof(size_t));
  T->fold[i] = y;
  T->nfold++;
}


static void fold_del (size_t y) {
  size_t i;
  for (i = 0; i < T->nfold; i++)
    if (T->fold[i] == y) {
      memmove(T->fold + i, T->fold + i + 1, (T->nfold - i - 1) * sizeof(size_t));
      T->nfold--;
      return;
    }
}


/* the folded region line y is hidden in: its start, or (size_t)-1 */
static size_t hidden_in (size_t y) {
  size_t i, found = (size_t)-1;
  for (i = 0; i < T->nfold && T->fold[i] < y; i++)
    if (fold_end(T->fold[i]) >= y && found == (size_t)-1) found = T->fold[i];	/* the outermost */
  return found;
}


/* the next line shown after y (past a folded region), and the one before */
static size_t line_next (size_t y) {
  return (T->nfold && is_folded(y)) ? fold_end(y) + 1 : y + 1;
}


static size_t line_prev (size_t y) {
  size_t s;
  if (y == 0) return 0;
  y--;
  if (T->nfold && (s = hidden_in(y)) != (size_t)-1) return s;
  return y;
}


/* the cursor went into a folded region (a find, a jump): it opens */
static void fold_reveal (void) {
  size_t s, guard = 0;
  if (!HAS_DOC || T->nfold == 0) return;
  while ((s = hidden_in(T->cur.y)) != (size_t)-1 && guard++ < 1000) fold_del(s);
}


/* an edit moved the lines: the folds after it move too */
static void fold_shift (int ins, Pos a, Pos b) {
  size_t i, n = 0;
  if (T->nfold == 0 || a.y == b.y) return;
  for (i = 0; i < T->nfold; i++) {
    size_t f = T->fold[i];
    if (f > a.y) {
      if (ins) f += b.y - a.y;
      else if (f <= b.y) continue;	/* its line went */
      else f -= b.y - a.y;
    }
    T->fold[n++] = f;
  }
  T->nfold = n;
}


/* the region the cursor is in: the innermost that has it */
static size_t fold_at_cursor (void) {
  size_t y = T->cur.y, k;
  for (k = 0; k < 5000; k++) {
    if (fold_end(y) >= T->cur.y && fold_end(y) > y) return y;
    if (y == 0) break;
    y--;
  }
  return (size_t)-1;
}


static void fold_cmd (int what) {
  size_t y;
  if (!HAS_DOC || G->diff) return;
  switch (what) {
    case CMD_FOLD:
      if ((y = fold_at_cursor()) != (size_t)-1) {
        fold_add(y);
        T->cur.y = y;
        T->cur = doc_clamp(T->doc, T->cur);
        T->sel = 0;
      }
      break;
    case CMD_UNFOLD:
      if (is_folded(T->cur.y)) fold_del(T->cur.y);
      break;
    case CMD_FOLD_ALL:
      for (y = 0; y < T->doc->n; y++)
        if (!blank(y) && y + 1 < T->doc->n && fold_end(y) > y) fold_add(y);
      {	/* the cursor to the line its region shows as */
        size_t s = hidden_in(T->cur.y);
        if (s != (size_t)-1) {
          T->cur.y = s;
          T->cur = doc_clamp(T->doc, T->cur);
          T->sel = 0;
        }
      }
      break;
    case CMD_UNFOLD_ALL: T->nfold = 0; break;
    case CMD_FOLD_REGIONS:	/* Ctrl+K Ctrl+8: the #regions */
      for (y = 0; y < T->doc->n; y++)
        if (region_mark(y) == 1 && region_end(y) != (size_t)-1) fold_add(y);
      break;
    case CMD_UNFOLD_REGIONS:
      for (y = 0; y < T->doc->n; y++)
        if (region_mark(y) == 1) fold_del(y);
      break;
    case CMD_FOLD_COMMENTS: {	/* Ctrl+K Ctrl+/: the block comments (the server's too) */
      size_t k;
      for (y = 0; y < T->doc->n; y++)
        if (comment_end(y) != (size_t)-1) fold_add(y);
      for (k = 0; FRG.d == T->doc && k < FRG.n; k++)
        if (FRG.v[k].comment && FRG.v[k].y0 < T->doc->n) fold_add(FRG.v[k].y0);
      break;
    }
    default:
      if (what >= CMD_FOLD_L1 && what <= CMD_FOLD_L7) {	/* Ctrl+K Ctrl+n: the regions n deep, not the cursor's */
        size_t *end = (size_t *)xmalloc((T->doc->n + 1) * sizeof(size_t)), ns = 0, e;
        size_t level = (size_t)(what - CMD_FOLD_L1) + 1;
        for (y = 0; y < T->doc->n; y++) {
          while (ns > 0 && end[ns - 1] < y) ns--;
          if (blank(y) || (e = fold_end(y)) <= y) continue;
          if (ns + 1 == level && !(y <= T->cur.y && T->cur.y <= e)) fold_add(y);
          end[ns++] = e;
        }
        free(end);
      }
  }
  if (what != CMD_UNFOLD_ALL && what != CMD_UNFOLD && what != CMD_UNFOLD_REGIONS) {	/* the cursor to the line its region shows as */
    size_t s = hidden_in(T->cur.y);
    if (s != (size_t)-1) {
      T->cur.y = s;
      T->cur = doc_clamp(T->doc, T->cur);
      T->sel = 0;
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** Find: the matches of E.find
** ===================================================================
*/

static int lower (int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}


/* E.find as a regular expression (".*" on): made again when it or Alt+C change */
static const Regex *find_re (void) {
  static Regex *re;
  static char was[258];
  char key[258];
  const char *err;
  snprintf(key, sizeof(key), "%c%s", E.find_case ? 'C' : 'c', E.find);
  if (re && strcmp(key, was) == 0) return re;
  re_free(re);
  re = re_compile(E.find, !E.find_case, &err);
  snprintf(was, sizeof(was), "%s", key);
  return re;
}


/* does E.find start at byte x of line y? the match's length, 0: no */
static size_t match_at (size_t y, size_t x) {
  const Row *r = row_at(y);
  size_t n = strlen(E.find), i;
  if (n == 0 || x >= r->len) return 0;
  if (E.find_insel) {	/* Find in Selection */
    Pos p;
    p.y = y;
    p.x = x;
    if (pos_cmp(p, E.fsel_a) < 0 || pos_cmp(p, E.fsel_b) >= 0) return 0;
  }
  if (E.find_regex) {
    const Regex *re = find_re();
    size_t e;
    if (re == NULL || (x > 0 && ((unsigned char)r->s[x] & 0xC0) == 0x80)) return 0;
    if (!re_at(re, r->s, r->len, x, &e, NULL) || e == x) return 0;
    n = e - x;
  }
  else {
    if (x + n > r->len) return 0;
    for (i = 0; i < n; i++) {
      int a = (unsigned char)r->s[x + i], b = (unsigned char)E.find[i];
      if (E.find_case ? a != b : lower(a) != lower(b)) return 0;
    }
  }
  if (E.find_word) {
    if (x > 0 && char_class(r, x - 1) == 1) return 0;
    if (x + n < r->len && char_class(r, x + n) == 1) return 0;
  }
  if (E.find_insel && (y > E.fsel_b.y || (y == E.fsel_b.y && x + n > E.fsel_b.x))) return 0;
  return n;
}


/* the next match from p (back: the one before p), round the end; 1 found */
static int find_from (Pos p, int back, Pos *found) {
  size_t i, y, x;
  if (E.find[0] == '\0') return 0;
  for (i = 0; i <= T->doc->n; i++) {
    const Row *r;
    y = back ? (p.y + T->doc->n - i) % T->doc->n : (p.y + i) % T->doc->n;
    if (E.find_insel && (y < E.fsel_a.y || y > E.fsel_b.y)) continue;
    r = row_at(y);
    if (!back) {
      size_t from = (i == 0) ? p.x : 0, to = (i == T->doc->n) ? p.x : r->len;
      for (x = from; x < to || (x == to && i != T->doc->n && x < r->len); x++)
        if (match_at(y, x)) goto hit;
    }
    else {
      size_t lim = (i == 0) ? p.x : r->len + 1;
      for (x = lim; x-- > 0;)
        if (match_at(y, x)) goto hit;
    }
  }
  return 0;
hit:
  found->y = y;
  found->x = x;
  return 1;
}


/* how many matches, and which one starts at p (0: none) */
static size_t count_matches (Pos p, size_t *which) {
  size_t y, x, n = 0;
  *which = 0;
  if (E.find[0] == '\0') return 0;
  for (y = 0; y < T->doc->n; y++) {
    const Row *r = row_at(y);
    if (E.find_insel && (y < E.fsel_a.y || y > E.fsel_b.y)) continue;
    for (x = 0; x < r->len; x++) {
      size_t m = match_at(y, x);
      if (m) {
        n++;
        if (y == p.y && x == p.x) *which = n;
        if (E.find_regex) x += m - 1;	/* a regex's matches do not overlap */
      }
    }
  }
  return n;
}

/* }================================================================== */


/*
** {==================================================================
** Drawing
** ===================================================================
*/

#define MM_W	12	/* the minimap: columns */
#define MM_CH	3	/* characters of text in one dot of it */
#define MM_LINES	4	/* lines of text in one row of it */

static void group_layout (void);
static int gutter_width (void);
static int text_cols (void);

/* total cells in n parts as their weights say, each at least min when there is room */
static void share (const int *wt, int n, int total, int min, int *out) {
  long sum = 0;
  int i, used = 0;
  if (n <= 0) return;
  for (i = 0; i < n; i++) sum += wt[i] > 0 ? wt[i] : 1;
  for (i = 0; i < n; i++) {
    out[i] = (int)((long)total * (wt[i] > 0 ? wt[i] : 1) / sum);
    used += out[i];
  }
  out[n - 1] += total - used;
  if (total < n * min) return;
  for (i = 0; i < n; i++)
    while (out[i] < min) {	/* from the biggest */
      int j, big = 0;
      for (j = 1; j < n; j++)
        if (out[j] > out[big]) big = j;
      if (big == i || out[big] <= min) break;
      out[big]--;
      out[i]++;
    }
}


static int grp_ncol (void) {
  return g_ngrp ? g_grp[g_ngrp - 1].col + 1 : 1;
}


/* the groups' places in the editor area: columns side by side, each split top down */
static void layout_groups (int ex, int ey, int ew, int eh) {
  int ncol = grp_ncol(), cw[MAX_GRP], c, x = ex, g = 0;
  if (E.centered && g_ngrp == 1 && ew > 100) {	/* Centered Layout: the text in the middle */
    int w = ew * 3 / 5 > 100 ? ew * 3 / 5 : 100;
    ex += (ew - w) / 2;
    ew = w;
    x = ex;
  }
  share(g_colw, ncol, ew - (ncol - 1), 8, cw);
  for (c = 0; c < ncol; c++) {
    int first = g, n, wt[MAX_GRP], rh[MAX_GRP], k, y = ey;
    while (g < g_ngrp && g_grp[g].col == c) g++;
    n = g - first;
    if (n == 0) continue;
    for (k = 0; k < n; k++) {
      if (g_grp[first + k].hw <= 0) g_grp[first + k].hw = 100;
      wt[k] = g_grp[first + k].hw;
    }
    share(wt, n, eh - (n - 1), 4, rh);
    for (k = 0; k < n; k++) {
      L.gx[first + k] = x;
      L.gw[first + k] = cw[c] > 1 ? cw[c] : 1;
      L.gy[first + k] = y;
      L.gh[first + k] = rh[k] > 1 ? rh[k] : 1;
      y += rh[k] + 1;
    }
    x += cw[c] + 1;
  }
}


/* the group g's place becomes the editor's: L.ed_*, L.text_y and the text's rows */
static void use_group (int g) {
  L.ed_x = L.gx[g];
  L.ed_w = L.gw[g];
  L.ed_y = L.gy[g];
  L.ed_h = L.gh[g];
  L.text_y = L.ed_y + 2;
  L.text_h0 = L.ed_h - 2 > 1 ? L.ed_h - 2 : 1;
}


static void layout (void) {
  int sw = 0;
  L.act_w = (E.zen || !opt.activitybar) ? 0 : ACT_W;
  L.body_y = SHOW_MENU ? 1 : 0;
  L.body_h = E.rows - L.body_y - (SHOW_STATUS ? 1 : 0);
  if (L.body_h < 1) L.body_h = 1;
  if (E.side && !E.zen) {
    sw = E.side_w;
    if (sw > E.cols - L.act_w - 20) sw = E.cols - L.act_w - 20;
    if (sw < 12) sw = E.cols - L.act_w > 24 ? 12 : 0;
  }
  L.side_w = sw;
  if (opt.side_right) {	/* the editor, the side bar, the activity bar */
    L.act_x = E.cols - L.act_w;
    L.area_x = 0;
    L.area_w = E.cols - L.act_w - sw > 1 ? E.cols - L.act_w - sw : 1;
    L.edge_x = L.area_w;
    L.side_x = L.edge_x + 1;
  }
  else {
    L.act_x = 0;
    L.side_x = L.act_w;
    L.area_x = L.act_w + sw;
    L.area_w = E.cols - L.area_x > 1 ? E.cols - L.area_x : 1;
    L.edge_x = L.area_x - 1;
  }
  L.panel_h = 0;
  if (E.panel && !E.zen && L.body_h >= 10) {	/* the panel: at least 4 rows, and room for the text */
    L.panel_h = E.panel_h;
    if (L.panel_h > L.body_h - 6) L.panel_h = L.body_h - 6;
    if (L.panel_h < 4) L.panel_h = 4;
    if (E.panel_max) L.panel_h = L.body_h;	/* the editor under it is not seen */
  }
  L.panel_y = L.body_y + L.body_h - L.panel_h;
  layout_groups(L.area_x, L.body_y, L.area_w, L.body_h - L.panel_h);
  use_group(g_gcur);
  group_layout();
}


/* the widest line, in columns: kept until the text changes */
static size_t doc_width (void) {
  size_t y, w = 0;
  if (!HAS_DOC) return 0;
  if (T->wmax_at == T->doc->edits + 1) return T->wmax;
  for (y = 0; y < T->doc->n; y++) {
    const Row *r = row_at(y);
    size_t c;
    if (r->len <= w) continue;	/* it can't be wider than its bytes */
    c = col_of(r, r->len);
    if (c > w) w = c;
  }
  T->wmax = w;
  T->wmax_at = T->doc->edits + 1;
  return w;
}


/* the group in front's minimap and scrollbars; the text's rows under them */
static void group_layout (void) {
  L.mm_w = (E.minimap && HAS_DOC && !G->diff && !T->page && L.ed_w >= 60) ? MM_W : 0;
  L.sb_w = (HAS_DOC && !G->diff && !T->page && L.ed_w >= 30) ? 1 : 0;
  L.mml_w = 0;
  L.mm_x = L.ed_x + L.ed_w - L.sb_w - L.mm_w;
  if (L.mm_w > 0 && vopt.mm_left) {	/* editor.minimap.side "left": left of the text, which moves over */
    L.mml_w = L.mm_w;
    L.mm_w = 0;
    L.mm_x = L.ed_x;
    L.ed_x += L.mml_w;
    L.ed_w -= L.mml_w;
  }
  L.text_h = L.text_h0;
  L.hsb = 0;
  if (HAS_DOC && !G->diff && !T->page && !E.wrap && L.text_h0 > 3) {	/* a line wider than the text */
    int tc = L.ed_w - gutter_width() - L.mm_w - L.sb_w;
    if (tc > 1 && doc_width() + 1 > (size_t)tc) {
      L.hsb = 1;
      L.text_h = L.text_h0 - 1;
    }
  }
}


int side_width (void) {
  return L.side_w - 1;	/* without the edge */
}


static int gutter_width (void) {
  int w = 1;
  if (!opt.line_numbers) return 2;
  size_t n = T->doc->n;
  while (n >= 10) {
    n /= 10;
    w++;
  }
  return (w < 3 ? 3 : w) + 3;	/* " 123  " */
}


static int text_cols (void) {
  int w = L.ed_w - gutter_width() - L.mm_w - L.sb_w;
  return w > 1 ? w : 1;
}


/*
** Word wrap: a line too long for the text is cut into rows, after a space
** when there is one, like VS Code's. wrap_segs gives where each row starts.
*/
#define MAXSEG	512

static size_t wrap_segs (size_t y, size_t *st) {
  const Row *r = row_at(y);
  size_t n = 1, x = 0, col = 0, segcol = 0, lastsp = 0, width = (size_t)text_cols(), len;
  st[0] = 0;
  if (!E.wrap || width < 4) return 1;
  while (x < r->len) {
    size_t w = char_width(r, x, col, &len);
    if (col + w - segcol > width && x > st[n - 1]) {	/* it does not fit: a new row */
      size_t cut = lastsp > st[n - 1] ? lastsp : x;
      if (n == MAXSEG) break;
      st[n++] = cut;
      x = cut;
      col = segcol = col_of(r, cut);
      lastsp = 0;
      continue;
    }
    if (r->s[x] == ' ' || r->s[x] == '\t') lastsp = x + len;
    col += w;
    x += len;
  }
  return n;
}


/* the row of line y that x is in */
static size_t seg_of (size_t y, size_t x) {
  size_t st[MAXSEG], n = wrap_segs(y, st), k = 0;
  while (k + 1 < n && st[k + 1] <= x) k++;
  return k;
}


/* screen row sy of the text: its line, the bytes from..to, the column it starts at; 0: past the end */
static int vis_goto (int sy, size_t *line, size_t *from, size_t *to, size_t *left) {
  size_t y = T->top, k, st[MAXSEG], n;
  if (!E.wrap) {
    if (T->nfold) {	/* folded regions: line by line */
      size_t yy = T->top;
      while (sy-- > 0 && yy < T->doc->n) yy = line_next(yy);
      if (yy >= T->doc->n) return 0;
      *line = yy;
      *from = 0;
      *to = row_at(yy)->len;
      *left = T->left;
      return 1;
    }
    if (T->top + (size_t)sy >= T->doc->n) return 0;
    *line = T->top + (size_t)sy;
    *from = 0;
    *to = row_at(*line)->len;
    *left = T->left;
    return 1;
  }
  n = wrap_segs(y, st);
  k = T->sub < n ? T->sub : n - 1;
  while (sy > 0) {
    if (++k >= n) {
      if ((y = line_next(y)) >= T->doc->n) return 0;
      n = wrap_segs(y, st);
      k = 0;
    }
    sy--;
  }
  *line = y;
  *from = st[k];
  *to = k + 1 < n ? st[k + 1] : row_at(y)->len;
  *left = col_of(row_at(y), st[k]);
  return 1;
}


/*
** {==================================================================
** What the language server adds to the text: inlay hints (drawn in
** it, not with word wrap: vcol is a character's column with the hints
** before it), semantic tokens (the names' colors) and code lenses
** ===================================================================
*/

static struct {
  const Doc *d;
  InlayHint *v;	/* by position */
  int *w;	/* their widths */
  size_t n;
  size_t y0, y1;	/* the lines they are for */
  unsigned long edits;	/* the text they are for */
  const Doc *ask_d;	/* what was asked last, and when the text last changed */
  unsigned long ask_edits, seen;
  size_t ask_y0, ask_y1;
  long long since;
} IH;


static int cmp_hint (const void *a, const void *b) {
  const InlayHint *x = (const InlayHint *)a, *y = (const InlayHint *)b;
  return pos_cmp(x->at, y->at);
}


void on_inlay (Doc *d, size_t y0, size_t y1, InlayHint *v, size_t n) {
  size_t i;
  if (d != IH.ask_d) {	/* another file's, late */
    for (i = 0; i < n; i++) free(v[i].label);
    free(v);
    return;
  }
  for (i = 0; i < IH.n; i++) free(IH.v[i].label);
  free(IH.v);
  free(IH.w);
  qsort(v, n, sizeof(InlayHint), cmp_hint);
  IH.v = v;
  IH.n = n;
  IH.w = (int *)xmalloc((n + 1) * sizeof(int));
  for (i = 0; i < n; i++) IH.w[i] = (int)str_cols(v[i].label);
  IH.d = d;
  IH.y0 = y0;
  IH.y1 = y1;
  IH.edits = IH.ask_edits;
}


static int ih_on (void) {
  return opt.inlay && !E.wrap && IH.n > 0 && IH.d == T->doc;
}


/* the first hint of line y (or after it) */
static size_t ih_first (size_t y) {
  size_t lo = 0, hi = IH.n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (IH.v[mid].at.y < y) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}


/* the width of line y's hints before its byte x (those at x come before its character) */
static size_t ih_shift (size_t y, size_t x) {
  size_t i, w = 0;
  if (!ih_on()) return 0;
  for (i = ih_first(y); i < IH.n && IH.v[i].at.y == y && IH.v[i].at.x <= x; i++) w += (size_t)IH.w[i];
  return w;
}


/* the screen column of byte x of line y: its column with the hints before it */
static size_t vcol (const Row *r, size_t y, size_t x) {
  return col_of(r, x) + ih_shift(y, x);
}


/* where a range that ends at byte x ends: the hints at x come after it */
static size_t vcol_end (const Row *r, size_t y, size_t x) {
  return col_of(r, x) + (x > 0 ? ih_shift(y, x - 1) : 0);
}


/* the byte at screen column c of line y (on a hint: the character after it) */
static size_t x_of_vcol (size_t y, size_t c) {
  const Row *r = row_at(y);
  size_t x = 0, lc = 0, vs = 0, len, i;
  if (!ih_on()) return x_of_col(r, c);
  i = ih_first(y);
  while (x < r->len) {
    size_t w;
    for (; i < IH.n && IH.v[i].at.y == y && IH.v[i].at.x <= x; i++) vs += (size_t)IH.w[i];
    if (c < lc + vs) return x;
    w = char_width(r, x, lc, &len);
    if (c < lc + vs + w) return x;
    lc += w;
    x += len;
  }
  return r->len;
}


/* line y's hints from index *i on that are at byte x or before: drawn at column *vc, which moves on */
static void draw_hints (int sy, size_t y, size_t x, size_t *i, size_t col, size_t *vs, int gw, size_t left) {
  size_t right = left + (size_t)text_cols();
  for (; *i < IH.n && IH.v[*i].at.y == y && IH.v[*i].at.x <= x; (*i)++) {
    const char *s = IH.v[*i].label;
    size_t n = strlen(s), k = 0, len, c = col + *vs;
    while (k < n) {
      uint32_t cp = utf8_decode(s + k, n - k, &len);
      int w = uc_width(cp);
      if (w < 1) w = 1;
      if (c >= left && c + (size_t)w <= right)
        scr_put_rgb(L.ed_x + gw + (int)(c - left), sy, cp, ui_color(C_INLAY_FG), ui_color(C_INLAY_BG), RGB_ITALIC);
      c += (size_t)w;
      k += len;
    }
    *vs += (size_t)IH.w[*i];
  }
}


/* semantic tokens: the whole file's, for one version of its text */
static struct {
  const Doc *d;
  SemTok *v;
  size_t n;
  unsigned long edits;
  const Doc *ask_d;
  unsigned long ask_edits, seen;
  long long since;
} SM;


void on_semantic (Doc *d, unsigned long edits, SemTok *v, size_t n) {
  if (d != SM.ask_d) {
    free(v);
    return;
  }
  free(SM.v);
  SM.v = v;
  SM.n = n;
  SM.d = d;
  SM.edits = edits;
}


/* line y's names in the server's colors; keywords, strings, comments stay the highlighter's */
static void sem_apply (size_t y, unsigned char *tok, size_t len) {
  size_t lo = 0, hi, i;
  if (!opt.semantic || SM.d != T->doc || SM.edits != T->doc->edits || SM.n == 0) return;
  hi = SM.n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (SM.v[mid].y < y) lo = mid + 1;
    else hi = mid;
  }
  for (i = lo; i < SM.n && SM.v[i].y == y; i++) {
    size_t x;
    for (x = SM.v[i].x0; x < SM.v[i].x1 && x < len; x++) {
      int t = tok[x];
      if (t == T_TEXT || t == T_VAR || t == T_FUNC || t == T_TYPE || t == T_CONST) tok[x] = (unsigned char)SM.v[i].tok;
    }
  }
}


/* code lenses: shown after the end of their line, dim; a click runs one */
#define MAX_LENS_HIT	64

static struct {
  const Doc *d;
  Lens *v;
  size_t n;
  const Doc *ask_d;
  unsigned long ask_edits, seen;
  long long since;
  int nhit;
  int hx0[MAX_LENS_HIT], hx1[MAX_LENS_HIT], hy[MAX_LENS_HIT];
  size_t hidx[MAX_LENS_HIT];
} LN;


void on_lens (Doc *d, Lens *v, size_t n) {
  size_t i;
  for (i = 0; i < LN.n; i++) free(LN.v[i].title);
  free(LN.v);
  LN.v = v;
  LN.n = n;
  LN.d = d;
}


/*
** The server is asked again when the text rested a while after a change
** (or another file is shown): what is asked, when; 1 when it is time
*/
static int time_to_ask (const Doc **ask_d, unsigned long *ask_edits, unsigned long *seen, long long *since,
                        long long wait) {
  long long now = os_now_us();
  if (*ask_d == T->doc && *ask_edits == T->doc->edits) return 0;	/* asked already */
  if (*seen != T->doc->edits) {
    *seen = T->doc->edits;
    *since = now;
    return 0;
  }
  if (now - *since < wait && *ask_d == T->doc) return 0;
  *ask_d = T->doc;
  *ask_edits = T->doc->edits;
  return 1;
}


/* when nothing happens: hints for the lines shown, the tokens, the lenses */
static void extras_idle (void) {
  size_t y0, y1;
  if (!HAS_DOC || G->diff || T->page || !lsp_active(T->doc)) return;
  if (opt.inlay && !E.wrap) {
    y0 = T->top > 40 ? T->top - 40 : 0;
    y1 = T->top + (size_t)L.text_h + 40;
    if (y1 > T->doc->n) y1 = T->doc->n;
    {	/* scrolled away from the lines asked about: asked again */
      size_t ve = T->top + (size_t)L.text_h < T->doc->n ? T->top + (size_t)L.text_h : T->doc->n;
      if (IH.ask_d == T->doc && IH.ask_edits == T->doc->edits && (T->top < IH.ask_y0 || ve > IH.ask_y1))
        IH.ask_edits = T->doc->edits - 1;
    }
    if (time_to_ask(&IH.ask_d, &IH.ask_edits, &IH.seen, &IH.since, 200000)) {
      IH.ask_y0 = y0;
      IH.ask_y1 = y1;
      lsp_inlay(T->doc, y0, y1);
    }
  }
  if (opt.semantic && time_to_ask(&SM.ask_d, &SM.ask_edits, &SM.seen, &SM.since, 300000)) lsp_semantic(T->doc);
  if (opt.codelens && time_to_ask(&LN.ask_d, &LN.ask_edits, &LN.seen, &LN.since, 600000)) lsp_lens(T->doc);
  if (time_to_ask(&FRG.ask_d, &FRG.ask_edits, &FRG.seen, &FRG.since, 500000)) lsp_folding(T->doc);
}


/* the lenses of line y as one text: "run test | 3 references"; its width, 0: none */
static int lens_text (size_t y, char *out, size_t cap) {
  size_t i, o = 0;
  out[0] = '\0';
  if (!opt.codelens || LN.d != T->doc) return 0;
  for (i = 0; i < LN.n; i++) {
    if (LN.v[i].y != y || LN.v[i].title[0] == '\0') continue;
    o += (size_t)snprintf(out + o, o < cap ? cap - o : 0, "%s%s", o ? " | " : "", LN.v[i].title);
    if (o >= cap) break;
  }
  return (int)str_cols(out);
}


/* editor.codeLens: after the end of their lines, dim; each remembered for a click */
static void draw_lenses (int gw) {
  size_t i;
  int end = L.ed_x + gw + text_cols();
  LN.nhit = 0;
  if (!opt.codelens || LN.d != T->doc) return;
  for (i = 0; i < LN.n; i++) {
    Pos e;
    int cx, cy, k, w;
    size_t j;
    if (LN.v[i].title[0] == '\0') continue;
    e.y = LN.v[i].y;
    if (e.y >= T->doc->n) continue;
    e.x = row_at(e.y)->len;
    if (!screen_at(e, &cx, &cy)) continue;
    cx += 3;
    for (j = 0; j < i; j++)	/* after the ones before it on the line */
      if (LN.v[j].y == e.y && LN.v[j].title[0]) cx += (int)str_cols(LN.v[j].title) + 3;
    if (j > 0 && cx > L.ed_x + gw + 3) {
      size_t m;
      for (m = 0; m < i; m++)
        if (LN.v[m].y == e.y && LN.v[m].title[0]) break;
      if (m < i) scr_puts(cx - 2, cy, "|", TOK(T_WS, B_EDITOR)), scr_set_fg(cx - 2, cy, ui_color(C_LENS));
    }
    if (cx >= end - 3) continue;
    w = scr_putsw(cx, cy, end - cx, LN.v[i].title, TOK(T_WS, e.y == T->cur.y && !T->sel ? B_LINE : B_EDITOR));
    for (k = 0; k < w; k++) scr_set_fg(cx + k, cy, ui_color(C_LENS));
    if (LN.nhit < MAX_LENS_HIT) {
      LN.hx0[LN.nhit] = cx;
      LN.hx1[LN.nhit] = cx + w;
      LN.hy[LN.nhit] = cy;
      LN.hidx[LN.nhit] = i;
      LN.nhit++;
    }
  }
}

/* }================================================================== */


/* the screen row of p (from the top of the text), -1 when not shown */
static int vis_row (Pos p) {
  size_t y, rows = 0, st[MAXSEG];
  if (T->nfold && hidden_in(p.y) != (size_t)-1) return -1;
  if (!E.wrap && T->nfold == 0)
    return (p.y >= T->top && p.y < T->top + (size_t)L.text_h) ? (int)(p.y - T->top) : -1;
  if (p.y < T->top) return -1;
  if (!E.wrap) {
    for (y = T->top; y < p.y && rows < (size_t)L.text_h; y = line_next(y)) rows++;
    return (y == p.y && rows < (size_t)L.text_h) ? (int)rows : -1;
  }
  for (y = T->top; y < p.y && rows < (size_t)L.text_h; y = line_next(y)) rows += wrap_segs(y, st);
  rows += seg_of(p.y, p.x);
  if (rows < T->sub) return -1;
  rows -= T->sub;
  return rows < (size_t)L.text_h ? (int)rows : -1;
}


/* where p is on the screen; 0 when it is not shown */
static int screen_at (Pos p, int *sx, int *sy) {
  int row;
  size_t col, left;
  if (p.y >= T->doc->n || p.x > row_at(p.y)->len) return 0;	/* a place the text no longer has */
  row = vis_row(p);
  if (row < 0) return 0;
  col = vcol(row_at(p.y), p.y, p.x);
  if (E.wrap) {
    size_t st[MAXSEG];
    wrap_segs(p.y, st);
    left = col_of(row_at(p.y), st[seg_of(p.y, p.x)]);
  }
  else left = T->left;
  if (col < left || col >= left + (size_t)text_cols()) return 0;
  *sx = L.ed_x + gutter_width() + (int)(col - left);
  *sy = L.text_y + row;
  return 1;
}


/* the view one row further down (d > 0) or up */
static void vis_scroll (int d) {
  size_t st[MAXSEG];
  if (!E.wrap) {
    if (d < 0) T->top = line_prev(T->top);
    else if (line_next(T->top) < T->doc->n) T->top = line_next(T->top);
    return;
  }
  if (d > 0) {
    if (T->sub + 1 < wrap_segs(T->top, st)) T->sub++;
    else if (line_next(T->top) < T->doc->n) {
      T->top = line_next(T->top);
      T->sub = 0;
    }
  }
  else if (T->sub > 0) T->sub--;
  else if (T->top > 0) {
    T->top = line_prev(T->top);
    T->sub = wrap_segs(T->top, st) - 1;
  }
}


static void scroll_to_cursor (void) {
  if (E.wrap) {	/* rows, not lines; no scrolling to the side */
    size_t seg = seg_of(T->cur.y, T->cur.x), guard = 0;
    T->left = 0;
    if (T->cur.y < T->top || (T->cur.y == T->top && seg < T->sub)) {
      T->top = T->cur.y;
      T->sub = seg;
    }
    while (vis_row(T->cur) < 0 && guard++ < 100000) vis_scroll(1);
    return;
  }
  T->sub = 0;

  size_t th = (size_t)L.text_h, tw = (size_t)text_cols();
  {
    size_t col = vcol(row_at(T->cur.y), T->cur.y, T->cur.x);
    if (T->cur.y < T->top) T->top = T->cur.y;
    if (T->nfold == 0) {
      size_t sl = (size_t)eopt.surround;	/* editor.cursorSurroundingLines */
      if (th > 2 && sl > (th - 1) / 2) sl = (th - 1) / 2;
      if (sl && T->cur.y < T->top + sl) T->top = T->cur.y > sl ? T->cur.y - sl : 0;
      if (T->cur.y + sl >= T->top + th) T->top = T->cur.y + sl - th + 1;
      if (T->top >= T->doc->n) T->top = T->doc->n ? T->doc->n - 1 : 0;
    }
    else {	/* folded regions: row by row */
      size_t guard = 0;
      while (vis_row(T->cur) < 0 && guard++ < 1000000 && T->top < T->cur.y) T->top = line_next(T->top);
    }
    if (col < T->left) T->left = col;
    if (col >= T->left + tw) T->left = col - tw + 1;
  }
}


static void sel_range (Pos *a, Pos *b) {
  if (pos_cmp(T->anchor, T->cur) <= 0) {
    *a = T->anchor;
    *b = T->cur;
  }
  else {
    *a = T->cur;
    *b = T->anchor;
  }
}


/*
** {==================================================================
** Indentation guides and rulers
** ===================================================================
*/

/* a line's indent in columns; -1 when it is blank */
static long guide_ind (size_t y) {
  const Row *r = row_at(y);
  size_t ind = indent_end(r);
  if (ind == r->len) return -1;
  return (long)col_of(r, ind);
}


static long guide_unit (void) {
  long u = T->doc->tabs ? (long)TABW : (long)T->doc->indent;
  return u > 0 ? u : 4;
}


/*
** How many guides line y has: its indent in units; a blank line's from
** the lines around it, as VS Code does (Python's end with the block).
*/
static long guide_level (size_t y) {
  long ind = guide_ind(y), up = -1, down = -1, u = guide_unit();
  size_t k;
  if (ind >= 0) return (ind + u - 1) / u;
  for (k = y; k > 0 && y - k < 1000;)
    if ((up = guide_ind(--k)) >= 0) break;
  for (k = y + 1; k < T->doc->n && k - y < 1000; k++)
    if ((down = guide_ind(k)) >= 0) break;
  if (up < 0 || down < 0) return 0;
  if (up < down) return 1 + up / u;
  if (up == down) return (down + u - 1) / u;
  return strcmp(syntax_name(T->sx), "Python") == 0 ? (down + u - 1) / u : 1 + down / u;
}


/* the guide of the block the cursor is in: level, lines y0 .. y1 (0: none) */
static struct {
  const Doc *d;
  unsigned long edits;
  size_t cur, y0, y1;
  long unit, level;
} AG;


static void active_guide (void) {
  size_t n = T->doc->n, c = T->cur.y, s = c;
  long lv, a;
  if (AG.d == T->doc && AG.edits == T->doc->edits && AG.cur == c && AG.unit == guide_unit()) return;
  AG.d = T->doc;
  AG.edits = T->doc->edits;
  AG.cur = c;
  AG.unit = guide_unit();
  AG.level = 0;
  lv = guide_level(c);
  a = lv;
  if (c + 1 < n && guide_ind(c) >= 0 && guide_level(c + 1) > lv) {	/* a block's first line: the block under it */
    a = lv + 1;
    s = c + 1;
  }
  if (a <= 0) return;
  AG.level = a;
  AG.y0 = AG.y1 = s;
  while (AG.y0 > 0 && c - AG.y0 < 5000 && guide_level(AG.y0 - 1) >= a) AG.y0--;
  while (AG.y1 + 1 < n && AG.y1 - c < 5000 && guide_level(AG.y1 + 1) >= a) AG.y1++;
}


/* editor.guides.indentation: a thin line at each indent level, in the blanks before the text */
static void draw_guides (int sy, size_t y, int gw, size_t left) {
  long lv, k, u;
  size_t right = left + (size_t)text_cols();
  if (!opt.guides) return;
  lv = guide_level(y);
  if (lv <= 0) return;
  u = guide_unit();
  if (opt.guides_active) active_guide();
  for (k = 0; k < lv; k++) {
    size_t c = (size_t)(k * u);
    int sx, on;
    uint32_t ch;
    if (c < left || c >= right) continue;
    sx = L.ed_x + gw + (int)(c - left);
    ch = scr_ch(sx, sy);
    if (ch != ' ' && ch != 0xB7) continue;	/* text, or a tab's arrow */
    on = opt.guides_active && k + 1 == AG.level && y >= AG.y0 && y <= AG.y1;
    scr_glyph(sx, sy, 0x2502, ui_color(on ? C_GUIDE_ON : C_GUIDE));
  }
}


/* editor.rulers: a thin line at those columns where the row is blank */
static void draw_rulers (int sy, int gw, size_t left) {
  size_t right = left + (size_t)text_cols();
  int i;
  for (i = 0; i < opt.nrulers; i++) {
    size_t c = (size_t)opt.rulers[i];
    int sx;
    if (c < left || c >= right) continue;
    sx = L.ed_x + gw + (int)(c - left);
    if (scr_ch(sx, sy) == ' ') scr_glyph(sx, sy, 0x2502, ui_color(C_RULER));
  }
}

/* }================================================================== */


/* the lightbulb: the code actions of the cursor's line, asked when it rests there */
static struct {
  const Doc *d;
  unsigned long edits;
  size_t y;
  size_t n;	/* how many there are */
  int fix;	/* a quick fix among them: the lightbulb with the spark */
  int asked;
  long long since;	/* when the line or the text last changed */
} LB;


void on_bulb (Doc *d, size_t y, size_t n, int fix) {
  if (d != LB.d || y != LB.y) return;
  LB.n = n;
  LB.fix = fix;
}


static void bulb_idle (void) {
  if (!HAS_DOC || G->diff || T->page || !opt.lightbulb || !lsp_active(T->doc)) return;
  if (LB.d != T->doc || LB.y != T->cur.y || LB.edits != T->doc->edits) {
    LB.d = T->doc;
    LB.y = T->cur.y;
    LB.edits = T->doc->edits;
    LB.n = 0;
    LB.fix = 0;
    LB.asked = 0;
    LB.since = os_now_us();
    return;
  }
  if (!LB.asked && os_now_us() - LB.since > 250000) {	/* it rested: ask */
    LB.asked = 1;
    lsp_bulb(T->doc, LB.y);
  }
}


static void draw_row (int sy, size_t y, int gw, Pos sa, Pos sb, size_t from, size_t to, size_t left) {
  static unsigned char *tok;
  static size_t tokcap;
  const Row *r = row_at(y);
  int is_cur = (y == T->cur.y) && !T->sel && eopt.line_hl >= 2;	/* editor.renderLineHighlight: "line", "all" */
  int base = is_cur ? B_LINE : B_EDITOR, x0 = L.ed_x;
  size_t x = 0, col = 0, len, right = left + (size_t)text_cols(), n = strlen(E.find), m;
  size_t hit_end = 0;	/* a find match lasts to here */
  size_t vs = 0, hi = 0;	/* the inlay hints' columns so far; the next hint */
  int hints = ih_on() && from == 0, tmc;
  const uint32_t *tfg = NULL;	/* a TextMate grammar's colors: the theme's own for each scope */
  const unsigned char *tfs = NULL, *tcls = NULL;
  char num[32];
  if (r->len + 1 > tokcap) {
    tokcap = r->len + 256;
    tok = (unsigned char *)xrealloc(tok, tokcap);
  }
  syntax_line(T->doc, T->sx, y, tok);
  tmc = tm_colors(T->doc, y, &tfg, &tfs, &tcls);
  sem_apply(y, tok, r->len);
  if (hints) hi = ih_first(y);
  if (opt.line_numbers && from == 0) {
    unsigned long v = (unsigned long)(y + 1);
    if (eopt.line_nums == 2 && y != T->cur.y) v = (unsigned long)(y > T->cur.y ? y - T->cur.y : T->cur.y - y);	/* relative */
    if (eopt.line_nums == 3 && y != T->cur.y && (y + 1) % 10 != 0) snprintf(num, sizeof(num), "%*s  ", gw - 3, "");	/* interval */
    else snprintf(num, sizeof(num), "%*lu  ", gw - 3, v);
  }
  else snprintf(num, sizeof(num), " ");	/* a wrapped line's next rows: no number */
  scr_fill(x0, sy, 1, S_GUTTER);
  scr_puts(x0 + 1, sy, num, y == T->cur.y ? S_GUTTER_CUR : S_GUTTER);
  if (y == T->cur.y && !T->sel && (eopt.line_hl == 1 || eopt.line_hl == 3)) {	/* "gutter", "all" */
    int i;
    for (i = 0; i < gw; i++) scr_set_bg(x0 + i, sy, ui_color(C_LINE_BG));
  }
  scr_fill(x0 + gw, sy, text_cols(), is_cur ? S_LINE : S_TEXT);
  if (y == T->cur.y && from == 0 && opt.lightbulb && lsp_active(T->doc)) {	/* code actions here: VS Code's lightbulb */
    size_t nd, i;
    const Diag *dv = lsp_diags(T->doc, &nd);
    uint32_t bulb = 0;
    for (i = 0; i < nd && !bulb; i++)
      if (dv[i].a.y <= y && dv[i].b.y >= y) bulb = 0xEA61;	/* codicon lightbulb */
    if (LB.d == T->doc && LB.y == y && LB.edits == T->doc->edits && LB.n > 0)
      bulb = LB.fix ? 0xEB13 : 0xEA61;	/* lightbulb-autofix: a quick fix is there */
    if (bulb) scr_put_rgb(x0, sy, bulb, ui_color(C_LIGHTBULB), ui_color(is_cur ? C_LINE_BG : C_EDITOR_BG), 0);
  }
  if (from == 0) draw_quick(sy, y, gw, is_cur);	/* scm.diffDecorations: the change's bar */
  while (x < r->len && x < to && col + vs < right) {
    size_t w = char_width(r, x, col, &len), i;
    uint32_t cp = utf8_decode(r->s + x, r->len - x, &len);
    Pos p;
    int bg = base, st, uh;
    p.y = y;
    p.x = x;
    if (hints) draw_hints(sy, y, x, &hi, col, &vs, gw, left);	/* editor.inlayHints: before the character */
    if (E.find_open && n > 0 && x >= hit_end && (m = match_at(y, x)) > 0) hit_end = x + m;
    if (x < from) {	/* an earlier row of a wrapped line */
      col += w;
      x += len;
      continue;
    }
    if ((T->sel && pos_cmp(p, sa) >= 0 && pos_cmp(p, sb) < 0) || (T->nmc && in_other_sel(p))) bg = B_SEL;
    else if (x < hit_end) bg = B_MATCH;
    st = TOK(tok[x], bg);
    uh = cp >= 0x80 && tok[x] != T_COMMENT && uni_flag(cp, NULL);	/* editor.unicodeHighlight */
    if ((cp == ' ' || cp == '\t') && (opt.render_ws == 2 || (opt.render_ws == 1 && bg == B_SEL)))
      st = TOK(T_WS, bg), cp = (cp == ' ') ? 0xB7 : 0x2192;	/* editor.renderWhitespace */
    for (i = 0; i < w; i++) {	/* column by column: a tab, ^X, a cut wide one */
      size_t c = col + vs + i;
      int sx = x0 + gw + (int)(c - left);
      if (c < left || c >= right) continue;
      if (cp == 0x2192) scr_put(sx, sy, i == 0 ? 0x2192 : ' ', st);
      else if (cp == '\t') scr_put(sx, sy, ' ', st);
      else if ((cp < 32 || cp == 127) && eopt.control_chars)	/* editor.renderControlCharacters: its picture */
        scr_put(sx, sy, cp == 127 ? 0x2421 : 0x2400 + cp, bg == base ? S_CTRL : st);
      else if (cp < 32 || cp == 127)
        scr_put(sx, sy, i == 0 ? '^' : (cp == 127 ? '?' : cp + '@'), bg == base ? S_CTRL : st);
      else if (i == 0 && col + vs >= left && col + vs + w <= right) {
        if (tmc && tfg[x] && tok[x] == tcls[x] && st == TOK(tok[x], bg)) {	/* the scope's color, VS Code's */
          uint32_t f0, b0;
          theme_tok(st, &f0, &b0);
          scr_put_rgb(sx, sy, cp, tfg[x] & 0xFFFFFF, b0, tfs[x]);
        }
        else scr_put(sx, sy, cp, st);
      }
      else if (i == 0 || col + vs < left) scr_put(sx, sy, ' ', st);
      if (uh) scr_set_bg(sx, sy, 0x5C4B00);	/* editorUnicodeHighlight: VS Code's box, as a tint */
    }
    col += w;
    x += len;
  }
  if (hints && x >= r->len) draw_hints(sy, y, r->len, &hi, col, &vs, gw, left);	/* the ones at the end */
  col += vs;
  if (from == 0) draw_guides(sy, y, gw, left);
  draw_rulers(sy, gw, left);
  /* the newline is selected too: one cell of it, like VS Code */
  if (T->sel && to == r->len && y >= sa.y && y < sb.y && col >= left && col < right)
    scr_put(x0 + gw + (int)(col - left), sy, ' ', S_SEL);
  {	/* Run and Debug: a breakpoint's dot, the paused line's arrow and color */
    int dm = dbg_mark(T->real, y);
    if (dm) {
      uint32_t bg = ui_color(is_cur ? C_LINE_BG : C_EDITOR_BG);
      if (from == 0) {
        uint32_t dot = (dm & DM_LOG) ? 0xEAAB : (dm & DM_COND) ? 0xEAA7 : 0xEA71;	/* logpoint, conditional */
        if (dm & (DM_TOP | DM_FRAME)) scr_put_rgb(x0, sy, 0xEB8B, (dm & DM_TOP) ? 0xFFCC00 : 0x89D185, bg, 0);
        else if (dm & DM_DISABLED) scr_put_rgb(x0, sy, dot, 0x848484, bg, 0);
        else if (dm & DM_UNVERIFIED) scr_put_rgb(x0, sy, dot == 0xEA71 ? 0xEABC : dot - 1, 0x848484, bg, 0);
        else scr_put_rgb(x0, sy, dot, 0xE51400, bg, 0);	/* debug-breakpoint */
      }
      if (dm & (DM_TOP | DM_FRAME)) {	/* editor.stackFrameHighlightBackground, over the text */
        uint32_t hi = (dm & DM_TOP) ? 0xFFFF00 : 0x7ABD7A, mix = 0;
        int sh, i;
        for (sh = 0; sh <= 16; sh += 8) {
          uint32_t a = (bg >> sh) & 255, b = (hi >> sh) & 255;
          mix |= ((a * 4 + b) / 5) << sh;
        }
        for (i = 0; i < text_cols(); i++) scr_set_bg(x0 + gw + i, sy, mix);
      }
    }
  }
  if (from == 0 && T->real && !dbg_mark(T->real, y)) {	/* Testing: a test's line, ▷ ✓ ✗ (testing.gutterEnabled) */
    int tm = test_mark(T->real, y);
    if (tm) {
      uint32_t bg = ui_color(is_cur ? C_LINE_BG : C_EDITOR_BG);
      if (tm == TM_PASS) scr_put_rgb(x0, sy, 0xEBA4, 0x73C991, bg, 0);
      else if (tm == TM_FAIL) scr_put_rgb(x0, sy, 0xEA87, 0xF14C4C, bg, 0);
      else if (tm == TM_RUNNING || tm == TM_QUEUED) scr_put_rgb(x0, sy, 0xEB19, 0xCCA700, bg, 0);
      else if (tm == TM_SKIP) scr_put_rgb(x0, sy, 0xEABD, 0x848484, bg, 0);
      else scr_put_rgb(x0, sy, 0xEB2C, 0x73C991, bg, 0);	/* run */
    }
  }
  if (from == 0 && gw >= 4) {	/* the fold's chevron: folded, or where the cursor is */
    if (T->nfold && is_folded(y)) scr_put(x0 + gw - 2, sy, 0xEAB6, S_GUTTER_CUR);
    else if (y == T->cur.y && fold_end(y) > y) scr_put(x0 + gw - 2, sy, 0xEAB4, S_GUTTER);
  }
  if (to == r->len && T->nfold && is_folded(y) && col + 1 >= left && col + 4 <= right) {
    int fx = x0 + gw + (int)(col + 1 - left);	/* the folded lines: ... */
    scr_put(fx, sy, ' ', S_SEL);
    scr_put(fx + 1, sy, 0x22EF, S_SEL);
    scr_put(fx + 2, sy, ' ', S_SEL);
  }
  {	/* the problems the language server found: squiggles */
    size_t nd, i;
    const Diag *dv = lsp_diags(T->doc, &nd);
    for (i = 0; i < nd; i++) {
      const Diag *dg = &dv[i];
      size_t fx, tx, c0, c1;
      if (y < dg->a.y || y > dg->b.y) continue;
      fx = dg->a.y == y ? dg->a.x : 0;
      tx = dg->b.y == y ? dg->b.x : r->len;
      if (fx > r->len) fx = r->len;
      if (tx > r->len) tx = r->len;
      if (tx < from || fx > to || (fx == to && to < r->len)) continue;	/* not in this row */
      c0 = vcol(r, y, fx < from ? from : fx);
      c1 = vcol_end(r, y, tx > to ? to : tx);
      if (c1 <= c0) c1 = c0 + 1;	/* a point: one cell */
      if (c0 < left) c0 = left;
      if (c1 > right) c1 = right;
      if (c1 > c0)
        scr_squiggle(x0 + gw + (int)(c0 - left), sy, (int)(c1 - c0),
                     ui_color(dg->sev == 1 ? C_ERROR : dg->sev == 2 ? C_WARNING : C_INFO));
    }
  }
  hl_row(sy, y, gw, from, to, left);
  for (x = 0; x < (size_t)T->nmc; x++) {	/* the other cursors: a block each */
    const Cur *c = &T->mc[x];
    size_t cc;
    if (c->cur.y != y || c->cur.x < from || (c->cur.x >= to && to < r->len)) continue;
    cc = vcol(r, y, c->cur.x);
    if (cc >= left && cc < right) scr_restyle(x0 + gw + (int)(cc - left), sy, 1, S_MCURSOR);
  }
}


static const char *tab_name (const Tab *t) {
  static char pv[2][300];	/* two at once: the tab bar draws while a list is made */
  static int k;
  if (t->page) return t->page == PAGE_SETTINGS ? "Settings" : "Welcome";
  if (t->md) {
    k = !k;
    snprintf(pv[k], sizeof(pv[k]), "Preview %s", t->doc->path ? path_basename(t->doc->path) : "Untitled-1");
    return pv[k];
  }
  return t->doc->path ? path_basename(t->doc->path) : "Untitled-1";
}


static const char *doc_name (void) {
  return tab_name(T);
}


/* the tabs on the screen, for clicks: tab i from x0[i] to x1[i]; the diff last */
static struct {
  int *x0, *x1, *close;
  int cap, n, first;	/* first: the leftmost one shown */
} g_tabs;


static int tab_width (const char *name) {
  return (int)str_cols(name) + 7;
}


/* a tab's name as its tab shows it: "x.c (deleted)" when the file went from the disk */
static const char *tab_label (const Tab *t, char *buf, size_t n) {
  if (t->page || t->doc->disk_state != DISK_GONE) return tab_name(t);
  snprintf(buf, n, "%s (deleted)", tab_name(t));
  return buf;
}


/* the color of a file changed against git's HEAD, as the Explorer shows it */
static uint32_t git_fg (int mark) {
  switch (mark) {
    case 'M': return ui_color(C_GIT_M);
    case 'A': return ui_color(C_GIT_A);
    case 'D': return ui_color(C_GIT_D);
    case 'U': return ui_color(C_GIT_U);
  }
  return 0;
}


static int draw_tab (int x, int y, const char *name, int on, int dirty, int preview, int pinned, int *close_x,
                     int page, int gmark) {
  int ist, w = tab_width(name), x1, n;
  uint32_t icon = page == PAGE_SETTINGS ? 0xEAF8 : page == PAGE_WELCOME ? 0xF121 : file_icon(name, &ist);	/* a page: gear, </> */
  int st = on ? S_TAB_ON : S_TAB;
  if (x + w > L.ed_x + L.ed_w) w = L.ed_x + L.ed_w - x;
  if (w <= 0) return x;
  x1 = x + w;
  scr_fill(x, y, w, st);
  (void)ist;
  scr_put(x + 1, y, icon, st);
  n = scr_putsw(x + 3, y, w - 6, name, preview && !on ? S_TAB_PREVIEW : st);
  if (gmark && vopt.tab_colors) {	/* the name in git's color, like the Explorer's */
    int i;
    for (i = 0; i < n; i++) scr_set_fg(x + 3 + i, y, git_fg(gmark));
  }
  *close_x = x1 - 2;
  if (dirty) scr_put(x1 - 2, y, 0x25CF, st);	/* the dot */
  else if (pinned) scr_put(x1 - 2, y, 0xEBA0, st);	/* codicon pinned */
  else if (on) scr_put(x1 - 2, y, 0xEA76, st);	/* codicon close */
  else if (gmark && vopt.tab_badges) {	/* VS Code's badge: M, U, A, D */
    scr_put(x1 - 2, y, (uint32_t)gmark, st);
    scr_set_fg(x1 - 2, y, git_fg(gmark));
  }
  scr_put(x1 - 1, y, ' ', st);
  return x1;
}


static void draw_tabs (void) {
  int y = L.ed_y, x, i, n = G->ntab + (HAS_DIFF ? 1 : 0), end = L.ed_x + L.ed_w;
  int on = G->diff ? G->ntab : G->active;
  scr_fill(L.ed_x, y, L.ed_w, S_TABS);
  if (n + 1 > g_tabs.cap) {
    g_tabs.cap = n + 16;
    g_tabs.x0 = (int *)xrealloc(g_tabs.x0, (size_t)g_tabs.cap * sizeof(int));
    g_tabs.x1 = (int *)xrealloc(g_tabs.x1, (size_t)g_tabs.cap * sizeof(int));
    g_tabs.close = (int *)xrealloc(g_tabs.close, (size_t)g_tabs.cap * sizeof(int));
  }
  g_tabs.n = n;
  if (on != G->last_on || G->first >= n) {	/* another in front: it is shown (the wheel may scroll away) */
    G->last_on = on;
    if (G->first > on) G->first = on;
    for (;;) {
      int w = 0;
      for (i = G->first; i <= on && i < n; i++) {
        char lb[300];
        w += tab_width(i < G->ntab ? tab_label(G->tab[i], lb, sizeof(lb)) : diff_title());
      }
      if (w <= L.ed_w || G->first >= on) break;
      G->first++;
    }
  }
  if (G->first >= n) G->first = n ? n - 1 : 0;
  x = L.ed_x;
  for (i = 0; i < n; i++) {
    int cx = -1;
    g_tabs.x0[i] = g_tabs.x1[i] = g_tabs.close[i] = -1;
    if (i < G->first || x >= end) continue;
    g_tabs.x0[i] = x;
    if (i < G->ntab) {
      const Tab *t = G->tab[i];
      char lb[300];
      x = draw_tab(x, y, tab_label(t, lb, sizeof(lb)), i == on, doc_dirty(t->doc) && !t->md, t->preview, t->pinned, &cx,
                   t->page, t->page || t->md || t->real == NULL ? 0 : git_mark(t->real, 0));
    }
    else x = draw_tab(x, y, diff_title(), i == on, 0, 0, 0, &cx, 0, 0);
    g_tabs.x1[i] = x;
    g_tabs.close[i] = cx;
  }
}


/* the breadcrumbs on the screen, for clicks: a folder or the file, or a symbol */
#define MAX_CRUMB	24

static struct {
  int n;
  int x0[MAX_CRUMB], x1[MAX_CRUMB];
  char *dir[MAX_CRUMB];	/* the folder whose entries are its siblings; NULL: a symbol */
  size_t sym[MAX_CRUMB];	/* a symbol's index in symbols() */
} CB;


static void crumb_add (int x0, int x1, const char *dir, size_t sym) {
  if (CB.n == MAX_CRUMB) return;
  CB.x0[CB.n] = x0;
  CB.x1[CB.n] = x1;
  CB.dir[CB.n] = dir ? xstrdup(dir) : NULL;
  CB.sym[CB.n] = sym;
  CB.n++;
}


/* the path of the file from the folder, as VS Code's breadcrumbs */
static void draw_crumbs (void) {
  const char *path = G->diff ? diff_path() : (HAS_DOC ? T->doc->path : NULL);
  const char *root = side_root(), *p;
  char *real;
  int x = L.ed_x + 1, y = L.ed_y + 1, end = L.ed_x + L.ed_w - 1, x0;
  const char *full;
  scr_fill(L.ed_x, y, L.ed_w, S_TEXT);
  while (CB.n > 0) free(CB.dir[--CB.n]);
  if (path == NULL) return;
  real = os_realpath(path);
  p = full = real ? real : path;
  if (m_fnncmp(p, root, strlen(root)) == 0 && path_is_sep(p[strlen(root)])) p += strlen(root) + 1;
  while (*p && x < end) {
    const char *e = p;
    char *part, *dir;
    while (*e && !path_is_sep(*e)) e++;
    part = xstrndup(p, (size_t)(e - p));
    x0 = x;
    if (*e == '\0') {	/* the file: with its icon */
      int ist;
      uint32_t icon = file_icon(part, &ist);
      x += scr_put(x, y, icon, S_CRUMB) + 1;
    }
    x += scr_putsw(x, y, end - x, part, S_CRUMB);
    dir = p > full ? xstrndup(full, (size_t)(p - full - 1)) : xstrdup(".");	/* where it is */
    crumb_add(x0, x, dir, 0);
    free(dir);
    free(part);
    if (*e) {
      x += 1;
      x += scr_put(x, y, 0xEAB6, S_CRUMB) + 1;	/* codicon chevron-right */
      e++;
    }
    p = e;
  }
  free(real);
  if (!G->diff && HAS_DOC) {	/* and the symbols the cursor is in: > struct Ed > ... */
    size_t n, path[16], np, i;
    const Sym *v = symbols(&n);
    np = sym_path(v, n, path, 16);
    for (i = 0; i < np && x < end - 4; i++) {
      int ist;
      uint32_t icon = sym_icon(v[path[i]].kind, &ist);
      x += 1;
      x += scr_put(x, y, 0xEAB6, S_CRUMB) + 1;
      x0 = x;
      x += scr_put(x, y, icon, S_CRUMB) + 1;
      x += scr_putsw(x, y, end - x, v[path[i]].name, S_CRUMB);
      crumb_add(x0, x, NULL, path[i]);
    }
  }
}


static int open_file (const char *path, int preview);
static void move_h (Pos p, int extend);
static void key_home (int extend);
static void center_cursor (void);

/* a folder's entries to pick from, folders first; a folder picked goes into it */
static void crumb_files (const char *dir) {
  char *cur = xstrdup(dir);
  for (;;) {
    Vec v, files, dirs;
    Pick p;
    size_t i;
    int r;
    char title[512];
    vec_init(&v);
    vec_init(&files);
    vec_init(&dirs);
    os_listdir(cur, &v);
    vec_sort(&v);
    for (i = 0; i < v.n; i++) {
      char *f = path_join(cur, v.v[i]);
      OsStat st;
      if (os_stat(f, &st) == 0 && st.is_dir) vec_push(&dirs, xstrdup(v.v[i]));
      else vec_push(&files, xstrdup(v.v[i]));
      free(f);
    }
    snprintf(title, sizeof(title), "%s", path_basename(cur));
    pick_init(&p, title);
    for (i = 0; i < dirs.n; i++) pick_add(&p, dirs.v[i], NULL, 0xEA83);	/* codicon folder */
    for (i = 0; i < files.n; i++) {
      int ist;
      pick_add(&p, files.v[i], NULL, (int)file_icon(files.v[i], &ist));
    }
    p.keep_order = 0;
    r = pick_run(&p);
    pick_free(&p);
    if (r >= 0) {
      int isdir = (size_t)r < dirs.n;
      char *f = path_join(cur, isdir ? dirs.v[r] : files.v[(size_t)r - dirs.n]);
      if (isdir) {	/* into the folder */
        free(cur);
        cur = f;
      }
      else {
        if (open_file(f, 1) == 0) E.focus = F_EDITOR;
        free(f);
        r = -1;
      }
      vec_free(&v);
      vec_free(&files);
      vec_free(&dirs);
      if (!isdir) break;
      continue;
    }
    vec_free(&v);
    vec_free(&files);
    vec_free(&dirs);
    break;
  }
  free(cur);
}


/* the symbols next to symbol k (the same parent): pick one, the cursor goes there */
static size_t *g_csl;

static void crumb_sym_preview (int i) {
  Pos q;
  q.y = g_csl[i];
  q.x = 0;
  move_h(doc_clamp(T->doc, q), 0);
  key_home(0);
  center_cursor();
}


static void crumb_symbols (size_t k) {
  size_t n, i, m = 0, pl = 0, pe = (size_t)-1;
  const Sym *v = symbols(&n);
  Pick p;
  Cur was;
  size_t top = T->top;
  int r, start = 0;
  if (k >= n) return;
  for (i = k; i-- > 0;)	/* its parent: the nearest one before it that is less deep and holds it */
    if (v[i].depth < v[k].depth && v[i].line <= v[k].line && v[i].end >= v[k].line) {
      pl = v[i].line;
      pe = v[i].end;
      break;
    }
  g_csl = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  pick_init(&p, "Go to Symbol");
  for (i = 0; i < n; i++) {
    int st;
    if (v[i].depth != v[k].depth || v[i].line < pl || v[i].line > pe) continue;
    if (i == k) start = (int)m;
    g_csl[m++] = v[i].line;
    pick_add(&p, v[i].name, NULL, (int)sym_icon(v[i].kind, &st));
  }
  p.keep_order = 1;
  p.start = start;
  p.on_move = crumb_sym_preview;
  cur_get(&was);
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) {	/* Esc: back where it was */
    cur_set(&was);
    T->top = top;
  }
  else crumb_sym_preview(r);
  free(g_csl);
  g_csl = NULL;
  E.focus = F_EDITOR;
}


/* a click on the breadcrumbs (or Ctrl+Shift+. on the last one): its siblings */
static void crumb_open (int i) {
  if (i < 0 || i >= CB.n) return;
  if (CB.dir[i]) crumb_files(CB.dir[i]);
  else crumb_symbols(CB.sym[i]);
}


/* the empty editor: VS Code's watermark with the keys to start */
static void draw_watermark (void) {
  static const int cmds[] = {CMD_PALETTE, CMD_QUICK_OPEN, CMD_OPEN_FILE, CMD_OPEN_FOLDER,
                             CMD_OPEN_PROJECT, CMD_NEW};
  int i, n = (int)(sizeof(cmds) / sizeof(cmds[0]));
  int y = L.text_y + (L.text_h - n * 2) / 2, mid = L.ed_x + L.ed_w / 2;
  scr_box(L.ed_x, L.ed_y + 1, L.ed_w, L.ed_h - 1, S_TEXT);
  for (i = 0; i < n; i++, y += 2) {
    const char *name = cmd_name(cmds[i]), *keys = cmd_keys(cmds[i]);
    int w = (int)str_cols(name);
    if (y >= L.text_y + L.text_h) break;
    scr_puts(mid - 1 - w, y, name, S_CRUMB);
    if (keys[0]) {
      scr_fill(mid + 1, y, (int)strlen(keys) + 2, S_TAB);
      scr_puts(mid + 2, y, keys, S_TAB);
    }
  }
}


/* the find widget, top right of the editor, like VS Code's */
static struct {
  int x, y, w, in_x, in_w, up_x, down_x, close_x, case_x, word_x, re_x, sel_x;
  int chev_x, one_x, all_x, h;	/* the chevron, Replace, Replace All; rows */
} g_fw;


static void draw_find (void) {
  size_t total, which;
  Pos a, b;
  char cnt[32];
  int w = L.ed_w - 4 < 56 ? L.ed_w - 4 : 56, x, y = L.text_y, cx;
  if (w < 24) return;
  x = L.ed_x + L.ed_w - w - 2;
  sel_range(&a, &b);
  total = count_matches(T->sel ? a : T->cur, &which);
  if (E.find[0] == '\0') cnt[0] = '\0';
  else if (E.find_regex && find_re() == NULL) snprintf(cnt, sizeof(cnt), "Bad regex");
  else if (total == 0) snprintf(cnt, sizeof(cnt), "No results");
  else if (which == 0) snprintf(cnt, sizeof(cnt), "? of %lu", (unsigned long)total);
  else snprintf(cnt, sizeof(cnt), "%lu of %lu", (unsigned long)which, (unsigned long)total);
  g_fw.x = x;
  g_fw.y = y;
  g_fw.w = w;
  g_fw.h = E.replacing ? 2 : 1;
  g_fw.chev_x = x;
  g_fw.close_x = x + w - 2;
  g_fw.down_x = x + w - 4;
  g_fw.up_x = x + w - 6;
  g_fw.sel_x = x + w - 8;
  g_fw.in_x = x + 2;
  g_fw.in_w = w - 23;
  g_fw.case_x = g_fw.in_x + g_fw.in_w - 7;
  g_fw.word_x = g_fw.in_x + g_fw.in_w - 5;
  g_fw.re_x = g_fw.in_x + g_fw.in_w - 3;
  scr_fill(x, y, w, S_BOX);
  scr_put(g_fw.chev_x, y, E.replacing ? 0xEAB4 : 0xEAB6, S_BOX);	/* the chevron: replace */
  scr_fill(g_fw.in_x, y, g_fw.in_w, S_INPUT);
  if (E.find[0]) cx = g_fw.in_x + 1 + scr_putsw(g_fw.in_x + 1, y, g_fw.in_w - 9, E.find, S_INPUT_ON);
  else {
    scr_putsw(g_fw.in_x + 1, y, g_fw.in_w - 9, "Find", S_INPUT_HINT);
    cx = g_fw.in_x + 1;
  }
  scr_put(g_fw.case_x, y, 0xEAB1, E.find_case ? S_TOGGLE_ON : S_INPUT);
  scr_put(g_fw.word_x, y, 0xEB7E, E.find_word ? S_TOGGLE_ON : S_INPUT);
  scr_put(g_fw.re_x, y, 0xEB38, E.find_regex ? S_TOGGLE_ON : S_INPUT);	/* codicon regex */
  scr_putsw(g_fw.in_x + g_fw.in_w + 1, y, 11, cnt, total == 0 && E.find[0] ? S_TOAST_WARN : S_BOX);
  scr_put(g_fw.sel_x, y, 0xEB85, E.find_insel ? S_TOGGLE_ON : S_BOX);	/* codicon selection: Find in Selection */
  scr_put(g_fw.up_x, y, 0xEAA1, S_BOX);	/* codicon arrow-up */
  scr_put(g_fw.down_x, y, 0xEA9A, S_BOX);	/* arrow-down */
  scr_put(g_fw.close_x, y, 0xEA76, S_BOX);	/* close */
  if (E.replacing) {	/* the replace box, Replace and Replace All */
    int ry = y + 1, rcx;
    scr_fill(x, ry, w, S_BOX);
    scr_fill(g_fw.in_x, ry, g_fw.in_w, S_INPUT);
    if (E.repl[0]) rcx = g_fw.in_x + 1 + scr_putsw(g_fw.in_x + 1, ry, g_fw.in_w - 2, E.repl, S_INPUT_ON);
    else {
      scr_putsw(g_fw.in_x + 1, ry, g_fw.in_w - 2, "Replace", S_INPUT_HINT);
      rcx = g_fw.in_x + 1;
    }
    g_fw.one_x = g_fw.in_x + g_fw.in_w + 1;
    g_fw.all_x = g_fw.one_x + 2;
    scr_put(g_fw.one_x, ry, 0xEB3D, S_BOX);	/* codicon replace */
    scr_put(g_fw.all_x, ry, 0xEB3C, S_BOX);	/* replace-all */
    if (E.finding && E.in_repl) {
      scr_cursor(rcx, ry);
      return;
    }
  }
  if (E.finding) scr_cursor(cx, y);
}


/* where the status bar's items are, for clicks */
static struct {
  int branch_x0, branch_x1, pos_x0, pos_x1, prob_x0, prob_x1, sync_x0, sync_x1;
  int ind_x0, ind_x1, eol_x0, eol_x1, lang_x0, lang_x1, bell_x, enc_x0, enc_x1;
} g_sb;


static void draw_status (void) {
  char right[256], pos[64], tmp[64];
  const char *br = git_branch();
  int y = E.rows - 1, x = 0, n;
  scr_fill(0, y, E.cols, S_STATUS);
  g_sb.branch_x0 = g_sb.branch_x1 = g_sb.pos_x0 = g_sb.pos_x1 = -1;
  g_sb.ind_x0 = g_sb.ind_x1 = g_sb.eol_x0 = g_sb.eol_x1 = g_sb.lang_x0 = g_sb.lang_x1 = -1;
  g_sb.bell_x = E.cols - 2;	/* the notifications: codicon bell, bell-dot when there are new ones */
  scr_put(g_sb.bell_x, y, toast_unread() ? 0xEB9A : 0xEAA2, S_STATUS);
  if (br[0]) {	/* the branch, on the left */
    g_sb.branch_x0 = x;
    x += 1;
    x += scr_put(x, y, 0xEA68, S_STATUS) + 1;
    x += scr_puts(x, y, br, S_STATUS);
    if (git_count() > 0) x += scr_put(x, y, '*', S_STATUS);
    x += 1;
    g_sb.branch_x1 = x;
  }
  g_sb.sync_x0 = g_sb.sync_x1 = -1;
  if (br[0] && git_has_upstream()) {	/* VS Code's sync item: behind and ahead */
    g_sb.sync_x0 = x;
    x += scr_put(x, y, 0xEA77, S_STATUS);	/* codicon sync */
    if (git_sync_text()[0]) x += 1 + scr_puts(x + 1, y, git_sync_text(), S_STATUS);
    x += 1;
    g_sb.sync_x1 = x;
  }
  g_sb.prob_x0 = g_sb.prob_x1 = -1;
  {	/* the problems: VS Code's error and warning counts */
    int ne, nw;
    char num[32];
    lsp_counts(&ne, &nw);
    if (ne || nw || (HAS_DOC && lsp_active(T->doc))) {
      g_sb.prob_x0 = x;
      x += 1;
      x += scr_put(x, y, 0xEA87, S_STATUS) + 1;	/* codicon error */
      snprintf(num, sizeof(num), "%d", ne);
      x += scr_puts(x, y, num, S_STATUS) + 1;
      x += scr_put(x, y, 0xEA6C, S_STATUS) + 1;	/* warning */
      snprintf(num, sizeof(num), "%d", nw);
      x += scr_puts(x, y, num, S_STATUS) + 1;
      g_sb.prob_x1 = x;
    }
  }
  if (!HAS_DOC || G->diff || T->page) return;
  if (opt.blame_status && T->real && !doc_dirty(T->doc)) {	/* git.blame.statusBarItem */
    const char *bl = git_blame(T->real, T->cur.y, 1);
    if (bl && x + 20 < E.cols / 2) {
      x += 1;
      x += scr_put(x, y, 0xEAFC, S_STATUS) + 1;	/* codicon git-commit */
      x += scr_putsw(x, y, E.cols / 2 - x, bl, S_STATUS) + 1;
    }
  }
  snprintf(pos, sizeof(pos), "Ln %lu, Col %lu", (unsigned long)(T->cur.y + 1),
           (unsigned long)(col_of(row_at(T->cur.y), T->cur.x) + 1));
  if (T->nmc > 0) {
    snprintf(tmp, sizeof(tmp), " (%d selections)", T->nmc + 1);
    strncat(pos, tmp, sizeof(pos) - strlen(pos) - 1);
  }
  else if (T->sel) {
    Pos a, b;
    size_t len;
    char *t;
    sel_range(&a, &b);
    t = doc_text(T->doc, a, b, &len);
    snprintf(tmp, sizeof(tmp), " (%lu selected)", (unsigned long)utf8_count(t, len));
    strncat(pos, tmp, sizeof(pos) - strlen(pos) - 1);
    free(t);
  }
  if (T->doc->tabs) snprintf(tmp, sizeof(tmp), "Tab Size: %d", TABW);
  else snprintf(tmp, sizeof(tmp), "Spaces: %d", T->doc->indent);
  g_sb.enc_x0 = g_sb.enc_x1 = -1;
  n = snprintf(right, sizeof(right), "%s   %s   %s   %s   %s%s  ", pos, tmp, enc_name(T->doc->enc),
               T->doc->crlf ? "CRLF" : "LF", T->sx ? syntax_name(T->sx) : ext_lang_label(T->doc->path),
               E.tab_focus ? "   Tab Moves Focus" : "");
  n = (int)str_cols(right) + 2;
  if (x + n + 2 > E.cols) {	/* narrow: the position only */
    snprintf(right, sizeof(right), "%s  ", pos);
    n = (int)str_cols(right) + 2;
  }
  if (x + n <= E.cols) {
    int rx = E.cols - n;
    g_sb.pos_x0 = rx;
    g_sb.pos_x1 = rx + (int)strlen(pos);
    if (strlen(right) > strlen(pos) + 4) {	/* "Spaces: 4", "LF", the language */
      g_sb.ind_x0 = g_sb.pos_x1 + 3;
      g_sb.ind_x1 = g_sb.ind_x0 + (int)strlen(tmp);
      g_sb.enc_x0 = g_sb.ind_x1 + 3;
      g_sb.enc_x1 = g_sb.enc_x0 + (int)strlen(enc_name(T->doc->enc));
      g_sb.eol_x0 = g_sb.enc_x1 + 3;
      g_sb.eol_x1 = g_sb.eol_x0 + (T->doc->crlf ? 4 : 2);
      g_sb.lang_x0 = g_sb.eol_x1 + 3;
      g_sb.lang_x1 = g_sb.lang_x0 + (int)str_cols(syntax_name(T->sx));
    }
    scr_puts(rx, y, right, S_STATUS);
  }
}


static void update_title (void) {
  const char *root = ws_active() ? ws_title() : path_basename(side_root());	/* "x (Workspace)" */
  if (HAS_DOC)
    snprintf(ui_title, sizeof(ui_title), "%s%s - %s - " MME_NAME,
             doc_dirty(T->doc) ? "\xE2\x97\x8F " : "", G->diff ? diff_title() : doc_name(), root);
  else snprintf(ui_title, sizeof(ui_title), "%s - " MME_NAME, root);
}


/*
** The minimap, as VS Code's: the text in small. Every cell is a Braille
** character, 2 x 4 dots: a dot is a few characters of a line (as many as
** editor.minimap.maxColumn asks for), 4 lines in a row (fewer with
** editor.minimap.scale), so words come out as little dashes in their
** token's color. When the file is longer than it can show it scrolls
** along with the editor. Its left edge has the git changes; problems,
** find matches and the symbol's occurrences tint where they are.
*/
static int g_mm_hover;	/* the mouse is over it: editor.minimap.showSlider "mouseover" */

static int mm_width (void) {
  return L.mm_w + L.mml_w;
}


static size_t mm_lpr (void) {	/* lines of text in one row of it */
  return vopt.mm_scale >= 3 ? 1 : vopt.mm_scale == 2 ? 2 : MM_LINES;
}


static size_t mm_ch (void) {	/* characters of text in one dot */
  size_t c = ((size_t)vopt.mm_maxcol + 2 * MM_W - 1) / (2 * MM_W);
  return c ? c : 1;
}


static size_t mm_top (void) {
  size_t lpr = mm_lpr(), cap = (size_t)L.text_h * lpr, n = T->doc->n, t;
  if (n <= cap || n <= (size_t)L.text_h) return 0;
  t = (size_t)((double)T->top * (double)(n - cap) / (double)(n - (size_t)L.text_h));
  return t > n - cap ? n - cap : t;
}


static uint32_t blend (uint32_t c, uint32_t bg, int pct) {
  uint32_t r = (((c >> 16) & 255) * (unsigned)pct + ((bg >> 16) & 255) * (unsigned)(100 - pct)) / 100;
  uint32_t g = (((c >> 8) & 255) * (unsigned)pct + ((bg >> 8) & 255) * (unsigned)(100 - pct)) / 100;
  uint32_t b = ((c & 255) * (unsigned)pct + (bg & 255) * (unsigned)(100 - pct)) / 100;
  return (r << 16) | (g << 8) | b;
}


#define MM_NONE	0xFFFFFFFFu

/* the color of each minimap dot of line y (2 * MM_W of them); MM_NONE: empty */
static void mm_line (size_t y, uint32_t *dot) {
  static unsigned char *tok;
  static size_t cap;
  const Row *r;
  size_t x = 0, c = 0, len, ch = mm_ch();
  int i;
  for (i = 0; i < 2 * MM_W; i++) dot[i] = MM_NONE;
  if (y >= T->doc->n) return;
  r = row_at(y);
  if (r->len + 1 > cap) {
    cap = r->len + 256;
    tok = (unsigned char *)xrealloc(tok, cap);
  }
  syntax_line_quick(T->doc, T->sx, y, tok);	/* the grammar is too slow for hundreds of lines a frame */
  while (x < r->len && c / ch < 2 * MM_W) {
    size_t w = char_width(r, x, c, &len);
    unsigned char b = (unsigned char)r->s[x];
    if (b != ' ' && b != '\t' && dot[c / ch] == MM_NONE) dot[c / ch] = tok_color(tok[x]);
    c += w;
    x += len;
  }
}


/* the minimap's marks: its cell of line y, column x (a byte), tinted col when it is stronger (pri) */
static void mm_mark (uint32_t *tint, unsigned char *pri, size_t top, size_t y, size_t x, uint32_t col, int p) {
  size_t lpr = mm_lpr(), cell;
  long row;
  if (y < top || y >= T->doc->n) return;
  row = (long)((y - top) / lpr);
  if (row >= L.text_h) return;
  cell = col_of(row_at(y), x) / (2 * mm_ch());
  if (cell >= (size_t)mm_width()) return;
  if (pri[row * MM_W + cell] < p) {
    pri[row * MM_W + cell] = (unsigned char)p;
    tint[row * MM_W + cell] = col;
  }
}


#define MM_ADD	0x2EA043	/* the git changes: editorGutter.addedBackground ... Dark Modern's */
#define MM_MOD	0x0078D4
#define MM_DEL	0xF85149

static void draw_minimap (void) {
  /* the Braille dots: bit[line][left, right] */
  static const unsigned char bit[MM_LINES][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
  int row, i, k, x0 = L.mm_x, mw = mm_width();
  size_t top = mm_top(), lpr = mm_lpr(), last = top + (size_t)L.text_h * lpr, y;
  int slider = vopt.mm_slider || g_mm_hover || E.drag_mm;
  uint32_t dot[MM_LINES][2 * MM_W];
  uint32_t *tint = (uint32_t *)calloc((size_t)L.text_h * MM_W + 1, sizeof(uint32_t));
  unsigned char *pri = (unsigned char *)calloc((size_t)L.text_h * MM_W + 1, 1);
  if (tint == NULL || pri == NULL) {
    free(tint);
    free(pri);
    return;
  }
  if (last > T->doc->n) last = T->doc->n;
  {	/* the marks: occurrences, find matches, warnings, errors (the strongest shows) */
    size_t nd, nh, d;
    const Diag *dv = lsp_diags(T->doc, &nd);
    const Pos *hv = hl_ranges(&nh);
    for (d = 0; d < nh; d++) mm_mark(tint, pri, top, hv[d * 2].y, hv[d * 2].x, 0xA0A0A0, 1);
    if (E.find_open && E.find[0])
      for (y = top; y < last; y++) {
        const Row *r = row_at(y);
        size_t xx;
        for (xx = 0; xx < r->len; xx++)
          if (match_at(y, xx)) mm_mark(tint, pri, top, y, xx, 0xD18616, 2);
      }
    for (d = 0; d < nd; d++)
      if (dv[d].sev <= 2) {
        size_t x1 = dv[d].b.y == dv[d].a.y ? dv[d].b.x : (dv[d].a.y < T->doc->n ? row_at(dv[d].a.y)->len : 0), xx;
        for (xx = dv[d].a.x; xx <= x1 && xx < dv[d].a.x + 400; xx += 2 * mm_ch())
          mm_mark(tint, pri, top, dv[d].a.y, xx, ui_color(dv[d].sev == 1 ? C_ERROR : C_WARNING), dv[d].sev == 1 ? 4 : 3);
      }
  }
  for (row = 0; row < L.text_h; row++) {
    size_t y0 = top + (size_t)row * lpr;
    int in_view = slider && (y0 + lpr > T->top && y0 < T->top + (size_t)L.text_h);
    uint32_t bg = ui_color(in_view ? C_MINIMAP_SLIDER : C_EDITOR_BG), git = 0;
    for (k = 0; k < MM_LINES; k++) mm_line(y0 + (size_t)k * lpr / MM_LINES, dot[k]);
    if (opt.scm_decor && T->real && !T->page) {	/* the git changes of its lines: the left edge */
      size_t j;
      for (j = 0; j < lpr; j++) {
        int qm = quick_mark(T->doc, T->real, y0 + j);
        if (qm & QM_MOD) git = MM_MOD;
        else if ((qm & QM_ADD) && git != MM_MOD) git = MM_ADD;
        else if (qm && !git) git = MM_DEL;
      }
    }
    for (i = 0; i < mw; i++) {
      uint32_t fg = MM_NONE, cbg = bg;
      unsigned m = 0;
      uint32_t ch;
      for (k = 0; k < MM_LINES; k++) {
        int d;
        for (d = 0; d < 2; d++)
          if (dot[k][2 * i + d] != MM_NONE) {
            m |= bit[k][d];
            if (fg == MM_NONE) fg = dot[k][2 * i + d];
          }
      }
      if (i < MM_W && pri[row * MM_W + i]) cbg = blend(tint[row * MM_W + i], bg, 55);
      if (vopt.mm_chars) ch = m ? 0x2800 + m : ' ';
      else {	/* editor.minimap.renderCharacters false: blocks of color */
        int up = (m & 0x1B) != 0, down = (m & 0xE4) != 0;
        ch = up && down ? 0x2588 : up ? 0x2580 : down ? 0x2584 : ' ';
      }
      if (i == 0 && git) scr_put_rgb(x0 + i, L.text_y + row, 0x258E, git, cbg, 0);	/* ▎ */
      else scr_put_rgb(x0 + i, L.text_y + row, ch, m ? blend(fg, cbg, vopt.mm_chars ? 80 : 45) : cbg, cbg, 0);
    }
  }
  free(tint);
  free(pri);
}


/* a click or a drag on the minimap: the slider taken follows the mouse; elsewhere, that line in the middle */
static void minimap_mouse (Mouse *m) {
  size_t n = T->doc->n, h = (size_t)L.text_h, lpr = mm_lpr(), top0 = mm_top(), top;
  long ry = m->y - L.text_y, s0, sh = (long)((h + lpr - 1) / lpr), want;
  if (n <= h) {
    T->top = 0;
    E.drag_mm = 1;
    return;
  }
  s0 = (long)((T->top > top0 ? T->top - top0 : 0) / lpr);
  if (!E.drag_mm) {
    if (ry >= s0 && ry < s0 + sh) E.drag_mm = 1 + (int)(ry - s0);	/* the slider is taken */
    else {
      size_t line = top0 + (size_t)(ry < 0 ? 0 : ry) * lpr, half = h / 2;
      T->top = line > half ? line - half : 0;
      if (T->top > n - h) T->top = n - h;
      E.drag_mm = 1 + (int)(sh / 2);
      return;
    }
  }
  want = ry - (E.drag_mm - 1);	/* the slider's top row wanted */
  if (want < 0) want = 0;
  if (n <= h * lpr) top = (size_t)want * lpr;
  else {
    double f = (double)want / (double)(L.text_h - sh > 0 ? L.text_h - sh : 1);
    top = (size_t)((f > 1 ? 1 : f) * (double)(n - h) + 0.5);
  }
  T->top = top > n - h ? n - h : top;
}


/*
** The scrollbar, VS Code's: a thumb for what the editor shows, and the
** overview ruler's marks in it: problems, find matches, the cursor.
*/
static int sb_thumb (int *pos) {
  size_t n = T->doc->n, h = (size_t)L.text_h;
  int size;
  if (n <= h) {
    *pos = 0;
    return (int)h;
  }
  size = (int)(h * h / n);
  if (size < 1) size = 1;
  *pos = (int)((double)T->top * (double)(L.text_h - size) / (double)(n - h) + 0.5);
  return size;
}


static void draw_scrollbar (void) {
  int x = L.ed_x + L.ed_w - 1, row, pos, size = sb_thumb(&pos);
  size_t n = T->doc->n, nd, i;
  const Diag *dv = lsp_diags(T->doc, &nd);
  unsigned char *mark = (unsigned char *)calloc((size_t)L.text_h + 1, 1);
  /* the marks: 1 cursor, 2 3 4 git added, modified, deleted, 5 occurrence, 6 find match, 7 warning, 8 error; the strongest shows */
#define MARK(line, m)	do { double d_ = (double)(line) * L.text_h / (double)(n > 0 ? n : 1); \
    int r_ = d_ < (double)L.text_h ? (int)d_ : L.text_h - 1;	/* a line past the end (a server's): the last row */ \
    if (r_ < 0) r_ = 0; if (mark[r_] < (m)) mark[r_] = (unsigned char)(m); } while (0)
  if (mark == NULL) return;
  MARK(T->cur.y, 1);
  if (opt.scm_decor && T->real && !T->page) {	/* the git changes, VS Code's left lane */
    size_t nq, q, j;
    const QHunk *qh = quick_hunks(T->doc, T->real, &nq);
    for (q = 0; qh && q < nq; q++) {
      int kind = qh[q].nn == 0 ? 4 : qh[q].on == 0 ? 2 : 3;
      if (qh[q].nn == 0) MARK(qh[q].n0 ? qh[q].n0 - 1 : 0, kind);
      for (j = 0; j < qh[q].nn && j < 100000; j++) MARK(qh[q].n0 + j, kind);
    }
  }
  if (E.find_open && E.find[0] && n < 20000) {
    size_t y;
    for (y = 0; y < n; y++) {
      const Row *r = row_at(y);
      size_t xx;
      for (xx = 0; xx < r->len; xx++)
        if (match_at(y, xx)) {
          MARK(y, 6);
          break;
        }
    }
  }
  {	/* the occurrences of the symbol at the cursor */
    size_t nh, k;
    const Pos *hv = hl_ranges(&nh);
    for (k = 0; k < nh; k++) MARK(hv[k * 2].y, 5);
  }
  for (i = 0; i < nd; i++)
    if (dv[i].sev <= 2) MARK(dv[i].a.y, dv[i].sev == 1 ? 8 : 7);
#undef MARK
  for (row = 0; row < L.text_h; row++) {
    int on = row >= pos && row < pos + size && size < L.text_h;
    uint32_t bg = ui_color(on ? (E.drag_sb ? C_THUMB_ON : C_THUMB) : C_EDITOR_BG);
    uint32_t col[9];
    col[0] = 0;
    col[1] = ui_color(C_GUTTER_ON);
    col[2] = MM_ADD;	/* editorOverviewRuler.addedForeground ... */
    col[3] = MM_MOD;
    col[4] = MM_DEL;
    col[5] = 0xA0A0A0u;	/* selectionHighlightForeground */
    col[6] = 0xD18616u;	/* findMatchForeground */
    col[7] = ui_color(C_WARNING);
    col[8] = ui_color(C_ERROR);
    if (mark[row]) scr_put_rgb(x, L.text_y + row, mark[row] == 1 ? 0x2500 : mark[row] <= 4 ? 0x258C : 0x25AC, col[mark[row]], bg, 0);
    else scr_put_rgb(x, L.text_y + row, ' ', bg, bg, 0);
  }
  free(mark);
}


/* the horizontal scrollbar: the part of the widest line that shows */
static int hsb_thumb (int *pos, int *x0, int *w) {
  size_t width = doc_width() + 1, tc = (size_t)text_cols();
  int size;
  *x0 = L.ed_x + gutter_width();
  *w = (int)tc;
  if (width <= tc) {
    *pos = 0;
    return *w;
  }
  size = (int)(tc * tc / width);
  if (size < 2) size = 2;
  *pos = (int)((double)T->left * (double)(*w - size) / (double)(width - tc) + 0.5);
  if (*pos > *w - size) *pos = *w - size;
  return size;
}


static void draw_hscrollbar (void) {
  int pos, x0, w, size = hsb_thumb(&pos, &x0, &w), i, y = L.text_y + L.text_h;
  scr_fill(L.ed_x, y, gutter_width(), S_TEXT);
  for (i = 0; i < w; i++) {	/* half a row high: the lower half of the cell */
    int on = i >= pos && i < pos + size;
    uint32_t fg = ui_color(on ? (E.drag_hsb ? C_THUMB_ON : C_THUMB) : C_EDITOR_BG);
    scr_put_rgb(x0 + i, y, 0x2584, fg, ui_color(C_EDITOR_BG), 0);
  }
}


static void hscrollbar_mouse (Mouse *m) {
  int pos, x0, w, size = hsb_thumb(&pos, &x0, &w), rx = m->x - x0;
  size_t width = doc_width() + 1, tc = (size_t)text_cols(), left;
  if (width <= tc || w - size <= 0) return;
  if (!E.drag_hsb) {
    if (rx >= pos && rx < pos + size) E.drag_hsb = 1 + (rx - pos);	/* the thumb is taken */
    else E.drag_hsb = 1 + size / 2;	/* elsewhere: it jumps there */
  }
  rx -= E.drag_hsb - 1;
  if (rx < 0) rx = 0;
  left = (size_t)((double)rx * (double)(width - tc) / (double)(w - size) + 0.5);
  T->left = left > width - tc ? width - tc : left;
}


/* a click or a drag on the scrollbar */
static void scrollbar_mouse (Mouse *m) {
  int pos, size = sb_thumb(&pos), ry = m->y - L.text_y;
  size_t n = T->doc->n, h = (size_t)L.text_h, top;
  if (n <= h) return;
  if (!E.drag_sb) {
    if (ry >= pos && ry < pos + size) E.drag_sb = 1 + (ry - pos);	/* the thumb is taken */
    else E.drag_sb = 1 + size / 2;	/* elsewhere: the thumb jumps there, like VS Code */
  }
  ry -= E.drag_sb - 1;
  if (ry < 0) ry = 0;
  if (L.text_h - size <= 0) return;
  top = (size_t)((double)ry * (double)(n - h) / (double)(L.text_h - size) + 0.5);
  T->top = top > n - h ? n - h : top;
}


/* the panel's title row and the terminal under it */
static struct {
  int prob_x0, prob_x1, out_x0, out_x1, dbg_x0, dbg_x1, term_x0, term_x1;	/* the panel's own tabs */
  int kill_x, close_x, add_x, prof_x, split_x, max_x, clear_x, chan_x0, chan_x1;
  int tab_x0[16], tab_x1[16], ntab;	/* the terminals' tabs */
} g_pn;


/*
** PROBLEMS, OUTPUT, DEBUG CONSOLE and TERMINAL, like VS Code's; E.panel_view
** is 1, 3, 2 and 0.
*/
static void draw_panel (void) {
  int y = L.panel_y, x1 = L.area_x + L.area_w, i, focus = E.focus == F_PANEL;
  char t[160];
  for (i = 0; i < L.area_w; i++) scr_put(L.area_x + i, y, 0x2500, S_BORDER);
  {	/* the tabs, the one shown underlined */
    int ne, nw;
    lsp_counts(&ne, &nw);
    snprintf(t, sizeof(t), " PROBLEMS %d ", ne + nw);
    g_pn.prob_x0 = L.area_x + 2;
    g_pn.prob_x1 = g_pn.prob_x0 + scr_puts(g_pn.prob_x0, y, t, E.panel_view == 1 ? S_PANEL_TAB_ON : S_PANEL_TAB);
    g_pn.out_x0 = g_pn.prob_x1 + 1;
    g_pn.out_x1 = g_pn.out_x0 + scr_puts(g_pn.out_x0, y, " OUTPUT ", E.panel_view == 3 ? S_PANEL_TAB_ON : S_PANEL_TAB);
    g_pn.dbg_x0 = g_pn.out_x1 + 1;
    g_pn.dbg_x1 = g_pn.dbg_x0 + scr_puts(g_pn.dbg_x0, y, " DEBUG CONSOLE ", E.panel_view == 2 ? S_PANEL_TAB_ON : S_PANEL_TAB);
    g_pn.term_x0 = g_pn.dbg_x1 + 1;
    g_pn.term_x1 = g_pn.term_x0 + scr_puts(g_pn.term_x0, y, " TERMINAL ", E.panel_view == 0 ? S_PANEL_TAB_ON : S_PANEL_TAB);
  }
  g_pn.close_x = x1 - 3;
  g_pn.max_x = x1 - 5;
  g_pn.kill_x = g_pn.add_x = g_pn.prof_x = g_pn.split_x = g_pn.clear_x = g_pn.chan_x0 = g_pn.chan_x1 = -1;
  g_pn.ntab = 0;
  scr_put(g_pn.close_x, y, 0xEA76, S_PANEL_TAB);	/* close */
  scr_put(g_pn.max_x, y, E.panel_max ? 0xEAB4 : 0xEAB7, S_PANEL_TAB);	/* chevron: maximize, restore */
  if (E.panel_view == 1) {
    draw_problems(L.area_x, y + 1, L.area_w, L.panel_h - 1, focus);
    problems_title(y, x1);	/* after: the counts are known */
    return;
  }
  if (E.panel_view == 2) {
    console_draw(L.area_x, y + 1, L.area_w, L.panel_h - 1, focus);
    return;
  }
  if (E.panel_view == 3) {	/* the channel's list, and Clear Output */
    int w;
    g_pn.clear_x = x1 - 7;
    scr_put(g_pn.clear_x, y, 0xEABF, S_PANEL_TAB);	/* codicon clear-all */
    snprintf(t, sizeof(t), " %s \xE2\x8C\x84 ", out_count() ? out_name(out_current()) : "Output");	/* ⌄ */
    w = (int)str_cols(t);
    g_pn.chan_x1 = x1 - 9;
    g_pn.chan_x0 = g_pn.chan_x1 - w;
    if (g_pn.chan_x0 > g_pn.term_x1 + 1) scr_puts(g_pn.chan_x0, y, t, S_INPUT);
    out_draw(L.area_x, y + 1, L.area_w, L.panel_h - 1, focus);
    return;
  }
  g_pn.kill_x = x1 - 7;
  g_pn.split_x = x1 - 9;
  g_pn.prof_x = x1 - 11;
  g_pn.add_x = x1 - 13;
  if (panel_count() == 1 || L.area_w < 40) {	/* a tab for each terminal: "1: mmc", the one in front underlined */
    int tx = g_pn.term_x1 + 2, n = panel_count(), k;
    for (k = 0; k < n && k < 16; k++) {
      int w;
      snprintf(t, sizeof(t), " %d: %s ", k + 1, panel_name(k));
      w = (int)str_cols(t);
      if (tx + w >= g_pn.add_x - 1) break;
      scr_puts(tx, y, t, k == panel_current() ? S_PANEL_TAB_ON : S_PANEL_TAB);
      g_pn.tab_x0[k] = tx;
      g_pn.tab_x1[k] = tx + w;
      g_pn.ntab = k + 1;
      tx += w + 1;
    }
    if (tx + 2 < g_pn.add_x - 1) {	/* the title of the one in front, dim */
      snprintf(t, sizeof(t), "%s", panel_title());
      scr_putsw(tx + 1, y, g_pn.add_x - 3 - tx, t, S_PANEL_TAB);
    }
  }
  else if (g_pn.term_x1 + 4 < g_pn.add_x - 1) {	/* the tabs list has them: the title only */
    snprintf(t, sizeof(t), "%s", panel_title());
    scr_putsw(g_pn.term_x1 + 2, y, g_pn.add_x - 3 - g_pn.term_x1, t, S_PANEL_TAB);
  }
  scr_put(g_pn.add_x, y, 0xEA60, S_PANEL_TAB);	/* codicon add */
  scr_put(g_pn.prof_x, y, 0xEAB4, S_PANEL_TAB);	/* chevron-down: the profiles */
  scr_put(g_pn.split_x, y, 0xEB56, S_PANEL_TAB);	/* split-horizontal */
  scr_put(g_pn.kill_x, y, 0xEA81, S_PANEL_TAB);	/* codicon trash */
  panel_draw(L.area_x, y + 1, L.area_w, L.panel_h - 1, focus);
}


/* the tab bar, the breadcrumbs and the text of the group G, in L.ed_x / ed_w */
/* git.blame.editorDecoration: who changed the cursor's line, dim after its end */
static void draw_blame (int gw) {
  const char *bl;
  Pos e;
  int cx, cy, end = L.ed_x + gw + text_cols(), i, n;
  if (!opt.blame_line || T->real == NULL || doc_dirty(T->doc) || T->nmc > 0 || dbg_stopped()) return;
  if ((bl = git_blame(T->real, T->cur.y, 0)) == NULL) return;
  e.y = T->cur.y;
  e.x = row_at(e.y)->len;
  if (!screen_at(e, &cx, &cy)) return;
  cx += 3;	/* VS Code leaves a little room */
  {
    char lt[256];
    int lw = lens_text(e.y, lt, sizeof(lt));
    if (lw > 0) cx += lw + 3;	/* after the code lenses */
  }
  if (cx >= end - 4) return;
  n = scr_putsw(cx, cy, end - cx, bl, TOK(T_WS, T->sel ? B_EDITOR : B_LINE));
  for (i = 0; i < n; i++) scr_set_fg(cx + i, cy, ui_color(C_DIM));
}


/* debug.inlineValues: while paused, "x = 3" dim after the lines of the frame's function */
static void draw_inline_values (int gw) {
  int sy, end = L.ed_x + gw + text_cols();
  if (!opt.inline_values || T->real == NULL || !dbg_stopped()) return;
  for (sy = 0; sy < L.text_h; sy++) {
    size_t y, from, to, left;
    char val[512];
    const Row *r;
    Pos e;
    int cx, cy, n, i;
    if (!vis_goto(sy, &y, &from, &to, &left)) break;
    r = row_at(y);
    if (to < r->len || dbg_inline(T->real, y, r->s, r->len, val, sizeof(val)) <= 0) continue;
    e.y = y;
    e.x = r->len;
    if (!screen_at(e, &cx, &cy)) continue;
    cx += 2;
    if (cx >= end - 4) continue;
    n = scr_putsw(cx, cy, end - cx, val, TOK(T_WS, y == T->cur.y && !T->sel ? B_LINE : B_EDITOR));
    for (i = 0; i < n; i++) {	/* editor.inlineValuesForeground / Background */
      uint32_t bg = ui_color(y == T->cur.y ? C_LINE_BG : C_EDITOR_BG), mix = 0;
      int sh;
      for (sh = 0; sh <= 16; sh += 8) mix |= ((((bg >> sh) & 255) * 4 + ((0xFFC800 >> sh) & 255)) / 5) << sh;
      scr_set_fg(cx + i, cy, ui_color(C_DIM));
      scr_set_bg(cx + i, cy, mix);
    }
  }
}


/* stopped on an exception: VS Code's peek under the line, its name and message */
static void draw_exception (void) {
  const char *title, *desc;
  Pos p;
  int r, y0, w = L.ed_w - L.sb_w, i, rows;
  if (T->real == NULL || !dbg_stopped()) return;
  p.y = T->cur.y;
  {	/* the frame's line: where the arrow is */
    size_t y;
    for (y = T->top; y < T->doc->n && y < T->top + (size_t)L.text_h + 50; y++)
      if (dbg_exception(T->real, y, &title, &desc)) break;
    if (y >= T->doc->n || y >= T->top + (size_t)L.text_h + 50) return;
    p.y = y;
  }
  p.x = 0;
  r = vis_row(p);
  if (r < 0 || r >= L.text_h) return;
  rows = desc[0] ? 4 : 3;
  y0 = L.text_y + r + 1;
  if (y0 + rows > L.text_y + L.text_h) y0 = L.text_y + L.text_h - rows;
  if (y0 <= L.text_y) return;
  for (i = 0; i < w; i++) {	/* the red frame, top and bottom */
    scr_put_rgb(L.ed_x + i, y0, 0x2500, 0xF14C4C, ui_color(C_EDITOR_BG), 0);
    scr_put_rgb(L.ed_x + i, y0 + rows - 1, 0x2500, 0xF14C4C, ui_color(C_EDITOR_BG), 0);
  }
  scr_fill(L.ed_x, y0 + 1, w, S_BOX);
  scr_put_rgb(L.ed_x + 1, y0 + 1, 0xEA87, 0xF14C4C, ui_color(C_MENU_BG), 0);	/* error */
  {
    char t[512];
    snprintf(t, sizeof(t), "Exception has occurred: %s", title);
    scr_putsw(L.ed_x + 3, y0 + 1, w - 4, t, S_BOX_TITLE);
  }
  if (desc[0]) {
    scr_fill(L.ed_x, y0 + 2, w, S_BOX);
    scr_putsw(L.ed_x + 3, y0 + 2, w - 4, desc, S_BOX);
  }
}


/* the diff's file opened at the line of its cursor (Enter, or the title's icon) */
static void diff_edit_file (void) {
  size_t line = diff_line();
  char *path = xstrdup(diff_path());
  if (open_file(path, 0) == 0) {
    Pos p;
    p.y = line - 1;
    p.x = 0;
    move_h(doc_clamp(T->doc, p), 0);
    center_cursor();
  }
  free(path);
}


/* the tab bar and the breadcrumbs go over the whole group, a minimap on the left too */
static void draw_tabs_full (void) {
  int sw = L.mml_w;
  L.ed_x -= sw;
  L.ed_w += sw;
  draw_tabs();
  L.ed_x += sw;
  L.ed_w -= sw;
}


static void draw_crumbs_full (void) {
  int sw = L.mml_w;
  L.ed_x -= sw;
  L.ed_w += sw;
  draw_crumbs();
  L.ed_x += sw;
  L.ed_w -= sw;
}


static void draw_group (int other) {
  int gw = gutter_width(), sy, ns;
  Pos sa, sb;
  draw_tabs_full();
  if (G->diff && HAS_DIFF) {
    draw_crumbs_full();
    diff_draw(L.ed_x, L.text_y, L.ed_w, L.text_h);
    diff_title_draw(L.ed_x + L.ed_w, L.ed_y);	/* its actions in the tab bar, like VS Code's */
  }
  else if (!HAS_DOC) draw_watermark();
  else if (T->page) page_draw(other);
  else if (T->md) {	/* the Markdown preview */
    draw_crumbs_full();
    md_draw(T->doc, L.ed_x, L.text_y, L.ed_w, L.text_h + L.hsb, &T->top);
  }
  else {
    draw_crumbs_full();
    sel_range(&sa, &sb);
    {
      Pos ba, bb;
      int lit = !other && cursor_brackets(&ba, &bb);
      if (!other) conflict_reset();	/* the conflicts' links, drawn again */
      for (sy = 0; sy < L.text_h; sy++) {
        size_t y, from, to, left;
        if (!vis_goto(sy, &y, &from, &to, &left)) break;
        draw_row(L.text_y + sy, y, gw, sa, sb, from, to, left);
        if (T->sx) color_brackets(L.text_y + sy, y, gw, from, to, left, ba, bb, lit);
        conflict_row(L.text_y + sy, y, gw, from, other);
      }
      for (; sy < L.text_h; sy++) draw_rulers(L.text_y + sy, gw, E.wrap ? 0 : T->left);	/* under the end too */
    }
    draw_lenses(gw);
    if (!other) draw_blame(gw);
    if (!other) draw_inline_values(gw);
    ns = draw_sticky(gw, sa, sb);
    if (!other && E.link_y > 0 && (size_t)E.link_y <= T->doc->n) {	/* Ctrl+hover */
      Pos a, b;
      int ax, ay, bx, by;
      a.y = b.y = (size_t)E.link_y - 1;
      a.x = E.link_x0;
      b.x = E.link_x1;
      if (b.x <= row_at(a.y)->len && screen_at(a, &ax, &ay) && screen_at(b, &bx, &by) && ay == by && bx > ax)
        scr_underline(ax, ay, bx - ax, ui_color(C_ACCENT));
    }
    if (!other && E.focus == F_EDITOR && !E.finding && !peek_is_open()) {
      int cx, cy;
      if (screen_at(E.drag_drop && E.drop_moved ? E.drop : T->cur, &cx, &cy) && cy >= L.text_y + ns)
        scr_cursor(cx, cy);
    }
    if (mm_width() > 0) draw_minimap();
    if (L.sb_w > 0) draw_scrollbar();
    if (L.hsb) draw_hscrollbar();
    if (!other) draw_peek();
    if (!other) draw_dirty_peek();
    if (!other) draw_exception();
    if (E.find_open && !other) draw_find();
    if (!other) {
      draw_signature(gw);
      draw_comp(gw);
      draw_hover(gw);
    }
  }
}


/* the text may have changed under a view (the other group's undo): keep it inside */
static void clamp_view (void) {
  int i;
  if (!HAS_DOC) return;
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
  if (T->top >= T->doc->n && !T->md) T->top = T->doc->n - 1;
  for (i = 0; i < T->nmc; i++) {
    T->mc[i].cur = doc_clamp(T->doc, T->mc[i].cur);
    T->mc[i].anchor = doc_clamp(T->doc, T->mc[i].anchor);
  }
}


/* the lines to the right of group g and under it, where other groups are */
static void draw_group_edges (int g) {
  int i, x = L.gx[g] + L.gw[g], y = L.gy[g] + L.gh[g];
  int ex = L.area_x + L.area_w, ey = L.body_y + L.body_h - L.panel_h;
  int on = E.grp_resize == 1000 + g, onb = E.grp_resize == 2000 + g;
  if (g_ngrp < 2) return;	/* one group (maybe centered): no line */
  if (x < ex)
    for (i = L.gy[g]; i < L.gy[g] + L.gh[g] + (y < ey ? 1 : 0); i++)
      scr_put(x, i, 0x2502, on ? S_TOGGLE_ON : S_BORDER);
  if (y < ey)
    for (i = L.gx[g]; i < L.gx[g] + L.gw[g]; i++) scr_put(i, y, 0x2500, onb ? S_TOGGLE_ON : S_BORDER);
}


/* a tab being dragged: where it would go, tinted like VS Code's editorGroup.dropBackground */
static void draw_drop (void) {
  int g = E.tdrop_g, x0, y0, x1, y1, x, y;
  if (E.tdrop_zone == DROP_NONE || g < 0 || g >= g_ngrp) return;
  x0 = L.gx[g];
  y0 = L.gy[g] + 1;
  x1 = L.gx[g] + L.gw[g];
  y1 = L.gy[g] + L.gh[g];
  if (E.tdrop_zone == DROP_IN && E.tdrop_at >= 0) {	/* the tab bar: a bar where it would go */
    int gs = g_gcur;
    Group *g0 = G;
    Tab *t0 = T;
    G = &g_grp[g];
    T = G->ntab ? G->tab[G->active] : &g_none;
    use_group(g);
    draw_tabs();
    x = E.tdrop_at < g_tabs.n && g_tabs.x0[E.tdrop_at] >= 0 ? g_tabs.x0[E.tdrop_at] : (g_tabs.n ? g_tabs.x1[g_tabs.n - 1] : L.gx[g]);
    G = g0;
    T = t0;
    use_group(gs);
    if (x >= L.gx[g] && x < x1) scr_put(x, L.gy[g], 0x258F, S_TOGGLE_ON);	/* ▏ */
    return;
  }
  if (E.tdrop_zone == DROP_LEFT) x1 = x0 + (x1 - x0) / 2;
  else if (E.tdrop_zone == DROP_RIGHT) x0 = x1 - (x1 - x0) / 2;
  else if (E.tdrop_zone == DROP_UP) y1 = y0 + (y1 - y0) / 2;
  else if (E.tdrop_zone == DROP_DOWN) y0 = y1 - (y1 - y0) / 2;
  for (y = y0; y < y1; y++)
    for (x = x0; x < x1; x++) scr_set_bg(x, y, ui_color(C_SEL_BG));
}


/* everything but the overlays; emenu.c draws those on top */
static void compose (void) {
  layout();
  update_title();
  scr_clear(S_TEXT);
  scr_cursor(0, -1);
  if (SHOW_MENU) menubar_draw(-1);
  if (L.act_w > 0) act_draw(L.act_x, L.body_y, L.body_h, E.view, E.side);
  if (L.side_w > 0) {
    int ey;
    int tree_h = L.body_h, oe_h = 0, ol_h = 0, tl_h = 0, py, sf = E.focus == F_SIDE;
    if (E.view == VIEW_FILES && L.body_h > 10) {	/* the panes: OPEN EDITORS over the folder, the others under it */
      oe_h = oe_height();
      tl_h = tl_height();
      tree_h = L.body_h - oe_h - tl_h;
      if (HAS_DOC && T->sx && (tree_h > 12 || !OL.open)) {
        ol_h = OL.open ? tree_h * 2 / 5 : 2;	/* collapsed: its title only */
        tree_h -= ol_h;
      }
      if (tree_h < 4) {
        oe_h = ol_h = tl_h = 0;
        tree_h = L.body_h;
      }
      else tree_h += oe_h;	/* the folder's pane has OPEN EDITORS in it, under "EXPLORER" */
    }
    files_gap(oe_h);
    side_draw(E.view, L.side_x, L.body_y, L.side_w - 1, tree_h,
              sf && E.outline_focus == PANE_TREE, HAS_DOC ? T->real : NULL);
    py = L.body_y + tree_h;
    OE.h = OL.h = TL.h = 0;
    if (oe_h) {	/* its line goes: "EXPLORER" is above it */
      draw_open_editors(L.side_x, L.body_y, L.side_w - 1, oe_h, sf && E.outline_focus == PANE_EDITORS);
      scr_fill(L.side_x, L.body_y, L.side_w - 1, S_SIDE);
      scr_puts(L.side_x + 2, L.body_y, "EXPLORER", S_SIDE_HEAD);
    }
    if (ol_h) draw_outline(L.side_x, py, L.side_w - 1, ol_h, sf && E.outline_focus == PANE_OUTLINE);
    py += ol_h;
    if (tl_h) draw_timeline(L.side_x, py, L.side_w - 1, tl_h, sf && E.outline_focus == PANE_TIMELINE);
    if (E.outline_focus && ((E.outline_focus == PANE_OUTLINE && !OL.h) || (E.outline_focus == PANE_EDITORS && !OE.h) ||
                            (E.outline_focus == PANE_TIMELINE && !TL.h)))
      E.outline_focus = PANE_TREE;	/* its pane went */
    for (ey = 0; ey < L.body_h; ey++)	/* the edge that drags */
      scr_put(L.edge_x, L.body_y + ey, 0x2502, E.resizing ? S_TOGGLE_ON : S_BORDER);
  }
  if (!(E.panel_max && L.panel_h >= L.body_h)) {	/* every editor group (a maximized panel hides them) */
    int g, cur = g_gcur, other;
    for (g = 0; g < g_ngrp; g++) {
      other = (g != cur);
      G = &g_grp[g];
      T = G->ntab ? G->tab[G->active] : &g_none;
      use_group(g);
      group_layout();
      clamp_view();
      draw_group(other);
      draw_group_edges(g);
    }
    if (E.tdrag == 2) draw_drop();
    G = &g_grp[cur];
    T = G->ntab ? G->tab[G->active] : &g_none;
    layout();
  }
  if (L.panel_h > 0) draw_panel();
  if (SHOW_STATUS) draw_status();
  if (dbg_active()) {	/* statusBar.debuggingBackground; the debug toolbar over the editor */
    int x;
    if (SHOW_STATUS)
      for (x = 0; x < E.cols; x++) {
        scr_set_bg(x, E.rows - 1, 0xCC6633);
        scr_set_fg(x, E.rows - 1, 0xFFFFFF);
      }
    dbg_toolbar_draw(L.area_x, L.area_w, L.body_y);
  }
  toast_draw();
}


static void check_size (void) {
  int c, r;
  term_size(&c, &r);
  if (c != E.cols || r != E.rows) {
    E.cols = c;
    E.rows = r;
    scr_resize(c, r);
  }
}


static void background (void) {
  check_size();
  compose();
}


static void draw (void) {
  background();
  scr_cursor_shape(E.focus == F_PANEL && E.panel && E.panel_view == 0 && panel_alive() ? panel_cursor_shape()
                                                                                          : editor_shape());
  scr_flush();
}

/* }================================================================== */


/*
** {==================================================================
** Moving and selecting
** ===================================================================
*/

static void move_to (Pos p, int extend) {
  if (extend && !T->sel) {
    T->sel = 1;
    T->anchor = T->cur;
  }
  else if (!extend) T->sel = 0;
  T->cur = doc_clamp(T->doc, p);
  if (T->sel && pos_cmp(T->anchor, T->cur) == 0) T->sel = 0;
}


/* horizontal moves also set the column Up and Down aim for */
static void move_h (Pos p, int extend) {
  move_to(p, extend);
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


static void move_v (long dy, int extend) {
  Pos p = T->cur;
  if (T->nfold) {	/* folded regions are one line */
    long i;
    for (i = 0; i < (dy < 0 ? -dy : dy); i++) {
      size_t ny = dy < 0 ? line_prev(p.y) : line_next(p.y);
      if (dy > 0 && ny >= T->doc->n) break;
      if (dy < 0 && p.y == 0) break;
      p.y = ny;
    }
    p.x = x_of_col(row_at(p.y), T->want);
    move_to(p, extend);
    return;
  }
  if (dy < 0 && (size_t)-dy > p.y) {	/* above the first line: its start */
    p.y = 0;
    p.x = 0;
    move_h(p, extend);
    return;
  }
  if (dy > 0 && p.y + (size_t)dy >= T->doc->n) {
    move_h(doc_end(T->doc), extend);
    return;
  }
  p.y = (size_t)((long)p.y + dy);
  p.x = x_of_col(row_at(p.y), T->want);
  move_to(p, extend);
}


/* word wrap: Up / Down go to the row above / below, even in the same line; 0: not wrapped here */
static int wrap_move (int d, int extend) {
  size_t st[MAXSEG], n = wrap_segs(T->cur.y, st), k = seg_of(T->cur.y, T->cur.x);
  const Row *r = row_at(T->cur.y);
  size_t rel = col_of(r, T->cur.x) - col_of(r, st[k]);
  Pos p = T->cur;
  if (d < 0 && k > 0) {	/* the row above, in this line */
    size_t c = col_of(r, st[k - 1]) + rel;
    p.x = x_of_col(r, c);
    if (p.x >= st[k]) p.x = prev_x(r, st[k]);
  }
  else if (d > 0 && k + 1 < n) {	/* the row below, in this line */
    size_t end = k + 2 < n ? st[k + 2] : r->len;
    p.x = x_of_col(r, col_of(r, st[k + 1]) + rel);
    if (p.x >= end && end < r->len) p.x = prev_x(r, end);
  }
  else if (d < 0 && p.y > 0) {	/* the last row of the line above */
    size_t m;
    p.y = line_prev(p.y);
    r = row_at(p.y);
    m = wrap_segs(p.y, st);
    p.x = x_of_col(r, col_of(r, st[m - 1]) + rel);
  }
  else if (d > 0 && line_next(p.y) < T->doc->n) {	/* the first row of the line below */
    size_t m;
    p.y = line_next(p.y);
    r = row_at(p.y);
    m = wrap_segs(p.y, st);
    p.x = x_of_col(r, rel);
    if (m > 1 && p.x >= st[1]) p.x = prev_x(r, st[1]);
  }
  else return 0;
  move_to(p, extend);
  return 1;
}


static void key_home (int extend) {
  Pos p = T->cur;
  size_t ind = indent_end(row_at(p.y));
  p.x = (p.x == ind) ? 0 : ind;	/* first the indent, then column 0 */
  move_h(p, extend);
}


/* selects the text from a to a + n, like a find match */
static void select_range (Pos a, size_t n) {
  Pos b = a;
  b.x += n;
  move_to(a, 0);
  move_h(doc_clamp(T->doc, b), 1);
}


static void center_cursor (void) {
  size_t half = (size_t)L.text_h / 2;
  T->top = T->cur.y > half ? T->cur.y - half : 0;
}

/* }================================================================== */


/*
** {==================================================================
** Editing
** ===================================================================
*/

static int delete_sel (void) {
  Pos a, b;
  if (!T->sel) return 0;
  sel_range(&a, &b);
  ed_delete(a, b);
  T->cur = a;
  T->sel = 0;
  return 1;
}


static void insert (const char *s, size_t n) {
  delete_sel();
  T->cur = ed_insert(T->cur, s, n);
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


static void insert_char (uint32_t cp) {
  char u[4];
  insert(u, (size_t)utf8_encode(cp, u));
}


static void newline (void) {
  const Row *r;
  char *s;
  size_t ind, more = 0, i;
  delete_sel();
  r = row_at(T->cur.y);
  ind = indent_end(r);
  if (ind > T->cur.x) ind = T->cur.x;
  if (eopt.auto_indent == 0) ind = 0;	/* editor.autoIndent "none" */
  else if (eopt.auto_indent >= 2 && indent_more(r->s, T->cur.x))	/* after "{", ":" ...: one more */
    more = T->doc->tabs ? 1 : (size_t)T->doc->indent;
  s = (char *)xmalloc(ind + more + 1);	/* the new line keeps this one's indent */
  s[0] = '\n';
  memcpy(s + 1, r->s, ind);
  for (i = 0; i < more; i++) s[1 + ind + i] = T->doc->tabs ? '\t' : ' ';
  insert(s, ind + more + 1);
  free(s);
}


static void backspace (int word) {
  Pos a = T->cur;
  if (delete_sel()) return;
  if (word) a = word_left(a);
  else if (a.x > 0) a.x = prev_x(row_at(a.y), a.x);
  else if (a.y > 0) {
    a.y--;
    a.x = row_at(a.y)->len;
  }
  ed_delete(a, T->cur);
  move_h(a, 0);
}


static void delete_forward (int word) {
  Pos b = T->cur;
  const Row *r = row_at(b.y);
  if (delete_sel()) return;
  if (word) b = word_right(b);
  else if (b.x < r->len) b.x = next_x(r, b.x);
  else if (b.y + 1 < T->doc->n) {
    b.y++;
    b.x = 0;
  }
  ed_delete(T->cur, b);
}


/* the lines the selection touches (a selection ending at column 0 stops before) */
static void sel_lines (size_t *ly, size_t *hy) {
  Pos a, b;
  if (!T->sel) {
    *ly = *hy = T->cur.y;
    return;
  }
  sel_range(&a, &b);
  *ly = a.y;
  *hy = (b.x == 0 && b.y > a.y) ? b.y - 1 : b.y;
}


static void indent_lines (int out) {
  size_t ly, hy, y;
  sel_lines(&ly, &hy);
  for (y = ly; y <= hy; y++) {
    const Row *r = row_at(y);
    Pos a, b;
    size_t n;
    a.y = b.y = y;
    a.x = 0;
    if (!out) {
      char sp[16];
      if (r->len == 0) continue;
      n = T->doc->tabs ? 1 : (size_t)T->doc->indent;
      memset(sp, T->doc->tabs ? '\t' : ' ', n);
      ed_insert(a, sp, n);
      if (T->cur.y == y) T->cur.x += n;
      if (T->sel && T->anchor.y == y && T->anchor.x > 0) T->anchor.x += n;
      continue;
    }
    if (r->len > 0 && r->s[0] == '\t') n = 1;
    else for (n = 0; n < r->len && n < (size_t)T->doc->indent && r->s[n] == ' '; n++) ;
    if (n == 0) continue;
    b.x = n;
    ed_delete(a, b);
    if (T->cur.y == y) T->cur.x = T->cur.x > n ? T->cur.x - n : 0;
    if (T->sel && T->anchor.y == y) T->anchor.x = T->anchor.x > n ? T->anchor.x - n : 0;
  }
}


static void tab (void) {
  size_t ly, hy;
  sel_lines(&ly, &hy);
  if (T->sel && hy > ly) {
    indent_lines(0);
    return;
  }
  if (T->doc->tabs) insert("\t", 1);
  else {
    char sp[16];
    size_t col = col_of(row_at(T->cur.y), T->cur.x);
    size_t n = (size_t)T->doc->indent - col % (size_t)T->doc->indent;
    memset(sp, ' ', n);
    insert(sp, n);
  }
}


/* Alt+Up / Alt+Down: the selected lines trade places with the one next to them */
static void move_lines (int down) {
  size_t ly, hy, len;
  char *t, *s;
  Pos a, b;
  sel_lines(&ly, &hy);
  if (down ? hy + 1 >= T->doc->n : ly == 0) return;
  a.y = ly;
  a.x = 0;
  b.y = hy;
  b.x = row_at(hy)->len;
  t = doc_text(T->doc, a, b, &len);
  s = (char *)xmalloc(len + 1);
  if (down) {
    b.y = hy + 1;
    b.x = 0;
    ed_delete(a, b);	/* the lines and their newline */
    a.x = row_at(ly)->len;
    s[0] = '\n';
    memcpy(s + 1, t, len);
  }
  else {
    a.y = ly - 1;
    a.x = row_at(ly - 1)->len;
    ed_delete(a, b);	/* the newline before and the lines */
    a.x = 0;
    memcpy(s, t, len);
    s[len] = '\n';
  }
  ed_insert(a, s, len + 1);
  free(s);
  free(t);
  T->cur.y = down ? T->cur.y + 1 : T->cur.y - 1;
  if (T->sel) T->anchor.y = down ? T->anchor.y + 1 : T->anchor.y - 1;
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
}


/* the main cursor stays with its text (ed_insert and ed_delete move the others) */
static void keep_ins (Pos at, Pos e) {
  T->cur = shift_ins(T->cur, at, e);
  T->anchor = shift_ins(T->anchor, at, e);
}


static void keep_del (Pos a, Pos b) {
  T->cur = shift_del(T->cur, a, b);
  T->anchor = shift_del(T->anchor, a, b);
}


static int cmp_size (const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  return x < y ? -1 : x > y;
}


/* the lines every cursor's selection touches, each once, in order; free it */
static size_t *cursor_lines (size_t *n) {
  size_t cap = 16, *v = (size_t *)xmalloc(cap * sizeof(size_t)), i, m = 0;
  int c;
  Cur was;
  *n = 0;
  cur_get(&was);
  for (c = -1; c < T->nmc; c++) {
    size_t ly, hy, y;
    if (c >= 0) cur_set(&T->mc[c]);
    sel_lines(&ly, &hy);
    for (y = ly; y <= hy; y++) {
      if (*n == cap) v = (size_t *)xrealloc(v, (cap *= 2) * sizeof(size_t));
      v[(*n)++] = y;
    }
  }
  cur_set(&was);
  qsort(v, *n, sizeof(size_t), cmp_size);
  for (i = 0; i < *n; i++)
    if (m == 0 || v[m - 1] != v[i]) v[m++] = v[i];
  *n = m;
  return v;
}


static void toggle_block_comment (void);


/* the language id of the file in front: mme's syntax, else an extension's */
static const char *doc_lang_id (void) {
  const char *id;
  if (T->sx) return snip_lang(syntax_name(T->sx));
  id = ext_lang_for(T->doc->path);
  return id ? id : "plaintext";
}


/* its comments: mme's, else its extension's language-configuration.json */
static void doc_comment (const char **line, const char **open, const char **close) {
  syntax_comment(T->sx, line, open, close);
  if (*line == NULL && *open == NULL && !T->sx) ext_comment(doc_lang_id(), line, open, close);
}

/* Ctrl+/: the lines get the language's line comment, or lose it when all have it */
static void comment_lines (int mode);

static void toggle_line_comment (void) {
  comment_lines(0);
}


/* the lines' comments: mode 0 toggles, 1 adds (Ctrl+K Ctrl+C), 2 removes (Ctrl+K Ctrl+U) */
static void comment_lines (int mode) {
  const char *lc, *bo, *bc;
  size_t *ln, nl, i, min = (size_t)-1, cl;
  int all = 1, any = 0;
  doc_comment(&lc, &bo, &bc);
  if (lc == NULL) {
    if (bo && bc) toggle_block_comment();
    else toast(0, "%s files have no comments", syntax_name(T->sx));
    return;
  }
  cl = strlen(lc);
  ln = cursor_lines(&nl);
  for (i = 0; i < nl; i++) {
    const Row *r = row_at(ln[i]);
    size_t ind = indent_end(r);
    if (ind == r->len) continue;	/* blank lines do not count */
    any = 1;
    if (ind < min) min = ind;
    if (!(r->len >= ind + cl && memcmp(r->s + ind, lc, cl) == 0)) all = 0;
  }
  if (!any) {	/* only blank lines: they get it at their end */
    all = 0;
    min = (size_t)-1;
  }
  if (mode) all = mode == 2;
  if (mode == 2 && !any) {
    free(ln);
    return;
  }
  doc_group(T->doc);
  for (i = nl; i-- > 0;) {
    const Row *r = row_at(ln[i]);
    size_t ind = indent_end(r);
    Pos a, b;
    a.y = b.y = ln[i];
    if (any && ind == r->len) continue;
    if (all && !(r->len >= ind + cl && memcmp(r->s + ind, lc, cl) == 0)) continue;	/* Remove: not commented */
    if (all) {	/* "// x" -> "x" */
      a.x = ind;
      b.x = ind + cl + (r->len > ind + cl && r->s[ind + cl] == ' ' ? 1 : 0);
      ed_delete(a, b);
      keep_del(a, b);
    }
    else {
      char *t = (char *)xmalloc(cl + 2);
      Pos e;
      memcpy(t, lc, cl);
      t[cl] = ' ';
      a.x = min < r->len ? min : r->len;
      e = ed_insert(a, t, cl + 1);
      if (!(pos_cmp(T->cur, a) == 0 && !T->sel && a.x == 0 && r->len == 0)) keep_ins(a, e);
      else T->cur = e;
      free(t);
    }
  }
  doc_group(T->doc);
  free(ln);
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


/* Shift+Alt+A: the selection in a block comment, or out of the one it is in */
static void toggle_block_comment (void) {
  const char *lc, *bo, *bc;
  size_t ol, cl;
  Pos a, b;
  const Row *ra, *rb;
  int around;
  doc_comment(&lc, &bo, &bc);
  if (bo == NULL || bc == NULL) {
    if (lc) toggle_line_comment();
    return;
  }
  ol = strlen(bo);
  cl = strlen(bc);
  sel_range(&a, &b);
  if (!T->sel) a = b = T->cur;
  ra = row_at(a.y);
  rb = row_at(b.y);
  doc_group(T->doc);
  {	/* the comment's start and end around the selection? (the spaces may be missing) */
    size_t s1 = a.x > 0 && ra->s[a.x - 1] == ' ' ? 1 : 0, s2 = b.x < rb->len && rb->s[b.x] == ' ' ? 1 : 0;
    if (!(a.x >= s1 + ol && memcmp(ra->s + a.x - s1 - ol, bo, ol) == 0)) s1 = 0;
    if (!(b.x + s2 + cl <= rb->len && memcmp(rb->s + b.x + s2, bc, cl) == 0)) s2 = 0;
    around = T->sel && a.x >= s1 + ol && memcmp(ra->s + a.x - s1 - ol, bo, ol) == 0 &&
             b.x + s2 + cl <= rb->len && memcmp(rb->s + b.x + s2, bc, cl) == 0;
    if (around) {
      ol += s1;
      cl += s2;
    }
  }
  if (around) {	/* around it: out */
    Pos p = b, q = a;
    q.x -= ol;
    p.x += cl;
    ed_delete(b, p);
    ed_delete(q, a);
    keep_del(b, p);
    keep_del(q, a);
  }
  else if (T->sel && pos_cmp(a, b) < 0 && a.x + ol <= ra->len && memcmp(ra->s + a.x, bo, ol) == 0 &&
           b.x >= cl && memcmp(rb->s + b.x - cl, bc, cl) == 0) {	/* in it: out */
    Pos p = b, q = a;
    size_t sp1 = (a.x + ol < ra->len && ra->s[a.x + ol] == ' ') ? 1 : 0;
    size_t sp2 = (b.x >= cl + 1 && rb->s[b.x - cl - 1] == ' ') ? 1 : 0;
    p.x -= cl + sp2;
    q.x += ol + sp1;
    ed_delete(p, b);
    ed_delete(a, q);
    keep_del(p, b);
    keep_del(a, q);
  }
  else {	/* the comment around the selection */
    Buf o, c;
    Pos e;
    buf_init(&o);
    buf_init(&c);
    buf_printf(&o, "%s ", bo);
    buf_printf(&c, " %s", bc);
    e = ed_insert(b, c.s, c.len);	/* the selection ends before it */
    e = ed_insert(a, o.s, o.len);
    if (T->sel) keep_ins(a, e);
    else T->cur = T->anchor = e;	/* in the middle of the empty one */
    buf_free(&o);
    buf_free(&c);
  }
  doc_group(T->doc);
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


/* Shift+Alt+Up / Down: the lines once more, above or below; the cursor goes with the copy */
static void copy_lines (int down) {
  size_t ly, hy, len, n;
  char *t, *s;
  Pos a, b;
  sel_lines(&ly, &hy);
  n = hy - ly + 1;
  a.y = ly;
  a.x = 0;
  b.y = hy;
  b.x = row_at(hy)->len;
  t = doc_text(T->doc, a, b, &len);
  s = (char *)xmalloc(len + 1);
  doc_group(T->doc);
  if (down) {
    s[0] = '\n';
    memcpy(s + 1, t, len);
    ed_insert(b, s, len + 1);
    T->cur.y += n;
    if (T->sel) T->anchor.y += n;
  }
  else {
    memcpy(s, t, len);
    s[len] = '\n';
    ed_insert(a, s, len + 1);	/* the cursor stays: on the copy above */
  }
  doc_group(T->doc);
  free(s);
  free(t);
}


/* Ctrl+Shift+K: the lines go */
static void delete_lines (void) {
  size_t ly, hy, n = T->doc->n;
  Pos a, b;
  sel_lines(&ly, &hy);
  a.y = ly;
  a.x = 0;
  if (hy + 1 < n) {
    b.y = hy + 1;
    b.x = 0;
  }
  else {	/* the last lines: the newline before them goes */
    b.y = hy;
    b.x = row_at(hy)->len;
    if (ly > 0) {
      a.y = ly - 1;
      a.x = row_at(ly - 1)->len;
    }
  }
  doc_group(T->doc);
  T->nmc = 0;
  T->sel = 0;
  ed_delete(a, b);
  doc_group(T->doc);
  T->cur.y = ly < T->doc->n ? ly : T->doc->n - 1;
  T->cur.x = x_of_col(row_at(T->cur.y), T->want);
}


/* Ctrl+L: the line selected, again: the next one too */
static void select_line (void) {
  Pos a, b;
  sel_range(&a, &b);
  if (!T->sel) b = a;
  T->anchor.y = a.y;
  T->anchor.x = 0;
  if (b.y + 1 < T->doc->n) {	/* whole lines already: one more */
    T->cur.y = b.y + 1;
    T->cur.x = 0;
  }
  else {
    T->cur.y = b.y;
    T->cur.x = row_at(b.y)->len;
  }
  T->sel = 1;
}


/* Ctrl+Enter / Ctrl+Shift+Enter: a new line under / over this one, with its indent */
static void insert_line (int above) {
  size_t y = T->cur.y;
  const Row *r = row_at(y);
  size_t ind = indent_end(r);
  T->sel = 0;
  doc_group(T->doc);
  if (above) {
    char *t = (char *)xmalloc(ind + 1);
    Pos a;
    memcpy(t, r->s, ind);
    t[ind] = '\n';
    a.y = y;
    a.x = 0;
    ed_insert(a, t, ind + 1);
    free(t);
    T->cur.y = y;
    T->cur.x = ind;
  }
  else {
    T->cur.x = r->len;
    newline();
  }
  doc_group(T->doc);
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}

/* }================================================================== */


/*
** {==================================================================
** Clipboard
** ===================================================================
*/

static void osc52 (const char *s, size_t n);

/* the clipboard, and the system's (the terminal's selection copied) */
void clip_set (const char *s, size_t n) {
  free(E.clip);
  E.clip = (char *)xmalloc(n + 1);
  memcpy(E.clip, s, n);
  E.clip[n] = '\0';
  E.cliplen = n;
  osc52(E.clip, E.cliplen);
}


const char *clip_get (size_t *n) {
  *n = E.clip ? E.cliplen : 0;
  return E.clip;
}


/* the system clipboard, through the terminal (OSC 52) */
static void osc52 (const char *s, size_t n) {
  static const char b64[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  Buf o;
  size_t i;
  buf_init(&o);
  buf_puts(&o, "\033]52;c;");
  for (i = 0; i < n; i += 3) {
    unsigned v = (unsigned)(unsigned char)s[i] << 16;
    if (i + 1 < n) v |= (unsigned)(unsigned char)s[i + 1] << 8;
    if (i + 2 < n) v |= (unsigned char)s[i + 2];
    buf_putc(&o, b64[(v >> 18) & 63]);
    buf_putc(&o, b64[(v >> 12) & 63]);
    buf_putc(&o, i + 1 < n ? b64[(v >> 6) & 63] : '=');
    buf_putc(&o, i + 2 < n ? b64[v & 63] : '=');
  }
  buf_puts(&o, "\a");
  term_write(o.s, o.len);
  buf_free(&o);
}


/* without a selection the whole line is taken, with its newline, like VS Code */
static void copy (int cut) {
  Pos a, b;
  size_t y = T->cur.y;
  if (!T->sel && !eopt.empty_sel_clip) return;	/* editor.emptySelectionClipboard: off */
  free(E.clip);
  if (T->sel) {
    sel_range(&a, &b);
    E.clip = doc_text(T->doc, a, b, &E.cliplen);
    osc52(E.clip, E.cliplen);
    if (cut) delete_sel();
    return;
  }
  a.y = b.y = y;
  a.x = 0;
  b.x = row_at(y)->len;
  E.clip = doc_text(T->doc, a, b, &E.cliplen);
  E.clip = (char *)xrealloc(E.clip, E.cliplen + 2);
  E.clip[E.cliplen++] = '\n';
  E.clip[E.cliplen] = '\0';
  osc52(E.clip, E.cliplen);
  if (!cut) return;
  if (y + 1 < T->doc->n) {	/* the line and the newline after it */
    b.y = y + 1;
    b.x = 0;
  }
  else if (y > 0) {	/* the last line: the newline before it */
    a.y = y - 1;
    a.x = row_at(y - 1)->len;
  }
  ed_delete(a, b);
  a.x = 0;
  move_h(doc_clamp(T->doc, a), 0);
}

/* }================================================================== */


/*
** {==================================================================
** Files and folders
** ===================================================================
*/

static void set_real (void) {
  free(T->real);
  T->real = T->doc->path ? os_realpath(T->doc->path) : NULL;
}


/* files.trimTrailingWhitespace, files.insertFinalNewline: one step of undo (auto: the cursor's line stays) */
static void before_save (int auto_) {
  size_t y;
  doc_group(T->doc);
  if (opt.trim_ws)
    for (y = 0; y < T->doc->n; y++) {
      const Row *r = row_at(y);
      Pos a, b;
      size_t e = r->len;
      while (e > 0 && (r->s[e - 1] == ' ' || r->s[e - 1] == '\t')) e--;
      if (e == r->len || (auto_ && y == T->cur.y)) continue;
      a.y = b.y = y;
      a.x = e;
      b.x = r->len;
      if (T->cur.y == y && T->cur.x > e) T->cur.x = e;
      ed_delete(a, b);
    }
  if (opt.final_newline && row_at(T->doc->n - 1)->len > 0) ed_insert(doc_end(T->doc), "\n", 1);
  T->cur = doc_clamp(T->doc, T->cur);
  doc_group(T->doc);
}


/* editor.cursorStyle, editor.cursorBlinking: DECSCUSR's 1 .. 6 */
static int editor_shape (void) {
  static const int shape[] = {5, 1, 3, 5, 1, 3};	/* bar, block, underline; the blinking one */
  return shape[opt.cursor_style] + (opt.cursor_blink ? 0 : 1);
}


/* settings.json read again: what it says now shows at once */
static void apply_settings (int report) {
  int r = settings_load();
  ext_init();	/* the extensions' themes, before the theme is set */
  E.minimap = opt.minimap;
  scr_cursor_shape(editor_shape());
  theme_set(opt.theme);
  E.wrap = opt.word_wrap;
  if (report) {
    if (r != 0) toast(1, "settings.json is not valid JSON: not applied");
    else toast(0, "Settings applied");
  }
}


/* the group in front becomes group g */
static void focus_group (int g) {
  if (g < 0 || g >= g_ngrp) return;
  g_gcur = g;
  G = &g_grp[g];
  T = G->ntab ? G->tab[G->active] : &g_none;
  clamp_view();	/* its text may have changed in another group (undo ...) */
}


static void tab_new (void);
static void set_real (void);

/*
** A new empty group next to group at: DROP_RIGHT, DROP_LEFT (a column of
** its own), DROP_DOWN, DROP_UP (in its column). It takes half of at's room.
** Its index, -1: there are MAX_GRP.
*/
static int group_add (int at, int where) {
  int c = g_grp[at].col, i, ng, ncol = grp_ncol();
  Group n;
  if (g_ngrp >= MAX_GRP) {
    toast(0, "There are %d editor groups already", MAX_GRP);
    return -1;
  }
  memset(&n, 0, sizeof(n));
  if (where == DROP_RIGHT || where == DROP_LEFT) {
    int nc = where == DROP_RIGHT ? c + 1 : c, w = g_colw[c] > 1 ? g_colw[c] : 100;
    memmove(g_colw + nc + 1, g_colw + nc, (size_t)(ncol - nc) * sizeof(int));
    g_colw[nc] = w / 2 > 0 ? w / 2 : 1;
    g_colw[where == DROP_RIGHT ? c : c + 1] = w - w / 2 > 0 ? w - w / 2 : 1;
    for (i = 0; i < g_ngrp; i++)
      if (g_grp[i].col >= nc) g_grp[i].col++;
    for (ng = 0; ng < g_ngrp && g_grp[ng].col < nc; ng++) ;	/* where the column starts */
    n.col = nc;
    n.hw = 100;
  }
  else {
    int w = g_grp[at].hw > 1 ? g_grp[at].hw : 100;
    ng = where == DROP_DOWN ? at + 1 : at;
    n.col = c;
    n.hw = w / 2 > 0 ? w / 2 : 1;
    g_grp[at].hw = w - w / 2 > 0 ? w - w / 2 : 1;
  }
  memmove(g_grp + ng + 1, g_grp + ng, (size_t)(g_ngrp - ng) * sizeof(Group));
  g_grp[ng] = n;
  g_ngrp++;
  if (g_gcur >= ng) g_gcur++;
  G = &g_grp[g_gcur];
  E.grp_max = 0;
  return ng;
}


/* group g goes (its tabs are gone or moved): its room goes to the one next to it */
static void group_remove (int g) {
  int c = g_grp[g].col, i, alone = 1, ncol = grp_ncol();
  if (g_ngrp < 2) return;
  for (i = 0; i < g_ngrp; i++)
    if (i != g && g_grp[i].col == c) alone = 0;
  if (!alone) {	/* the one above or under it gets taller */
    int nb = (g > 0 && g_grp[g - 1].col == c) ? g - 1 : g + 1;
    g_grp[nb].hw += g_grp[g].hw;
  }
  free(g_grp[g].tab);
  memmove(g_grp + g, g_grp + g + 1, (size_t)(g_ngrp - g - 1) * sizeof(Group));
  g_ngrp--;
  if (alone) {	/* its column goes too */
    if (c > 0) g_colw[c - 1] += g_colw[c];
    else if (ncol > 1) g_colw[1] += g_colw[0];
    memmove(g_colw + c, g_colw + c + 1, (size_t)(MAX_GRP - c - 1) * sizeof(int));
    g_colw[MAX_GRP - 1] = 100;
    for (i = 0; i < g_ngrp; i++)
      if (g_grp[i].col > c) g_grp[i].col--;
  }
  if (g_gcur > g || g_gcur >= g_ngrp) g_gcur = g_gcur > 0 ? g_gcur - 1 : 0;
  G = &g_grp[g_gcur];
  T = G->ntab ? G->tab[G->active] : &g_none;
  E.grp_max = 0;
}


/* Ctrl+\\ (right), Ctrl+K Ctrl+\\ (down): the editor splits; the file in front shows in the new group too */
static void split_editor_to (int where) {
  Tab *src = T;
  int ng;
  if (!HAS_DOC || T->page) return;	/* a page is not split */
  if ((ng = group_add(g_gcur, where)) < 0) return;
  focus_group(ng);
  tab_new();
  doc_free(T->doc);	/* the same text, not a new one */
  free(T->doc);
  T->doc = src->doc;
  T->doc->refs++;
  T->sx = src->sx;
  T->cur = src->cur;
  T->anchor = src->anchor;
  T->sel = src->sel;
  T->want = src->want;
  T->top = src->top;
  T->left = src->left;
  set_real();
  E.focus = F_EDITOR;
}


static void split_editor (void) {
  split_editor_to(DROP_RIGHT);
}


/* the group in front goes (its last tab closed) */
static void close_group (void) {
  group_remove(g_gcur);
}


static void theme_preview (int i) {
  theme_set(theme_name(i));
}


/* Ctrl+K Ctrl+T: the themes, each shown while it is selected; Enter keeps one */
static void pick_theme (void) {
  Pick p;
  int i, r, was = theme_current();
  pick_init(&p, "Select Color Theme (Up/Down Keys to Preview)");
  for (i = 0; i < theme_count(); i++)
    pick_add(&p, theme_name(i), i == was ? "current" : NULL, 0xEB5C);	/* codicon symbol-color */
  p.keep_order = 1;
  p.on_move = theme_preview;
  p.start = was;
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) {
    theme_set(theme_name(was));	/* Esc: the theme there was */
    return;
  }
  theme_set(theme_name(r));
  snprintf(opt.theme, sizeof(opt.theme), "%s", theme_name(r));
  settings_put("workbench.colorTheme", theme_name(r));
}


/* the tab in front becomes tab i */
static void focus_tab (int i) {
  if (i < 0 || i >= G->ntab) return;
  G->active = i;
  T = G->tab[i];
  G->diff = 0;
  clamp_view();
  if (E.side && E.view == VIEW_FILES) side_follow(T->real);
}


/* a new, empty tab right after the one in front, and in front */
static void tab_new (void) {
  Tab *t = (Tab *)xmalloc(sizeof(Tab));
  int at = G->ntab ? G->active + 1 : 0, k;
  memset(t, 0, sizeof(*t));
  for (k = 0; k < G->ntab && G->tab[k]->pinned; k++) ;
  if (at < k) at = k;	/* not among the pinned ones */
  t->doc = (Doc *)xmalloc(sizeof(Doc));
  doc_init(t->doc);
  t->doc->refs = 1;
  if (G->ntab == G->captab) {
    G->captab = G->captab ? G->captab * 2 : 8;
    G->tab = (Tab **)xrealloc(G->tab, (size_t)G->captab * sizeof(Tab *));
  }
  memmove(G->tab + at + 1, G->tab + at, (size_t)(G->ntab - at) * sizeof(Tab *));
  G->tab[at] = t;
  G->ntab++;
  G->active = at;
  T = t;
  G->diff = 0;
}


/* tab i goes; the one after it comes to the front, like VS Code */
static void tab_free (int i) {
  Tab *t = G->tab[i];
  if (--t->doc->refs <= 0) {	/* the last tab that shows it */
    lsp_close(t->doc);
    doc_free(t->doc);
    free(t->doc);
  }
  free(t->real);
  free(t->mc);
  free(t->fold);
  free(t);
  memmove(G->tab + i, G->tab + i + 1, (size_t)(G->ntab - i - 1) * sizeof(Tab *));
  G->ntab--;
  if (G->ntab == 0) {
    G->active = 0;
    T = &g_none;
    if (HAS_DIFF) G->diff = 1;
    return;
  }
  if (G->active > i || G->active >= G->ntab) G->active--;
  T = G->tab[G->active];
}


/* the tab in front is about to go: 1 when that is fine (saved, or not wanted) */
static int save_changes (void) {
  static const char *const bt[] = {"Save", "Don't Save", "Cancel"};
  char msg[512];
  if (!HAS_DOC || !doc_dirty(T->doc)) return 1;
  snprintf(msg, sizeof(msg), "Do you want to save the changes you made to %s?", doc_name());
  switch (dialog(msg, "Your changes will be lost if you don't save them.", bt, 3)) {
    case 0: run_command(CMD_SAVE); return !doc_dirty(T->doc);
    case 1: return 1;
  }
  return 0;
}


static void close_tab (int i) {
  Tab *keep;
  int k;
  if (i < 0 || i >= G->ntab) return;
  keep = (i != G->active) ? T : NULL;	/* closing another tab keeps this one in front */
  if (doc_dirty(G->tab[i]->doc) && G->tab[i]->doc->refs == 1) {
    focus_tab(i);	/* show it while asking */
    if (!save_changes()) return;
  }
  closed_push(G->tab[i]);
  tab_free(i);
  for (k = 0; keep && k < G->ntab; k++)
    if (G->tab[k] == keep) {
      G->active = k;
      T = keep;
    }
}


/* every tab, when the window closes: 1 when it may */
static int save_all_changes (void) {
  static const char *const bt[] = {"Save All", "Don't Save", "Cancel"};
  char msg[160], detail[512];
  int i, n = 0;
  for (i = 0; i < G->ntab; i++) n += doc_dirty(G->tab[i]->doc);
  if (n == 0) return 1;
  if (n == 1) {
    for (i = 0; i < G->ntab; i++)
      if (doc_dirty(G->tab[i]->doc)) focus_tab(i);
    return save_changes();
  }
  snprintf(msg, sizeof(msg), "Do you want to save the changes to the following %d files?", n);
  detail[0] = '\0';
  for (i = 0; i < G->ntab; i++)
    if (doc_dirty(G->tab[i]->doc)) {
      const char *nm = G->tab[i]->doc->path ? path_basename(G->tab[i]->doc->path) : "Untitled-1";
      if (strlen(detail) + strlen(nm) + 3 < sizeof(detail)) {
        if (detail[0]) strcat(detail, ", ");
        strcat(detail, nm);
      }
    }
  switch (dialog(msg, detail, bt, 3)) {
    case 0:
      for (i = 0; i < G->ntab; i++)
        if (doc_dirty(G->tab[i]->doc)) {
          focus_tab(i);
          run_command(CMD_SAVE);
          if (doc_dirty(T->doc)) return 0;
        }
      return 1;
    case 1: return 1;
  }
  return 0;
}


static void reset_view (void) {
  T->cur.y = T->cur.x = 0;
  T->sel = 0;
  T->top = T->left = 0;
  T->want = 0;
  T->last_kind = KIND_OTHER;
}


/* the text of path when a tab of the other group has it */
static Doc *other_doc (const char *path) {
  int g, i;
  for (g = 0; g < g_ngrp; g++) {
    if (&g_grp[g] == G) continue;
    for (i = 0; i < g_grp[g].ntab; i++) {
      Doc *d = g_grp[g].tab[i]->doc;
      if (d->path && m_fncmp(d->path, path) == 0) return d;
    }
  }
  return NULL;
}


static void recent_file_add (const char *path);

/*
** Opens path in a tab; 0 when it did. A preview (a single click, like VS
** Code) takes the place of the preview tab there is, until it is edited.
*/
static int open_file (const char *path, int preview) {
  int i, r;
  if (!opt.preview_tabs) preview = 0;	/* every file in a tab of its own */
  char *p = xstrdup(path), *real = os_realpath(path);
  for (i = 0; i < G->ntab; i++)	/* it is open: to the front ("main.go" and its full path are one) */
    if (!G->tab[i]->md && G->tab[i]->doc->path && (m_fncmp(G->tab[i]->doc->path, p) == 0 ||
                                 (real && G->tab[i]->real && m_fncmp(G->tab[i]->real, real) == 0))) {
      focus_tab(i);
      if (!preview) T->preview = 0;
      recent_file_add(p);
      free(p);
      free(real);
      return 0;
    }
  free(real);
  for (i = 0; preview && i < G->ntab; i++)
    if (G->tab[i]->preview && !G->tab[i]->md && !doc_dirty(G->tab[i]->doc)) break;
  if (preview && i < G->ntab) {	/* the preview tab takes the new file */
    focus_tab(i);
    if (T->doc->refs > 1) {	/* its text is the other group's too: a text of its own */
      T->doc->refs--;
      T->doc = (Doc *)xmalloc(sizeof(Doc));
      doc_init(T->doc);
    }
  }
  else tab_new();
  {	/* open in the other group: the same text, like VS Code */
    Doc *same = other_doc(p);
    if (same) {
      if (--T->doc->refs <= 0) {
        lsp_close(T->doc);	/* a preview tab's old text may have been the server's */
        doc_free(T->doc);
        free(T->doc);
      }
      T->doc = same;
      same->refs++;
      T->preview = preview;
      T->sx = syntax_detect(p, T->doc);
      G->diff = 0;
      reset_view();
      set_real();
      if (E.side && E.view == VIEW_FILES) side_follow(T->real);
      free(p);
      return 0;
    }
  }
  lsp_close(T->doc);
  r = doc_load(T->doc, p);
  T->doc->refs = 1;
  if (r < 0) {
    toast(1, "Unable to open '%s'", path_basename(p));
    tab_free(G->active);
    free(p);
    return -1;
  }
  T->preview = preview;
  T->sx = syntax_detect(p, T->doc);
  G->diff = 0;
  reset_view();
  set_real();
  if (T->sx) lsp_open(T->doc, syntax_name(T->sx));
  if (E.side && E.view == VIEW_FILES) side_follow(T->real);
  recent_file_add(p);
  free(p);
  return 0;
}


static char *recent_file (void) {
  return data_path("recent");
}


static void recent_load (Vec *v) {
  char *f = recent_file(), *s, *p;
  vec_init(v);
  if (f == NULL) return;
  s = read_file(f, NULL);
  free(f);
  if (s == NULL) return;
  for (p = strtok(s, "\r\n"); p; p = strtok(NULL, "\r\n"))
    if (*p) vec_push(v, xstrdup(p));
  free(s);
}


/* the folder goes on top of the list that Open Project shows */
static void recent_add (const char *dir) {
  Vec v;
  Buf b;
  size_t i;
  char *f = recent_file();
  int fd;
  if (f == NULL) return;
  recent_load(&v);
  buf_init(&b);
  buf_printf(&b, "%s\n", dir);
  for (i = 0; i < v.n && i < 20; i++)
    if (m_fncmp(v.v[i], dir) != 0) buf_printf(&b, "%s\n", v.v[i]);
  fd = os_open(f, OS_WRITE);
  if (fd >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  buf_free(&b);
  vec_free(&v);
  free(f);
}


static int is_workspace_file (const char *path) {
  size_t n = strlen(path);
  OsStat st;
  return n > 15 && m_fncmp(path + n - 15, ".code-workspace") == 0 && os_stat(path, &st) == 0 && !st.is_dir;
}


/* a .code-workspace: its folders in the Explorer, its settings over the user's */
static void open_workspace (const char *file) {
  if (ws_open(file) != 0) {
    toast(1, "'%s' is not a workspace that can be opened", path_basename(file));
    return;
  }
  recent_add(ws_file());
  apply_settings(0);
  git_refresh();
  E.side = 1;
  E.view = VIEW_FILES;
  E.focus = F_SIDE;
  if (HAS_DOC && T->real) side_reveal(T->real);
}


static void open_folder (const char *dir) {
  if (is_workspace_file(dir)) {
    open_workspace(dir);
    return;
  }
  ws_forget();
  side_open(dir);
  recent_add(side_root());
  apply_settings(0);	/* its .vscode/settings.json */
  git_refresh();
  E.side = 1;
  E.view = VIEW_FILES;
  E.focus = F_SIDE;
  if (T->real) side_reveal(T->real);
}


static int is_absolute (const char *s) {
  return path_is_sep(s[0]) || (s[0] && s[1] == ':');
}


/* VS Code's simple file dialog: a folder's list in the quick input box */
static char *file_dialog (const char *title, int folders) {
  char *dir = xstrdup(side_root());
  for (;;) {
    Pick p;
    Vec names;
    int *isdir, r, first = folders ? 2 : 1;
    size_t i;
    char *prefix = path_join(dir, "");
    vec_init(&names);
    os_listdir(dir, &names);
    vec_sort(&names);
    isdir = (int *)xmalloc((names.n + 1) * sizeof(int));
    pick_init(&p, title);
    p.prefix = prefix;
    p.keep_order = 1;
    if (folders) pick_add(&p, "Select This Folder", dir, 0xEAB2);	/* codicon check */
    pick_add(&p, "..", NULL, 0xEA83);	/* folder */
    for (i = 0; i < names.n; i++) {	/* folders first, then files */
      char *full = path_join(dir, names.v[i]);
      OsStat st;
      isdir[i] = os_stat(full, &st) == 0 && st.is_dir;
      free(full);
    }
    {
      size_t *order = (size_t *)xmalloc((names.n + 1) * sizeof(size_t)), k = 0, j;
      for (j = 0; j < names.n; j++)
        if (isdir[j]) order[k++] = j;
      if (!folders)
        for (j = 0; j < names.n; j++)
          if (!isdir[j]) order[k++] = j;
      for (j = 0; j < k; j++) {
        int ist;
        const char *nm = names.v[order[j]];
        pick_add(&p, nm, NULL, isdir[order[j]] ? 0xEA83 : (int)file_icon(nm, &ist));
      }
      free(order);
    }
    r = pick_run(&p);
    if (r == PICK_CANCEL) {
      pick_free(&p);
      vec_free(&names);
      free(isdir);
      free(prefix);
      free(dir);
      return NULL;
    }
    {
      char *next = NULL, *chosen = NULL;
      if (r == PICK_UP || (r >= first - 1 && r == first - 1)) next = path_dirname(dir);
      else if (r == 0 && folders) chosen = xstrdup(dir);
      else if (r == PICK_TEXT) {
        char *t = is_absolute(p.text) ? xstrdup(p.text) : path_join(dir, p.text);
        OsStat st;
        if (os_stat(t, &st) == 0 && st.exists && st.is_dir) next = t;
        else if (!folders) chosen = t;
        else {
          toast(1, "'%s' is not a folder", p.text);
          free(t);
        }
      }
      else {
        char *t = path_join(dir, p.item[r].label);
        OsStat st;
        if (os_stat(t, &st) == 0 && st.is_dir) next = t;
        else chosen = t;
      }
      pick_free(&p);
      vec_free(&names);
      free(isdir);
      free(prefix);
      if (chosen) {
        free(dir);
        return chosen;
      }
      if (next) {
        free(dir);
        dir = next;
      }
    }
  }
}


/* the files opened lately, newest first (mme-data/recent-files), for Go to File and Open Recent */
static void recent_files (Vec *v) {
  char *f = data_path("recent-files"), *s, *p;
  vec_init(v);
  s = read_file(f, NULL);
  free(f);
  if (s == NULL) return;
  for (p = strtok(s, "\r\n"); p; p = strtok(NULL, "\r\n"))
    if (*p) vec_push(v, xstrdup(p));
  free(s);
}


static void recent_file_add (const char *path) {
  Vec v;
  Buf b;
  size_t i;
  char *f = data_path("recent-files"), *real = os_realpath(path);
  const char *p = real ? real : path;
  int fd;
  recent_files(&v);
  if (v.n && m_fncmp(v.v[0], p) == 0) {	/* already on top: nothing to write */
    vec_free(&v);
    free(real);
    free(f);
    return;
  }
  buf_init(&b);
  buf_printf(&b, "%s\n", p);
  for (i = 0; i < v.n && i < 50; i++)
    if (m_fncmp(v.v[i], p) != 0) buf_printf(&b, "%s\n", v.v[i]);
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  buf_free(&b);
  vec_free(&v);
  free(real);
  free(f);
}


/* the folder's files for Go to File; paths gets each one's full path; skip: already there */
static void walk_files (const char *dir, const char *rel, Pick *p, Vec *paths, const Vec *skip, int depth) {
  Vec v;
  size_t i, k;
  vec_init(&v);
  if (depth > 32 || p->n > 2000000 || os_listdir(dir, &v) != 0) return;
  vec_sort(&v);
  for (i = 0; i < v.n; i++) {
    char *path = path_join(dir, v.v[i]);
    char *r = rel[0] ? xstrcat3(rel, "/", v.v[i]) : xstrdup(v.v[i]);
    OsStat st;
    const char *nm = v.v[i];
    if (files_excluded(r)) ;	/* files.exclude */
    else if (os_stat(path, &st) == 0 && st.is_dir) {
      if (strcmp(nm, ".git") != 0 && strcmp(nm, "node_modules") != 0 && strcmp(nm, "mme-data") != 0)
        walk_files(path, r, p, paths, skip, depth + 1);
    }
    else {
      int ist, dup = 0;
      for (k = 0; k < skip->n && !dup; k++) dup = m_fncmp(skip->v[k], path) == 0;
      if (!dup) {
        pick_add(p, nm, rel[0] ? rel : NULL, (int)file_icon(nm, &ist));
        vec_push(paths, path);
        path = NULL;
      }
    }
    free(path);
    free(r);
  }
  vec_free(&v);
}


/* where path is from the folder, for a dim detail ("src/x"); NULL: the folder itself */
static char *rel_dir (const char *path) {
  const char *root = side_root();
  size_t n = strlen(root);
  char *dir = path_dirname(path), *r, *c;
  if (m_strnicmp(dir, root, n) == 0 && (dir[n] == '\0' || path_is_sep(dir[n]))) {
    r = dir[n] ? xstrdup(dir + n + 1) : NULL;
    for (c = r; c && *c; c++)
      if (*c == '\\') *c = '/';
    free(dir);
    return r;
  }
  return dir;
}


/* Ctrl+P: any file of the folder by its name */
/* in Go to File, a first '>', '@', '#' or ':' is another picker, as in VS Code */
static int quick_tick (Pick *p, int changed) {
  (void)changed;
  return p->text[0] && strchr(">@#:", p->text[0]) ? 2 : 0;
}


/*
** {==================================================================
** The folder's files for Go to File
** ===================================================================
*/

/*
** Found once and kept, like VS Code's file search cache: the walk goes a
** little at a time (Go to File shows what there is and adds the rest as it
** comes), a small folder is walked again each time, a big one when a file
** was made, renamed or deleted (files_index_stale) or after a while.
*/
static struct {
  char *key;	/* the folders it is for */
  Vec name, dir, path;	/* each file: its name, its folder from the root (NULL none), its path */
  Vec todo, todo_rel;	/* folders still to read, the last first */
  int done, stale;
  long long made;	/* when the walk started */
} FI;


void files_index_stale (void) {
  FI.stale = 1;
}


/* left out of Go to File (VS Code: files.exclude, search.exclude, .gitignore) */
static int fi_skip (const char *name, const char *rel) {
  if (strcmp(name, ".git") == 0 || strcmp(name, "node_modules") == 0 || strcmp(name, "mme-data") == 0) return 1;
  return search_ignored(rel);
}


static char *fi_key (void) {
  Buf b;
  int i;
  buf_init(&b);
  for (i = 0; i < ws_count(); i++) buf_printf(&b, "%s\n", ws_folder(i));
  buf_putc(&b, '\0');
  return buf_take(&b);
}


static void fi_reset (char *key) {
  int i;
  free(FI.key);
  FI.key = key;
  vec_free(&FI.name);
  vec_free(&FI.path);
  vec_free(&FI.todo);
  vec_free(&FI.todo_rel);
  {	/* dir may hold NULLs: freed by hand */
    size_t k;
    for (k = 0; k < FI.dir.n; k++) free(FI.dir.v[k]);
    free(FI.dir.v);
    FI.dir.v = NULL;
    FI.dir.n = FI.dir.cap = 0;
  }
  vec_init(&FI.name);
  vec_init(&FI.path);
  vec_init(&FI.todo);
  vec_init(&FI.todo_rel);
  for (i = ws_count() - 1; i >= 0; i--) {	/* every folder of a workspace, by its name */
    vec_push(&FI.todo, xstrdup(ws_folder(i)));
    vec_push(&FI.todo_rel, xstrdup(ws_count() > 1 ? ws_folder_name(i) : ""));
  }
  FI.done = 0;
  FI.stale = 0;
  FI.made = os_now_us();
}


/* on with the walk for about budget us; 1 when it is done */
static int fi_step (long long budget) {
  long long end = os_now_us() + budget;
  while (FI.todo.n > 0 && FI.name.n < 2000000) {
    char *dir = FI.todo.v[--FI.todo.n], *rel = FI.todo_rel.v[--FI.todo_rel.n];
    Vec v, sub, subrel;
    size_t i;
    vec_init(&v);
    vec_init(&sub);
    vec_init(&subrel);
    if (os_listdir(dir, &v) == 0) {
      vec_sort(&v);
      for (i = 0; i < v.n; i++) {
        char *path = path_join(dir, v.v[i]), *r = rel[0] ? xstrcat3(rel, "/", v.v[i]) : xstrdup(v.v[i]);
        OsStat st;
        if (fi_skip(v.v[i], r)) {
          free(path);
          free(r);
          continue;
        }
        if (os_stat(path, &st) == 0 && st.is_dir) {
          vec_push(&sub, path);
          vec_push(&subrel, r);
          continue;
        }
        vec_push(&FI.name, xstrdup(v.v[i]));
        if (FI.dir.n == FI.dir.cap) {
          FI.dir.cap = FI.dir.cap ? FI.dir.cap * 2 : 1024;
          FI.dir.v = (char **)xrealloc(FI.dir.v, FI.dir.cap * sizeof(char *));
        }
        FI.dir.v[FI.dir.n++] = rel[0] ? xstrdup(rel) : NULL;
        vec_push(&FI.path, path);
        free(r);
      }
    }
    for (i = sub.n; i-- > 0;) {	/* the first subfolder is read next */
      vec_push(&FI.todo, sub.v[i]);
      vec_push(&FI.todo_rel, subrel.v[i]);
    }
    free(sub.v);
    free(subrel.v);
    vec_free(&v);
    free(dir);
    free(rel);
    if (os_now_us() > end) break;
  }
  FI.done = FI.todo.n == 0;
  return FI.done;
}


/* the index made ready for Go to File: kept, or walked again */
static void fi_begin (void) {
  char *key = fi_key();
  int reuse = FI.key && strcmp(FI.key, key) == 0 && !FI.stale &&
              (!FI.done || (FI.name.n >= 5000 && os_now_us() - FI.made < 300000000LL));	/* big: 5 minutes */
  if (reuse) free(key);
  else fi_reset(key);
  if (!FI.done) fi_step(150000);	/* a small folder is done before the list shows */
}

/* }================================================================== */


/* Go to File: the items taken from the index so far, the paths of the items */
static struct {
  size_t added;	/* FI entries looked at */
  Vec *paths;
  const Vec *recent;
  char status[64];
} QO;


static void qo_add (Pick *p) {
  for (; QO.added < FI.name.n; QO.added++) {
    size_t i = QO.added, k;
    int ist, dup = 0;
    for (k = 0; k < QO.recent->n && !dup; k++) dup = m_fncmp(QO.recent->v[k], FI.path.v[i]) == 0;
    if (dup) continue;
    pick_add(p, FI.name.v[i], FI.dir.v[i], (int)file_icon(FI.name.v[i], &ist));
    vec_push(QO.paths, xstrdup(FI.path.v[i]));
  }
  if (FI.done) p->status = NULL;
  else {
    snprintf(QO.status, sizeof(QO.status), "Indexing... %lu files", (unsigned long)FI.name.n);
    p->status = QO.status;
  }
}


/* while Go to File shows: the walk goes on, what it finds is added */
static int qo_tick (Pick *p, int changed) {
  size_t n0 = p->n;
  if (quick_tick(p, changed) == 2) return 2;
  if (FI.done && p->status == NULL) return 0;
  fi_step(60000);
  qo_add(p);
  return p->n != n0 || FI.done;
}


static void quick_open (void) {
  Pick p;
  Vec paths, recent;
  size_t i;
  int r;
  pick_init(&p, "Search files by name (append : to go to line, @ to go to symbol, # for the workspace's)");
  p.match_detail = 1;
  vec_init(&paths);
  recent_files(&recent);
  for (i = 0; i < recent.n; i++) {	/* recently opened, first */
    OsStat st;
    char *d;
    int ist;
    if (os_stat(recent.v[i], &st) != 0 || !st.exists || st.is_dir) continue;
    d = rel_dir(recent.v[i]);
    pick_add(&p, path_basename(recent.v[i]), d, (int)file_icon(recent.v[i], &ist));
    vec_push(&paths, xstrdup(recent.v[i]));
    free(d);
  }
  fi_begin();
  QO.added = 0;
  QO.paths = &paths;
  QO.recent = &recent;
  qo_add(&p);
  p.on_tick = qo_tick;
  r = pick_run(&p);
  if (r == PICK_SWITCH) {
    char text[512];
    snprintf(text, sizeof(text), "%s", p.text + 1);
    switch (p.text[0]) {
      case '>': palette(text); break;
      case '@': if (HAS_DOC && !G->diff) goto_symbol(text); break;
      case '#': workspace_symbols(text); break;
      default: if (HAS_DOC && !G->diff) goto_line(text); break;
    }
  }
  else if (r >= 0 && open_file(paths.v[r], 0) == 0) E.focus = F_EDITOR;
  pick_free(&p);
  vec_free(&paths);
  vec_free(&recent);
}


static void open_project (void) {
  Vec v;
  Pick p;
  size_t i;
  int r;
  Vec files;
  recent_load(&v);
  recent_files(&files);
  pick_init(&p, "Select to open (folders, then files)");
  for (i = 0; i < v.n; i++) pick_add(&p, path_basename(v.v[i]), v.v[i], 0xEA83);
  pick_add(&p, "Open Folder...", NULL, 0xEA83);
  for (i = 0; i < files.n; i++) {	/* recently opened files, like VS Code's Open Recent */
    int ist;
    char *d = path_dirname(files.v[i]);
    pick_add(&p, path_basename(files.v[i]), d, (int)file_icon(files.v[i], &ist));
    free(d);
  }
  r = pick_run(&p);
  if (r >= 0 && (size_t)r < v.n) open_folder(v.v[r]);
  else if (r >= 0 && (size_t)r == v.n) run_command(CMD_OPEN_FOLDER);
  else if (r > 0 && (size_t)r - v.n - 1 < files.n && open_file(files.v[r - v.n - 1], 0) == 0) E.focus = F_EDITOR;
  pick_free(&p);
  vec_free(&v);
  vec_free(&files);
}

/* }================================================================== */


static struct {
  char *text;	/* what the server said about the name, or NULL */
  Pos at;	/* the name */
  int from_mouse;
  int mx, my;	/* the mouse, where it rests */
  long long mt;
  int waiting;	/* it moved: after a while it is asked about */
  char *diag;	/* the problems there, waiting for the server's hover to join them */
  int qf_y, qf_x0, qf_x1;	/* where "Quick Fix..." is drawn, for a click; qf_y -1: none */
} HV;

static struct {
  char *label;	/* the signature, or NULL */
  size_t a0, a1;	/* the parameter the cursor is in */
  size_t y;	/* the line it is for */
} SG;


static void hover_close (void) {
  free(HV.text);
  HV.text = NULL;
  free(HV.diag);
  HV.diag = NULL;
}


/* the problems at p as the hover's first part (markdown); NULL: none */
static char *diag_text (Pos p) {
  size_t nd, i;
  const Diag *dv;
  Buf b;
  int any = 0;
  char *uni;
  if (!HAS_DOC) return NULL;
  dv = lsp_diags(T->doc, &nd);
  buf_init(&b);
  if ((uni = uni_explain(p)) != NULL) {	/* editor.unicodeHighlight: what the character is */
    buf_puts(&b, uni);
    free(uni);
    any = 2;
  }
  for (i = 0; i < nd; i++) {
    const Diag *g = &dv[i];
    char ic[4];
    int n;
    if (pos_cmp(p, g->a) < 0 || (pos_cmp(p, g->b) > 0 && !(g->a.y == g->b.y && g->a.x == g->b.x && p.y == g->a.y)))
      continue;
    n = utf8_encode(g->sev == 1 ? 0xEA87 : g->sev == 2 ? 0xEA6C : 0xEA74, ic);	/* codicons error, warning, info */
    ic[n] = '\0';
    buf_printf(&b, "%s%s %s", any ? "\n" : "", ic, g->msg);
    any = 1;
  }
  if (!any) {
    buf_free(&b);
    return NULL;
  }
  if (any == 1) buf_puts(&b, "\n\x01Quick Fix... (Ctrl+.)");	/* \x01: the link */
  buf_putc(&b, '\0');
  return buf_take(&b);
}



/*
** {==================================================================
** IntelliSense: suggestions, definitions, problems
** ===================================================================
*/

static struct {
  CompItem *v;	/* what the server suggested */
  size_t n;
  size_t *vis;	/* the ones that match what is typed, best first */
  size_t nvis, sel, top;
  Pos start;	/* where the word being completed starts */
  int open;
  int forced;	/* Ctrl+Space: say so when there is nothing */
  unsigned gen;	/* the server's answer it is (lsp_comp_gen) */
  int choice;	/* a snippet's choices: not filtered by what is typed */
} CP;

static int g_comp_details = 1;	/* the documentation beside the list (Ctrl+Space: Read Less / More) */


static void comp_free (void) {
  size_t i;
  for (i = 0; i < CP.n; i++) {
    free(CP.v[i].label);
    free(CP.v[i].detail);
    free(CP.v[i].insert);
    free(CP.v[i].filter);
    free(CP.v[i].sort);
    free(CP.v[i].doc);
    free(CP.v[i].json);
    while (CP.v[i].nextra > 0) free(CP.v[i].extra[--CP.v[i].nextra].text);
    free(CP.v[i].extra);
  }
  free(CP.v);
  free(CP.vis);
  memset(&CP, 0, sizeof(CP));
}


static Pos word_start (void) {
  Pos p = T->cur;
  const Row *r = row_at(p.y);
  while (p.x > 0 && char_class(r, p.x - 1) == 1) p.x--;
  return p;
}


/* the word around p: where it starts, where it ends */
static size_t word_start_at (Pos p) {
  const Row *r = row_at(p.y);
  size_t x = p.x;
  while (x > 0 && char_class(r, x - 1) == 1) x--;
  return x;
}


static size_t word_end_at (Pos p) {
  const Row *r = row_at(p.y);
  size_t x = p.x;
  while (x < r->len && char_class(r, x) == 1) x++;
  return x;
}


static void comp_grow (CompItem **v, size_t *n, size_t *cap) {
  if (*n + 1 >= *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *v = (CompItem *)xrealloc(*v, *cap * sizeof(CompItem));
  }
}


static void snippet_insert (Pos a, Pos b, const char *body);

/* the abbreviation before the cursor, if Emmet works in this file here; its body */
static char *emmet_here (Pos *a, int *worth) {
  int mode;
  const Row *r;
  size_t x0, i;
  if (!HAS_DOC || G->diff || T->sel || T->nmc || T->page) return NULL;
  mode = emmet_mode(doc_lang_id(), T->doc->path);
  if (!mode) return NULL;
  r = row_at(T->cur.y);
  x0 = emmet_start(r->s, T->cur.x, mode);
  if (x0 >= T->cur.x) return NULL;
  if (mode == EMMET_CSS) {	/* a declaration's place: after { or ; with only spaces */
    for (i = x0; i > 0 && (r->s[i - 1] == ' ' || r->s[i - 1] == '\t'); i--) ;
    if (i > 0 && r->s[i - 1] != '{' && r->s[i - 1] != ';') return NULL;
    if (i == 0) {	/* the line's start: inside a rule's braces? */
      long depth = 0;
      size_t y = T->cur.y, from = y > 300 ? y - 300 : 0, k;
      for (k = from; k < y; k++) {
        const Row *q = row_at(k);
        size_t c;
        for (c = 0; c < q->len; c++) depth += q->s[c] == '{' ? 1 : q->s[c] == '}' ? -1 : 0;
      }
      if (depth <= 0) return NULL;
    }
  }
  else {	/* not inside a tag: <a hr| */
    long lt = -1, gt = -1;
    for (i = 0; i < x0; i++) {
      if (r->s[i] == '<') lt = (long)i;
      if (r->s[i] == '>') gt = (long)i;
    }
    if (lt > gt) return NULL;
  }
  a->y = T->cur.y;
  a->x = x0;
  return emmet_expand(r->s + x0, T->cur.x - x0, mode, worth);
}


/* Tab after an abbreviation (emmet.triggerExpansionOnTab): expanded; 1 when it was */
static int emmet_tab (void) {
  Pos a;
  int worth;
  char *body;
  if (!opt.emmet_tab || (body = emmet_here(&a, &worth)) == NULL) return 0;
  if (worth) snippet_insert(a, T->cur, body);
  free(body);
  return worth;
}


/* the suggestion "Emmet Abbreviation", first; c is set again as the typing goes on */
static int emmet_item (CompItem *c) {
  Pos a, ws = word_start();
  int worth;
  char *body = opt.emmet_suggest ? emmet_here(&a, &worth) : NULL;
  const Row *r = row_at(T->cur.y);
  free(c->label);
  free(c->insert);
  free(c->filter);
  c->label = c->insert = c->filter = NULL;
  if (body && worth) {
    c->label = (char *)xmalloc(T->cur.x - a.x + 1);
    memcpy(c->label, r->s + a.x, T->cur.x - a.x);
    c->label[T->cur.x - a.x] = '\0';
    c->filter = (char *)xmalloc(T->cur.x - ws.x + 1);	/* the word typed: it matches */
    memcpy(c->filter, r->s + ws.x, T->cur.x - ws.x);
    c->filter[T->cur.x - ws.x] = '\0';
    c->insert = body;
    c->has_range = 1;
    c->a = a;
    c->b = T->cur;
    return 1;
  }
  free(body);
  c->label = xstrdup("");
  c->insert = xstrdup("");
  c->filter = xstrdup("");	/* hidden */
  return 0;
}


static int is_emmet_item (const CompItem *c) {
  return c->kind == 15 && c->detail && strcmp(c->detail, "Emmet Abbreviation") == 0;
}


/* the language's snippets, as suggestions */
static void add_snippets (CompItem **v, size_t *n, size_t *cap) {
  size_t ns, i;
  const Snip *sn;
  {	/* Emmet's first */
    CompItem e;
    memset(&e, 0, sizeof(e));
    if (emmet_item(&e)) {
      comp_grow(v, n, cap);
      e.detail = xstrdup("Emmet Abbreviation");
      e.sort = xstrdup(" ");
      e.kind = 15;
      e.snippet = 1;
      (*v)[(*n)++] = e;
    }
    else {
      free(e.label);
      free(e.insert);
      free(e.filter);
    }
  }
  if (!opt.snippet_suggest) return;
  sn = snip_list(doc_lang_id(), &ns);
  for (i = 0; i < ns; i++) {
    CompItem *c;
    comp_grow(v, n, cap);
    c = &(*v)[(*n)++];
    memset(c, 0, sizeof(*c));
    c->label = xstrdup(sn[i].prefix);
    c->detail = xstrdup(sn[i].desc);
    c->insert = xstrdup(sn[i].body);
    c->filter = xstrdup(sn[i].prefix);
    c->sort = xstrdup(sn[i].prefix);
    c->kind = 15;
    c->snippet = 1;
  }
}


typedef struct Word {
  const char *s;
  size_t n;
} Word;

static int cmp_word (const void *a, const void *b) {
  const Word *x = (const Word *)a, *y = (const Word *)b;
  size_t m = x->n < y->n ? x->n : y->n;
  int c = memcmp(x->s, y->s, m);
  return c ? c : (x->n < y->n ? -1 : x->n > y->n);
}


/* the file's words (editor.wordBasedSuggestions), each once; not the one being typed */
static void add_words (CompItem **v, size_t *n, size_t *cap) {
  Word *w = NULL;
  size_t nw = 0, cw = 0, y, i, from, to, got = 0;
  Pos ws = word_start();
  if (!opt.word_suggest) return;
  from = T->cur.y > 5000 ? T->cur.y - 5000 : 0;	/* around the cursor in a big file */
  to = T->cur.y + 5000 < T->doc->n ? T->cur.y + 5000 : T->doc->n;
  for (y = from; y < to; y++) {
    const Row *r = row_at(y);
    size_t x = 0;
    while (x < r->len) {
      size_t b;
      if (char_class(r, x) != 1) {
        x++;
        continue;
      }
      b = x;
      while (x < r->len && char_class(r, x) == 1) x++;
      if (x - b < 2 || (r->s[b] >= '0' && r->s[b] <= '9')) continue;
      if (y == ws.y && b == ws.x) continue;	/* the word typed */
      if (nw == cw) w = (Word *)xrealloc(w, (cw = cw ? cw * 2 : 256) * sizeof(Word));
      w[nw].s = r->s + b;
      w[nw++].n = x - b;
    }
  }
  qsort(w, nw, sizeof(Word), cmp_word);
  for (i = 0; i < nw && got < 3000; i++) {
    CompItem *c;
    if (i > 0 && cmp_word(&w[i - 1], &w[i]) == 0) continue;
    comp_grow(v, n, cap);
    c = &(*v)[(*n)++];
    memset(c, 0, sizeof(*c));
    c->label = (char *)xmalloc(w[i].n + 1);
    memcpy(c->label, w[i].s, w[i].n);
    c->label[w[i].n] = '\0';
    c->detail = xstrdup("");
    c->insert = xstrdup(c->label);
    c->filter = xstrdup(c->label);
    c->sort = (char *)xmalloc(w[i].n + 2);	/* after the snippets */
    snprintf(c->sort, w[i].n + 2, "~%s", c->label);
    c->kind = 1;
    got++;
  }
  free(w);
}


static void comp_filter (void);
static void comp_resolve_sel (void);

/* no language server: the suggestions are the snippets and the file's words */
static void comp_local (int forced) {
  CompItem *v = NULL;
  size_t n = 0, cap = 0;
  add_snippets(&v, &n, &cap);
  add_words(&v, &n, &cap);
  comp_free();
  CP.v = v;
  CP.n = n;
  CP.vis = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  comp_filter();
  if (!CP.open && forced) toast(0, "No suggestions.");
}


/* ask the server what fits here (the answer comes to on_completion) */
static void comp_ask (int forced) {
  if (!HAS_DOC) return;
  if (!lsp_active(T->doc)) {
    comp_local(forced);
    return;
  }
  CP.forced = forced;
  lsp_complete(T->doc, T->cur);
}


/* how well s matches what is typed: 0 same start, 1 same start in another case, 2 in it, 3 letters in order; -1 no */
static int comp_score (const char *s, const char *pre, size_t n) {
  size_t i, j, m = strlen(s);
  if (n == 0) return 0;
  if (m >= n && strncmp(s, pre, n) == 0) return 0;
  if (m >= n && m_strnicmp(s, pre, n) == 0) return 1;
  for (i = 0; i + n <= m; i++)
    if (m_strnicmp(s + i, pre, n) == 0) return 2;
  for (i = 0, j = 0; i < m && j < n; i++)
    if (lower((unsigned char)s[i]) == lower((unsigned char)pre[j])) j++;
  return j == n ? 3 : -1;
}


static const CompItem *g_cv;
static const int *g_cs;

static int cmp_comp (const void *a, const void *b) {
  size_t x = *(const size_t *)a, y = *(const size_t *)b;
  if (g_cs[x] != g_cs[y]) return g_cs[x] - g_cs[y];
  return strcmp(g_cv[x].sort, g_cv[y].sort);
}


/* the suggestions that match the word typed so far; closes when none does */
static void comp_filter (void) {
  const Row *r;
  char pre[256];
  size_t n, i;
  int *score;
  if (CP.choice) return;	/* a snippet's choices: all of them */
  CP.start = word_start();
  r = row_at(T->cur.y);
  n = T->cur.x - CP.start.x;
  if (CP.n == 0 || n >= sizeof(pre)) {
    CP.open = 0;
    return;
  }
  memcpy(pre, r->s + CP.start.x, n);
  pre[n] = '\0';
  score = (int *)xmalloc((CP.n + 1) * sizeof(int));
  CP.nvis = 0;
  for (i = 0; i < CP.n; i++)	/* Emmet's follows what is typed */
    if (is_emmet_item(&CP.v[i])) emmet_item(&CP.v[i]);
  for (i = 0; i < CP.n; i++) {
    score[i] = is_emmet_item(&CP.v[i]) && CP.v[i].filter[0] == '\0' ? -1 :
               CP.v[i].kind == 1 && !CP.v[i].snippet && strcmp(CP.v[i].filter, pre) == 0
                   ? -1	/* a word that is already typed */
                   : comp_score(CP.v[i].filter, pre, n);
    if (score[i] >= 0) CP.vis[CP.nvis++] = i;
  }
  g_cv = CP.v;
  g_cs = score;
  qsort(CP.vis, CP.nvis, sizeof(size_t), cmp_comp);
  free(score);
  CP.open = CP.nvis > 0;
  CP.sel = CP.top = 0;
  comp_recent_pick(pre);	/* editor.suggestSelection */
}


void on_completion (Doc *d, CompItem *v, size_t n) {
  int forced = CP.forced;
  size_t cap = n + 1;
  comp_free();
  if (HAS_DOC && d == T->doc && !G->diff) {	/* the snippets too; the words when the server has nothing */
    int none = n == 0;
    add_snippets(&v, &n, &cap);
    if (none) add_words(&v, &n, &cap);
  }
  CP.v = v;
  CP.n = n;
  CP.vis = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  if (!HAS_DOC || d != T->doc || G->diff) {	/* too late: the editor moved on */
    comp_free();
    return;
  }
  CP.gen = lsp_comp_gen();
  comp_filter();
  comp_resolve_sel();
  if (!CP.open && forced) toast(0, "No suggestions.");
}


/* the item selected: its documentation and imports asked for, once (resolved: 2 asked, 1 answered) */
static void comp_resolve_sel (void) {
  CompItem *c;
  if (!CP.open || CP.sel >= CP.nvis || !HAS_DOC) return;
  c = &CP.v[CP.vis[CP.sel]];
  if (c->json == NULL || c->resolved) return;
  c->resolved = 2;
  lsp_comp_resolve(T->doc, CP.vis[CP.sel], c->json);
}


void on_comp_resolve (Doc *d, unsigned gen, size_t i, const char *detail, const char *doc, TextEdit *extra, size_t nextra) {
  CompItem *c;
  if (CP.v && gen == CP.gen && i < CP.n && HAS_DOC && d == T->doc) {
    c = &CP.v[i];
    c->resolved = 1;
    if (detail && *detail) {
      free(c->detail);
      c->detail = xstrdup(detail);
    }
    if (doc) {
      free(c->doc);
      c->doc = xstrdup(doc);
    }
    if (nextra && c->nextra == 0) {	/* its imports */
      c->extra = extra;
      c->nextra = nextra;
      return;
    }
  }
  while (nextra > 0) free(extra[--nextra].text);
  free(extra);
}


/* a snippet's stop with choices: the list of them, the first selected */
static void choice_open (const char *choices, Pos a) {
  const char *p = choices;
  size_t n = 0, cap = 0;
  comp_free();
  while (*p) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    char sort[16];
    CompItem *c;
    if (n + 1 >= cap) CP.v = (CompItem *)xrealloc(CP.v, (cap = cap ? cap * 2 : 8) * sizeof(CompItem));
    c = &CP.v[n];
    memset(c, 0, sizeof(*c));
    c->label = xstrndup(p, len);
    c->detail = xstrdup("");
    c->insert = xstrdup(c->label);
    c->filter = xstrdup(c->label);
    snprintf(sort, sizeof(sort), "%04lu", (unsigned long)n);
    c->sort = xstrdup(sort);
    c->kind = 12;	/* value */
    n++;
    p += len;
    if (*p) p++;
  }
  if (n == 0) return;
  CP.n = CP.nvis = n;
  CP.vis = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  for (cap = 0; cap < n; cap++) CP.vis[cap] = cap;
  CP.start = a;
  CP.choice = CP.open = 1;
}


/* the suggestion goes in, in place of the word typed so far */
static void snippet_insert (Pos a, Pos b, const char *body);

static void comp_accept (void) {
  CompItem *c = &CP.v[CP.vis[CP.sel]];
  Pos a = CP.start, b = T->cur;
  char *ins;
  TextEdit *extra;
  size_t nextra;
  if (T->cur.y == CP.start.y && T->cur.x >= CP.start.x)
    comp_recent_add(c->label, row_at(T->cur.y)->s + CP.start.x, T->cur.x - CP.start.x);
  if (c->json && c->resolved != 1 && c->nextra == 0) {	/* its imports may come with resolve: waited for, a little */
    unsigned gen = CP.gen;
    long long end = os_now_us() + 800000;
    if (c->resolved == 0) {
      c->resolved = 2;
      lsp_comp_resolve(T->doc, CP.vis[CP.sel], c->json);
    }
    while (CP.v && CP.gen == gen && CP.v[CP.vis[CP.sel]].resolved != 1 && os_now_us() < end)
      if (!lsp_poll()) os_wait_readable(-1, 10);
    if (CP.v == NULL || CP.gen != gen) return;	/* another answer came in the meantime */
    c = &CP.v[CP.vis[CP.sel]];
  }
  ins = xstrdup(c->insert);
  extra = c->extra;	/* additionalTextEdits: after the text goes in */
  nextra = c->nextra;
  c->extra = NULL;
  c->nextra = 0;
  if (c->has_range && c->a.y == T->cur.y && pos_cmp(c->a, T->cur) <= 0) {
    a = c->a;
    if (c->b.y == b.y && pos_cmp(c->b, b) > 0) b = c->b;
  }
  int snippet = c->snippet;
  if (snippet && !c->has_range) {	/* "#ifn" for "#ifndef": the '#' typed before the word goes too */
    const Row *r = row_at(a.y);
    size_t typed = b.x - a.x, k, ll = strlen(c->label);
    for (k = ll > typed ? ll - typed : 0; k > 0; k--)
      if (k <= a.x && memcmp(r->s + a.x - k, c->label, k) == 0 && m_strnicmp(c->label + k, r->s + a.x, typed) == 0) {
        a.x -= k;
        break;
      }
  }
  comp_free();
  if (snippet) snippet_insert(a, b, ins);
  else {
    doc_group(T->doc);
    T->sel = 0;
    ed_delete(a, b);
    T->cur = ed_insert(a, ins, strlen(ins));
    T->want = col_of(row_at(T->cur.y), T->cur.x);
    doc_group(T->doc);
  }
  free(ins);
  if (nextra) {	/* an import ...: the cursor stays with its text */
    on_format(T->doc, extra, nextra, 0);
    T->want = col_of(row_at(T->cur.y), T->cur.x);
  }
  while (nextra > 0) free(extra[--nextra].text);
  free(extra);
}


/* the stops numbered SN.order[SN.at] get the cursors, their text selected */
static void snip_select (void) {
  size_t i;
  int idx = SN.order[SN.at], first = 1;
  T->nmc = 0;
  T->sel = 0;
  for (i = 0; i < SN.n; i++) {
    Stop *st = &SN.v[i];
    if (st->idx != idx) continue;
    if (first) {
      T->anchor = st->a;
      T->cur = st->b;
      T->sel = pos_cmp(st->a, st->b) != 0;
      first = 0;
    }
    else {
      if (T->nmc == T->capmc) {
        T->capmc = T->capmc ? T->capmc * 2 : 8;
        T->mc = (Cur *)xrealloc(T->mc, (size_t)T->capmc * sizeof(Cur));
      }
      T->mc[T->nmc].anchor = st->a;
      T->mc[T->nmc].cur = st->b;
      T->mc[T->nmc].sel = pos_cmp(st->a, st->b) != 0;
      T->mc[T->nmc].want = col_of(row_at(st->b.y), st->b.x);
      T->nmc++;
    }
  }
  T->want = col_of(row_at(T->cur.y), T->cur.x);
  for (i = 0; i < SN.n; i++)	/* ${1|a,b|}: its choices to pick from */
    if (SN.v[i].idx == idx && SN.v[i].choices && T->nmc == 0) {
      choice_open(SN.v[i].choices, SN.v[i].a);
      break;
    }
  if (idx == 0) {	/* $0: the end; the snippet is done */
    T->nmc = 0;
    snip_end();
  }
}


/* Tab / Shift+Tab in a snippet */
static void snip_next (int d) {
  SN.at += d;
  if (SN.at < 0) SN.at = 0;
  if (SN.at >= SN.norder) {
    snip_end();
    return;
  }
  doc_group(T->doc);
  snip_select();
}


/* body's text in place of a..b, the cursor on its first tab stop */
static void snippet_insert (Pos a, Pos b, const char *body) {
  SnipCtx c;
  const Row *r = row_at(a.y);
  size_t ind = indent_end(r), n, i, len, ns;
  char *indent, tab[17], *sel = NULL, *line, *word, *clip, *text;
  SnipStop *st;
  Pos e, ws, we;
  int k;
  if (ind > a.x) ind = a.x;
  indent = (char *)xmalloc(ind + 1);
  memcpy(indent, r->s, ind);
  indent[ind] = '\0';
  n = T->doc->tabs ? 1 : (size_t)T->doc->indent;
  if (n > 16) n = 16;
  memset(tab, T->doc->tabs ? '\t' : ' ', n);
  tab[n] = '\0';
  if (pos_cmp(a, b) < 0) sel = doc_text(T->doc, a, b, &len);
  line = (char *)xmalloc(r->len + 1);
  memcpy(line, r->s, r->len);
  line[r->len] = '\0';
  ws = we = a;
  ws.x = word_start_at(a);
  we.x = word_end_at(a);
  word = doc_text(T->doc, ws, we, &len);
  clip = (char *)xmalloc(E.cliplen + 1);
  if (E.clip) memcpy(clip, E.clip, E.cliplen);
  clip[E.clip ? E.cliplen : 0] = '\0';
  memset(&c, 0, sizeof(c));
  c.indent = indent;
  c.tab = tab;
  c.path = T->doc->path;
  c.selected = sel;
  c.line = line;
  c.word = word;
  c.clipboard = clip;
  c.line_no = a.y;
  doc_comment(&c.line_comment, &c.block_open, &c.block_close);
  text = snip_expand(body, &c, &st, &ns, &len);
  free(indent);
  free(sel);
  free(line);
  free(word);
  free(clip);
  snip_end();
  doc_group(T->doc);
  T->sel = 0;
  T->nmc = 0;
  ed_delete(a, b);
  e = ed_insert(a, text, len);
  T->cur = e;
  T->want = col_of(row_at(e.y), e.x);
  if (ns > 0) {	/* the stops, and $0 at the end when it has none */
    int has0 = 0;
    SN.v = (Stop *)xmalloc((ns + 1) * sizeof(Stop));
    for (i = 0; i < ns; i++) {
      SN.v[i].idx = st[i].idx;
      SN.v[i].a = pos_after(a, text, st[i].a);
      SN.v[i].b = pos_after(a, text, st[i].b);
      SN.v[i].choices = st[i].choices;	/* SN has it now */
      st[i].choices = NULL;
      if (st[i].idx == 0) has0 = 1;
    }
    SN.n = ns;
    if (!has0) {
      SN.v[SN.n].idx = 0;
      SN.v[SN.n].choices = NULL;
      SN.v[SN.n].a = SN.v[SN.n].b = e;
      SN.n++;
    }
    for (k = 1; k <= 99 && SN.norder < 63; k++)	/* 1, 2 ... */
      for (i = 0; i < SN.n; i++)
        if (SN.v[i].idx == k) {
          SN.order[SN.norder++] = k;
          break;
        }
    SN.order[SN.norder++] = 0;
    SN.t = T;
    SN.a = a;
    SN.b = e;
    SN.at = 0;
    snip_select();
  }
  doc_group(T->doc);
  for (i = 0; i < ns; i++) free(st[i].choices);	/* those not taken (no stops kept) */
  free(st);
  free(text);
}


/* a key while the suggestions show: 1 when it was theirs */
static int comp_key (int k) {
  int code = KEY_CODE(k);
  switch (code) {
    case K_UP: CP.sel = CP.sel > 0 ? CP.sel - 1 : CP.nvis - 1; comp_resolve_sel(); return 1;
    case K_DOWN: CP.sel = CP.sel + 1 < CP.nvis ? CP.sel + 1 : 0; comp_resolve_sel(); return 1;
    case K_PGUP: CP.sel = CP.sel > 10 ? CP.sel - 10 : 0; comp_resolve_sel(); return 1;
    case K_PGDN: CP.sel = CP.sel + 10 < CP.nvis ? CP.sel + 10 : CP.nvis - 1; comp_resolve_sel(); return 1;
    case K_ENTER: case K_TAB:
      if (k & (KM_SHIFT | KM_CTRL)) return 0;
      if (code == K_ENTER && eopt.accept_enter != 1 && !CP.choice) {	/* editor.acceptSuggestionOnEnter */
        const CompItem *c = &CP.v[CP.vis[CP.sel]];
        const Row *r = row_at(T->cur.y);
        size_t n = T->cur.x > CP.start.x && T->cur.y == CP.start.y ? T->cur.x - CP.start.x : 0;
        if (eopt.accept_enter == 0 || (strlen(c->insert) == n && memcmp(c->insert, r->s + CP.start.x, n) == 0)) {
          comp_free();	/* "off", or "smart" and nothing would change: a new line */
          return 0;
        }
      }
      comp_accept();
      return 1;
    case K_ESC: comp_free(); return 1;
  }
  return 0;
}


/* after a key: the list follows the typing; a word's first letter or a '.' asks */
static void comp_after_key (int k) {
  int code = KEY_CODE(k);
  const Row *r;
  if (!HAS_DOC || G->diff) return;
  r = row_at(T->cur.y);
  if (CP.choice) {	/* typing in the stop: its own text, the list goes */
    comp_free();
    return;
  }
  if (CP.open || CP.n) {
    if ((IS_TEXT(k) && (code == '_' || code >= 0x80 || (code >= '0' && code <= '9') ||
                        (lower(code) >= 'a' && lower(code) <= 'z'))) || code == K_BS) {
      comp_filter();
      comp_resolve_sel();
      if (CP.open || code == K_BS) return;
    }
    else if (code != K_UP && code != K_DOWN) comp_free();
  }
  if (SG.label && (code == ')' || code == K_ESC || T->cur.y != SG.y)) sig_close();
  if (!IS_TEXT(k) || T->nmc) return;
  if (!lsp_active(T->doc)) {	/* the snippets and the words, from the first letter */
    if (T->cur.x > 0 && char_class(r, T->cur.x - 1) == 1 && T->cur.x - word_start().x == 1 && qs_allowed()) comp_local(0);
    return;
  }
  if (code == '(' || code == ',' || (SG.label && IS_TEXT(k))) lsp_signature(T->doc, T->cur);
  if (code == '.' || (code == '>' && T->cur.x >= 2 && r->s[T->cur.x - 2] == '-') ||
      (code == ':' && T->cur.x >= 2 && r->s[T->cur.x - 2] == ':')) {
    if (eopt.trigger_chars) comp_ask(0);	/* editor.suggestOnTriggerCharacters */
  }
  else if (char_class(r, T->cur.x - 1) == 1 && T->cur.x - word_start().x == 1 && qs_allowed())
    comp_ask(0);	/* the first letter of a word, as VS Code's quick suggestions */
}


static uint32_t kind_icon (int kind, int *st) {
  switch (kind) {
    case 2: case 3: case 4: *st = S_ICON_PURPLE; return 0xEA8C;	/* method, function */
    case 5: case 10: *st = S_ICON_BLUE; return 0xEB5F;	/* field, property */
    case 6: *st = S_ICON_BLUE; return 0xEA88;	/* variable */
    case 7: *st = S_ICON_ORANGE; return 0xEB5B;	/* class */
    case 8: *st = S_ICON_BLUE; return 0xEB61;	/* interface */
    case 9: *st = S_ICON_GREY; return 0xEA8B;	/* module */
    case 13: *st = S_ICON_ORANGE; return 0xEA95;	/* enum */
    case 14: *st = S_ICON_GREY; return 0xEB62;	/* keyword */
    case 15: *st = S_ICON_GREY; return 0xEB66;	/* snippet */
    case 20: *st = S_ICON_BLUE; return 0xEB5E;	/* enum member */
    case 21: *st = S_ICON_BLUE; return 0xEB5D;	/* constant */
    case 22: *st = S_ICON_ORANGE; return 0xEA91;	/* struct */
    case 25: *st = S_ICON_ORANGE; return 0xEA92;	/* type parameter */
  }
  *st = S_ICON_GREY;
  return 0xEA93;	/* text */
}


/* s's lines wrapped at w columns (by words) into v; the code fences go */
static void wrap_text (const char *s, int w, Vec *v) {
  while (*s) {
    const char *e = strchr(s, '\n'), *p;
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len >= 3 && strncmp(s, "```", 3) == 0) {
      s += len + (e ? 1 : 0);
      continue;
    }
    p = s;
    while (p < s + len || len == 0) {
      const char *q = p, *cut = NULL;
      int cols = 0;
      while (q < s + len && cols < w) {
        if (*q == ' ') cut = q;
        if (((unsigned char)*q & 0xC0) != 0x80) cols++;
        q++;
      }
      if (q < s + len && cut && cut > p) q = cut + 1;	/* at a space */
      vec_push(v, xstrndup(p, (size_t)(q - p)));
      p = q;
      if (len == 0) break;
    }
    s += len + (e ? 1 : 0);
  }
}


/* VS Code's details beside the suggestions: the item's detail, then its documentation */
static void draw_comp_details (const CompItem *c, int lx, int ly, int lw, int lh) {
  Buf b;
  Vec lines;
  int pw = 50, x, y, h, i, nd = 0;
  size_t k;
  if ((c->doc == NULL || !*c->doc) && (!c->detail || !*c->detail)) return;
  if (lx + lw + pw + 1 <= L.ed_x + L.ed_w) x = lx + lw;
  else if (lx - pw >= L.ed_x) x = lx - pw;
  else return;	/* no room beside it */
  vec_init(&lines);
  if (c->detail && *c->detail) {
    wrap_text(c->detail, pw - 2, &lines);
    nd = (int)lines.n;
  }
  if (c->doc && *c->doc) {
    buf_init(&b);
    plain_md(c->doc, strlen(c->doc), &b);
    buf_putc(&b, '\0');
    if (nd) vec_push(&lines, xstrdup(""));
    wrap_text(b.s, pw - 2, &lines);
    buf_free(&b);
  }
  while (lines.n > 0 && lines.v[lines.n - 1][0] == '\0') free(lines.v[--lines.n]);
  h = (int)lines.n;
  if (h > 16) h = 16;
  if (h < lh && h < (int)lines.n) h = lh;
  y = ly;
  if (y + h > L.text_y + L.text_h) y = L.text_y + L.text_h - h;
  if (y < L.text_y) y = L.text_y;
  for (i = 0; i < h; i++) {
    k = (size_t)i;
    scr_fill(x, y + i, pw, S_BOX);
    if (k < lines.n) scr_putsw(x + 1, y + i, pw - 2, lines.v[k], i < nd ? S_BOX_DIM : S_BOX);
  }
  vec_free(&lines);
}


/* the suggestions, under the word (above it when there is no room) */
static void draw_comp (int gw) {
  int w = 30, h, x, y, cy, i;
  size_t k;
  (void)gw;	/* screen_at knows the gutter */
  if (!CP.open || T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) return;
  if (CP.start.y != T->cur.y || CP.start.x > T->cur.x) {	/* the text moved under the list (undo, a click ...) */
    comp_free();
    return;
  }
  h = CP.nvis < 10 ? (int)CP.nvis : 10;
  for (k = 0; k < CP.nvis && k < 200; k++) {
    const CompItem *c = &CP.v[CP.vis[k]];
    int need = (int)str_cols(c->label) + (int)str_cols(c->detail) + 6;
    if (need > w) w = need;
  }
  if (w > 64) w = 64;
  if (w > L.ed_w - 2) w = L.ed_w - 2;
  if (w < 8) return;	/* no room for it */
  if (!screen_at(CP.start, &x, &cy)) return;
  x -= 2;
  if (x + w > L.ed_x + L.ed_w) x = L.ed_x + L.ed_w - w;
  if (x < L.ed_x) x = L.ed_x;
  y = cy + 1;
  if (y + h > L.text_y + L.text_h && cy - h >= L.text_y) y = cy - h;
  if (CP.sel < CP.top) CP.top = CP.sel;
  if (CP.sel >= CP.top + (size_t)h) CP.top = CP.sel - (size_t)h + 1;
  for (i = 0; i < h; i++) {
    size_t v = CP.top + (size_t)i;
    const CompItem *c;
    int on, st, ist, lx;
    uint32_t icon;
    if (v >= CP.nvis) break;
    c = &CP.v[CP.vis[v]];
    on = (v == CP.sel);
    st = on ? S_BOX_SEL : S_BOX;
    scr_fill(x, y + i, w, st);
    icon = kind_icon(c->kind, &ist);
    scr_put(x + 1, y + i, icon, on ? st : ist);
    lx = x + 3 + scr_putsw(x + 3, y + i, w - 4, c->label, st);
    {	/* the letters typed, in blue */
      size_t typed = T->cur.x - CP.start.x;
      if (typed > 0 && m_strnicmp(c->label, row_at(T->cur.y)->s + CP.start.x, typed) == 0) {
        char *pre = xstrndup(c->label, typed);
        scr_putsw(x + 3, y + i, w - 4, pre, on ? S_BOX_HIT_SEL : S_BOX_HIT);
        free(pre);
      }
    }
    if (c->detail[0] && lx + 3 < x + w) {
      int dw = (int)str_cols(c->detail), room = x + w - lx - 2;
      if (dw > room) dw = room;
      scr_putsw(x + w - 1 - dw, y + i, dw, c->detail, on ? S_MENU_KEY_SEL : S_BOX_DIM);
    }
  }
  if (g_comp_details && CP.sel < CP.nvis) draw_comp_details(&CP.v[CP.vis[CP.sel]], x, y, w, h);
}


/* F12: where the name under the cursor is made */
void on_definition (const char *path, Pos p, int utf16) {
  const Row *r;
  if (open_file(path, 0) != 0) return;
  if (utf16 && p.y < T->doc->n) {	/* the server counted UTF-16 units */
    size_t i = 0, u = 0, len;
    r = row_at(p.y);
    while (i < r->len && u < p.x) {
      u += utf8_decode(r->s + i, r->len - i, &len) >= 0x10000 ? 2 : 1;
      i += len;
    }
    p.x = i;
  }
  move_h(doc_clamp(T->doc, p), 0);
  center_cursor();
  E.focus = F_EDITOR;
}


/* an extension's page (eext.c wrote it): in a preview tab, the view keeps the keys */
void on_ext_page (const char *path) {
  open_file(path, 1);
}


/* F8 / Shift+F8: to the next / previous problem, and what it says */
static void next_problem (int back) {
  size_t n, i, best = (size_t)-1, wrap = (size_t)-1;
  const Diag *v;
  if (!HAS_DOC) return;
  v = lsp_diags(T->doc, &n);
  for (i = 0; i < n; i++) {
    int c = pos_cmp(v[i].a, T->cur);
    if (back ? c < 0 : c > 0) {
      if (best == (size_t)-1 || (back ? pos_cmp(v[i].a, v[best].a) > 0 : pos_cmp(v[i].a, v[best].a) < 0))
        best = i;
    }
    if (wrap == (size_t)-1 || (back ? pos_cmp(v[i].a, v[wrap].a) > 0 : pos_cmp(v[i].a, v[wrap].a) < 0))
      wrap = i;
  }
  if (best == (size_t)-1) best = wrap;
  if (best == (size_t)-1) {
    toast(0, "No problems have been detected in this file.");
    return;
  }
  move_h(doc_clamp(T->doc, v[best].a), 0);
  if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
  toast(v[best].sev == 1, "%s", v[best].msg);
}

/* }================================================================== */


/*
** {==================================================================
** IntelliSense: hover, signatures, rename, code actions, problems
** ===================================================================
*/

void on_hover (const char *md) {
  char *dg = HV.diag;	/* the problems there come first, as in VS Code */
  HV.diag = NULL;
  hover_close();
  if (dg && md && *md) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "%s\n---\n%s", dg, md);
    buf_putc(&b, '\0');
    HV.text = buf_take(&b);
    free(dg);
  }
  else if (dg) HV.text = dg;
  else if (md && *md) HV.text = xstrdup(md);
  else if (!HV.from_mouse) toast(0, "No hover information.");
}


static void sig_close (void) {
  free(SG.label);
  SG.label = NULL;
}


void on_signature (const char *label, size_t a0, size_t a1) {
  sig_close();
  if (label == NULL || !HAS_DOC) return;
  SG.label = xstrdup(label);
  SG.a0 = a0;
  SG.a1 = a1;
  SG.y = T->cur.y;
}


/* the text position under screen cell x, y of the group in front; 0: not text */
static int pos_at_screen (int x, int y, Pos *p) {
  int gw;
  long col;
  if (!HAS_DOC || G->diff || y < L.text_y || y >= L.text_y + L.text_h) return 0;
  gw = gutter_width();
  col = x - L.ed_x - gw;
  if (col < 0 || col >= text_cols()) return 0;
  {
    size_t from, to, left;
    if (!vis_goto(y - L.text_y, &p->y, &from, &to, &left)) return 0;
    p->x = x_of_vcol(p->y, left + (size_t)col);
    if (p->x >= to && to < row_at(p->y)->len) return 0;
  }
  return p->x < row_at(p->y)->len && char_class(row_at(p->y), p->x) == 1;
}


/* the mouse has rested a while on a name: ask the server about it */
static char *hover_expr (Pos p);

static void hover_idle (void) {
  Pos p;
  char *dg;
  if (!HV.waiting || os_now_us() - HV.mt < 500000) return;
  HV.waiting = 0;
  if (E.focus == F_PANEL && L.panel_h > 0 && HV.my >= L.panel_y) return;
  if (!pos_at_screen(HV.mx, HV.my, &p)) {	/* not a name: a squiggle's problem still shows */
    Pos q;
    int gw = gutter_width();
    size_t from, to, left;
    long col = HV.mx - L.ed_x - gw;
    if (!HAS_DOC || G->diff || T->page || HV.my < L.text_y || HV.my >= L.text_y + L.text_h || col < 0 ||
        col >= text_cols() || !vis_goto(HV.my - L.text_y, &q.y, &from, &to, &left))
      return;
    q.x = x_of_vcol(q.y, left + (size_t)col);
    if (q.x >= row_at(q.y)->len || (HV.text && HV.at.y == q.y && HV.at.x == q.x)) return;
    if ((dg = diag_text(q)) == NULL) return;
    hover_close();
    HV.at = q;
    HV.from_mouse = 1;
    HV.text = dg;
    return;
  }
  if (!lsp_active(T->doc) && !dbg_stopped()) {
    if ((dg = diag_text(p)) == NULL) return;
    hover_close();
    HV.at = p;
    HV.from_mouse = 1;
    HV.text = dg;
    return;
  }
  if (HV.text && HV.from_mouse && p.y == HV.at.y && p.x >= word_start_at(HV.at) && p.x <= word_end_at(HV.at)) return;
  HV.at = p;
  HV.from_mouse = 1;
  free(HV.diag);
  HV.diag = diag_text(p);
  if (dbg_stopped()) {	/* paused: the value of what is under the mouse */
    char *e = hover_expr(p);
    if (e) dbg_hover(e);
    free(e);
    return;
  }
  lsp_hover(T->doc, p);
}


/* markdown, simply: no ``, **, \ escapes */
static void plain_md (const char *s, size_t n, Buf *o) {
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!<>", s[i + 1])) {
      buf_putc(o, s[++i]);
      continue;
    }
    if (s[i] == '`') continue;
    if (s[i] == '*' && i + 1 < n && s[i + 1] == '*') {
      i++;
      continue;
    }
    buf_putc(o, s[i]);
  }
}


/* the hover box: text, and its code in the file's colors */
static void draw_hover (int gw) {
  const char *p;
  int w = 20, h = 0, x, y, ay, i, maxw, in_code = 0, state = 0;
  char *lines[64];
  int code[64], n = 0;
  (void)gw;	/* screen_at knows the gutter */
  HV.qf_y = -1;
  if (HV.text == NULL || HV.at.y < T->top || HV.at.y >= T->top + (size_t)L.text_h) return;
  maxw = L.ed_w - 4 < 80 ? L.ed_w - 4 : 80;
  for (p = HV.text; *p && n < 64;) {	/* the lines to show */
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    Buf b;
    if (len >= 3 && strncmp(p, "```", 3) == 0) in_code = !in_code;
    else if (len == 3 && strncmp(p, "---", 3) == 0) {
      lines[n] = NULL;	/* a line between parts */
      code[n++] = 0;
    }
    else if (len > 0 || (n > 0 && lines[n - 1] && lines[n - 1][0])) {
      buf_init(&b);
      if (in_code) buf_putn(&b, p, len);
      else plain_md(p, len, &b);
      buf_putc(&b, '\0');
      lines[n] = buf_take(&b);
      code[n] = in_code;
      if ((int)str_cols(lines[n]) + 2 > w) w = (int)str_cols(lines[n]) + 2;
      n++;
    }
    p += len + (e ? 1 : 0);
  }
  while (n > 0 && lines[n - 1] && lines[n - 1][0] == '\0') free(lines[--n]);
  if (w > maxw) w = maxw;
  h = n < 14 ? n : 14;
  if (h == 0) return;
  if (!screen_at(HV.at, &x, &ay)) {
    for (i = 0; i < n; i++) free(lines[i]);
    return;
  }
  if (x + w > L.ed_x + L.ed_w) x = L.ed_x + L.ed_w - w;
  if (x < L.ed_x) x = L.ed_x;
  y = (ay - h >= L.text_y) ? ay - h : ay + 1;	/* above the name, else under it */
  for (i = 0; i < h; i++) {
    scr_fill(x, y + i, w, S_BOX);
    if (lines[i] == NULL) {
      int k;
      for (k = 1; k < w - 1; k++) scr_put(x + k, y + i, 0x2500, S_MENU_LINE);
    }
    else if (code[i]) {	/* code: the file's colors on the box */
      size_t len = strlen(lines[i]);
      unsigned char *tok = (unsigned char *)xmalloc(len + 1);
      state = syntax_scan(T->sx, lines[i], len, state, tok);
      scr_code(x + 1, y + i, w - 2, lines[i], len, 0, tok, B_EDITOR, 0, 0, B_EDITOR);
      scr_restyle(x, y + i, 1, S_BOX);
      free(tok);
    }
    else if (lines[i][0] == '\x01') {	/* Quick Fix...: a link, clicked in on_mouse */
      HV.qf_y = y + i;
      HV.qf_x0 = x + 1;
      HV.qf_x1 = x + 1 + scr_putsw(x + 1, y + i, w - 2, lines[i] + 1, S_BOX_HIT);
    }
    else scr_putsw(x + 1, y + i, w - 2, lines[i], S_BOX);
  }
  for (i = 0; i < n; i++) free(lines[i]);
}


/* the signature over the line, the parameter the cursor is in in blue */
static void draw_signature (int gw) {
  int w, x, y, cy;
  (void)gw;	/* screen_at knows the gutter */
  if (SG.label == NULL || SG.y != T->cur.y || T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) return;
  w = (int)str_cols(SG.label) + 2;
  if (w > L.ed_w - 2) w = L.ed_w - 2;
  if (!screen_at(T->cur, &x, &cy)) return;
  y = cy - 1 >= L.text_y ? cy - 1 : cy + 1;
  x -= 2;
  if (x + w > L.ed_x + L.ed_w) x = L.ed_x + L.ed_w - w;
  if (x < L.ed_x) x = L.ed_x;
  scr_fill(x, y, w, S_BOX);
  scr_putsw(x + 1, y, w - 2, SG.label, S_BOX);
  if (SG.a1 > SG.a0 && SG.a1 <= strlen(SG.label)) {
    char *before = xstrndup(SG.label, SG.a0), *par = xstrndup(SG.label + SG.a0, SG.a1 - SG.a0);
    int bx = x + 1 + (int)str_cols(before);
    if (bx < x + w - 1) scr_putsw(bx, y, x + w - 1 - bx, par, S_BOX_HIT);
    free(before);
    free(par);
  }
}


/* F2: the name under the cursor, everywhere the server knows it */
static void rename_symbol (void) {
  Pos a, b;
  char *old, *name;
  if (!HAS_DOC || G->diff) return;
  if (!lsp_active(T->doc)) {
    toast(0, "No result.");
    return;
  }
  a = T->cur;
  a.x = word_start_at(T->cur);
  b = T->cur;
  b.x = word_end_at(T->cur);
  if (a.x == b.x) {
    toast(1, "The element can't be renamed.");
    return;
  }
  old = doc_text(T->doc, a, b, NULL);
  name = ask_text("Rename: Enter to rename, Escape to cancel", old);
  if (name && *name && strcmp(name, old) != 0) lsp_rename(T->doc, T->cur, name);
  free(name);
  free(old);
}


/* the tab that shows path, in any group; NULL when none does */
static Tab *tab_of (const char *path, Group **in) {
  char *real = os_realpath(path);
  int g, i;
  Tab *found = NULL;
  for (g = 0; g < g_ngrp && !found; g++)
    for (i = 0; i < g_grp[g].ntab && !found; i++) {
      Tab *t = g_grp[g].tab[i];
      if (t->real && real && m_fncmp(t->real, real) == 0) {
        found = t;
        if (in) *in = &g_grp[g];
      }
    }
  free(real);
  return found;
}


static size_t units_to_x (const Row *r, size_t c, int utf16) {
  size_t i = 0, u = 0, len;
  if (!utf16) return c < r->len ? c : r->len;
  while (i < r->len && u < c) {
    u += utf8_decode(r->s + i, r->len - i, &len) >= 0x10000 ? 2 : 1;
    i += len;
  }
  return i;
}


static const TextEdit *g_ev;

static int cmp_edit (const void *a, const void *b) {	/* the last in the text first */
  const TextEdit *x = &g_ev[*(const size_t *)a], *y = &g_ev[*(const size_t *)b];
  if (x->l0 != y->l0) return x->l0 < y->l0 ? 1 : -1;
  if (x->c0 != y->c0) return x->c0 < y->c0 ? 1 : -1;
  return 0;
}


/* the server's changes: every file's, the files not open are opened */
void on_edit (const TextEdit *v, size_t n) {
  Group *g0 = G;
  Tab *t0 = T;
  size_t i, j, *ord, edits = 0;
  int files = 0, k;
  char *done = (char *)calloc(n + 1, 1);
  if (n == 0 || done == NULL) {
    free(done);
    return;
  }
  ord = (size_t *)xmalloc(n * sizeof(size_t));
  for (i = 0; i < n; i++) {
    size_t m = 0;
    Tab *t;
    if (done[i]) continue;
    for (j = i; j < n; j++)	/* the edits of this file */
      if (!done[j] && m_fncmp(v[j].path, v[i].path) == 0) {
        ord[m++] = j;
        done[j] = 1;
      }
    t = tab_of(v[i].path, NULL);
    if (t == NULL) {	/* not open: it opens, and stays open with the changes */
      if (open_file(v[i].path, 0) != 0) continue;
      t = T;
    }
    g_ev = v;
    qsort(ord, m, sizeof(size_t), cmp_edit);
    T = t;
    doc_group(T->doc);
    for (j = 0; j < m; j++) {
      const TextEdit *e = &v[ord[j]];
      Pos a, b;
      if (e->l0 >= T->doc->n) continue;
      a.y = e->l0;
      a.x = units_to_x(row_at(e->l0), e->c0, e->utf16);
      b.y = e->l1 < T->doc->n ? e->l1 : T->doc->n - 1;
      b.x = units_to_x(row_at(b.y), e->l1 < T->doc->n ? e->c1 : row_at(b.y)->len, e->utf16);
      if (pos_cmp(b, a) < 0) b = a;
      ed_delete(a, b);
      ed_insert(a, e->text, strlen(e->text));
      edits++;
    }
    doc_group(T->doc);
    files++;
  }
  G = g0;	/* the tab in front stays in front */
  T = t0;
  for (k = 0; k < G->ntab; k++)
    if (G->tab[k] == t0) G->active = k;
  free(ord);
  free(done);
  if (files > 1 || edits > 1) toast(0, "Made %lu edits in %d files", (unsigned long)edits, files);
}


/* the tab of path, found cheaply: the name first, the real path only then */
static Tab *open_tab_of (const char *path, Group **in) {
  const char *base = path_basename(path);
  int g, i;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      const Tab *t = g_grp[g].tab[i];
      if (t->doc->path && m_fncmp(path_basename(t->doc->path), base) == 0) return tab_of(path, in);
    }
  return NULL;
}


/* Search: the text of path when the editor has it open (its edits too), else NULL */
char *open_doc_text (const char *path, size_t *len) {
  Tab *t = open_tab_of(path, NULL);
  Pos a;
  if (t == NULL) return NULL;
  a.y = a.x = 0;
  return doc_text(t->doc, a, doc_end(t->doc), len);
}


/* Search's Replace: the edits made in the open file's text, one step of undo; 0: not open */
int open_doc_edit (const char *path, const TextEdit *v, size_t n) {
  Group *g0 = G, *in = NULL;
  Tab *t0 = T, *t = open_tab_of(path, &in);
  size_t i, *ord;
  if (t == NULL) return 0;
  ord = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  for (i = 0; i < n; i++) ord[i] = i;
  g_ev = v;
  qsort(ord, n, sizeof(size_t), cmp_edit);
  G = in;
  T = t;
  doc_group(T->doc);
  for (i = 0; i < n; i++) {
    const TextEdit *e = &v[ord[i]];
    Pos a, b;
    if (e->l0 >= T->doc->n) continue;
    a.y = b.y = e->l0;
    a.x = units_to_x(row_at(e->l0), e->c0, 0);
    b.x = units_to_x(row_at(e->l0), e->c1, 0);
    ed_delete(a, b);
    ed_insert(a, e->text, strlen(e->text));
  }
  doc_group(T->doc);
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
  T->preview = 0;
  G = g0;
  T = t0;
  free(ord);
  return 1;
}


/* the code actions came: shown by the main loop, not in the middle of reading */
static struct {
  char **title;
  size_t n;
  int ready;
} QF;


void on_actions (const char *const *titles, size_t n) {
  size_t i;
  for (i = 0; i < QF.n; i++) free(QF.title[i]);
  free(QF.title);
  QF.title = n ? (char **)xmalloc(n * sizeof(char *)) : NULL;
  for (i = 0; i < n; i++) QF.title[i] = xstrdup(titles[i]);
  QF.n = n;
  QF.ready = 1;
}


static void quickfix (void) {
  Pos a, b;
  if (!HAS_DOC || G->diff) return;
  if (!lsp_active(T->doc)) {
    toast(0, "No code actions available");
    return;
  }
  sel_range(&a, &b);
  if (!T->sel) a = b = T->cur;
  lsp_actions(T->doc, a, b);
}


static void quickfix_idle (void) {
  Pick p;
  size_t i;
  int r;
  if (!QF.ready) return;
  QF.ready = 0;
  if (QF.n == 0) {
    toast(0, "No code actions available");
    return;
  }
  pick_init(&p, "Quick Fix");
  for (i = 0; i < QF.n; i++) pick_add(&p, QF.title[i], NULL, 0xEA61);	/* codicon lightbulb */
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) lsp_action_run((size_t)r);
}


/* ---- the Problems panel: every file's problems, the file first; filtered like VS Code's */

typedef struct PRow {
  size_t file;	/* lsp_diag_file(file) */
  long diag;	/* -1: the file's own row */
  int count;	/* the file's row: its problems shown */
} PRow;

static struct {
  PRow *row;
  size_t n, cap, sel, top;
  int h;
  char filter[160];	/* terms by commas: text, a glob of the path, !glob */
  int in_filter;	/* the keys go to the filter box */
  int hide_err, hide_warn, hide_info;	/* the funnel's Show Errors / Warnings / Infos off */
  int active_only;	/* Show Active File Only */
  Vec closed;	/* the files collapsed */
  int shown, total;	/* the problems shown, of all */
  int box_x0, box_x1, funnel_x, coll_x;	/* the title row's filter box and icons */
} PB;


static int pb_filtered (void) {
  return PB.filter[0] || PB.hide_err || PB.hide_warn || PB.hide_info || PB.active_only;
}


/* a glob: ** anything, * anything but '/', ? one character; letters' case not minded */
static int glob_match (const char *p, const char *s) {
  if (*p == '\0') return *s == '\0';
  if (p[0] == '*' && p[1] == '*') {
    p += 2;
    if (*p == '/') p++;	/* two stars and a slash: at any depth, the top too */
    for (;; s++) {
      if (glob_match(p, s)) return 1;
      if (*s == '\0') return 0;
    }
  }
  if (*p == '*') {
    for (p++;; s++) {
      if (glob_match(p, s)) return 1;
      if (*s == '\0' || *s == '/') return 0;
    }
  }
  if (*s == '\0') return 0;
  if (*p == '?' || lower((unsigned char)*p) == lower((unsigned char)*s)) return glob_match(p + 1, s + 1);
  return 0;
}


static int has_text (const char *s, const char *t) {
  size_t n = strlen(t);
  for (; *s; s++)
    if (m_strnicmp(s, t, n) == 0) return 1;
  return n == 0;
}


/*
** Does the filter let a problem (msg; NULL: only the file is asked) of
** path through? Its terms, by commas: text in the message or the file's
** name, a glob of the file's path from the folder, !glob: not those.
*/
static int pb_pass (const char *path, const char *msg) {
  char buf[160], rel[1024], *t, *c;
  const char *root = side_root();
  size_t rn = strlen(root);
  int globs = 0, glob_ok = 0;
  if (PB.active_only && !(HAS_DOC && T->real && m_fncmp(T->real, path) == 0)) return 0;
  if (PB.filter[0] == '\0') return 1;
  snprintf(rel, sizeof(rel), "%s", m_strnicmp(path, root, rn) == 0 && path_is_sep(path[rn]) ? path + rn + 1 : path);
  for (c = rel; *c; c++)
    if (*c == '\\') *c = '/';
  snprintf(buf, sizeof(buf), "%s", PB.filter);
  for (t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
    char *e;
    while (*t == ' ') t++;
    for (e = t + strlen(t); e > t && e[-1] == ' '; e--) ;
    *e = '\0';
    if (*t == '\0') continue;
    if (*t == '!') {	/* not these files */
      if (glob_match(t + 1, rel)) return 0;
    }
    else if (strchr(t, '*') || strchr(t, '/') || strchr(t, '?')) {
      globs++;
      if (glob_match(t, rel)) glob_ok = 1;
    }
    else if (msg && !has_text(msg, t) && !has_text(path_basename(path), t)) return 0;
  }
  return globs == 0 || glob_ok;
}


static int pb_sev_shown (int sev) {
  return sev == 1 ? !PB.hide_err : sev == 2 ? !PB.hide_warn : !PB.hide_info;
}


static int pb_closed (const char *path) {
  size_t i;
  for (i = 0; i < PB.closed.n; i++)
    if (m_fncmp(PB.closed.v[i], path) == 0) return 1;
  return 0;
}


static void pb_close (const char *path, int close) {
  size_t i;
  for (i = 0; i < PB.closed.n; i++)
    if (m_fncmp(PB.closed.v[i], path) == 0) {
      if (!close) {
        free(PB.closed.v[i]);
        PB.closed.v[i] = PB.closed.v[--PB.closed.n];
      }
      return;
    }
  if (close) vec_push(&PB.closed, xstrdup(path));
}


static void problems_rows (void) {
  size_t f, nf = lsp_diag_files();
  PB.n = 0;
  PB.shown = PB.total = 0;
  for (f = 0; f < nf; f++) {
    char *path;
    size_t n, k;
    int vis = 0, closed;
    const Diag *dv = lsp_diag_file(f, &path, &n);
    PB.total += (int)n;
    if (n == 0 || !pb_pass(path, NULL)) {
      free(path);
      continue;
    }
    for (k = 0; k < n; k++) vis += pb_sev_shown(dv[k].sev) && pb_pass(path, dv[k].msg);
    closed = pb_closed(path);
    free(path);
    if (vis == 0) continue;
    PB.shown += vis;
    if (PB.n + n + 1 > PB.cap) {
      PB.cap = (PB.n + n + 1) * 2;
      PB.row = (PRow *)xrealloc(PB.row, PB.cap * sizeof(PRow));
    }
    PB.row[PB.n].file = f;
    PB.row[PB.n].count = vis;
    PB.row[PB.n++].diag = -1;
    if (closed) continue;
    lsp_diag_file(f, &path, &n);
    for (k = 0; k < n; k++)
      if (pb_sev_shown(dv[k].sev) && pb_pass(path, dv[k].msg)) {
        PB.row[PB.n].file = f;
        PB.row[PB.n].count = 0;
        PB.row[PB.n++].diag = (long)k;
      }
    free(path);
  }
  if (PB.sel >= PB.n) PB.sel = PB.n ? PB.n - 1 : 0;
}


static void draw_problems (int x, int y, int w, int h, int focus) {
  int i;
  scr_box(x, y, w, h, S_PANEL);
  problems_rows();
  PB.h = h;
  if (PB.n == 0) {
    scr_puts(x + 2, y, PB.total && pb_filtered() ? "No results found with provided filter criteria."
                                                   : "No problems have been detected in the workspace.", S_PANEL_TAB);
    return;
  }
  if (PB.sel < PB.top) PB.top = PB.sel;
  if (PB.sel >= PB.top + (size_t)h) PB.top = PB.sel - (size_t)h + 1;
  for (i = 0; i < h; i++) {
    size_t k = PB.top + (size_t)i, n;
    const PRow *pr;
    char *path, line[512];
    const Diag *dv;
    int st, sy = y + i, cx;
    if (k >= PB.n) break;
    pr = &PB.row[k];
    dv = lsp_diag_file(pr->file, &path, &n);
    st = (k == PB.sel && focus && !PB.in_filter) ? S_SIDE_SEL : (k == PB.sel ? S_SIDE_CUR : S_PANEL);
    scr_fill(x, sy, w, st);
    if (pr->diag < 0) {	/* the file: chevron, icon, name, folder, count */
      int ist;
      uint32_t icon = file_icon(path_basename(path), &ist);
      cx = x + 1;
      cx += scr_put(cx, sy, pb_closed(path) ? 0xEAB6 : 0xEAB4, st) + 1;
      cx += scr_put(cx, sy, icon, st) + 1;
      cx += scr_puts(cx, sy, path_basename(path), st) + 1;
      snprintf(line, sizeof(line), " %d ", pr->count);
      cx += scr_putsw(cx, sy, x + w - cx - 8, path, st == S_PANEL ? S_PANEL_TAB : st) + 1;
      scr_puts(cx, sy, line, st == S_PANEL ? S_TOGGLE_ON : st);
    }
    else {
      const Diag *d = &dv[pr->diag];
      uint32_t icon = d->sev == 1 ? 0xEA87 : d->sev == 2 ? 0xEA6C : 0xEA74;
      uint32_t col = ui_color(d->sev == 1 ? C_ERROR : d->sev == 2 ? C_WARNING : C_INFO);
      char *msg = xstrdup(d->msg), *c;
      for (c = msg; *c; c++)
        if (*c == '\n' || *c == '\r' || *c == '\t') *c = ' ';
      cx = x + 5;
      if (st == S_PANEL) scr_put_rgb(cx, sy, icon, col, ui_color(C_EDITOR_BG), 0);
      else scr_put(cx, sy, icon, st);
      cx += 2;
      snprintf(line, sizeof(line), "  [Ln %lu, Col %lu]", (unsigned long)(d->a.y + 1), (unsigned long)(d->a.x + 1));
      cx += scr_putsw(cx, sy, x + w - cx - (int)strlen(line) - 1, msg, st);
      scr_putsw(cx, sy, x + w - cx - 1, line, st == S_PANEL ? S_PANEL_TAB : st);
      free(msg);
    }
    free(path);
  }
}


/* the Problems' title row: the filter box, the funnel (Show Errors ...), Collapse All */
static void problems_title (int y, int x1) {
  int bw, filtered = pb_filtered();
  PB.coll_x = x1 - 7;
  PB.funnel_x = x1 - 9;
  scr_put(PB.coll_x, y, 0xEAC5, S_PANEL_TAB);	/* collapse-all */
  scr_put(PB.funnel_x, y, filtered ? 0xEBCE : 0xEAF1, filtered ? S_TOGGLE_ON : S_PANEL_TAB);	/* filter, filled when on */
  PB.box_x0 = PB.box_x1 = -1;
  bw = x1 - 11 - (g_pn.term_x1 + 2);
  if (bw > 44) bw = 44;
  if (bw < 14) return;
  PB.box_x1 = x1 - 11;
  PB.box_x0 = PB.box_x1 - bw;
  scr_fill(PB.box_x0, y, bw, S_INPUT);
  {
    char cnt[48];
    int cw = 0, tw;
    cnt[0] = '\0';
    if (filtered && PB.shown < PB.total) snprintf(cnt, sizeof(cnt), "Showing %d of %d ", PB.shown, PB.total);
    if ((int)strlen(cnt) + 12 < bw) cw = (int)strlen(cnt);
    else cnt[0] = '\0';
    if (cw) scr_puts(PB.box_x1 - cw, y, cnt, S_INPUT_HINT);
    if (PB.filter[0]) tw = scr_putsw(PB.box_x0 + 1, y, bw - 2 - cw, PB.filter, S_INPUT_ON);
    else {
      scr_putsw(PB.box_x0 + 1, y, bw - 2 - cw, "Filter (e.g. text, **/*.ts, !**/node_modules/**)", S_INPUT_HINT);
      tw = 0;
    }
    if (PB.in_filter && E.focus == F_PANEL) scr_cursor(PB.box_x0 + 1 + tw, y);
  }
}


/* the funnel: VS Code's filter menu */
static void problems_menu (void) {
  Pick p;
  int r;
  pick_init(&p, "Filter Problems");
  p.keep_order = 1;
  pick_add(&p, "Show Errors", PB.hide_err ? NULL : "on", PB.hide_err ? 0 : 0xEAB2);	/* check */
  pick_add(&p, "Show Warnings", PB.hide_warn ? NULL : "on", PB.hide_warn ? 0 : 0xEAB2);
  pick_add(&p, "Show Infos", PB.hide_info ? NULL : "on", PB.hide_info ? 0 : 0xEAB2);
  pick_add(&p, "Show Active File Only", PB.active_only ? "on" : NULL, PB.active_only ? 0xEAB2 : 0);
  pick_add(&p, "Clear Filters", NULL, 0xEABF);
  r = pick_run(&p);
  pick_free(&p);
  switch (r) {
    case 0: PB.hide_err = !PB.hide_err; break;
    case 1: PB.hide_warn = !PB.hide_warn; break;
    case 2: PB.hide_info = !PB.hide_info; break;
    case 3: PB.active_only = !PB.active_only; break;
    case 4:
      PB.hide_err = PB.hide_warn = PB.hide_info = PB.active_only = 0;
      PB.filter[0] = '\0';
      break;
  }
}


static void problems_collapse_all (void) {
  size_t f, nf = lsp_diag_files();
  for (f = 0; f < nf; f++) {
    char *path;
    size_t n;
    lsp_diag_file(f, &path, &n);
    if (n) pb_close(path, 1);
    free(path);
  }
  PB.sel = 0;
}


/* the title row clicked at x: 1 when it was the Problems' */
static int problems_title_click (int x) {
  if (x == PB.coll_x) problems_collapse_all();
  else if (x == PB.funnel_x) problems_menu();
  else if (PB.box_x0 >= 0 && x >= PB.box_x0 && x < PB.box_x1) PB.in_filter = 1;
  else return 0;
  E.focus = F_PANEL;
  return 1;
}


/* Problems: Copy Message */
static void problems_copy (void) {
  char *path;
  size_t n;
  const Diag *dv;
  problems_rows();
  if (PB.sel >= PB.n || PB.row[PB.sel].diag < 0) return;
  dv = lsp_diag_file(PB.row[PB.sel].file, &path, &n);
  clip_text(dv[PB.row[PB.sel].diag].msg);
  free(path);
}


/* the problem of row k: its file, there */
static void problem_go (size_t k) {
  char *path;
  size_t n;
  const Diag *dv;
  Pos p;
  if (k >= PB.n) return;
  dv = lsp_diag_file(PB.row[k].file, &path, &n);
  if (PB.row[k].diag >= 0 && open_file(path, 0) == 0) {
    p = dv[PB.row[k].diag].a;
    move_h(doc_clamp(T->doc, p), 0);
    center_cursor();
    E.focus = F_EDITOR;
  }
  free(path);
}


/* a URL in the system's browser */
static void open_url (const char *url) {
  char *argv[4];
  int io[3], null;
  OsProc proc;
  long pid;
#ifdef _WIN32
  argv[0] = find_program("rundll32");
  argv[1] = (char *)"url.dll,FileProtocolHandler";
  argv[2] = (char *)url;
  argv[3] = NULL;
  null = os_open("NUL", OS_WRITE);
#else
#ifdef __APPLE__
  argv[0] = find_program("open");
#else
  argv[0] = find_program("xdg-open");
#endif
  argv[1] = (char *)url;
  argv[2] = NULL;
  null = os_open("/dev/null", OS_WRITE);
#endif
  io[0] = -1;
  io[1] = io[2] = null;
  if (argv[0] == NULL || os_spawn(argv[0], argv, NULL, io, 3, &proc, &pid) != 0)
    toast(1, "Could not open %s", url);
  else os_detach(proc);
  if (null >= 0) os_close(null);
  free(argv[0]);
}


/* a link in the terminal: the file at its line and column, or the URL */
static void open_link (PanelLink *lk) {
  if (lk->url) open_url(lk->target);
  else if (open_file(lk->target, 0) == 0) {
    if (lk->line > 0) {
      Pos p;
      p.y = (size_t)lk->line - 1;
      p.x = 0;
      p = doc_clamp(T->doc, p);
      if (lk->col > 0) p.x = x_of_col(row_at(p.y), (size_t)lk->col - 1);
      move_h(doc_clamp(T->doc, p), 0);
      center_cursor();
    }
    E.focus = F_EDITOR;
  }
  free(lk->target);
}


/* the shells found (Terminal: Select Default Profile, the + dropdown); -1: Esc */
static int pick_profile (const char *title) {
  Pick p;
  int i, r;
  pick_init(&p, title);
  for (i = 0; i < panel_profiles(); i++)
    pick_add(&p, panel_profile_name(i), panel_profile_path(i), 0xEA85);	/* codicon terminal */
  p.keep_order = 1;
  r = pick_run(&p);
  pick_free(&p);
  return r;
}


/* Output: Show Output Channels... */
static void pick_channel (void) {
  Pick p;
  int i, r;
  if (out_count() == 0) {
    toast(0, "Nothing has been written to the output yet.");
    return;
  }
  pick_init(&p, "Select Output Channel");
  for (i = 0; i < out_count(); i++) pick_add(&p, out_name(i), i == out_current() ? "current" : NULL, 0xEB9D);
  p.keep_order = 1;
  p.start = out_current();
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) return;
  out_select(r);
  E.panel = 1;
  E.panel_view = 3;
}


/* a row of the list clicked: a file folds, a problem opens */
static void problems_click (size_t k) {
  problems_rows();
  PB.in_filter = 0;
  E.focus = F_PANEL;
  if (k >= PB.n) return;
  PB.sel = k;
  if (PB.row[k].diag < 0) {
    char *path;
    size_t n;
    lsp_diag_file(PB.row[k].file, &path, &n);
    pb_close(path, !pb_closed(path));
    free(path);
  }
  else problem_go(k);
}


static void problems_key (int k) {
  int code = KEY_CODE(k);
  size_t len = strlen(PB.filter);
  if (PB.in_filter) {	/* the filter box */
    if (code == K_ESC && len) PB.filter[0] = '\0';
    else if (code == K_ESC || code == K_DOWN || code == K_ENTER || code == K_TAB) PB.in_filter = 0;
    else if (code == K_BS) {
      while (len > 0 && ((unsigned char)PB.filter[len - 1] & 0xC0) == 0x80) len--;
      if (len > 0) PB.filter[len - 1] = '\0';
    }
    else if (IS_TEXT(k) && len + 4 < sizeof(PB.filter)) {
      len += (size_t)utf8_encode((uint32_t)k, PB.filter + len);
      PB.filter[len] = '\0';
      PB.sel = PB.top = 0;
    }
    return;
  }
  if (k == CTRL('f')) {	/* Problems: Focus Filter */
    PB.in_filter = 1;
    return;
  }
  if (k == CTRL('c')) {
    problems_copy();
    return;
  }
  if (IS_TEXT(k) && k != ' ') {	/* typing filters */
    PB.in_filter = 1;
    problems_key(k);
    return;
  }
  problems_rows();
  switch (code) {
    case K_UP: if (PB.sel > 0) PB.sel--; break;
    case K_DOWN: if (PB.sel + 1 < PB.n) PB.sel++; break;
    case K_PGUP: PB.sel = PB.sel > (size_t)PB.h ? PB.sel - (size_t)PB.h : 0; break;
    case K_PGDN: PB.sel = PB.sel + (size_t)PB.h < PB.n ? PB.sel + (size_t)PB.h : (PB.n ? PB.n - 1 : 0); break;
    case K_HOME: PB.sel = 0; break;
    case K_END: PB.sel = PB.n ? PB.n - 1 : 0; break;
    case K_LEFT: case K_RIGHT:	/* a file folds, unfolds; from a problem, Left goes to its file */
      if (PB.sel < PB.n) {
        char *path;
        size_t n;
        if (PB.row[PB.sel].diag >= 0 && code == K_LEFT) {
          while (PB.sel > 0 && PB.row[PB.sel].diag >= 0) PB.sel--;
          break;
        }
        lsp_diag_file(PB.row[PB.sel].file, &path, &n);
        if (PB.row[PB.sel].diag < 0) pb_close(path, code == K_LEFT);
        free(path);
      }
      break;
    case K_ENTER: case ' ':
      if (PB.sel < PB.n && PB.row[PB.sel].diag < 0) problems_click(PB.sel);
      else problem_go(PB.sel);
      break;
    case K_ESC: E.focus = F_EDITOR; break;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Symbols: the Outline and the breadcrumbs. The language server says
** them when there is one; else a simple reading of the file finds the
** functions, structs, classes, headings ...
** ===================================================================
*/

static struct {
  const Doc *d;
  unsigned long at;	/* d->edits + 1 when they were found */
  Sym *v;
  size_t n;
  unsigned long asked_at;	/* the server was asked, for this edit */
  long long asked;
} SY;


static void sym_clear (Sym *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) free(v[i].name);
  free(v);
}


static void sym_add (Sym **v, size_t *n, size_t *cap, const char *name, size_t len, int kind,
                     size_t line, size_t end, int depth) {
  Sym *y;
  if (len == 0) return;
  if (*n == *cap) {
    *cap = *cap ? *cap * 2 : 64;
    *v = (Sym *)xrealloc(*v, *cap * sizeof(Sym));
  }
  y = &(*v)[(*n)++];
  y->name = xstrndup(name, len);
  y->kind = kind;
  y->line = line;
  y->end = end < line ? line : end;
  y->depth = depth;
}


static int word_is (const char *s, size_t n, const char *w) {
  size_t k = strlen(w);
  return n >= k && strncmp(s, w, k) == 0 &&
         (n == k || !(s[k] == '_' || ((s[k] | 32) >= 'a' && (s[k] | 32) <= 'z') || (s[k] >= '0' && s[k] <= '9')));
}


static int ident_ch (int c) {
  return c == '_' || c >= 0x80 || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z');
}


/* the region of a block that starts at line y: its indented lines, and a } after them */
static size_t block_end (size_t y) {
  size_t e = fold_end(y), k = e + 1;
  if (e == y && y + 1 < T->doc->n && row_at(y + 1)->len > 0 && row_at(y + 1)->s[0] == '{') e = fold_end(y + 1), k = e + 1;
  if (k < T->doc->n && row_at(k)->len > 0 && row_at(k)->s[indent_end(row_at(k))] == '}') return k;
  return e;
}


/* the simple reading: line by line, the language's ways of starting a symbol */
static void scan_symbols (Sym **v, size_t *n, size_t *cap) {
  const char *lang = syntax_name(T->sx);
  size_t y;
  int md = strcmp(lang, "Markdown") == 0, py = strcmp(lang, "Python") == 0;
  int go = strcmp(lang, "Go") == 0;
  static const char *const skip[] = {"if", "for", "while", "switch", "return", "else", "do",
                                     "case", "sizeof", "typedef", "static_assert", "defined", NULL};
  for (y = 0; y < T->doc->n; y++) {
    const Row *r = row_at(y);
    size_t ind = indent_end(r), len = r->len - ind, i;
    const char *t = r->s + ind;
    if (len == 0) continue;
    if (md) {	/* # headings, deeper with more # */
      size_t lv = 0;
      while (lv < len && t[lv] == '#') lv++;
      if (lv > 0 && lv < 7 && lv < len && t[lv] == ' ' && ind == 0)
        sym_add(v, n, cap, t + lv + 1, len - lv - 1, 15, y, y, (int)lv - 1);
      continue;
    }
    if (py) {
      int depth = (int)(col_of(r, ind) / (size_t)(T->doc->indent ? T->doc->indent : 4));
      const char *q = t;
      size_t k = len;
      int kind = 0;
      if (word_is(q, k, "async")) q += 6, k -= k > 6 ? 6 : k;
      if (word_is(q, k, "def")) kind = depth ? 6 : 12, q += 4, k -= k > 4 ? 4 : k;
      else if (word_is(q, k, "class")) kind = 5, q += 6, k -= k > 6 ? 6 : k;
      if (kind) {
        for (i = 0; i < k && ident_ch((unsigned char)q[i]); i++) ;
        sym_add(v, n, cap, q, i, kind, y, fold_end(y), depth);
      }
      continue;
    }
    if (ind != 0) continue;	/* the top level */
    if (t[0] == '#') {	/* #define NAME */
      const char *q = t + 1;
      while (q < t + len && (*q == ' ' || *q == '\t')) q++;
      if ((size_t)(t + len - q) > 6 && strncmp(q, "define", 6) == 0) {
        q += 6;
        while (q < t + len && *q == ' ') q++;
        for (i = 0; q + i < t + len && ident_ch((unsigned char)q[i]); i++) ;
        sym_add(v, n, cap, q, i, 14, y, y, 0);
      }
      continue;
    }
    if (t[0] == '}' && *n > 0 && (*v)[*n - 1].end >= y && strcmp((*v)[*n - 1].name, "(anonymous)") == 0) {
      const char *q = t + 1;	/* typedef struct { ... } Name; */
      while (q < t + len && (*q == ' ' || *q == '*')) q++;
      for (i = 0; q + i < t + len && ident_ch((unsigned char)q[i]); i++) ;
      if (i) {
        free((*v)[*n - 1].name);
        (*v)[*n - 1].name = xstrndup(q, i);
      }
      continue;
    }
    {	/* struct / enum / class ... Name { */
      const char *q = t;
      size_t k = len;
      int kind = 0, again = 1;
      while (again) {	/* the words before it */
        again = 0;
        if (word_is(q, k, "typedef") || word_is(q, k, "export") || word_is(q, k, "pub") ||
            word_is(q, k, "public") || word_is(q, k, "static") || word_is(q, k, "abstract") ||
            word_is(q, k, "default") || word_is(q, k, "final") || word_is(q, k, "const")) {
          while (k > 0 && ident_ch((unsigned char)*q)) q++, k--;
          while (k > 0 && *q == ' ') q++, k--;
          again = 1;
        }
      }
      if (word_is(q, k, "struct") || word_is(q, k, "union")) kind = 23;
      else if (word_is(q, k, "enum")) kind = 10;
      else if (word_is(q, k, "class")) kind = 5;
      else if (word_is(q, k, "interface") || word_is(q, k, "trait")) kind = 11;
      else if (word_is(q, k, "impl")) kind = 5;
      else if (go && word_is(q, k, "type")) kind = 23;
      else if (word_is(q, k, "fn") || word_is(q, k, "func") || word_is(q, k, "function") ||
               (strcmp(lang, "Lua") == 0 && word_is(q, k, "local"))) kind = 12;
      if (kind) {
        const char *nm;
        while (k > 0 && ident_ch((unsigned char)*q)) q++, k--;
        while (k > 0 && *q == ' ') q++, k--;
        if (kind == 12 && k > 0 && *q == '(') {	/* Go's receiver: func (r *T) Name */
          while (k > 0 && *q != ')') q++, k--;
          if (k > 0) q++, k--;
          while (k > 0 && *q == ' ') q++, k--;
          kind = 6;
        }
        if (word_is(q, k, "function")) {	/* Lua: local function */
          q += 8, k -= k > 8 ? 8 : k;
          while (k > 0 && *q == ' ') q++, k--;
        }
        nm = q;
        for (i = 0; i < k && (ident_ch((unsigned char)nm[i]) || nm[i] == '.' || nm[i] == ':'); i++) ;
        if (i == 0 && kind != 12 && kind != 6) sym_add(v, n, cap, "(anonymous)", 11, kind, y, block_end(y), 0);
        else if (memchr(t, ';', len) == NULL || kind == 12 || kind == 6)
          sym_add(v, n, cap, nm, i, kind, y, block_end(y), 0);
        continue;
      }
    }
    if (!ident_ch((unsigned char)t[0]) || memchr(t, '(', len) == NULL) continue;
    {	/* a function: name( ... ) and then its body, here or on the next line */
      const char *paren = (const char *)memchr(t, '(', len), *e = paren;
      const char *last = t + len - 1;
      int bad = 0, body;
      while (last > t && (*last == ' ' || *last == '\t' || *last == '\r')) last--;
      for (i = 0; skip[i]; i++)
        if (word_is(t, len, skip[i])) bad = 1;
      if (bad || *last == ';' || *last == ',' || memchr(t, '=', (size_t)(paren - t))) continue;
      body = *last == '{' || (y + 1 < T->doc->n && row_at(y + 1)->len > 0 && row_at(y + 1)->s[0] == '{') ||
             (*last == ')' && y + 1 < T->doc->n && indent_end(row_at(y + 1)) > 0 &&
              strcmp(lang, "C") != 0 && strcmp(lang, "C++") != 0);
      if (!body) continue;
      while (e > t && (e[-1] == ' ' || e[-1] == '\t')) e--;
      for (i = 0; e - i > t && ident_ch((unsigned char)e[-1 - (long)i]); i++) ;
      if (i) sym_add(v, n, cap, e - i, i, 12, y, block_end(y), 0);
    }
  }
  if (md) {	/* a heading goes to the next one as deep or less */
    size_t k, j;
    for (k = 0; k < *n; k++) {
      (*v)[k].end = T->doc->n - 1;
      for (j = k + 1; j < *n; j++)
        if ((*v)[j].depth <= (*v)[k].depth) {
          (*v)[k].end = (*v)[j].line - 1;
          break;
        }
    }
  }
}


/* the symbols of the file in front */
static const Sym *symbols (size_t *n) {
  *n = 0;
  if (!HAS_DOC || T->sx == NULL) return NULL;
  if (SY.d != T->doc || SY.at != T->doc->edits + 1) {
    size_t cap = 0;
    sym_clear(SY.v, SY.n);
    SY.v = NULL;
    SY.n = 0;
    scan_symbols(&SY.v, &SY.n, &cap);
    SY.d = T->doc;
    SY.at = T->doc->edits + 1;
  }
  if (lsp_active(T->doc) && SY.asked_at != T->doc->edits + 1 && os_now_us() - SY.asked > 800000) {
    SY.asked_at = T->doc->edits + 1;	/* the server's, a little later */
    SY.asked = os_now_us();
    lsp_symbols(T->doc);
  }
  *n = SY.n;
  return SY.v;
}


void on_symbols (Doc *d, Sym *v, size_t n) {
  if (d != SY.d || n == 0 || SY.asked_at != d->edits + 1) {
    sym_clear(v, n);
    return;
  }
  sym_clear(SY.v, SY.n);
  SY.v = v;
  SY.n = n;
}


/* the icon and color VS Code gives each SymbolKind (1 File ... 26 TypeParameter) */
static uint32_t sym_icon (int kind, int *st) {
  switch (kind) {
    case 1: *st = S_ICON_GREY; return 0xEB60;	/* file */
    case 2: case 3: case 4: *st = S_ICON_GREY; return 0xEA8B;	/* module, namespace, package */
    case 5: *st = S_ICON_ORANGE; return 0xEB5B;	/* class */
    case 6: case 9: case 12: *st = S_ICON_PURPLE; return 0xEA8C;	/* method, constructor, function */
    case 7: *st = S_ICON_BLUE; return 0xEB65;	/* property */
    case 8: *st = S_ICON_BLUE; return 0xEB5F;	/* field */
    case 10: *st = S_ICON_ORANGE; return 0xEA95;	/* enum */
    case 11: *st = S_ICON_BLUE; return 0xEB61;	/* interface */
    case 13: *st = S_ICON_BLUE; return 0xEA88;	/* variable */
    case 14: *st = S_ICON_BLUE; return 0xEB5D;	/* constant */
    case 15: *st = S_ICON_GREY; return 0xEB8D;	/* string: a heading */
    case 16: *st = S_ICON_GREY; return 0xEA90;	/* number */
    case 17: *st = S_ICON_GREY; return 0xEA8F;	/* boolean */
    case 18: *st = S_ICON_GREY; return 0xEA8A;	/* array */
    case 19: *st = S_ICON_GREY; return 0xEA8B;	/* object */
    case 20: *st = S_ICON_GREY; return 0xEA93;	/* key */
    case 21: *st = S_ICON_GREY; return 0xEA8F;	/* null */
    case 22: *st = S_ICON_BLUE; return 0xEB5E;	/* enum member */
    case 23: *st = S_ICON_ORANGE; return 0xEA91;	/* struct */
    case 24: *st = S_ICON_ORANGE; return 0xEA86;	/* event */
    case 25: *st = S_ICON_GREY; return 0xEB64;	/* operator */
    case 26: *st = S_ICON_ORANGE; return 0xEA92;	/* type parameter */
  }
  *st = S_ICON_GREY;
  return 0xEA88;
}


/* the symbols the cursor is in, the outermost first; how many */
static size_t sym_path (const Sym *v, size_t n, size_t *out, size_t cap) {
  size_t i, k = 0;
  for (i = 0; i < n; i++)
    if (v[i].line <= T->cur.y && v[i].end >= T->cur.y) {
      while (k > 0 && v[out[k - 1]].depth >= v[i].depth) k--;
      if (k < cap) out[k++] = i;
    }
  return k;
}


/* where the symbols inside v[i] end: the one after them */
static size_t sym_end (const Sym *v, size_t n, size_t i) {
  size_t j = i + 1;
  while (j < n && v[j].depth > v[i].depth) j++;
  return j;
}


static char *ol_key (const Sym *y) {
  char d[16];
  snprintf(d, sizeof(d), "\n%d", y->depth);
  return xstrcat3(y->name, d, "");
}


static int ol_closed (const Sym *y) {
  char *key = ol_key(y);
  size_t i;
  int r = 0;
  for (i = 0; i < OL.closed.n && !r; i++) r = strcmp(OL.closed.v[i], key) == 0;
  free(key);
  return r;
}


static void ol_close (const Sym *y, int close) {
  char *key = ol_key(y);
  size_t i;
  for (i = 0; i < OL.closed.n; i++)
    if (strcmp(OL.closed.v[i], key) == 0) {
      if (!close) {
        free(OL.closed.v[i]);
        OL.closed.v[i] = OL.closed.v[--OL.closed.n];
      }
      free(key);
      return;
    }
  if (close) vec_push(&OL.closed, key);
  else free(key);
}


static const Sym *g_olv;	/* for cmp_sym */

static int cmp_sym (const void *a, const void *b) {
  const Sym *x = &g_olv[*(const size_t *)a], *y = &g_olv[*(const size_t *)b];
  const char *p, *q;
  if (OL.sort == 2 && x->kind != y->kind) return x->kind - y->kind;	/* by category */
  if (OL.sort >= 1)	/* by name */
    for (p = x->name, q = y->name; *p || *q; p++, q++)
      if (lower((unsigned char)*p) != lower((unsigned char)*q)) return lower((unsigned char)*p) - lower((unsigned char)*q);
  return x->line < y->line ? -1 : x->line > y->line;
}


/* v[i] or one inside it has the filter's text */
static int ol_match (const Sym *v, size_t n, size_t i) {
  size_t e = sym_end(v, n, i), j;
  if (OL.filter[0] == '\0') return 1;
  for (j = i; j < e; j++)
    if (has_text(v[j].name, OL.filter)) return 1;
  return 0;
}


/* the symbols lo..hi (siblings and what is inside them) into OL.vis, sorted */
static void ol_add (const Sym *v, size_t n, size_t lo, size_t hi) {
  size_t *kid = NULL, nk = 0, j;
  for (j = lo; j < hi; j = sym_end(v, n, j)) {
    kid = (size_t *)xrealloc(kid, (nk + 1) * sizeof(size_t));
    kid[nk++] = j;
  }
  if (OL.sort && nk > 1) {
    g_olv = v;
    qsort(kid, nk, sizeof(size_t), cmp_sym);
  }
  for (j = 0; j < nk; j++) {
    size_t i = kid[j];
    if (!ol_match(v, n, i)) continue;
    if (OL.nvis == OL.capvis) {
      OL.capvis = OL.capvis ? OL.capvis * 2 : 64;
      OL.vis = (size_t *)xrealloc(OL.vis, OL.capvis * sizeof(size_t));
    }
    OL.vis[OL.nvis++] = i;
    if (OL.filter[0] || !ol_closed(&v[i])) ol_add(v, n, i + 1, sym_end(v, n, i));
  }
  free(kid);
}


/* the file's symbols (*n), and the ones shown in OL.vis */
static const Sym *outline_rows (size_t *n) {
  const Sym *v = symbols(n);
  if (OL.doc != (HAS_DOC ? T->doc : NULL)) {	/* another file: its own folds, no filter */
    OL.doc = HAS_DOC ? T->doc : NULL;
    vec_free(&OL.closed);
    OL.filter[0] = '\0';
  }
  OL.nvis = 0;
  if (v) ol_add(v, *n, 0, *n);
  if (OL.sel >= OL.nvis) OL.sel = OL.nvis ? OL.nvis - 1 : 0;
  return v;
}


static void pane_title (int x, int y, int w, const char *name, int open, int focus);

static void draw_outline (int x, int y, int w, int h, int focus) {
  size_t n, path[16], np;
  const Sym *v = outline_rows(&n);
  int row;
  OL.y0 = y;
  OL.h = h;
  scr_box(x, y, w, h, S_SIDE);
  pane_title(x, y, w, "OUTLINE", OL.open, focus);
  if (!OL.open) return;
  if (OL.filter[0] && w > 20) {	/* what is typed: the filter */
    scr_put(x + 11, y + 1, 0xEA6D, S_SIDE_DIM);	/* search */
    scr_putsw(x + 13, y + 1, w - 20, OL.filter, S_SIDE);
  }
  if (w > 14) {
    scr_put(x + w - 5, y + 1, 0xEAC5, S_SIDE_TITLE);	/* collapse-all */
    scr_put(x + w - 3, y + 1, 0xEA7C, S_SIDE_TITLE);	/* ...: Follow Cursor, Sort By */
  }
  if (n == 0 || OL.nvis == 0) {
    scr_putsw(x + 2, y + 2, w - 3, n ? "No symbols match the filter." : "No symbols found in this file.", S_SIDE_DIM);
    return;
  }
  side_bar(x, y + 2, w, h - 2, OL.nvis, OL.top, (size_t)(h - 2));
  np = sym_path(v, n, path, 16);
  if (!focus && !OL.nofollow && np) {	/* Follow Cursor: the deepest one shown */
    size_t k, d;
    for (d = np; d-- > 0;) {
      for (k = 0; k < OL.nvis && OL.vis[k] != path[d]; k++) ;
      if (k < OL.nvis) {
        OL.sel = k;
        break;
      }
    }
  }
  h -= 2;
  if (OL.sel < OL.top) OL.top = OL.sel;
  if (OL.sel >= OL.top + (size_t)h) OL.top = OL.sel - (size_t)h + 1;
  for (row = 0; row < h; row++) {
    size_t k = OL.top + (size_t)row, s0;
    int st, ist, cx;
    uint32_t icon;
    if (k >= OL.nvis) break;
    s0 = OL.vis[k];
    st = (k == OL.sel) ? (focus ? S_SIDE_SEL : S_SIDE_CUR) : S_SIDE;
    scr_fill(x, y + 2 + row, w, st);
    cx = x + 1 + v[s0].depth * 2;
    if (sym_end(v, n, s0) > s0 + 1)	/* it has symbols in it: its chevron */
      scr_put(cx, y + 2 + row, OL.filter[0] || !ol_closed(&v[s0]) ? 0xEAB4 : 0xEAB6, st);
    icon = sym_icon(v[s0].kind, &ist);
    scr_put(cx + 2, y + 2 + row, icon, st == S_SIDE ? ist : st);
    scr_putsw(cx + 4, y + 2 + row, x + w - cx - 5, v[s0].name, st);
  }
}


/* the Outline's "...": Follow Cursor, Sort By, Filter on Type */
static void outline_menu (void) {
  Pick p;
  int r;
  pick_init(&p, "Outline");
  p.keep_order = 1;
  pick_add(&p, "Follow Cursor", OL.nofollow ? NULL : "on", OL.nofollow ? 0 : 0xEAB2);
  pick_add(&p, "Filter on Type", OL.nofilter ? NULL : "on", OL.nofilter ? 0 : 0xEAB2);
  pick_add(&p, "Sort By: Position", OL.sort == 0 ? "on" : NULL, OL.sort == 0 ? 0xEAB2 : 0);
  pick_add(&p, "Sort By: Name", OL.sort == 1 ? "on" : NULL, OL.sort == 1 ? 0xEAB2 : 0);
  pick_add(&p, "Sort By: Category", OL.sort == 2 ? "on" : NULL, OL.sort == 2 ? 0xEAB2 : 0);
  pick_add(&p, "Collapse All", NULL, 0xEAC5);
  r = pick_run(&p);
  pick_free(&p);
  if (r == 0) OL.nofollow = !OL.nofollow;
  else if (r == 1) OL.nofilter = !OL.nofilter;
  else if (r >= 2 && r <= 4) OL.sort = r - 2;
  else if (r == 5) run_command(CMD_OUTLINE_COLLAPSE);
}


static void outline_collapse_all (void) {
  size_t n, i;
  const Sym *v = symbols(&n);
  for (i = 0; i < n; i++)
    if (sym_end(v, n, i) > i + 1) ol_close(&v[i], 1);
  OL.sel = OL.top = 0;
}


static void outline_go (int keep_focus) {
  size_t n;
  const Sym *v = outline_rows(&n);
  Pos p;
  if (OL.sel >= OL.nvis) return;
  p.y = v[OL.vis[OL.sel]].line;
  p.x = 0;
  move_h(doc_clamp(T->doc, p), 0);
  key_home(0);
  center_cursor();
  if (!keep_focus) {
    E.focus = F_EDITOR;
    E.outline_focus = 0;
  }
}


/* Tab in the Explorer: the folders, OPEN EDITORS, OUTLINE, TIMELINE, the folders ... */
static void next_pane (void) {
  static const int order[] = {PANE_TREE, PANE_EDITORS, PANE_OUTLINE, PANE_TIMELINE};
  int i, at = 0;
  for (i = 0; i < 4; i++)
    if (order[i] == E.outline_focus) at = i;
  for (i = 1; i <= 4; i++) {
    int p = order[(at + i) % 4];
    if (p == PANE_TREE || (p == PANE_OUTLINE && OL.h > 0) || (p == PANE_EDITORS && OE.h > 0) ||
        (p == PANE_TIMELINE && TL.h > 0)) {
      E.outline_focus = p;
      return;
    }
  }
}


/* a pane's title: the line over it, its chevron and name; the rows under it */
static void pane_title (int x, int y, int w, const char *name, int open, int focus) {
  int i;
  for (i = 0; i < w; i++) scr_put(x + i, y, 0x2500, S_BORDER);
  scr_fill(x, y + 1, w, focus && !open ? S_SIDE_SEL : S_SIDE);
  scr_put(x, y + 1, open ? 0xEAB4 : 0xEAB6, focus && !open ? S_SIDE_SEL : S_SIDE_TITLE);
  scr_puts(x + 2, y + 1, name, focus && !open ? S_SIDE_SEL : S_SIDE_TITLE);
}

/* }================================================================== */


/*
** {==================================================================
** OPEN EDITORS
** ===================================================================
*/

/* the rows: a group's title (-1 - g, with two groups), or a tab (g * 4096 + i) */
static size_t oe_rows (long *row, size_t cap) {
  size_t n = 0;
  int g, i;
  for (g = 0; g < g_ngrp; g++) {
    if (g_ngrp > 1 && n < cap) row[n++] = -1 - g;
    for (i = 0; i < g_grp[g].ntab && n < cap; i++) row[n++] = (long)g * 4096 + i;
  }
  return n;
}


static int oe_height (void) {
  long row[64];
  size_t n = oe_rows(row, 64);
  if (!OE.open) return 2;
  return 2 + (int)(n == 0 ? 1 : n < 9 ? n : 9);
}


static void draw_open_editors (int x, int y, int w, int h, int focus) {
  long row[4096];
  size_t n = oe_rows(row, 4096);
  int r;
  OE.y0 = y;
  OE.h = h;
  scr_box(x, y, w, h, S_SIDE);
  pane_title(x, y, w, "OPEN EDITORS", OE.open, focus);
  if (!OE.open) return;
  if (n == 0) {
    scr_putsw(x + 2, y + 2, w - 3, "No open editors.", S_SIDE_DIM);
    return;
  }
  if (OE.sel >= n) OE.sel = n - 1;
  h -= 2;
  if (OE.sel < OE.top) OE.top = OE.sel;
  if (OE.sel >= OE.top + (size_t)h) OE.top = OE.sel - (size_t)h + 1;
  side_bar(x, y + 2, w, h, n, OE.top, (size_t)h);
  for (r = 0; r < h; r++) {
    size_t k = OE.top + (size_t)r;
    int sy = y + 2 + r, st;
    if (k >= n) break;
    st = (k == OE.sel && focus) ? S_SIDE_SEL : S_SIDE;
    scr_fill(x, sy, w, st);
    if (row[k] < 0) {
      char gt[32];
      snprintf(gt, sizeof(gt), "GROUP %d", (int)(-row[k]));
      scr_puts(x + 2, sy, gt, st == S_SIDE ? S_SIDE_DIM : st);
    }
    else {
      Group *g = &g_grp[row[k] / 4096];
      Tab *t = g->tab[row[k] % 4096];
      int ist, cx = x + 4, on = g == G && t == T;
      char *d = t->real ? rel_dir(t->real) : NULL;
      if (doc_dirty(t->doc)) scr_put(x + 2, sy, 0x25CF, st);	/* unsaved */
      if (k == OE.sel && w > 12) scr_put(x + w - 2, sy, 0xEA76, st);	/* close, at the end of the row */
      cx += scr_put(cx, sy, file_icon(tab_name(t), &ist), st == S_SIDE ? ist : st) + 1;
      cx += scr_putsw(cx, sy, x + w - cx - 1, tab_name(t), st == S_SIDE && on ? S_SIDE_ACTIVE : st);
      if (d && cx + 2 < x + w) scr_putsw(cx + 1, sy, x + w - cx - 2, d, st == S_SIDE ? S_SIDE_DIM : st);
      free(d);
    }
  }
}


/* row k: to its tab (close: the tab goes) */
static void oe_go (size_t k, int close, int keep_focus) {
  long row[4096];
  size_t n = oe_rows(row, 4096);
  int g, i;
  if (k >= n || row[k] < 0) return;
  g = (int)(row[k] / 4096);
  i = (int)(row[k] % 4096);
  focus_group(g);
  if (close) {
    close_tab(i);
    if (!HAS_DOC && !HAS_DIFF && g_ngrp > 1) close_group();
    return;
  }
  focus_tab(i);
  if (!keep_focus) {
    E.focus = F_EDITOR;
    E.outline_focus = PANE_TREE;
  }
}


static void oe_key (int k) {
  long row[4096];
  size_t n = oe_rows(row, 4096);
  switch (KEY_CODE(k)) {
    case K_UP: if (OE.sel > 0) OE.sel--; break;
    case K_DOWN: if (OE.sel + 1 < n) OE.sel++; break;
    case K_LEFT: OE.open = 0; break;
    case K_RIGHT: OE.open = 1; break;
    case K_ENTER: if (!OE.open) OE.open = 1; else oe_go(OE.sel, 0, 0); break;
    case ' ': if (!OE.open) OE.open = 1; else oe_go(OE.sel, 0, 1); break;
    case K_DEL: oe_go(OE.sel, 1, 1); break;
    case K_TAB: next_pane(); break;
    case K_ESC:
      E.outline_focus = PANE_TREE;
      E.focus = F_EDITOR;
      break;
  }
}


static void oe_click (int row, int col) {
  static long long last;	/* the x shows on the selected row: a double click must not hit it */
  static size_t last_row;
  size_t k;
  long long now = os_now_us();
  int quick;
  E.outline_focus = PANE_EDITORS;
  if (row <= 1) {
    OE.open = !OE.open;
    return;
  }
  k = OE.top + (size_t)(row - 2);
  quick = k == last_row && now - last < 500000;
  last = now;
  last_row = k;
  OE.sel = k;
  oe_go(OE.sel, col == side_width() - 2 && !quick, 1);	/* its x; a double click only opens */
}

/* }================================================================== */


/*
** {==================================================================
** TIMELINE
** ===================================================================
*/

static void tl_clear (void) {
  size_t i;
  for (i = 0; i < TL.n; i++) {
    free(TL.v[i].label);
    free(TL.v[i].detail);
    free(TL.v[i].file);
  }
  free(TL.v);
  TL.v = NULL;
  TL.n = 0;
}


static int cmp_titem (const void *a, const void *b) {
  long long x = ((const TItem *)a)->time, y = ((const TItem *)b)->time;
  return x < y ? 1 : x > y ? -1 : 0;
}


/* the file's items: its local copies, and its commits when git knows it */
static void tl_load (void) {
  HistEntry *h;
  size_t nh, i, cap;
  const char *path = G->diff && HAS_DIFF ? diff_path() : HAS_DOC ? T->real : NULL;	/* a diff: its file's */
  if (!TL.stale && ((path == NULL && TL.path == NULL) || (path && TL.path && m_fncmp(path, TL.path) == 0))) return;
  tl_clear();
  free(TL.path);
  TL.path = path ? xstrdup(path) : NULL;
  TL.stale = 0;
  TL.sel = TL.top = 0;
  if (path == NULL) return;
  nh = history_list(path, &h);
  cap = nh + 64;
  TL.v = (TItem *)xmalloc(cap * sizeof(TItem));
  for (i = 0; i < nh; i++) {
    TItem *t = &TL.v[TL.n++];
    memset(t, 0, sizeof(*t));
    t->time = h[i].time;
    t->label = xstrdup(h[i].source);
    t->file = xstrdup(h[i].file);
  }
  history_free(h, nh);
  if (git_root()) {	/* git log --follow of the file */
    const char *a[] = {"log", "--follow", "-n", "50", "--format=%h%x09%an%x09%at%x09%s", "--", path, NULL};
    Buf b;
    char *line;
    buf_init(&b);
    if (git_exec(&b, 0, a) == 0 && b.s) {
      buf_putc(&b, '\0');
      for (line = strtok(b.s, "\r\n"); line && TL.n < cap; line = strtok(NULL, "\r\n")) {
        char *f[4];
        int k;
        TItem *t;
        f[0] = line;
        for (k = 1; k < 4 && f[k - 1]; k++) {
          char *tab = strchr(f[k - 1], '\t');
          if (tab) *tab++ = '\0';
          f[k] = tab;
        }
        if (k < 4 || f[3] == NULL) continue;
        t = &TL.v[TL.n++];
        memset(t, 0, sizeof(*t));
        snprintf(t->hash, sizeof(t->hash), "%s", f[0]);
        t->detail = xstrdup(f[1]);
        t->time = strtoll(f[2], NULL, 10);
        t->label = xstrdup(f[3]);
      }
    }
    buf_free(&b);
  }
  qsort(TL.v, TL.n, sizeof(TItem), cmp_titem);
}


static int tl_height (void) {
  if (!TL.open) return 2;
  tl_load();
  return 2 + (int)(TL.n == 0 ? 1 : TL.n < 8 ? TL.n : 8);
}


static void draw_timeline (int x, int y, int w, int h, int focus) {
  int r;
  TL.y0 = y;
  TL.h = h;
  scr_box(x, y, w, h, S_SIDE);
  pane_title(x, y, w, "TIMELINE", TL.open, focus);
  if (!TL.open) return;
  if (TL.n == 0) {
    scr_putsw(x + 2, y + 2, w - 3, TL.path ? "No timeline information was provided." :
              "The active editor cannot provide timeline information.", S_SIDE_DIM);
    return;
  }
  if (TL.sel >= TL.n) TL.sel = TL.n - 1;
  h -= 2;
  if (TL.sel < TL.top) TL.top = TL.sel;
  if (TL.sel >= TL.top + (size_t)h) TL.top = TL.sel - (size_t)h + 1;
  side_bar(x, y + 2, w, h, TL.n, TL.top, (size_t)h);
  for (r = 0; r < h; r++) {
    size_t k = TL.top + (size_t)r;
    int sy = y + 2 + r, st, cx = x + 2, aw;
    const TItem *t;
    char age[32];
    if (k >= TL.n) break;
    t = &TL.v[k];
    st = (k == TL.sel && focus) ? S_SIDE_SEL : S_SIDE;
    scr_fill(x, sy, w, st);
    history_age(t->time, age, sizeof(age));
    aw = (int)strlen(age) + 1;
    cx += scr_put(cx, sy, t->file ? 0xEA82 : 0xEAFC, st == S_SIDE ? S_SIDE_DIM : st) + 1;	/* history, git-commit */
    cx += scr_putsw(cx, sy, x + w - cx - aw - 1, t->label, st);
    if (t->detail && cx + 2 < x + w - aw)
      scr_putsw(cx + 1, sy, x + w - aw - cx - 2, t->detail, st == S_SIDE ? S_SIDE_DIM : st);
    scr_puts(x + w - aw, sy, age, st == S_SIDE ? S_SIDE_DIM : st);
  }
}


/* Local History's copy k against the file, side by side (VS Code's Compare with File) */
static void history_compare (const char *copy, long long when, const char *source) {
  char age[32], title[512];
  SideAct act;
  if (!HAS_DOC || T->doc->path == NULL) return;
  history_age(when, age, sizeof(age));
  snprintf(title, sizeof(title), "%s (%s \xE2\x80\xA2 %s) \xE2\x86\x94 %s", doc_name(), source, age, doc_name());
  if (diff_open_files(T->doc->path, copy, T->doc->path, title) != 0) return;
  memset(&act, 0, sizeof(act));
  act.what = SA_SHOW_DIFF;
  act.go = 1;
  apply_act(&act);
}


/* the copy in place of the text: one step of undo, saved when the user saves */
static void history_restore (const char *copy) {
  static const char *const bt[] = {"Restore", "Cancel"};
  char msg[512];
  size_t len;
  char *s;
  Pos a;
  if (!HAS_DOC || G->diff) return;
  snprintf(msg, sizeof(msg), "Do you want to restore the contents of '%s'?", doc_name());
  if (dialog(msg, "Restoring will discard any unsaved changes.", bt, 2) != 0) return;
  if ((s = read_file(copy, &len)) == NULL) {
    toast(1, "Unable to read the Local History entry");
    return;
  }
  if (len > 0 && s[len - 1] == '\n') len--;	/* the last line's end is the text's end */
  if (len > 0 && s[len - 1] == '\r') len--;
  {	/* CR LF as the file has it: the editor keeps lines */
    size_t i, o = 0;
    for (i = 0; i < len; i++)
      if (!(s[i] == '\r' && i + 1 < len && s[i + 1] == '\n')) s[o++] = s[i];
    len = o;
  }
  a.y = a.x = 0;
  doc_group(T->doc);
  T->sel = 0;
  T->nmc = 0;
  ed_delete(a, doc_end(T->doc));
  ed_insert(a, s, len);
  doc_group(T->doc);
  T->cur = doc_clamp(T->doc, T->cur);
  free(s);
}


/* item k: its compare; a commit's change of the file */
static void tl_open (size_t k) {
  const TItem *t;
  if (k >= TL.n) return;
  t = &TL.v[k];
  if (t->file) history_compare(t->file, t->time, t->label);
  else {
    const char *root = git_root(), *path = TL.path;
    size_t n = root ? strlen(root) : 0;
    char *rel, *c, old[40], title[512];
    SideAct act;
    if (root == NULL || m_strnicmp(path, root, n) != 0) return;
    rel = xstrdup(path + n + (path_is_sep(path[n]) ? 1 : 0));
    for (c = rel; *c; c++)
      if (*c == '\\') *c = '/';
    snprintf(old, sizeof(old), "%s^", t->hash);
    snprintf(title, sizeof(title), "%s (%s^ \xE2\x86\x94 %s)", path_basename(path), t->hash, t->hash);
    if (diff_open_rev(path, rel, NULL, old, t->hash, title) == 0) {
      memset(&act, 0, sizeof(act));
      act.what = SA_SHOW_DIFF;
      act.go = 1;
      apply_act(&act);
    }
    free(rel);
  }
}


/* the right-click menu of a Local History item */
static void tl_menu (size_t k, int x, int y) {
  static const int cmd[] = {CMD_HISTORY_COMPARE, CMD_HISTORY_RESTORE, -1};
  static const char *const label[] = {"Compare with File", "Restore Contents"};
  int c;
  if (k >= TL.n || TL.v[k].file == NULL) return;
  c = menu_popup(x, y, cmd, label);
  if (c == CMD_HISTORY_COMPARE) tl_open(k);
  else if (c == CMD_HISTORY_RESTORE) history_restore(TL.v[k].file);
}


static void tl_key (int k) {
  switch (KEY_CODE(k)) {
    case K_UP: if (TL.sel > 0) TL.sel--; break;
    case K_DOWN: if (TL.sel + 1 < TL.n) TL.sel++; break;
    case K_LEFT: TL.open = 0; break;
    case K_RIGHT: TL.open = 1; break;
    case K_ENTER: if (!TL.open) TL.open = 1; else tl_open(TL.sel); break;
    case K_F10: if (k & KM_SHIFT) tl_menu(TL.sel, L.side_x + 4, TL.y0 + 2 + (int)(TL.sel - TL.top)); break;
    case K_TAB: next_pane(); break;
    case K_ESC:
      E.outline_focus = PANE_TREE;
      E.focus = F_EDITOR;
      break;
  }
}


static void tl_click (int row, int x, int button) {
  E.outline_focus = PANE_TIMELINE;
  if (row <= 1) {
    TL.open = !TL.open;
    return;
  }
  TL.sel = TL.top + (size_t)(row - 2);
  if (button == 2) tl_menu(TL.sel, x, TL.y0 + row);
  else tl_open(TL.sel);
}


/* Local History: Find Entry to Restore / Compare with File: the file's copies in a list */
static void history_pick (int restore) {
  HistEntry *v;
  size_t n, i;
  Pick p;
  int r;
  if (!HAS_DOC || G->diff || T->doc->path == NULL) {
    toast(0, "Open a file first to see its Local History.");
    return;
  }
  n = history_list(T->doc->path, &v);
  if (n == 0) {
    toast(0, "There is no Local History for '%s' yet: every save keeps one.", doc_name());
    return;
  }
  pick_init(&p, restore ? "Select Local History Entry to Restore" : "Select Local History Entry to Compare");
  p.keep_order = 1;
  for (i = 0; i < n; i++) {
    char age[32];
    history_age(v[i].time, age, sizeof(age));
    pick_add(&p, v[i].source, age, 0xEA82);
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) {
    if (restore) history_restore(v[r].file);
    else history_compare(v[r].file, v[r].time, v[r].source);
  }
  history_free(v, n);
}

/* }================================================================== */


static void outline_key (int k) {
  if (!OL.open) {	/* collapsed: only opening it again */
    if (KEY_CODE(k) == K_RIGHT || KEY_CODE(k) == K_ENTER || k == ' ') OL.open = 1;
    return;
  }
  size_t n, len = strlen(OL.filter);
  const Sym *v = outline_rows(&n);
  int code = KEY_CODE(k);
  if (IS_TEXT(k) && !OL.nofilter && (k != ' ' || len > 0) && len + 4 < sizeof(OL.filter)) {	/* Filter on Type */
    len += (size_t)utf8_encode((uint32_t)k, OL.filter + len);
    OL.filter[len] = '\0';
    OL.sel = OL.top = 0;
    return;
  }
  switch (code) {
    case K_UP: if (OL.sel > 0) OL.sel--; break;
    case K_DOWN: if (OL.sel + 1 < OL.nvis) OL.sel++; break;
    case K_HOME: OL.sel = 0; break;
    case K_END: OL.sel = OL.nvis ? OL.nvis - 1 : 0; break;
    case K_ENTER: outline_go(0); break;
    case ' ': outline_go(1); break;
    case K_TAB: next_pane(); break;
    case K_BS:
      while (len > 0 && ((unsigned char)OL.filter[len - 1] & 0xC0) == 0x80) len--;
      if (len > 0) OL.filter[len - 1] = '\0';
      break;
    case K_LEFT: case K_RIGHT:	/* fold, unfold; Left from inside: to the one it is in */
      if (OL.sel < OL.nvis && v) {
        size_t i = OL.vis[OL.sel];
        int kids = sym_end(v, n, i) > i + 1;
        if (code == K_RIGHT) ol_close(&v[i], 0);
        else if (kids && !ol_closed(&v[i]) && !OL.filter[0]) ol_close(&v[i], 1);
        else
          while (OL.sel > 0 && v[OL.vis[OL.sel]].depth >= v[i].depth) OL.sel--;
      }
      break;
    case K_ESC:
      if (OL.filter[0]) {
        OL.filter[0] = '\0';
        break;
      }
      E.outline_focus = 0;
      E.focus = F_EDITOR;
      break;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Peek: references, implementations, a definition, in a box under the
** line (VS Code's peek view): the text of the one selected on the left,
** the list of them by file on the right. Enter goes there, Esc closes.
** ===================================================================
*/

/* the text of files that are not open, for the peek's lines */
typedef struct FText {
  char *path;
  char *buf;
  size_t *off, *len;
  size_t n;
} FText;

static FText *g_ft;
static size_t g_nft;


static void ft_clear (void) {
  size_t i;
  for (i = 0; i < g_nft; i++) {
    free(g_ft[i].path);
    free(g_ft[i].buf);
    free(g_ft[i].off);
    free(g_ft[i].len);
  }
  free(g_ft);
  g_ft = NULL;
  g_nft = 0;
}


static const FText *ft_get (const char *path) {
  size_t i, n = 0, len = 0, k, start = 0;
  FText *f;
  char *s;
  for (i = 0; i < g_nft; i++)
    if (m_fncmp(g_ft[i].path, path) == 0) return &g_ft[i];
  g_ft = (FText *)xrealloc(g_ft, (g_nft + 1) * sizeof(FText));
  f = &g_ft[g_nft++];
  memset(f, 0, sizeof(*f));
  f->path = xstrdup(path);
  s = read_file(path, &len);
  f->buf = s ? s : xstrdup("");
  if (s == NULL) len = 0;
  for (k = 0; k < len; k++) n += f->buf[k] == '\n';
  f->off = (size_t *)xmalloc((n + 2) * sizeof(size_t));
  f->len = (size_t *)xmalloc((n + 2) * sizeof(size_t));
  for (k = 0; k <= len; k++)
    if (k == len || f->buf[k] == '\n') {
      size_t e = k;
      if (e > start && f->buf[e - 1] == '\r') e--;
      f->off[f->n] = start;
      f->len[f->n++] = e - start;
      start = k + 1;
    }
  return f;
}


/* line y of path: from its tab when it is open (edits and all), else from the disk */
static const char *ft_line (const char *path, const Doc *d, size_t y, size_t *len) {
  const FText *f;
  if (d) {
    if (y >= d->n) {
      *len = 0;
      return "";
    }
    *len = d->row[y].len;
    return d->row[y].s;
  }
  f = ft_get(path);
  if (y >= f->n) {
    *len = 0;
    return "";
  }
  *len = f->len[y];
  return f->buf + f->off[y];
}


/* the byte of the c-th UTF-16 unit of s */
static size_t u16_to_x (const char *s, size_t n, size_t c) {
  size_t i = 0, u = 0, len;
  while (i < n && u < c) {
    u += utf8_decode(s + i, n - i, &len) >= 0x10000 ? 2 : 1;
    i += len;
  }
  return i;
}


static struct {
  int open;
  int what;	/* LOC_* */
  Loc *v;	/* by file, then by place */
  size_t n, sel, top;
  Doc *d;	/* the text it is under */
  Pos anchor;	/* its line */
  int x, y, w, h, lx;	/* where it is: the list from lx */
  int rows[256];	/* the list's rows on the screen: a place's index, or -1 - file */
  int nrows;
} PK;


static void peek_close (void) {
  size_t i;
  for (i = 0; i < PK.n; i++) free(PK.v[i].path);
  free(PK.v);
  PK.v = NULL;
  PK.n = 0;
  PK.open = 0;
  ft_clear();
}


static int cmp_loc (const void *a, const void *b) {
  const Loc *x = (const Loc *)a, *y = (const Loc *)b;
  int c = strcmp(x->path, y->path);
  if (c) return c;
  return pos_cmp(x->a, y->a);
}


/* the open file of v's path, or NULL */
static const Doc *loc_doc (const Loc *v) {
  Tab *t = tab_of(v->path, NULL);
  return t ? t->doc : NULL;
}


/* a place's columns in bytes, now that its line is known */
static void loc_fix (Loc *v) {
  size_t len;
  const char *s;
  if (!v->utf16) return;
  s = ft_line(v->path, loc_doc(v), v->a.y, &len);
  v->a.x = u16_to_x(s, len, v->a.x);
  s = ft_line(v->path, loc_doc(v), v->b.y, &len);
  v->b.x = u16_to_x(s, len, v->b.x);
  v->utf16 = 0;
}


static void peek_show (int what, const Loc *v, size_t n) {
  size_t i;
  int ph;
  peek_close();
  PK.v = (Loc *)xmalloc((n + 1) * sizeof(Loc));
  for (i = 0; i < n; i++) {
    PK.v[i] = v[i];
    PK.v[i].path = xstrdup(v[i].path);
  }
  PK.n = n;
  qsort(PK.v, n, sizeof(Loc), cmp_loc);
  PK.what = what;
  PK.d = T->doc;
  PK.anchor = T->cur;
  PK.sel = PK.top = 0;
  for (i = 0; i < n; i++) {	/* the one at the cursor is selected first */
    loc_fix(&PK.v[i]);
    if (T->real && PK.v[i].a.y == T->cur.y && T->cur.x >= PK.v[i].a.x && T->cur.x <= PK.v[i].b.x &&
        tab_of(PK.v[i].path, NULL) == T)
      PK.sel = i;
  }
  PK.open = 1;
  E.focus = F_EDITOR;
  ph = L.text_h * 45 / 100;	/* room under the line */
  if (ph < 8) ph = 8;
  if (vis_row(PK.anchor) < 0 || vis_row(PK.anchor) + ph + 1 > L.text_h) T->top = PK.anchor.y > 2 ? PK.anchor.y - 2 : 0;
}


static int peek_is_open (void) {
  return PK.open && T->doc == PK.d;
}


/* Enter: to the place selected; the peek goes */
static void peek_go (size_t i) {
  char *path;
  Pos p;
  if (i >= PK.n) return;
  path = xstrdup(PK.v[i].path);
  p = PK.v[i].a;
  peek_close();
  on_definition(path, p, 0);
  free(path);
}


/* Shift+Alt+H: who calls the function at the cursor (what: LOC_CALLS_IN ... LOC_SUB), in the peek */
static void hierarchy_ask (int what) {
  if (!HAS_DOC || G->diff || T->page) return;
  if (!lsp_active(T->doc) || !lsp_hierarchy(T->doc, T->cur, what))
    toast(0, "No %s provider for '%s' files.", what <= LOC_CALLS_OUT ? "call hierarchy" : "type hierarchy",
          syntax_name(T->sx));
}


/* a rename's edits: across several files, asked first (VS Code's refactor preview, simply) */
void on_edit_confirm (const TextEdit *v, size_t n) {
  static const char *const bt[] = {"Apply", "Cancel"};
  char msg[200];
  Buf d;
  size_t i, j, files = 0;
  for (i = 0; i < n; i++) {
    for (j = 0; j < i && m_fncmp(v[j].path, v[i].path) != 0; j++) ;
    if (j == i) files++;
  }
  if (files <= 1) {
    on_edit(v, n);
    return;
  }
  buf_init(&d);
  for (i = 0; i < n; i++) {
    size_t c = 0;
    for (j = 0; j < i && m_fncmp(v[j].path, v[i].path) != 0; j++) ;
    if (j < i) continue;
    for (j = i; j < n; j++)
      if (m_fncmp(v[j].path, v[i].path) == 0) c++;
    if (d.len > 160) {
      buf_puts(&d, ", ...");
      break;
    }
    buf_printf(&d, "%s%s (%lu)", d.len ? ", " : "", path_basename(v[i].path), (unsigned long)c);
  }
  buf_putc(&d, '\0');
  snprintf(msg, sizeof(msg), "Rename will make %lu edits in %lu files.", (unsigned long)n, (unsigned long)files);
  if (dialog(msg, d.s, bt, 2) == 0) on_edit(v, n);
  buf_free(&d);
}


void on_locations (int what, const Loc *v, size_t n) {
  static const char *const none[] = {"No references found", "No implementation found",
                                     "No type definition found", "No definition found", "No incoming calls",
                                     "No outgoing calls", "No supertypes", "No subtypes"};
  if (!HAS_DOC || G->diff) return;
  if (n == 0) {
    toast(0, "%s", none[what]);
    return;
  }
  if ((what == LOC_IMPL || what == LOC_TYPE) && n == 1) {	/* one: go there, as F12 does */
    Loc one = v[0];
    one.path = xstrdup(v[0].path);
    PK.n = 0;
    if (one.utf16) {
      size_t len;
      const char *s = ft_line(one.path, loc_doc(&one), one.a.y, &len);
      one.a.x = u16_to_x(s, len, one.a.x);
      ft_clear();
    }
    on_definition(one.path, one.a, 0);
    free(one.path);
    return;
  }
  peek_show(what, v, n);
}


static void draw_peek (void) {
  int row, ph, i, pw, bh, gw = 6;
  const Loc *cur;
  const Doc *cd;
  const Syntax *sx;
  size_t first, k;
  char title[512], cnt[64];
  static unsigned char *tok;
  static size_t tokcap;
  if (!PK.open || PK.n == 0 || !HAS_DOC || T->doc != PK.d || G->diff) return;
  if (PK.sel >= PK.n) PK.sel = PK.n - 1;
  ph = L.text_h * 45 / 100;
  if (ph < 8) ph = 8;
  if (ph > 18) ph = 18;
  if (ph > L.text_h) ph = L.text_h;
  row = vis_row(PK.anchor);
  PK.y = L.text_y + (row < 0 ? 0 : row + 1);
  if (PK.y + ph > L.text_y + L.text_h) PK.y = L.text_y + L.text_h - ph;
  PK.x = L.ed_x;
  PK.w = L.ed_w - L.sb_w;
  PK.h = ph;
  pw = PK.w * 62 / 100;
  if (PK.w - pw < 24) pw = PK.w - 24;
  if (pw < 20) pw = PK.w / 2;
  PK.lx = PK.x + pw + 1;
  bh = ph - 2;
  cur = &PK.v[PK.sel];
  cd = loc_doc(cur);
  {	/* the title: the file, its folder, how many */
    char *dir = path_dirname(cur->path);
    const char *what[] = {"reference", "implementation", "type definition", "definition", "incoming call",
                          "outgoing call", "supertype", "subtype"};
    const char *root = side_root();
    size_t rl = strlen(root);
    if (m_strnicmp(dir, root, rl) == 0 && (dir[rl] == '\0' || dir[rl] == '\\' || dir[rl] == '/'))
      memmove(dir, dir + rl + (dir[rl] ? 1 : 0), strlen(dir + rl + (dir[rl] ? 1 : 0)) + 1);	/* from the folder open */
    snprintf(cnt, sizeof(cnt), "%lu %s%s", (unsigned long)PK.n, what[PK.what], PK.n == 1 ? "" : "s");
    scr_fill(PK.x, PK.y, PK.w, S_BOX_TITLE);
    i = PK.x + 1;
    i += scr_putsw(i, PK.y, PK.w / 3, path_basename(cur->path), S_BOX_TITLE) + 1;
    scr_putsw(i, PK.y, PK.w - (i - PK.x) - (int)strlen(cnt) - 6, dir, S_BOX_DIM);
    snprintf(title, sizeof(title), "%s", cnt);
    scr_puts(PK.x + PK.w - (int)strlen(title) - 4, PK.y, title, S_BOX_DIM);
    scr_put(PK.x + PK.w - 2, PK.y, 0xEA76, S_BOX_TITLE);	/* codicon close */
    free(dir);
  }
  for (i = 0; i < PK.w; i++)	/* the bottom edge, in the accent color */
    scr_put_rgb(PK.x + i, PK.y + ph - 1, 0x2500, ui_color(C_ACCENT), ui_color(C_EDITOR_BG), 0);
  {	/* the text around the one selected */
    Tab *t = cd ? tab_of(cur->path, NULL) : NULL;
    int state = 0;
    sx = t ? t->sx : syntax_for(cur->path);
    first = cur->a.y > (size_t)bh / 3 ? cur->a.y - (size_t)bh / 3 : 0;
    for (i = 0; i < bh; i++) {
      size_t y = first + (size_t)i, len;
      const char *s = ft_line(cur->path, cd, y, &len);
      int sy = PK.y + 1 + i, on = y == cur->a.y;
      char num[16];
      scr_fill(PK.x, sy, pw, on ? S_LINE : S_TEXT);
      if (cd == NULL && y >= ft_get(cur->path)->n) continue;
      if (cd && y >= cd->n) continue;
      snprintf(num, sizeof(num), "%5lu ", (unsigned long)(y + 1));
      scr_puts(PK.x, sy, num, on ? S_GUTTER_CUR : S_GUTTER);
      if (len + 1 > tokcap) {
        tokcap = len + 256;
        tok = (unsigned char *)xrealloc(tok, tokcap);
      }
      if (t && sx) syntax_line(t->doc, sx, y, tok);
      else if (sx) state = syntax_scan(sx, s, len, i == 0 ? 0 : state, tok);
      scr_code(PK.x + gw, sy, pw - gw, s, len, 0, sx ? tok : NULL, on ? B_LINE : B_EDITOR,
               on ? cur->a.x : 0, on ? (cur->b.y == y ? cur->b.x : len) : 0, B_MATCH);
    }
  }
  for (i = 0; i < ph - 1; i++) scr_put(PK.x + pw, PK.y + 1 + i - 1 + 1, 0x2502, S_BORDER);
  {	/* the list: each file, and its places under it */
    int lw = PK.w - pw - 1, sel_row = -1, n = 0;
    size_t f = 0;
    for (k = 0; k < PK.n && n < 256; k++) {
      if (k == 0 || strcmp(PK.v[k].path, PK.v[k - 1].path) != 0) PK.rows[n++] = -1 - (int)f++;
      if (n < 256) {
        if (k == PK.sel) sel_row = n;
        PK.rows[n++] = (int)k;
      }
    }
    PK.nrows = n;
    if (sel_row >= 0 && (size_t)sel_row < PK.top) PK.top = (size_t)sel_row > 0 ? (size_t)sel_row - 1 : 0;
    if (sel_row >= 0 && sel_row >= (int)PK.top + bh) PK.top = (size_t)(sel_row - bh + 1);
    for (i = 0; i < bh; i++) {
      int sy = PK.y + 1 + i, r = (int)PK.top + i;
      scr_fill(PK.lx, sy, lw, S_SIDE);
      if (r >= n) continue;
      if (PK.rows[r] < 0) {	/* a file: its icon, name, folder, count */
        size_t j, cnt_f = 0, at = 0;
        int ist, x = PK.lx + 1;
        uint32_t icon;
        char c[16];
        for (j = 0; j < PK.n; j++) {
          if (j == 0 || strcmp(PK.v[j].path, PK.v[j - 1].path) != 0) {
            if ((int)cnt_f == -1 - PK.rows[r]) break;
            cnt_f++;
          }
        }
        at = j;
        for (cnt_f = 0; j < PK.n && strcmp(PK.v[j].path, PK.v[at].path) == 0; j++) cnt_f++;
        icon = file_icon(path_basename(PK.v[at].path), &ist);
        scr_put(x, sy, 0xEAB4, S_SIDE);	/* expanded */
        x += 2;
        x += scr_put(x, sy, icon, ist) + 1;
        x += scr_putsw(x, sy, lw - (x - PK.lx) - 5, path_basename(PK.v[at].path), S_SIDE_TITLE) + 1;
        snprintf(c, sizeof(c), "%lu", (unsigned long)cnt_f);
        scr_puts(PK.lx + lw - (int)strlen(c) - 1, sy, c, S_SIDE_DIM);
      }
      else {	/* a place: its line, the name lit */
        Loc *v = &PK.v[PK.rows[r]];
        size_t len, ind = 0, h0, h1;
        const char *s;
        int st = PK.rows[r] == (int)PK.sel ? S_SIDE_SEL : S_SIDE;
        loc_fix(v);
        s = ft_line(v->path, loc_doc(v), v->a.y, &len);
        while (ind < len && (s[ind] == ' ' || s[ind] == '\t')) ind++;
        h0 = v->a.x > ind ? v->a.x - ind : 0;
        h1 = v->b.y == v->a.y && v->b.x > ind ? v->b.x - ind : h0;
        scr_fill(PK.lx, sy, lw, st);
        scr_text(PK.lx + 4, sy, lw - 5, s + ind, len - ind, 0, st, h0, h1, S_SIDE_HIT);
      }
    }
  }
}


/* a key while the peek shows: 1 when it was the peek's */
static int peek_key (int k) {
  int code = KEY_CODE(k);
  if (!PK.open) return 0;
  if (T->doc != PK.d) {
    peek_close();
    return 0;
  }
  switch (code) {
    case K_ESC: peek_close(); return 1;
    case K_UP: if (PK.sel > 0) PK.sel--; return 1;
    case K_DOWN: if (PK.sel + 1 < PK.n) PK.sel++; return 1;
    case K_F4: PK.sel = (k & KM_SHIFT) ? (PK.sel + PK.n - 1) % PK.n : (PK.sel + 1) % PK.n; return 1;
    case K_PGUP: PK.sel = PK.sel > 8 ? PK.sel - 8 : 0; return 1;
    case K_PGDN: PK.sel = PK.sel + 8 < PK.n ? PK.sel + 8 : PK.n - 1; return 1;
    case K_HOME: PK.sel = 0; return 1;
    case K_END: PK.sel = PK.n - 1; return 1;
    case K_ENTER: peek_go(PK.sel); return 1;
  }
  peek_close();	/* anything else: back to the text */
  return 0;
}


/* a click: 1 when it was in the peek */
static int peek_click (Mouse *m) {
  int press = m->button == 0 && m->press && !m->drag;
  if (!PK.open || T->doc != PK.d || m->x < PK.x || m->x >= PK.x + PK.w || m->y < PK.y || m->y >= PK.y + PK.h)
    return 0;
  if (m->wheel) {
    if (m->x >= PK.lx) {
      int st = wheel_step(m->mods);
      if (m->wheel < 0) PK.top = PK.top > (size_t)st ? PK.top - (size_t)st : 0;
      else if ((int)PK.top + st < PK.nrows) PK.top += (size_t)st;
    }
    return 1;
  }
  if (!press) return 1;
  if (m->y == PK.y) {
    if (m->x >= PK.x + PK.w - 3) peek_close();
    return 1;
  }
  if (m->x >= PK.lx) {	/* the list: select; the one selected again: go */
    int r = (int)PK.top + (m->y - PK.y - 1);
    if (r >= 0 && r < PK.nrows && PK.rows[r] >= 0) {
      if ((size_t)PK.rows[r] == PK.sel) peek_go(PK.sel);
      else PK.sel = (size_t)PK.rows[r];
    }
    return 1;
  }
  if (m->y < PK.y + PK.h - 1) peek_go(PK.sel);	/* the text: open it */
  return 1;
}


static void locations_ask (int what) {
  if (!HAS_DOC || G->diff) return;
  if (!lsp_active(T->doc)) {
    toast(0, "No %s provider for '%s' files.", what == LOC_REFS ? "reference" : "definition",
          syntax_name(T->sx));
    return;
  }
  lsp_locations(T->doc, T->cur, what);
}

/* }================================================================== */


/*
** {==================================================================
** Go to Symbol in Workspace (Ctrl+T, or '#' in Go to File): the server
** is asked again as the name is typed
** ===================================================================
*/

static struct {
  WSym *v;
  size_t n;
  int fresh;	/* an answer came, not shown yet */
  char asked[512];
  Doc *d;	/* whose server */
} WS;


static void ws_clear (void) {
  size_t i;
  for (i = 0; i < WS.n; i++) {
    free(WS.v[i].name);
    free(WS.v[i].detail);
    free(WS.v[i].path);
  }
  free(WS.v);
  WS.v = NULL;
  WS.n = 0;
}


void on_workspace_symbols (const WSym *v, size_t n) {
  size_t i;
  ws_clear();
  WS.v = (WSym *)xmalloc((n + 1) * sizeof(WSym));
  for (i = 0; i < n; i++) {
    WS.v[i] = v[i];
    WS.v[i].name = xstrdup(v[i].name);
    WS.v[i].detail = xstrdup(v[i].detail);
    WS.v[i].path = xstrdup(v[i].path);
  }
  WS.n = n;
  WS.fresh = 1;
}


static int ws_tick (Pick *p, int changed) {
  size_t i;
  (void)changed;
  if (strcmp(p->text, WS.asked) != 0) {
    snprintf(WS.asked, sizeof(WS.asked), "%s", p->text);
    lsp_workspace_symbols(WS.d, p->text);
  }
  lsp_poll();
  if (!WS.fresh) return 0;
  WS.fresh = 0;
  pick_clear(p);
  for (i = 0; i < WS.n && i < 500; i++) {
    char detail[600];
    const char *root = side_root();
    const char *rel = WS.v[i].path;
    int st;
    size_t rl = strlen(root);
    if (m_strnicmp(rel, root, rl) == 0 && (rel[rl] == '\\' || rel[rl] == '/')) rel += rl + 1;
    snprintf(detail, sizeof(detail), "%s%s%s", WS.v[i].detail, WS.v[i].detail[0] ? "  " : "", rel);
    pick_add(p, WS.v[i].name, detail, (int)sym_icon(WS.v[i].kind, &st));
  }
  return 1;
}


static void workspace_symbols (const char *init) {
  Pick p;
  int r, g, i;
  WS.d = NULL;
  if (HAS_DOC && lsp_active(T->doc)) WS.d = T->doc;
  for (g = 0; g < g_ngrp && WS.d == NULL; g++)	/* any file with a language server */
    for (i = 0; i < g_grp[g].ntab && WS.d == NULL; i++)
      if (lsp_active(g_grp[g].tab[i]->doc)) WS.d = g_grp[g].tab[i]->doc;
  if (WS.d == NULL) {
    toast(0, "No workspace symbol provider: open a file of a language with a server first.");
    return;
  }
  ws_clear();
  WS.asked[0] = '\1';	/* asked at once */
  WS.asked[1] = '\0';
  WS.fresh = 0;
  pick_init(&p, "Type the name of a symbol to open");
  p.prefix = "#";
  p.keep_order = 1;	/* the server's order */
  p.on_tick = ws_tick;
  if (init) snprintf(p.text, sizeof(p.text), "%s", init);
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && (size_t)r < WS.n) on_definition(WS.v[r].path, WS.v[r].p, WS.v[r].utf16);
  ws_clear();
}

/* }================================================================== */


/*
** {==================================================================
** Occurrences: the places of the symbol at the cursor are lit (the
** server's documentHighlight; else the same word), and those of the
** selected text
** ===================================================================
*/

static struct {
  Doc *d;
  unsigned long edits;	/* the text they are for */
  Pos at;	/* the cursor they are for */
  Pos *v;	/* pairs a, b */
  size_t n;
  Pos last;	/* the cursor, as it was last seen */
  int last_sel;
  long long t;	/* when it moved there */
  int due;	/* to be worked out */
} HL;


static void hl_set (Doc *d, Pos at, const Pos *v, size_t n) {
  free(HL.v);
  HL.v = n ? (Pos *)xmalloc(n * 2 * sizeof(Pos)) : NULL;
  if (n) memcpy(HL.v, v, n * 2 * sizeof(Pos));
  HL.n = n;
  HL.d = d;
  HL.at = at;
  HL.edits = d->edits;
}


void on_highlights (Doc *d, Pos at, const Pos *v, size_t n) {
  if (!HAS_DOC || d != T->doc || pos_cmp(at, T->cur) != 0 || T->sel) return;
  hl_set(d, at, v, n);
}


/* the same text, whole words when it is one, around the cursor's part of the file */
static void hl_text (const char *w, size_t wn, int word) {
  Pos *v = NULL;
  size_t n = 0, cap = 0, y, from, to;
  from = T->top > 3000 ? T->top - 3000 : 0;
  to = T->top + 3000 < T->doc->n ? T->top + 3000 : T->doc->n;
  for (y = from; y < to && n < 2000; y++) {
    const Row *r = row_at(y);
    size_t x;
    for (x = 0; x + wn <= r->len; x++) {
      if (memcmp(r->s + x, w, wn) != 0) continue;
      if (word && ((x > 0 && char_class(r, x - 1) == 1) || (x + wn < r->len && char_class(r, x + wn) == 1)))
        continue;
      if (n == cap) v = (Pos *)xrealloc(v, (cap = cap ? cap * 2 : 32) * 2 * sizeof(Pos));
      v[n * 2].y = v[n * 2 + 1].y = y;
      v[n * 2].x = x;
      v[n * 2 + 1].x = x + wn;
      n++;
      x += wn - 1;
    }
  }
  hl_set(T->doc, T->cur, v, n > 1 || !word ? n : 0);	/* a word alone: nothing to show */
  free(v);
}


/* after the cursor rested a moment: ask, or look */
static void hl_idle (void) {
  Pos a, b;
  if (!HAS_DOC || G->diff || !opt.word_hl) return;
  if (pos_cmp(HL.last, T->cur) != 0 || HL.last_sel != T->sel || HL.d != T->doc || HL.edits != T->doc->edits) {
    if (pos_cmp(HL.last, T->cur) != 0 || HL.last_sel != T->sel) HL.t = os_now_us();
    HL.last = T->cur;
    HL.last_sel = T->sel;
    if (HL.d != T->doc || HL.edits != T->doc->edits || pos_cmp(HL.at, T->cur) != 0) {
      HL.due = 1;
      HL.n = 0;
      HL.d = T->doc;
      HL.edits = T->doc->edits;
    }
  }
  if (!HL.due || os_now_us() - HL.t < 250000) return;
  HL.due = 0;
  if (T->sel) {	/* the selected text elsewhere (VS Code's selectionHighlight) */
    sel_range(&a, &b);
    if (a.y == b.y && b.x > a.x && b.x - a.x < 200) {
      const Row *r = row_at(a.y);
      size_t i;
      for (i = a.x; i < b.x && (r->s[i] == ' ' || r->s[i] == '\t'); i++) ;
      if (i < b.x) hl_text(r->s + a.x, b.x - a.x, 0);
    }
    return;
  }
  if (lsp_active(T->doc)) {
    lsp_highlights(T->doc, T->cur);
    return;
  }
  a = b = T->cur;
  a.x = word_start_at(T->cur);
  b.x = word_end_at(T->cur);
  if (b.x > a.x) hl_text(row_at(a.y)->s + a.x, b.x - a.x, 1);
}


static const Pos *hl_ranges (size_t *n) {
  *n = 0;
  if (!HAS_DOC || HL.d != T->doc || HL.edits != T->doc->edits || !opt.word_hl) return NULL;
  *n = HL.n;
  return HL.v;
}


/* after a row is drawn: its occurrences get VS Code's word highlight */
static void hl_row (int sy, size_t y, int gw, size_t from, size_t to, size_t left) {
  size_t n, i;
  const Pos *v = hl_ranges(&n);
  const Row *r = row_at(y);
  uint32_t bg0 = ui_color(C_EDITOR_BG), sel = ui_color(C_SEL_BG), bg = 0;
  int k;
  if (n == 0) return;
  for (k = 0; k < 3; k++) {	/* halfway between the text's color and the selection's */
    uint32_t c0 = (bg0 >> (k * 8)) & 0xFF, c1 = (sel >> (k * 8)) & 0xFF;
    bg |= ((c0 + c1) / 2) << (k * 8);
  }
  for (i = 0; i < n; i++) {
    size_t x0, x1, c0, c1, c, right = left + (size_t)text_cols();
    Pos p;
    if (v[i * 2].y != y) continue;
    x0 = v[i * 2].x;
    x1 = v[i * 2 + 1].y == y ? v[i * 2 + 1].x : r->len;
    if (x0 < from) x0 = from;
    if (x1 > to) x1 = to;
    if (x1 <= x0 || x0 > r->len) continue;
    p.y = y;
    p.x = x0;
    if (T->sel) {	/* the selection itself stays as it is */
      Pos a, b;
      sel_range(&a, &b);
      if (pos_cmp(p, a) >= 0 && pos_cmp(p, b) < 0) continue;
    }
    c0 = vcol(r, y, x0);
    c1 = vcol_end(r, y, x1 > r->len ? r->len : x1);
    for (c = c0; c < c1; c++)
      if (c >= left && c < right) scr_set_bg(L.ed_x + gw + (int)(c - left), sy, bg);
  }
}

/* }================================================================== */


/*
** {==================================================================
** Sticky Scroll: the first lines of the scopes the top line is in stay
** at the top of the editor (VS Code's, by indentation); a click goes
** there
** ===================================================================
*/

static struct {
  size_t line[5];
  int n;
} SS;


static size_t indent_cols (size_t y) {
  const Row *r = row_at(y);
  return col_of(r, indent_end(r));
}


/* the first lines of the scopes line y is in (not y itself), the outermost first */
static int scopes_of (size_t y, size_t *got, int max) {
  size_t h, ind, tmp[16];
  int n = 0, i;
  while (y < T->doc->n && blank(y)) y++;	/* the indent of the first line with text */
  if (y >= T->doc->n) return 0;
  ind = indent_cols(y);
  for (h = y; h-- > 0 && n < 16 && ind > 0;) {
    size_t ic;
    if (blank(h)) continue;
    ic = indent_cols(h);
    if (ic < ind) {
      if (fold_end(h) >= y) tmp[n++] = h;
      ind = ic;
    }
  }
  if (n > max) n = max;
  for (i = 0; i < n; i++) got[i] = tmp[n - 1 - i];
  return n;
}


/*
** VS Code's way: sticky line i is the (i+1)-th scope of the line shown under
** the i lines already stuck, until that line has no more
*/
static int draw_sticky (int gw, Pos sa, Pos sb) {
  int n = 0, i, max;
  SS.n = 0;
  if (!opt.sticky || E.wrap || !HAS_DOC || G->diff || T->top == 0 || T->doc->n > 200000) return 0;
  max = L.text_h / 3 < 5 ? L.text_h / 3 : 5;
  while (n < max) {
    size_t got[5];
    int c = scopes_of(T->top + (size_t)n, got, 5);
    if (c <= n || (n > 0 && got[n - 1] != SS.line[n - 1])) break;
    SS.line[n] = got[n];
    n++;
  }
  for (i = 0; i < n; i++) {
    const Row *r = row_at(SS.line[i]);
    draw_row(L.text_y + i, SS.line[i], gw, sa, sb, 0, r->len, T->left);
  }
  if (n > 0) {	/* a shadow under them */
    int x;
    for (x = L.ed_x; x < L.ed_x + L.ed_w - L.mm_w - L.sb_w; x++) scr_set_bg(x, L.text_y + n - 1, ui_color(C_LINE_BG));
  }
  SS.n = n;
  return n;
}


/* a click on a sticky line: 1, and the editor goes there */
static int sticky_click (Mouse *m) {
  int r = m->y - L.text_y;
  if (SS.n == 0 || r < 0 || r >= SS.n || m->x < L.ed_x || m->x >= L.ed_x + L.ed_w - L.mm_w - L.sb_w) return 0;
  T->nmc = 0;
  T->sel = 0;
  T->cur.y = SS.line[r];
  T->cur.x = 0;
  key_home(0);
  T->top = SS.line[r];
  E.focus = F_EDITOR;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Column (box) selection: Shift+Alt+drag, Ctrl+Shift+Alt+arrows: a
** cursor on each line, from one column to another
** ===================================================================
*/

static struct {
  int on;
  size_t ay, ac, by, bc;	/* where it began (anchor), where it is: line, column */
  Pos left;	/* the main cursor it left: another one ends it */
  const Doc *d;
} CS;


static void mc_add (Pos anchor, Pos cur) {
  if (T->nmc == T->capmc) {
    T->capmc = T->capmc ? T->capmc * 2 : 8;
    T->mc = (Cur *)xrealloc(T->mc, (size_t)T->capmc * sizeof(Cur));
  }
  T->mc[T->nmc].anchor = anchor;
  T->mc[T->nmc].cur = cur;
  T->mc[T->nmc].sel = pos_cmp(anchor, cur) != 0;
  T->mc[T->nmc].want = col_of(row_at(cur.y), cur.x);
  T->nmc++;
}


/* the cursors of the box: a line too short for it gets none (but the main one) */
static void column_build (void) {
  size_t y0 = CS.ay < CS.by ? CS.ay : CS.by, y1 = CS.ay < CS.by ? CS.by : CS.ay, y;
  size_t lo = CS.ac < CS.bc ? CS.ac : CS.bc;
  T->nmc = 0;
  for (y = y0; y <= y1 && y < T->doc->n; y++) {
    const Row *r = row_at(y);
    Pos a, c;
    if (y != CS.by && col_of(r, r->len) < lo) continue;
    a.y = c.y = y;
    a.x = x_of_col(r, CS.ac);
    c.x = x_of_col(r, CS.bc);
    if (y == CS.by) {
      T->anchor = a;
      T->cur = c;
      T->sel = pos_cmp(a, c) != 0;
      T->want = CS.bc;
    }
    else mc_add(a, c);
  }
  CS.left = T->cur;
  CS.d = T->doc;
  CS.on = 1;
}


static void column_start (void) {
  if (CS.on && CS.d == T->doc && pos_cmp(CS.left, T->cur) == 0) return;
  CS.ay = CS.by = T->cur.y;
  CS.ac = CS.bc = col_of(row_at(T->cur.y), T->cur.x);
}


/* Ctrl+Shift+Alt+arrows */
static void column_key (int code) {
  column_start();
  switch (code) {
    case K_UP: if (CS.by > 0) CS.by--; break;
    case K_DOWN: if (CS.by + 1 < T->doc->n) CS.by++; break;
    case K_PGUP: CS.by = CS.by > (size_t)L.text_h ? CS.by - (size_t)L.text_h : 0; break;
    case K_PGDN: CS.by = CS.by + (size_t)L.text_h < T->doc->n ? CS.by + (size_t)L.text_h : T->doc->n - 1; break;
    case K_LEFT: if (CS.bc > 0) CS.bc--; break;
    case K_RIGHT: CS.bc++; break;
  }
  column_build();
}


/* the line and column under the mouse */
static void mouse_line_col (Mouse *m, size_t *y, size_t *col) {
  size_t from, to, left;
  long ry = m->y - L.text_y, cx = m->x - L.ed_x - gutter_width();
  if (ry < 0) ry = 0;
  if (ry >= L.text_h) ry = L.text_h - 1;
  if (!vis_goto((int)ry, y, &from, &to, &left)) {
    *y = T->doc->n - 1;
    left = T->left;
  }
  *col = cx < 0 ? left : left + (size_t)cx;
}


/* Shift+Alt+press, then drag */
static void column_mouse (Mouse *m, int start) {
  size_t y, col;
  mouse_line_col(m, &y, &col);
  if (start) {
    CS.ay = y;
    CS.ac = col;
  }
  CS.by = y;
  CS.bc = col;
  column_build();
}

/* }================================================================== */


/*
** {==================================================================
** Text: case, sorting, joining, trimming; brackets; language, line
** ends and indentation (the status bar's items)
** ===================================================================
*/

/* the selection, or the word at the cursor: a and b; 0 when there is nothing */
static int sel_or_word (Pos *a, Pos *b) {
  if (T->sel) {
    sel_range(a, b);
    return pos_cmp(*a, *b) < 0;
  }
  *a = *b = T->cur;
  a->x = word_start_at(T->cur);
  b->x = word_end_at(T->cur);
  return b->x > a->x;
}


/* a..b in place of the text there; the selection covers it after */
static void replace_range (Pos a, Pos b, const char *s, size_t n, int select) {
  Pos e;
  ed_delete(a, b);
  e = ed_insert(a, s, n);
  if (select) {
    T->anchor = a;
    T->cur = e;
    T->sel = pos_cmp(a, e) != 0;
  }
  else T->cur = doc_clamp(T->doc, T->cur);
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


/* Transform to Uppercase (1), Lowercase (2), Title Case (3) */
static void transform_case (int mode) {
  Pos a, b;
  size_t len, i;
  char *t;
  int start = 1;
  if (!sel_or_word(&a, &b)) return;
  t = doc_text(T->doc, a, b, &len);
  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)t[i];
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (mode == 1 || (mode == 3 && start)) {
      if (c >= 'a' && c <= 'z') t[i] = (char)(c - 32);
    }
    else if (c >= 'A' && c <= 'Z') t[i] = (char)(c + 32);
    start = !(letter || c >= 0x80 || (c >= '0' && c <= '9') || c == '\'');
  }
  doc_group(T->doc);
  replace_range(a, b, t, len, T->sel);
  doc_group(T->doc);
  free(t);
}


/* the lines the command is for: the selection's, or (none) every one */
static void lines_or_all (size_t *ly, size_t *hy) {
  if (T->sel) sel_lines(ly, hy);
  else {
    *ly = 0;
    *hy = T->doc->n - 1;
    if (*hy > 0 && row_at(*hy)->len == 0) (*hy)--;	/* the file's last newline stays last */
  }
  if (*hy >= T->doc->n) *hy = T->doc->n - 1;
}


/* lines ly .. hy become v[0 .. n) */
static void set_lines (size_t ly, size_t hy, char **v, size_t *len, size_t n) {
  Buf o;
  size_t i;
  Pos a, b;
  buf_init(&o);
  for (i = 0; i < n; i++) {
    if (i) buf_putc(&o, '\n');
    buf_putn(&o, v[i], len[i]);
  }
  a.y = ly;
  a.x = 0;
  b.y = hy;
  b.x = row_at(hy)->len;
  doc_group(T->doc);
  ed_delete(a, b);
  ed_insert(a, o.s ? o.s : "", o.len);
  doc_group(T->doc);
  buf_free(&o);
  if (T->sel) {	/* the lines stay selected */
    T->anchor.y = ly;
    T->anchor.x = 0;
    T->cur.y = ly + n - 1;
    T->cur.x = row_at(T->cur.y)->len;
  }
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
}


static int g_sort_desc;

/* VS Code's collator: letters without case first, then case */
static int cmp_line (const void *a, const void *b) {
  const char *x = *(char *const *)a, *y = *(char *const *)b;
  size_t i;
  int c = 0;
  for (i = 0; x[i] && y[i]; i++) {
    int p = lower((unsigned char)x[i]), q = lower((unsigned char)y[i]);
    if (p != q) {
      c = p - q;
      break;
    }
  }
  if (c == 0) c = (x[i] != 0) - (y[i] != 0);
  if (c == 0) c = -strcmp(x, y);	/* the lowercase one first */
  return g_sort_desc ? -c : c;
}


/* Sort Lines Ascending / Descending; Delete Duplicate Lines (dedup) */
static void sort_lines (int desc, int dedup) {
  size_t ly, hy, n, i, m = 0, *len;
  char **v;
  lines_or_all(&ly, &hy);
  n = hy - ly + 1;
  if (n < 2) return;
  v = (char **)xmalloc(n * sizeof(char *));
  len = (size_t *)xmalloc(n * sizeof(size_t));
  for (i = 0; i < n; i++) {
    const Row *r = row_at(ly + i);
    v[i] = (char *)xmalloc(r->len + 1);
    memcpy(v[i], r->s, r->len);
    v[i][r->len] = '\0';
  }
  if (!dedup) {
    g_sort_desc = desc;
    qsort(v, n, sizeof(char *), cmp_line);
    m = n;
  }
  else
    for (i = 0; i < n; i++) {	/* the first of each stays, in its place */
      size_t j;
      for (j = 0; j < m && strcmp(v[j], v[i]) != 0; j++) ;
      if (j < m) free(v[i]);
      else v[m++] = v[i];
    }
  for (i = 0; i < m; i++) len[i] = strlen(v[i]);
  set_lines(ly, hy, v, len, m);
  for (i = 0; i < m; i++) free(v[i]);
  free(v);
  free(len);
}


/* Join Lines: the selected lines (or this and the next) with one space */
static void join_lines (void) {
  size_t ly, hy, y;
  Pos at;
  sel_lines(&ly, &hy);
  if (hy == ly) hy = ly + 1;
  if (hy >= T->doc->n) return;
  doc_group(T->doc);
  at = T->cur;
  for (y = ly; y < hy; y++) {	/* each time the next line comes up to this one */
    const Row *r = row_at(ly), *nx = row_at(ly + 1);
    size_t ind = indent_end(nx), end = r->len, nlen = nx->len;
    Pos a, b;
    while (end > 0 && (r->s[end - 1] == ' ' || r->s[end - 1] == '\t')) end--;
    a.y = ly;
    a.x = end;
    b.y = ly + 1;
    b.x = ind;
    ed_delete(a, b);
    if (end > 0 && ind < nlen) ed_insert(a, " ", 1);
    at = a;
  }
  doc_group(T->doc);
  T->sel = 0;
  T->cur = doc_clamp(T->doc, at);
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


/* Ctrl+K Ctrl+X: every line's trailing spaces and tabs go */
static void trim_trailing (void) {
  size_t y;
  doc_group(T->doc);
  for (y = 0; y < T->doc->n; y++) {
    const Row *r = row_at(y);
    size_t e = r->len;
    Pos a, b;
    while (e > 0 && (r->s[e - 1] == ' ' || r->s[e - 1] == '\t')) e--;
    if (e == r->len) continue;
    a.y = b.y = y;
    a.x = e;
    b.x = r->len;
    ed_delete(a, b);
    keep_del(a, b);
  }
  doc_group(T->doc);
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
}


/* Duplicate Selection: the selection again after it, selected; the line without one */
static void duplicate_selection (void) {
  Pos a, b, e;
  size_t len;
  char *t;
  if (!T->sel) {
    copy_lines(1);
    return;
  }
  sel_range(&a, &b);
  t = doc_text(T->doc, a, b, &len);
  doc_group(T->doc);
  e = ed_insert(b, t, len);
  doc_group(T->doc);
  T->anchor = b;
  T->cur = e;
  T->sel = 1;
  free(t);
}


/* the bracket pair around the cursor (at it, else the one it is inside): 1 found */
static int enclosing_brackets (Pos *o, Pos *c) {
  Pos a, b;
  size_t y = T->cur.y, guard = 0;
  long x = (long)T->cur.x - 1;
  int depth = 0;
  if (cursor_brackets(&a, &b)) {
    if (pos_cmp(a, b) < 0) {
      *o = a;
      *c = b;
    }
    else {
      *o = b;
      *c = a;
    }
    return 1;
  }
  for (;;) {	/* back to the open bracket not closed before the cursor */
    const Row *r = row_at(y);
    const unsigned char *tok = line_tokens(y);
    if (x >= (long)r->len) x = (long)r->len - 1;
    for (; x >= 0; x--) {
      int ch = (unsigned char)r->s[x];
      if (!code_at(tok, (size_t)x)) continue;
      if (is_close(ch)) depth++;
      else if (is_open(ch)) {
        if (depth == 0) {
          o->y = y;
          o->x = (size_t)x;
          return match_bracket(*o, c);
        }
        depth--;
      }
    }
    if (y == 0 || ++guard > 5000) return 0;
    y--;
    x = (long)row_at(y)->len - 1;
  }
}


/* Ctrl+Shift+\: to the other bracket of the pair; Select to Bracket */
static void bracket_cmd (int select) {
  Pos o, c;
  if (!enclosing_brackets(&o, &c)) return;
  if (select) {
    T->anchor = o;
    T->cur = c;
    T->cur.x++;
    T->sel = 1;
    return;
  }
  if (pos_cmp(T->cur, o) == 0 || (T->cur.y == o.y && T->cur.x == o.x + 1)) move_h(c, 0);
  else move_h(o, 0);
}


/* Reopen Closed Editor: the files closed, the last one last */
static struct {
  char *path[20];
  Pos at[20];
  int n;
} RC;


static void closed_push (const Tab *t) {
  if (t->doc->path == NULL) return;
  if (RC.n == 20) {
    free(RC.path[0]);
    memmove(RC.path, RC.path + 1, 19 * sizeof(char *));
    memmove(RC.at, RC.at + 1, 19 * sizeof(Pos));
    RC.n--;
  }
  RC.path[RC.n] = xstrdup(t->doc->path);
  RC.at[RC.n++] = t->cur;
}


static void reopen_closed (void) {
  char *path;
  Pos at;
  if (RC.n == 0) return;
  path = RC.path[--RC.n];
  at = RC.at[RC.n];
  if (open_file(path, 0) == 0) {
    move_h(doc_clamp(T->doc, at), 0);
    center_cursor();
    E.focus = F_EDITOR;
  }
  free(path);
}


/* Change Language Mode: the highlighting (and the language server) of another language */
static void change_language (void) {
  Pick p;
  int i, r, n = syntax_count();
  if (!HAS_DOC || G->diff) return;
  pick_init(&p, "Select Language Mode");
  pick_add(&p, "Plain Text", T->sx == NULL ? "current" : NULL, 0xEA7B);
  for (i = 0; i < n; i++) pick_add(&p, syntax_name(syntax_nth(i)), syntax_nth(i) == T->sx ? "current" : NULL, 0xEA7B);
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) return;
  T->sx = r == 0 ? NULL : syntax_nth(r - 1);
  T->doc->hl_n = 0;	/* highlighted again */
  T->doc->hl_from = 0;
  if (T->sx && T->doc->path) lsp_open(T->doc, syntax_name(T->sx));
}


/* Change End of Line Sequence: LF or CRLF, for the next save */
static void change_eol (void) {
  static const char *const bt[] = {"LF", "CRLF"};
  Pick p;
  int r;
  if (!HAS_DOC || G->diff) return;
  pick_init(&p, "Select End of Line Sequence");
  pick_add(&p, bt[0], T->doc->crlf ? NULL : "current", 0);
  pick_add(&p, bt[1], T->doc->crlf ? "current" : NULL, 0);
  p.keep_order = 1;
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0 || r == T->doc->crlf) return;
  T->doc->crlf = r;
  T->doc->changes++;	/* it is saved again */
}


/* leading tabs to spaces (to_tabs 0) or spaces to tabs, every line */
static void convert_indent (int to_tabs) {
  size_t y, iw = (size_t)(T->doc->indent > 0 ? T->doc->indent : 4);
  doc_group(T->doc);
  for (y = 0; y < T->doc->n; y++) {
    const Row *r = row_at(y);
    size_t e = indent_end(r), cols = col_of(r, e), n;
    char *s;
    Pos a, b;
    if (e == 0) continue;
    if (to_tabs) {
      n = cols / iw + cols % iw;
      s = (char *)xmalloc(n + 1);
      memset(s, '\t', cols / iw);
      memset(s + cols / iw, ' ', cols % iw);
    }
    else {
      n = cols;
      s = (char *)xmalloc(n + 1);
      memset(s, ' ', n);
    }
    if (n != e || memcmp(s, r->s, n) != 0) {
      a.y = b.y = y;
      a.x = 0;
      b.x = e;
      ed_delete(a, b);
      ed_insert(a, s, n);
      if (T->cur.y == y) T->cur.x = T->cur.x >= e ? T->cur.x - e + n : n;
    }
    free(s);
  }
  doc_group(T->doc);
  T->doc->tabs = to_tabs;
  T->sel = 0;
  T->cur = doc_clamp(T->doc, T->cur);
}


/* Indent Using Spaces / Tabs: how many columns an indent is */
static void indent_using (int tabs) {
  Pick p;
  int r, i;
  char s[8];
  if (!HAS_DOC || G->diff) return;
  pick_init(&p, "Select Tab Size for Current File");
  for (i = 1; i <= 8; i++) {
    snprintf(s, sizeof(s), "%d", i);
    pick_add(&p, s, i == T->doc->indent ? "Configured Tab Size" : NULL, 0);
  }
  p.keep_order = 1;
  p.start = T->doc->indent - 1;
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) return;
  T->doc->tabs = tabs;
  T->doc->indent = r + 1;
}


/* the status bar's "Spaces: 4": VS Code's choices */
static void change_indentation (void) {
  static const int cmds[] = {CMD_INDENT_SPACES, CMD_INDENT_TABS, CMD_DETECT_INDENT, CMD_TO_SPACES, CMD_TO_TABS};
  Pick p;
  int r;
  size_t i;
  if (!HAS_DOC || G->diff) return;
  pick_init(&p, "Select Action");
  for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) pick_add(&p, cmd_name(cmds[i]), NULL, 0);
  p.keep_order = 1;
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) run_command(cmds[r]);
}

/* }================================================================== */


/*
** {==================================================================
** Commands
** ===================================================================
*/

static void show_view (int v) {
  E.side = 1;
  E.view = v;
  E.focus = F_SIDE;
  if (v == VIEW_GIT) git_refresh();
  if (v == VIEW_EXT) ext_show();
  if (v == VIEW_TEST) test_show();
  if (v == VIEW_FILES && T->real) side_reveal(T->real);
}


static void save_as (void) {
  char *name = ask_text("Save As: the path of the file", T->doc->path ? T->doc->path : side_root());
  if (name == NULL || *name == '\0') {
    free(name);
    return;
  }
  if (!is_absolute(name)) {
    char *full = path_join(side_root(), name);
    free(name);
    name = full;
  }
  free(T->doc->path);
  T->doc->path = name;
  set_real();
}


static void goto_line (const char *init) {
  Pick p;
  char hint[160];
  int r;
  pick_init(&p, NULL);
  p.prefix = ":";
  if (init) snprintf(p.text, sizeof(p.text), "%s", init);
  snprintf(hint, sizeof(hint), "Current Line: %lu, Character: %lu. Type a line number between 1 and %lu to navigate to.",
           (unsigned long)(T->cur.y + 1), (unsigned long)(T->cur.x + 1), (unsigned long)T->doc->n);
  p.hint = hint;
  r = pick_run(&p);
  if (r == PICK_TEXT) {
    unsigned long line = strtoul(p.text, NULL, 10), col = 0;
    const char *c = strchr(p.text, ':');
    Pos q;
    if (c) col = strtoul(c + 1, NULL, 10);
    if (line > 0) {
      q.y = line - 1;
      q.x = col > 0 ? col - 1 : 0;
      move_h(doc_clamp(T->doc, q), 0);
      center_cursor();
    }
  }
  pick_free(&p);
}


/* Pin / Unpin: a pinned tab goes with the pinned ones, first */
static void pin_tab (int pin) {
  int i = G->active, k, to;
  Tab *t;
  if (!HAS_DOC || G->diff) return;
  t = G->tab[i];
  if (t->pinned == pin) return;
  for (k = 0; k < G->ntab && G->tab[k]->pinned; k++) ;	/* the pinned ones: 0..k-1 */
  t->pinned = pin;
  if (pin) t->preview = 0;
  to = pin ? k : k - 1;
  memmove(G->tab + i, G->tab + i + 1, (size_t)(G->ntab - i - 1) * sizeof(Tab *));
  memmove(G->tab + to + 1, G->tab + to, (size_t)(G->ntab - 1 - to) * sizeof(Tab *));
  G->tab[to] = t;
  G->active = to;
}


enum { CLOSE_OTHERS, CLOSE_RIGHT, CLOSE_SAVED, CLOSE_ALL };

/* the group's tabs go but the pinned ones (and the one in front for Others); a Cancel stops */
static void close_many (int what) {
  Tab *keep = HAS_DOC && !G->diff ? T : NULL;
  int i, on = G->active, k;
  for (i = G->ntab - 1; i >= 0; i--) {
    Tab *t = G->tab[i];
    int n0 = G->ntab;
    if (t->pinned && what != CLOSE_SAVED) continue;
    if (what == CLOSE_OTHERS && t == keep) continue;
    if (what == CLOSE_RIGHT && (i <= on || !keep)) continue;
    if (what == CLOSE_SAVED && doc_dirty(t->doc)) continue;
    if (t == keep) keep = NULL;
    close_tab(i);
    if (G->ntab == n0) return;	/* Cancel */
    if (keep)
      for (k = 0; k < G->ntab; k++)
        if (G->tab[k] == keep) {
          G->active = k;
          T = keep;
        }
  }
}


/* the right-click menu of tab i (at column x) */
static void tab_menu (int i, int x) {
  static const int cmd[] = {CMD_CLOSE, CMD_CLOSE_OTHERS, CMD_CLOSE_RIGHT, CMD_CLOSE_SAVED, CMD_CLOSE_GROUP_ALL, 0,
                            CMD_COPY_PATH, CMD_REVEAL, 0, CMD_KEEP_OPEN, CMD_PIN, 0, CMD_SPLIT, -1};
  const char *label[] = {"Close", "Close Others", "Close to the Right", "Close Saved", "Close All", NULL,
                         "Copy Path", "Reveal in Explorer View", NULL, "Keep Open", "Pin", NULL, "Split Right"};
  int c;
  focus_tab(i);
  if (T->pinned) label[10] = "Unpin";
  c = menu_popup(x, L.ed_y + 1, cmd, label);
  if (c == CMD_PIN && T->pinned) c = CMD_UNPIN;
  if (c != CMD_NONE) run_command(c);
}


static long g_used;	/* the clock of Tab.used */
static Tab *g_used_tab;

/* the main loop: the tab in front gets the time it came there */
static void mru_track (void) {
  if (HAS_DOC && T != g_used_tab) {
    T->used = ++g_used;
    g_used_tab = T;
  }
}


static Tab **g_mru;
static int *g_mru_grp;

static int cmp_used (const void *a, const void *b) {
  long x = g_mru[*(const int *)a]->used, y = g_mru[*(const int *)b]->used;
  return x < y ? 1 : x > y ? -1 : 0;
}


/*
** Ctrl+Tab: the group's editors, the one used last first, the one before
** it selected (more Ctrl+Tab goes on); all: every group's (Show All Editors).
*/
static void switch_editor (int all, int back) {
  Pick p;
  int n = 0, g, i, *ord, r, cap = 0;
  for (g = 0; g < g_ngrp; g++) cap += g_grp[g].ntab;
  if (cap == 0) return;
  g_mru = (Tab **)xmalloc((size_t)cap * sizeof(Tab *));
  g_mru_grp = (int *)xmalloc((size_t)cap * sizeof(int));
  ord = (int *)xmalloc((size_t)cap * sizeof(int));
  for (g = 0; g < g_ngrp; g++) {
    if (!all && &g_grp[g] != G) continue;
    for (i = 0; i < g_grp[g].ntab; i++) {
      g_mru[n] = g_grp[g].tab[i];
      g_mru_grp[n] = g;
      ord[n] = n;
      n++;
    }
  }
  qsort(ord, (size_t)n, sizeof(int), cmp_used);
  pick_init(&p, all ? "Search open editors by name" : "Select an editor (Ctrl+Tab for the next)");
  p.keep_order = 1;
  for (i = 0; i < n; i++) {
    Tab *t = g_mru[ord[i]];
    char *d = t->real ? rel_dir(t->real) : NULL;
    int ist;
    char det[300];
    snprintf(det, sizeof(det), "%s%s%s", d ? d : "", doc_dirty(t->doc) ? (d ? " ●" : "●") : "",
             "");
    if (all && g_ngrp > 1) {
      size_t dl = strlen(det);
      snprintf(det + dl, sizeof(det) - dl, "  Group %d", g_mru_grp[ord[i]] + 1);
    }
    pick_add(&p, tab_name(t), det[0] ? det : NULL, (int)file_icon(tab_name(t), &ist));
    free(d);
  }
  p.start = n > 1 ? (back ? n - 1 : 1) : 0;
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) {
    Tab *t = g_mru[ord[r]];
    focus_group(g_mru_grp[ord[r]]);
    for (i = 0; i < G->ntab; i++)
      if (G->tab[i] == t) focus_tab(i);
    E.focus = F_EDITOR;
  }
  free(g_mru);
  free(g_mru_grp);
  free(ord);
  g_mru = NULL;
}


/* the Command Palette's recently used commands, the last first (their ids, in mme-data) */
static Vec g_cmd_used;
static int g_cmd_used_read;

static void used_load (void) {
  char *f, *s, *p;
  size_t len;
  if (g_cmd_used_read) return;
  g_cmd_used_read = 1;
  vec_init(&g_cmd_used);
  f = data_path("commands");
  s = read_file(f, &len);
  free(f);
  for (p = s; p && *p;) {
    char *e = strchr(p, '\n');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n && p[n - 1] == '\r') n--;
    if (n) vec_push(&g_cmd_used, xstrndup(p, n));
    p = e ? e + 1 : p + strlen(p);
  }
  free(s);
}


static void used_add (int cmd) {
  const char *id = cmd_id(cmd);
  char *f;
  Buf b;
  size_t i;
  int fd;
  if (!*id) return;
  used_load();
  for (i = 0; i < g_cmd_used.n; i++)
    if (strcmp(g_cmd_used.v[i], id) == 0) {
      free(g_cmd_used.v[i]);
      memmove(g_cmd_used.v + i, g_cmd_used.v + i + 1, (g_cmd_used.n - i - 1) * sizeof(char *));
      g_cmd_used.n--;
      break;
    }
  vec_insert(&g_cmd_used, 0, xstrdup(id));
  while (g_cmd_used.n > 50) free(g_cmd_used.v[--g_cmd_used.n]);
  buf_init(&b);
  for (i = 0; i < g_cmd_used.n; i++) buf_printf(&b, "%s\n", g_cmd_used.v[i]);
  f = data_path("commands");
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  free(f);
  buf_free(&b);
}


static void palette (const char *init) {
  Pick p;
  int cmds[CMD_N], n = 0, c, r;
  char used[CMD_N];
  size_t i;
  pick_init(&p, NULL);
  if (init) snprintf(p.text, sizeof(p.text), "%s", init);
  p.prefix = ">";
  used_load();
  memset(used, 0, sizeof(used));
  for (i = 0; i < g_cmd_used.n; i++) {	/* VS Code's "recently used" first */
    char d[96];
    c = cmd_by_id(g_cmd_used.v[i]);
    if (c <= 0 || c == CMD_PALETTE || used[c]) continue;
    used[c] = 1;
    if (n == 0) snprintf(d, sizeof(d), "%s%srecently used", cmd_keys(c), *cmd_keys(c) ? "   " : "");
    else snprintf(d, sizeof(d), "%s", cmd_keys(c));
    cmds[n++] = c;
    pick_add(&p, cmd_name(c), d, 0);
  }
  for (c = 1; c < CMD_N; c++) {
    char d[96];
    if (c == CMD_PALETTE || used[c]) continue;
    if (n > 0 && used[0] == 0) snprintf(d, sizeof(d), "%s%sother commands", cmd_keys(c), *cmd_keys(c) ? "   " : "");
    else snprintf(d, sizeof(d), "%s", cmd_keys(c));
    used[0] = 1;	/* the label only once */
    cmds[n++] = c;
    pick_add(&p, cmd_name(c), d, 0);
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) {
    used_add(cmds[r]);
    run_command(cmds[r]);
  }
}


/*
** {==================================================================
** Pages: the Settings editor and Welcome, in an editor tab
** ===================================================================
*/

static Vec g_wrecent;	/* the Welcome page's recent folders */


/* the page kind in a tab of this group (that tab to the front), or a new tab */
static void page_open (int kind) {
  int i;
  for (i = 0; i < G->ntab && G->tab[i]->page != kind; i++) ;
  if (i < G->ntab) focus_tab(i);
  else {
    tab_new();
    T->page = kind;
  }
  E.focus = F_EDITOR;
  if (kind == PAGE_SETTINGS) sui_open();
  else {
    vec_free(&g_wrecent);
    recent_load(&g_wrecent);
  }
}


/* text to the clipboard (Copy Path, Copy Setting ID ...) */
static void clip_text (const char *s) {
  free(E.clip);
  E.clip = xstrdup(s);
  E.cliplen = strlen(s);
  osc52(E.clip, E.cliplen);
}


static void page_act (const PageAct *a) {
  switch (a->what) {
    case PA_APPLY: apply_settings(0); break;
    case PA_CMD: run_command(a->cmd); break;
    case PA_COPY: clip_text(a->text); break;
    case PA_FOLDER: {
      char *d = xstrdup(a->text);
      open_folder(d);
      free(d);
      break;
    }
  }
}


static void page_draw (int other) {
  int y = L.ed_y + 1, h = L.text_y + L.text_h - y, focus = !other && E.focus == F_EDITOR;
  if (T->page == PAGE_SETTINGS) sui_draw(L.ed_x, y, L.ed_w, h, focus);
  else welcome_draw(L.ed_x, y, L.ed_w, h, focus, &g_wrecent);
}


static void page_key (int k) {
  PageAct a;
  if (T->page == PAGE_SETTINGS) sui_key(k, &a);
  else welcome_key(k, &g_wrecent, &a);
  page_act(&a);
}


static void page_mouse (Mouse *m) {
  PageAct a;
  if (T->page == PAGE_SETTINGS) sui_mouse(m, &a);
  else welcome_mouse(m, &g_wrecent, &a);
  page_act(&a);
}

/* }================================================================== */


/* a path to the clipboard: the one selected in the Explorer, else the file in front */
static void copy_path (int relative) {
  const char *p = NULL, *root = side_root();
  size_t rl = strlen(root);
  if (E.focus == F_SIDE && E.side && E.view == VIEW_FILES && files_selected()) p = files_selected();
  else if (HAS_DOC && !T->page && T->doc->path) p = T->real ? T->real : T->doc->path;
  if (p == NULL) return;
  if (relative && m_fnncmp(p, root, rl) == 0 && path_is_sep(p[rl])) p += rl + 1;
  clip_text(p);
}


static void apply_act (const SideAct *act);

static void explorer_cmd (int what) {
  SideAct act;
  files_index_stale();	/* a file may come or go: Go to File walks again */
  if (!E.side || E.view != VIEW_FILES) show_view(VIEW_FILES);
  E.side = 1;
  E.focus = F_SIDE;
  E.outline_focus = 0;
  memset(&act, 0, sizeof(act));
  files_cmd(what, &act);
  apply_act(&act);
}


/* "Ctrl+Shift+P", "Ctrl+K Ctrl+T" */
static void keys_text (int k1, int k2, char *out, size_t n) {
  char a[48], b[48];
  if (k1 == 0) {
    snprintf(out, n, "-");
    return;
  }
  key_name(k1, 1, a, sizeof(a));
  if (k2) {
    key_name(k2, 1, b, sizeof(b));
    snprintf(out, n, "%s %s", a, b);
  }
  else snprintf(out, n, "%s", a);
}


/*
** Preferences: Import VS Code Settings: VS Code's User folder (or one
** picked) is read, what mme has of it shown, and on OK taken into mme-data.
*/
static void import_vscode (void) {
  Vec dirs;
  Pick p;
  size_t i;
  int r, ns, nk, nsn;
  char *dir = NULL, *pre, msg[600];
  static const char *const bt[] = {"Import", "Cancel"};
  import_find(&dirs);
  pick_init(&p, "Import VS Code Settings from");
  p.keep_order = 1;
  for (i = 0; i < dirs.n; i++) pick_add(&p, dirs.v[i], NULL, 0xEA83);
  pick_add(&p, "Select Folder...", "VS Code's User folder, or a copy of it", 0xEA83);
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && (size_t)r < dirs.n) dir = xstrdup(dirs.v[r]);
  else if (r >= 0) dir = file_dialog("VS Code's User Folder", 1);
  vec_free(&dirs);
  if (dir == NULL) return;
  pre = import_preview(dir);
  snprintf(msg, sizeof(msg), "Import from %s?", dir);
  if (dialog(msg, pre, bt, 2) == 0) {
    import_run(dir, &ns, &nk, &nsn);
    apply_settings(0);
    keys_load();
    snip_reload();
    toast(0, "Imported %d settings, %d keybindings and %d snippet files from VS Code", ns, nk, nsn);
  }
  free(pre);
  free(dir);
}


/* keybindings.json in a tab (made when there is none) */
static void open_keys_json (void) {
  char *f = keys_path();
  OsStat st;
  if (f == NULL) return;
  if (os_stat(f, &st) != 0 || !st.exists) keys_save();
  if (open_file(f, 0) == 0) E.focus = F_EDITOR;
  free(f);
}


/*
** Ctrl+K Ctrl+S: every command with its keys, the When and the Source
** (Default: mme's, User: keybindings.json) like VS Code's editor; Enter
** on one: change its keys or when, remove it, reset it.
*/
static void keyboard_shortcuts (void) {
  int start = 0;
  for (;;) {
    Pick p;
    int *rcmd, *rent, n = 0, cap = CMD_N * 2 + keys_count() + 2, c, i, r, k1, k2;
    char title[200];
    rcmd = (int *)xmalloc((size_t)cap * sizeof(int));
    rent = (int *)xmalloc((size_t)cap * sizeof(int));
    pick_init(&p, "Type to search in keybindings");
    p.start = start;
    rcmd[n] = CMD_NONE;
    rent[n++] = -1;
    pick_add(&p, "Open Keyboard Shortcuts (JSON)", "keybindings.json", 0);
    for (c = 1; c < CMD_N; c++) {
      char l[260], d[400], kt[100];
      int any = 0, removed = 0, e;
      for (i = 0; i < keys_count(); i++) {	/* a "-command" of its default */
        const char *w;
        int ec, a1, a2;
        if (keys_entry(i, &ec, &a1, &a2, &w) && ec == c) removed = 1;
      }
      if ((k1 = keys_default(c, &k2)) != 0) {	/* mme's own */
        keys_text(k1, k2, kt, sizeof(kt));
        snprintf(l, sizeof(l), "%-30s %s", cmd_name(c), kt);
        snprintf(d, sizeof(d), "%s   Default%s", cmd_id(c), removed ? " (removed)" : "");
        rcmd[n] = c;
        rent[n++] = -1;
        pick_add(&p, l, d, 0);
        any = 1;
      }
      for (e = 0; e < keys_count(); e++) {	/* the user's */
        const char *w;
        int ec, rm = keys_entry(e, &ec, &k1, &k2, &w);
        if (ec != c || rm) continue;
        keys_text(k1, k2, kt, sizeof(kt));
        snprintf(l, sizeof(l), "%-30s %s", cmd_name(c), kt);
        snprintf(d, sizeof(d), "%s   User%s%s", cmd_id(c), w ? "   when: " : "", w ? w : "");
        rcmd[n] = c;
        rent[n++] = e;
        pick_add(&p, l, d, 0);
        any = 1;
      }
      if (!any) {
        snprintf(l, sizeof(l), "%-30s -", cmd_name(c));
        rcmd[n] = c;
        rent[n++] = -1;
        pick_add(&p, l, cmd_id(c), 0);
      }
    }
    r = pick_run(&p);
    pick_free(&p);
    if (r < 0) {
      free(rcmd);
      free(rent);
      return;
    }
    c = rcmd[r];
    i = rent[r];
    free(rcmd);
    free(rent);
    start = r;
    if (c == CMD_NONE) {
      open_keys_json();
      return;
    }
    {	/* what to do with it */
      static const char *const acts[] = {"Change Keybinding...", "Change When Expression...", "Remove Keybinding",
                                         "Reset Keybinding", "Copy Command ID"};
      Pick q;
      int a, dk1, dk2;
      pick_init(&q, cmd_name(c));
      q.keep_order = 1;
      for (a = 0; a < 5; a++) pick_add(&q, acts[a], NULL, 0);
      a = pick_run(&q);
      pick_free(&q);
      dk1 = keys_default(c, &dk2);
      if (a == 0) {
        snprintf(title, sizeof(title), "%s", cmd_name(c));
        compose();
        k1 = key_capture(title, &k2);
        if (k1) {
          keys_set(c, k1, k2);
          toast(0, "%s: %s", cmd_name(c), cmd_keys(c));
        }
      }
      else if (a == 1) {
        const char *w = NULL;
        char *nw;
        int ec;
        if (i >= 0) keys_entry(i, &ec, &k1, &k2, &w);
        nw = ask_text("Type when expression and press Enter", w ? w : "");
        if (nw == NULL) continue;
        if (i >= 0) keys_set_when(i, nw);
        else if (dk1) {	/* a default: the user's copy of it gets the when, the default goes */
          keys_add(c, dk1, dk2, 1, NULL);
          keys_add(c, dk1, dk2, 0, nw);
        }
        free(nw);
      }
      else if (a == 2) {
        if (i >= 0) keys_del(i);
        else if (dk1) keys_add(c, dk1, dk2, 1, NULL);	/* "-command" */
      }
      else if (a == 3) keys_reset(c);
      else if (a == 4) {
        free(E.clip);
        E.clip = xstrdup(cmd_id(c));
        E.cliplen = strlen(E.clip);
        osc52(E.clip, E.cliplen);
      }
    }
  }
}


/* Shift+Alt+F: the language server formats the file (wait: until it did, for a save) */
static struct {
  Doc *d;
  int done;
} FM;


void on_format (Doc *d, const TextEdit *v, size_t n, int failed) {
  Group *g0 = G;
  Tab *t0 = T, *t = NULL;
  size_t i, *ord;
  int g, k;
  if (d == FM.d) FM.done = 1;
  if (failed) {
    toast(1, "The language server could not format the file");
    return;
  }
  if (n == 0) return;
  if (HAS_DOC && T->doc == d) t = T;
  for (g = 0; g < g_ngrp && t == NULL; g++)
    for (k = 0; k < g_grp[g].ntab && t == NULL; k++)
      if (g_grp[g].tab[k]->doc == d) t = g_grp[g].tab[k];
  if (t == NULL) return;
  ord = (size_t *)xmalloc(n * sizeof(size_t));
  for (i = 0; i < n; i++) ord[i] = i;
  g_ev = v;
  qsort(ord, n, sizeof(size_t), cmp_edit);
  T = t;
  doc_group(T->doc);
  for (i = 0; i < n; i++) {	/* the last first: the lines before stay where they were */
    const TextEdit *e = &v[ord[i]];
    Pos a, b, end;
    if (e->l0 >= T->doc->n) continue;
    a.y = e->l0;
    a.x = units_to_x(row_at(e->l0), e->c0, e->utf16);
    b.y = e->l1 < T->doc->n ? e->l1 : T->doc->n - 1;
    b.x = units_to_x(row_at(b.y), e->l1 < T->doc->n ? e->c1 : row_at(b.y)->len, e->utf16);
    if (pos_cmp(b, a) < 0) b = a;
    ed_delete(a, b);
    keep_del(a, b);
    end = ed_insert(a, e->text, strlen(e->text));
    if (pos_cmp(T->cur, a) > 0) keep_ins(a, end);
  }
  doc_group(T->doc);
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
  T = t0;
  G = g0;
  free(ord);
}


/*
** Shift+Alt+Right / Left: the selection grows to the next range around it
** (the server's selectionRange; else word, string, brackets, line, block,
** file) and shrinks back.
*/
static struct {
  Doc *d;
  unsigned long edits;
  Pos *v;	/* n pairs, the innermost first */
  size_t n;
  int at;	/* the one selected; -1: the selection it started from */
  Pos a0, b0;
  int waiting;
} SR;


static void sr_select (Pos a, Pos b) {
  T->anchor = a;
  T->cur = b;
  T->sel = pos_cmp(a, b) != 0;
  T->want = col_of(row_at(b.y), b.x);
  E.follow = 1;
}


/* the bytes from a to b */
static size_t sr_span (Pos a, Pos b) {
  size_t n = 0, y;
  if (a.y == b.y) return b.x - a.x;
  n = row_at(a.y)->len - a.x + 1;
  for (y = a.y + 1; y < b.y; y++) n += row_at(y)->len + 1;
  return n + b.x;
}


static void sr_add (Pos a, Pos b) {
  SR.v = (Pos *)xrealloc(SR.v, (2 * SR.n + 2) * sizeof(Pos));
  SR.v[2 * SR.n] = a;
  SR.v[2 * SR.n + 1] = b;
  SR.n++;
}


/* the unmatched opening bracket before p (in code), 1 found */
static int enclosing_open (Pos p, Pos *o) {
  size_t y = p.y, lines = 0;
  long x = (long)p.x - 1, depth = 0;
  for (;;) {
    const Row *r = row_at(y);
    const unsigned char *tok = r->len ? line_tokens(y) : NULL;
    for (; x >= 0; x--) {
      int c = (unsigned char)r->s[x];
      if (!tok || !code_at(tok, (size_t)x)) continue;
      if (is_close(c)) depth++;
      else if (is_open(c) && depth-- == 0) {
        o->y = y;
        o->x = (size_t)x;
        return 1;
      }
    }
    if (y == 0 || ++lines > 3000) return 0;
    y--;
    x = (long)row_at(y)->len - 1;
  }
}


/* the ranges without a server: word, string, brackets, line, indented block, the file */
static void sr_fallback (Pos p) {
  const Row *r = row_at(p.y);
  Pos a, b, o;
  size_t i, j, k, ind = indent_end(r);
  Pos *c = NULL;
  size_t nc = 0;
#define CAND(A, B)	do { c = (Pos *)xrealloc(c, (2 * nc + 2) * sizeof(Pos)); c[2 * nc] = (A); c[2 * nc + 1] = (B); nc++; } while (0)
  a = b = p;
  a.x = word_start_at(p);
  b.x = word_end_at(p);
  if (a.x < b.x) CAND(a, b);
  {	/* a string on the line around p */
    size_t x = 0;
    int q = 0;
    size_t qs = 0;
    for (x = 0; x < r->len; x++) {
      int ch = (unsigned char)r->s[x];
      if (q) {
        if (ch == '\\') {
          x++;
          continue;
        }
        if (ch == q) {
          if (qs <= p.x && p.x <= x + 1) {
            a.y = b.y = p.y;
            a.x = qs + 1;
            b.x = x;
            CAND(a, b);
            a.x = qs;
            b.x = x + 1;
            CAND(a, b);
          }
          q = 0;
        }
      }
      else if (ch == '"' || ch == '\'' || ch == '`') {
        q = ch;
        qs = x;
      }
    }
  }
  o = p;
  {	/* the brackets around p, a few levels */
    Pos at = p;
    int lv;
    const Row *rr = row_at(p.y);
    if (p.x < rr->len && is_open((unsigned char)rr->s[p.x])) at.x++;	/* on an opening one: it counts */
    for (lv = 0; lv < 8 && enclosing_open(at, &o); lv++) {
      Pos m;
      if (!match_bracket(o, &m)) break;
      a = o;
      a.x++;
      b = m;
      CAND(a, b);
      b.x++;
      CAND(o, b);
      at = o;
    }
  }
  a.y = b.y = p.y;	/* the line: its text, then all of it */
  a.x = ind;
  b.x = r->len;
  if (a.x < b.x) CAND(a, b);
  a.x = 0;
  if (p.y + 1 < T->doc->n) {
    b.y = p.y + 1;
    b.x = 0;
  }
  CAND(a, b);
  {	/* the indented blocks it is in: their first line to their last */
    size_t y = p.y, lv = 0, e;
    for (k = 0; k < 5000 && lv < 4; k++) {
      if ((e = fold_end(y)) >= p.y && e > y) {
        a.y = y;
        a.x = indent_end(row_at(y));
        b.y = e;
        b.x = row_at(e)->len;
        CAND(a, b);
        lv++;
      }
      if (y == 0) break;
      y--;
    }
  }
  a.y = a.x = 0;
  CAND(a, doc_end(T->doc));
#undef CAND
  for (i = 0; i < nc; i++)	/* the smaller first */
    for (j = i + 1; j < nc; j++)
      if (sr_span(c[2 * j], c[2 * j + 1]) < sr_span(c[2 * i], c[2 * i + 1])) {
        Pos t0 = c[2 * i], t1 = c[2 * i + 1];
        c[2 * i] = c[2 * j];
        c[2 * i + 1] = c[2 * j + 1];
        c[2 * j] = t0;
        c[2 * j + 1] = t1;
      }
  for (i = 0; i < nc; i++) {	/* each one around the one before */
    if (SR.n && !(pos_cmp(c[2 * i], SR.v[2 * SR.n - 2]) <= 0 && pos_cmp(c[2 * i + 1], SR.v[2 * SR.n - 1]) >= 0)) continue;
    if (SR.n && pos_cmp(c[2 * i], SR.v[2 * SR.n - 2]) == 0 && pos_cmp(c[2 * i + 1], SR.v[2 * SR.n - 1]) == 0) continue;
    sr_add(c[2 * i], c[2 * i + 1]);
  }
  free(c);
}


/* the next range around the selection */
static void sr_grow (void) {
  Pos a, b;
  int i;
  sel_range(&a, &b);
  if (!T->sel) a = b = T->cur;
  for (i = SR.at + 1; i < (int)SR.n; i++) {
    Pos ra = SR.v[2 * i], rb = SR.v[2 * i + 1];
    if (pos_cmp(ra, a) <= 0 && pos_cmp(rb, b) >= 0 && (pos_cmp(ra, a) != 0 || pos_cmp(rb, b) != 0)) {
      SR.at = i;
      sr_select(ra, rb);
      return;
    }
  }
}


static int sr_current (void) {
  Pos a, b;
  sel_range(&a, &b);
  if (!T->sel) a = b = T->cur;
  if (SR.d != T->doc || SR.edits != T->doc->edits || SR.n == 0) return 0;
  if (SR.at < 0) return pos_cmp(a, SR.a0) == 0 && pos_cmp(b, SR.b0) == 0;
  return pos_cmp(a, SR.v[2 * SR.at]) == 0 && pos_cmp(b, SR.v[2 * SR.at + 1]) == 0;
}


static void expand_selection (void) {
  Pos a, b;
  if (!HAS_DOC || G->diff || T->page) return;
  if (sr_current()) {
    sr_grow();
    return;
  }
  sel_range(&a, &b);
  if (!T->sel) a = b = T->cur;
  free(SR.v);
  memset(&SR, 0, sizeof(SR));
  SR.d = T->doc;
  SR.edits = T->doc->edits;
  SR.a0 = a;
  SR.b0 = b;
  SR.at = -1;
  T->nmc = 0;
  if (lsp_active(T->doc)) {
    SR.waiting = 1;
    lsp_selection_range(T->doc, T->cur);	/* the answer: on_selection_ranges */
    return;
  }
  sr_fallback(T->cur);
  sr_grow();
}


void on_selection_ranges (Doc *d, Pos at, const Pos *v, size_t n) {
  size_t i;
  (void)at;
  if (!SR.waiting || !HAS_DOC || d != T->doc || SR.d != d) return;
  SR.waiting = 0;
  if (n == 0) sr_fallback(T->cur);
  for (i = 0; i < n; i++) sr_add(doc_clamp(d, v[2 * i]), doc_clamp(d, v[2 * i + 1]));
  sr_grow();
}


static void shrink_selection (void) {
  if (!HAS_DOC || G->diff || T->page || !sr_current() || SR.at < 0) return;
  for (SR.at--; SR.at >= 0; SR.at--) {	/* the one before that holds where it started */
    Pos ra = SR.v[2 * SR.at], rb = SR.v[2 * SR.at + 1];
    if (pos_cmp(ra, SR.a0) <= 0 && pos_cmp(rb, SR.b0) >= 0) {
      sr_select(ra, rb);
      return;
    }
  }
  sr_select(SR.a0, SR.b0);
}


/* Ctrl+K Ctrl+F: the selection (the line, without one) formatted */
static void format_selection (void) {
  Pos a, b;
  if (!HAS_DOC || G->diff || T->page) return;
  sel_range(&a, &b);
  if (!T->sel) {
    a.y = b.y = T->cur.y;
    a.x = 0;
    b.x = row_at(b.y)->len;
  }
  if (!lsp_active(T->doc) || !lsp_format_range(T->doc, a, b))
    toast(0, "There is no formatter for '%s' files installed that formats selections.", syntax_name(T->sx));
}


/* Shift+Alt+O: the imports sorted, the unused gone (wait: before a save, quietly) */
static void organize_imports (int wait) {
  if (!HAS_DOC || G->diff || T->page) return;
  if (!lsp_active(T->doc)) {
    if (!wait) toast(0, "No organize imports action available");
    return;
  }
  lsp_source_action(T->doc, "source.organizeImports", wait ? 2 : 1);
  if (wait) {
    long long end = os_now_us() + 1500000;
    while (lsp_source_pending() && os_now_us() < end)
      if (!lsp_poll()) os_wait_readable(-1, 10);
  }
}


static void format_doc (int wait) {
  if (!HAS_DOC || G->diff) return;
  if (!lsp_active(T->doc) || !lsp_format(T->doc)) {
    if (!wait) toast(0, "There is no formatter for '%s' files installed.", syntax_name(T->sx));
    return;
  }
  FM.d = T->doc;
  FM.done = 0;
  if (wait) {	/* saving: the formatted text is what is saved */
    long long end = os_now_us() + 2000000;
    while (!FM.done && os_now_us() < end)
      if (!lsp_poll()) os_wait_readable(-1, 10);
  }
}


/* Ctrl+Shift+O: the file's symbols; each is shown while it is selected */
static size_t *g_gsl;

static void symbol_preview (int i) {
  Pos p;
  p.y = g_gsl[i];
  p.x = 0;
  move_h(doc_clamp(T->doc, p), 0);
  key_home(0);
  center_cursor();
}


static void goto_symbol (const char *init) {
  size_t n, i;
  const Sym *v;
  Pick p;
  Cur was;
  size_t top;
  int r;
  if (!HAS_DOC || G->diff) return;
  v = symbols(&n);
  if (n == 0) {
    toast(0, "No editor symbols");
    return;
  }
  cur_get(&was);
  top = T->top;
  g_gsl = (size_t *)xmalloc(n * sizeof(size_t));
  pick_init(&p, "Go to Symbol in Editor");
  if (init) snprintf(p.text, sizeof(p.text), "%s", init);
  p.prefix = "@";
  for (i = 0; i < n; i++) {
    char label[300];
    int st;
    snprintf(label, sizeof(label), "%*s%s", v[i].depth * 2, "", v[i].name);
    g_gsl[i] = v[i].line;
    pick_add(&p, label, NULL, (int)sym_icon(v[i].kind, &st));
  }
  p.on_move = symbol_preview;
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) {	/* Esc: back where it was */
    cur_set(&was);
    T->top = top;
  }
  else symbol_preview(r);
  free(g_gsl);
  g_gsl = NULL;
  E.focus = F_EDITOR;
}


/*
** Alt+Left / Alt+Right: where the editor was. A place is kept when the
** cursor jumps (another file, or 10 lines and more not by moving or typing).
*/
#define NAV_MAX	50

static struct {
  char *path[NAV_MAX];
  Pos p[NAV_MAX];
  int n, at;
} NV;


static void nav_track (void) {
  Pos p;
  int quiet = E.nav_quiet;
  E.nav_quiet = 0;
  if (!HAS_DOC || G->diff || T->real == NULL) return;
  p = T->cur;
  if (NV.n > 0 && m_fncmp(NV.path[NV.at], T->real) == 0) {
    size_t dy = p.y > NV.p[NV.at].y ? p.y - NV.p[NV.at].y : NV.p[NV.at].y - p.y;
    if (quiet || dy < 10) {	/* the same place, moved a little */
      NV.p[NV.at] = p;
      return;
    }
  }
  while (NV.n > NV.at + 1) free(NV.path[--NV.n]);	/* a new way: what was ahead goes */
  if (NV.n == NAV_MAX) {
    free(NV.path[0]);
    memmove(NV.path, NV.path + 1, (NAV_MAX - 1) * sizeof(NV.path[0]));
    memmove(NV.p, NV.p + 1, (NAV_MAX - 1) * sizeof(NV.p[0]));
    NV.n--;
  }
  NV.path[NV.n] = xstrdup(T->real);
  NV.p[NV.n] = p;
  NV.at = NV.n++;
}


static void nav_go (int d) {
  int to;
  nav_track();
  to = NV.at + d;
  if (to < 0 || to >= NV.n) return;
  NV.at = to;
  if (open_file(NV.path[to], 0) != 0) {	/* gone: so is its place */
    free(NV.path[to]);
    memmove(NV.path + to, NV.path + to + 1, (size_t)(NV.n - to - 1) * sizeof(NV.path[0]));
    memmove(NV.p + to, NV.p + to + 1, (size_t)(NV.n - to - 1) * sizeof(NV.p[0]));
    NV.n--;
    if (NV.at >= NV.n) NV.at = NV.n - 1;
    if (NV.at < 0) NV.at = 0;
    return;
  }
  move_h(doc_clamp(T->doc, NV.p[to]), 0);
  if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
  E.focus = F_EDITOR;
}


/* Insert Snippet...: the language's snippets, the selection in $TM_SELECTED_TEXT */
static void insert_snippet_pick (void) {
  size_t n, i;
  const Snip *v;
  Pick p;
  int r;
  Pos a, b;
  if (!HAS_DOC || G->diff) return;
  v = snip_list(doc_lang_id(), &n);
  if (keys_args()) {	/* a keybinding's {"snippet": "..."} or {"name": "..."} */
    const char *body = json_str(json_get(keys_args(), "snippet"), NULL);
    const char *name = json_str(json_get(keys_args(), "name"), NULL);
    for (i = 0; body == NULL && name && i < n; i++)
      if (strcmp(v[i].name, name) == 0) body = v[i].body;
    if (body) {
      sel_range(&a, &b);
      if (!T->sel) a = b = T->cur;
      snippet_insert(a, b, body);
      return;
    }
  }
  if (n == 0) {
    toast(0, "No snippets for %s: Configure Snippets makes some", syntax_name(T->sx));
    return;
  }
  pick_init(&p, "Select a snippet");
  for (i = 0; i < n; i++) pick_add(&p, v[i].prefix, v[i].desc, 0xEB66);	/* codicon symbol-snippet */
  r = pick_run(&p);
  pick_free(&p);
  E.focus = F_EDITOR;
  if (r < 0) return;
  sel_range(&a, &b);
  if (!T->sel) a = b = T->cur;
  snippet_insert(a, b, v[r].body);
}


/* Configure Snippets: ~/.mme/snippets/<language>.json in a tab */
static void configure_snippets (void) {
  const char *lang = doc_lang_id();
  char *dir = snip_dir(), name[80], *f;
  OsStat st;
  if (dir == NULL) return;
  snprintf(name, sizeof(name), "%s.json", lang);
  f = path_join(dir, name);
  if (os_stat(f, &st) != 0 || !st.exists) {
    Buf b;
    int fd;
    buf_init(&b);
    buf_printf(&b, "// %s snippets, written like VS Code's: \"prefix\" is what is typed, \"body\" what goes in\n"
                   "// ($1, $2 ... the tab stops, ${1:default}, ${1|one,two|}, $0 the end; $TM_FILENAME,\n"
                   "// $CURRENT_YEAR ... variables). Saving this file applies it.\n"
                   "{\n"
                   "  // \"Print to console\": {\n"
                   "  //   \"prefix\": \"log\",\n"
                   "  //   \"body\": [\"printf(\\\"$1\\\\n\\\");\", \"$0\"],\n"
                   "  //   \"description\": \"Log output to console\"\n"
                   "  // }\n"
                   "}\n", lang);
    if ((fd = os_open(f, OS_WRITE)) >= 0) {
      os_write(fd, b.s, b.len);
      os_close(fd);
    }
    buf_free(&b);
  }
  if (open_file(f, 0) == 0) E.focus = F_EDITOR;
  free(f);
  free(dir);
}


static void open_find (void) {
  Pos a, b;
  sel_range(&a, &b);
  if (T->sel && a.y == b.y && b.x - a.x < sizeof(E.find)) {	/* the selection is looked for */
    memcpy(E.find, row_at(a.y)->s + a.x, b.x - a.x);
    E.find[b.x - a.x] = '\0';
  }
  E.find_open = E.finding = 1;
  E.focus = F_EDITOR;
}


static void find_next (int back) {
  Pos a, b, f;
  if (E.find[0] == '\0') return;
  sel_range(&a, &b);
  if (!find_from(T->sel ? (back ? a : b) : T->cur, back, &f)) return;
  select_range(f, match_at(f.y, f.x));
  if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
}


static void undo (int redo) {
  Pos p;
  if (!(redo ? doc_redo(T->doc, &p) : doc_undo(T->doc, &p))) return;
  move_h(p, 0);
}


/*
** {==================================================================
** Everyday care, like VS Code's: auto save, hot exit and the session,
** files changed on disk, encodings, compare
** ===================================================================
*/

enum { SAVE_EXPLICIT, SAVE_AUTO_DELAY, SAVE_AUTO_FOCUS };

static int g_session;	/* a folder was opened: its session is kept (hot exit, restore) */
static char *g_cmp_sel;	/* Select for Compare's file */
static long long g_watch_at, g_backup_at;
static unsigned long g_backup_sum;	/* the dirty texts' edits when they were last backed up */

static void page_open (int kind);
static void after_save (void);


const char *compare_selected (void) {
  return g_cmp_sel;
}


int cmd_checked (int cmd) {
  return cmd == CMD_AUTO_SAVE && opt.auto_save != AUTO_OFF;
}


/* the tab t is in: its group (G and T become them); 0: it is not open any more */
static int tab_front (const Tab *t) {
  int g, i;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++)
      if (g_grp[g].tab[i] == t) {
        G = &g_grp[g];
        T = g_grp[g].tab[i];
        return 1;
      }
  return 0;
}


/* 1 when d is the first tab's of all that show it (a text is looked at once) */
static int first_view (int g, int i) {
  Doc *d = g_grp[g].tab[i]->doc;
  int gg, jj;
  for (gg = 0; gg <= g; gg++)
    for (jj = 0; jj < (gg == g ? i : g_grp[gg].ntab); jj++)
      if (g_grp[gg].tab[jj]->doc == d) return 0;
  return 1;
}


/*
** The file changed on disk since it was read, and Ctrl+S would write over
** it: VS Code's question. 1: write it.
*/
static void compare_saved (void);

static int save_conflict (void) {
  static const char *const bt[] = {"Compare", "Overwrite", "Cancel"};
  char msg[400];
  snprintf(msg, sizeof(msg), "Failed to save '%s': The content of the file is newer.", doc_name());
  switch (dialog(msg, "Please compare your version with the file contents or overwrite the content of the file with your changes.", bt, 3)) {
    case 0: compare_saved(); return 0;
    case 1: return 1;
  }
  return 0;
}


/* after a save: settings.json, keybindings.json and snippets apply; git and the Explorer look again */
static void after_save (void) {
  char *sp;
  if (T->real) test_saved(T->real);	/* its tests read again */
  history_add(T->doc->path, "File Saved");	/* Local History */
  TL.stale = 1;
  if (T->real && ws_file() && m_fncmp(T->real, ws_file()) == 0) {	/* the workspace: again */
    char *f = xstrdup(ws_file());
    if (ws_open(f) == 0) toast(0, "Workspace settings applied");
    free(f);
    apply_settings(0);
  }
  else if (T->real && !ws_active()) {	/* .vscode/settings.json of the folder */
    char *d = path_join(side_root(), ".vscode"), *f = path_join(d, "settings.json"), *r = os_realpath(f);
    if (r && m_fncmp(r, T->real) == 0) apply_settings(1);
    free(r);
    free(f);
    free(d);
  }
  sp = settings_path();
  if (sp && T->real && m_fncmp(sp, T->real) == 0) apply_settings(1);
  free(sp);
  sp = keys_path();
  if (sp && T->real && m_fncmp(sp, T->real) == 0) {
    int bad = keys_load() != 0;
    toast(bad, bad ? "keybindings.json is not valid JSON: not applied" : "Keyboard shortcuts applied");
  }
  free(sp);
  sp = snip_dir();
  if (sp && T->real) {
    char *dir = path_dirname(T->real);
    if (m_fncmp(dir, sp) == 0) {
      snip_reload();
      toast(0, "Snippets applied");
    }
    free(dir);
  }
  free(sp);
  T->preview = 0;
  T->sx = syntax_detect(T->doc->path, T->doc);
  if (T->sx) lsp_open(T->doc, syntax_name(T->sx));
  git_refresh();
  side_refresh();
}


/* the tab in front saved (its path known): format, trim, write, and what follows; 0 saved */
static int save_front (int reason) {
  Doc *d = T->doc;
  doc_disk_check(d);
  if (d->disk_state == DISK_NEWER) {
    if (reason != SAVE_EXPLICIT) return -1;	/* auto save does not write over a newer file */
    if (!save_conflict()) return -1;
  }
  if (opt.organize_save && reason != SAVE_AUTO_DELAY) organize_imports(1);	/* editor.codeActionsOnSave */
  if (opt.format_on_save && reason != SAVE_AUTO_DELAY) format_doc(1);	/* not after a delay, like VS Code */
  before_save(reason != SAVE_EXPLICIT);
  if (doc_save(d) != 0) {
    toast(1, "Failed to save '%s'", doc_name());
    return -1;
  }
  after_save();
  return 0;
}


/* tab t saved by auto save, wherever it is; the tab in front stays in front */
static void save_tab (Tab *t, int reason) {
  Group *g0 = G;
  Tab *t0 = T;
  if (!tab_front(t)) return;
  if (!t->page && t->doc->path && doc_dirty(t->doc)) save_front(reason);
  G = g0;
  T = t0;
}


/* files.autoSave afterDelay: a text not changed for files.autoSaveDelay ms is saved */
static void autosave_idle (void) {
  long long now = os_now_us();
  int g, i;
  if (opt.auto_save != AUTO_DELAY) return;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      Tab *t = g_grp[g].tab[i];
      Doc *d = t->doc;
      if (t->page || d->path == NULL || !doc_dirty(d) || !first_view(g, i)) continue;
      if (d->disk_state != DISK_SAME) continue;	/* newer on disk, or gone: Ctrl+S asks */
      if (now - d->changed_at < (long long)opt.auto_save_delay * 1000) continue;
      save_tab(t, SAVE_AUTO_DELAY);
    }
}


/* onFocusChange (and onWindowChange): the tab left, or the editor left, is saved */
static void autosave_focus (void) {
  static Tab *last;
  static int last_focus = F_EDITOR;
  if (opt.auto_save == AUTO_FOCUS || opt.auto_save == AUTO_WINDOW) {
    if (last && (last != (HAS_DOC ? T : NULL) || (last_focus == F_EDITOR && E.focus != F_EDITOR))) {
      Group *g0 = G;
      Tab *t0 = T;
      if (tab_front(last) && !last->page && last->doc->path && doc_dirty(last->doc) &&
          last->doc->disk_state == DISK_SAME) save_front(SAVE_AUTO_FOCUS);
      G = g0;
      T = t0;
    }
  }
  last = HAS_DOC ? T : NULL;
  last_focus = E.focus;
}


/* d read again from its file (it changed there); the tabs that show it keep their places */
static void reload_doc (Doc *d) {
  char *path = xstrdup(d->path);
  int refs = d->refs, enc = d->enc, g, i;
  unsigned long edits = d->edits;
  const Syntax *sx = NULL;
  lsp_close(d);
  doc_free(d);
  doc_init(d);
  doc_load_enc(d, path, enc);
  d->refs = refs;
  d->edits = edits + 1;	/* the views and the server see a new text */
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      Tab *t = g_grp[g].tab[i];
      if (t->doc != d) continue;
      sx = t->sx;
      t->cur = doc_clamp(d, t->cur);
      t->anchor = doc_clamp(d, t->anchor);
      if (t->top >= d->n) t->top = d->n - 1;
      t->nmc = 0;
      t->nfold = 0;
    }
  if (sx) lsp_open(d, syntax_name(sx));
  free(path);
}


/*
** Files changed on disk (another program, git ...): looked at every 1.5 s.
** A text not edited is read again; an edited one is left, and Ctrl+S asks;
** a file gone shows "(deleted)" on its tab.
*/
static void watch_idle (void) {
  long long now = os_now_us();
  int g, i;
  if (now - g_watch_at < 1500000) return;
  g_watch_at = now;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      Tab *t = g_grp[g].tab[i];
      Doc *d = t->doc;
      int r;
      if (t->page || d->path == NULL || !first_view(g, i)) continue;
      r = doc_disk_check(d);
      if ((r == 1 || r == 3) && !doc_dirty(d)) reload_doc(d);
    }
}


/* a short name for a folder: FNV-1a of its path (the case of Windows' paths not counted) */
static unsigned long long path_hash (const char *s) {
  unsigned long long h = 14695981039346656037ULL;
  for (; *s; s++) {
    int c = (unsigned char)*s;
#ifdef _WIN32
    if (c >= 'A' && c <= 'Z') c += 32;
    if (c == '/') c = '\\';
#endif
    h = (h ^ (unsigned long long)c) * 1099511628211ULL;
  }
  return h;
}


static char *state_path (const char *dir, const char *ext) {
  char name[64], *d = data_path(dir), *f;
  mkdir_p(d);
  snprintf(name, sizeof(name), "%016llx%s", path_hash(side_root()), ext);
  f = path_join(d, name);
  free(d);
  return f;
}


static void write_all (const char *f, const char *s, size_t n) {
  int fd = os_open(f, OS_WRITE);
  if (fd < 0) return;
  if (n) os_write(fd, s, n);
  os_close(fd);
}


/*
** The folder's session in mme-data/state/<hash>.json: the groups, their
** tabs, where the cursor was, the side bar and the panel. With backups (hot
** exit) the texts not saved go to mme-data/backups/<hash>-<n>.txt.
*/
static void session_write (int backups) {
  Buf b;
  char *f, prefix[40];
  Vec old;
  size_t k;
  int g, i, nb = 0;
  Doc *seen[64];
  int seen_n[64], nseen = 0;
  if (!g_session) return;
  {	/* the backups there were go */
    char *bd = data_path("backups");
    mkdir_p(bd);
    snprintf(prefix, sizeof(prefix), "%016llx-", path_hash(side_root()));
    vec_init(&old);
    os_listdir(bd, &old);
    for (k = 0; k < old.n; k++)
      if (strncmp(old.v[k], prefix, strlen(prefix)) == 0) {
        char *p = path_join(bd, old.v[k]);
        os_unlink(p);
        free(p);
      }
    vec_free(&old);
    free(bd);
  }
  buf_init(&b);
  buf_puts(&b, "{\"folder\": ");
  json_put_str(&b, side_root(), strlen(side_root()));
  buf_printf(&b, ", \"side\": %d, \"view\": %d, \"sideWidth\": %d, \"panel\": %d, \"panelView\": %d, "
                 "\"panelHeight\": %d, \"group\": %d, \"centered\": %d, "
                 "\"panes\": [%d, %d, %d],\n \"colw\": [", E.side, E.view, E.side_w,
             E.panel, E.panel_view, E.panel_h, g_gcur, E.centered, OE.open, OL.open, TL.open);
  for (g = 0; g < grp_ncol(); g++) buf_printf(&b, "%s%d", g ? ", " : "", g_colw[g]);
  buf_puts(&b, "],\n \"groups\": [");
  for (g = 0; g < g_ngrp; g++) {
    buf_printf(&b, "%s\n  {\"active\": %d, \"col\": %d, \"hw\": %d, \"diff\": 0, \"tabs\": [", g ? "," : "",
               g_grp[g].active, g_grp[g].col, g_grp[g].hw);
    for (i = 0; i < g_grp[g].ntab; i++) {
      Tab *t = g_grp[g].tab[i];
      Doc *d = t->doc;
      int backup = -1, s;
      buf_puts(&b, i ? ",\n    {" : "\n    {");
      if (t->page) buf_printf(&b, "\"page\": %d", t->page);
      else {
        if (d->path) {
          buf_puts(&b, "\"path\": ");
          json_put_str(&b, d->path, strlen(d->path));
        }
        else buf_puts(&b, "\"untitled\": 1");
        buf_printf(&b, ", \"y\": %lu, \"x\": %lu, \"top\": %lu, \"preview\": %d",
                   (unsigned long)t->cur.y, (unsigned long)t->cur.x, (unsigned long)t->top, t->preview);
        if (backups && doc_dirty(d)) {	/* its text, once for the tabs that share it */
          for (s = 0; s < nseen; s++)
            if (seen[s] == d) backup = seen_n[s];
          if (backup < 0) {
            Pos a;
            size_t len;
            char *text, *bf, name[64], *bd = data_path("backups");
            a.y = a.x = 0;
            text = doc_text(d, a, doc_end(d), &len);
            backup = nb++;
            snprintf(name, sizeof(name), "%s%d.txt", prefix, backup);
            bf = path_join(bd, name);
            write_all(bf, text, len);
            free(bf);
            free(bd);
            free(text);
            if (nseen < 64) {
              seen[nseen] = d;
              seen_n[nseen++] = backup;
            }
          }
          buf_printf(&b, ", \"backup\": \"%s%d.txt\"", prefix, backup);
        }
      }
      buf_puts(&b, "}");
    }
    buf_puts(&b, "]}");
  }
  buf_puts(&b, "\n]}\n");
  f = state_path("state", ".json");
  write_all(f, b.s, b.len);
  free(f);
  buf_free(&b);
}


/* the folder's editors as they were when mme last quit (window.restoreWindows) */
/* a number of the state file, def when it is not one in lo..hi (a file edited by hand, or broken) */
static double jnum (const Json *j, double def, double lo, double hi) {
  double v = json_num(j, def);
  return v >= lo && v <= hi ? v : def;
}


static void session_restore (void) {
  char *f = state_path("state", ".json"), *s, *bd = data_path("backups");
  size_t len, gi, ti;
  Json *j;
  const Json *groups;
  int active[MAX_GRP], ng = 0;
  s = read_file(f, &len);
  free(f);
  if (s == NULL || (j = json_parse(s, len)) == NULL) {
    free(s);
    free(bd);
    return;
  }
  free(s);
  if (m_fncmp(json_str(json_get(j, "folder"), ""), side_root()) != 0) {	/* another folder's */
    json_free(j);
    free(bd);
    return;
  }
  groups = json_get(j, "groups");
  memset(active, 0, sizeof(active));
  for (gi = 0; groups && groups->type == J_ARR && gi < groups->n && gi < MAX_GRP; gi++) {
    const Json *tabs = json_get(groups->kid[gi], "tabs");
    int col = (int)jnum(json_get(groups->kid[gi], "col"), ng, 0, MAX_GRP), hw = (int)jnum(json_get(groups->kid[gi], "hw"), 100, 1, 100000);
    if (tabs == NULL || tabs->type != J_ARR || tabs->n == 0) continue;
    if (ng >= 1) {	/* another group: where it was (an old state's is beside) */
      if (col < g_grp[g_ngrp - 1].col) col = g_grp[g_ngrp - 1].col;
      memset(&g_grp[g_ngrp], 0, sizeof(Group));
      focus_group(g_ngrp++);
    }
    G->col = ng == 0 ? 0 : col;
    G->hw = hw > 0 ? hw : 100;
    for (ti = 0; ti < tabs->n; ti++) {
      const Json *t = tabs->kid[ti];
      const char *path = json_str(json_get(t, "path"), NULL), *bk = json_str(json_get(t, "backup"), NULL);
      char *btext = NULL;
      size_t blen = 0;
      Pos p;
      OsStat st;
      int page = (int)jnum(json_get(t, "page"), 0, 0, 100);
      if (page == PAGE_SETTINGS || page == PAGE_WELCOME) {
        page_open(page);
        continue;
      }
      if (bk) {
        char *bf = path_join(bd, bk);
        btext = read_file(bf, &blen);
        free(bf);
      }
      if (path && (os_stat(path, &st) == 0 && st.exists) && open_file(path, 0) == 0) ;
      else if (path && btext) {	/* the file is gone; its text not saved stays */
        if (open_file(path, 0) != 0) {
          free(btext);
          continue;
        }
      }
      else if (!path && btext) tab_new();	/* Untitled */
      else {
        free(btext);
        continue;
      }
      if (btext) {	/* the text as it was, not saved */
        doc_set_text(T->doc, btext, blen);
        T->doc->changes = T->doc->saved + 1;
        free(btext);
      }
      T->preview = 0;
      p.y = (size_t)jnum(json_get(t, "y"), 0, 0, 1e15);
      p.x = (size_t)jnum(json_get(t, "x"), 0, 0, 1e15);
      T->cur = T->anchor = doc_clamp(T->doc, p);
      T->sel = 0;
      T->top = (size_t)jnum(json_get(t, "top"), 0, 0, 1e15);
      if (T->top >= T->doc->n) T->top = T->doc->n - 1;
      T->want = col_of(row_at(T->cur.y), T->cur.x);
    }
    if (G->ntab == 0 && ng >= 1) {	/* nothing came back in it */
      g_ngrp--;
      focus_group(0);
      continue;
    }
    active[ng++] = (int)jnum(json_get(groups->kid[gi], "active"), 0, 0, 1e6);
  }
  {	/* the columns: numbered from 0 again (a group may not have come back), their widths */
    const Json *cw = json_get(j, "colw");
    int c = -1, last = -1, k, w[MAX_GRP];
    for (k = 0; k < MAX_GRP; k++) w[k] = cw && cw->type == J_ARR && (size_t)k < cw->n ? (int)jnum(cw->kid[k], 100, 1, 100000) : 100;
    for (k = 0; k < g_ngrp; k++) {
      if (g_grp[k].col != last) {
        last = g_grp[k].col;
        c++;
        g_colw[c] = last >= 0 && last < MAX_GRP && w[last] > 0 ? w[last] : 100;
      }
      g_grp[k].col = c;
    }
    E.centered = (int)jnum(json_get(j, "centered"), 0, 0, 1);
  }
  for (gi = 0; gi < (size_t)g_ngrp; gi++) {
    focus_group((int)gi);
    if (active[gi] < G->ntab) focus_tab(active[gi]);
  }
  focus_group((int)jnum(json_get(j, "group"), 0, 0, g_ngrp - 1));
  {	/* the side bar as it was */
    int v = (int)jnum(json_get(j, "view"), E.view, 0, VIEW_N - 1), side = (int)jnum(json_get(j, "side"), E.side, 0, 1);
    if (v >= 0 && v < VIEW_N) show_view(v);
    E.side = side;
  }
  E.side_w = (int)jnum(json_get(j, "sideWidth"), E.side_w, 10, 1000);
  {	/* the panes that were folded */
    const Json *pn = json_get(j, "panes");
    if (pn && pn->type == J_ARR && pn->n >= 3) {
      OE.open = (int)jnum(pn->kid[0], 0, 0, 1);
      OL.open = (int)jnum(pn->kid[1], 1, 0, 1);
      TL.open = (int)jnum(pn->kid[2], 0, 0, 1);
    }
  }
  E.panel_h = (int)jnum(json_get(j, "panelHeight"), E.panel_h, 3, 1000);
  if (json_num(json_get(j, "panel"), 0)) {
    int pv = (int)jnum(json_get(j, "panelView"), 0, 0, 3);
    if (pv == 0) run_command(CMD_TERMINAL);	/* a terminal again, like VS Code */
    else {
      E.panel = 1;
      E.panel_view = pv;
    }
  }
  if (HAS_DOC) E.focus = F_EDITOR;
  if (T->real) side_reveal(T->real);
  json_free(j);
  free(bd);
}


/* now and then, while hot exit is on: the session and the texts not saved (a crash loses little) */
static void backup_idle (void) {
  long long now = os_now_us();
  unsigned long sum = 0;
  int g, i, any = 0;
  if (!g_session || !opt.hot_exit || now - g_backup_at < 5000000) return;
  g_backup_at = now;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++)
      if (doc_dirty(g_grp[g].tab[i]->doc)) {
        sum += g_grp[g].tab[i]->doc->edits;
        any = 1;
      }
  if (sum == g_backup_sum) return;
  g_backup_sum = sum;
  if (any || sum == 0) session_write(1);
}


static void work_idle (void) {
  watch_idle();
  autosave_idle();
  backup_idle();
}


/* two texts side by side in the diff editor (left: before, right: after) */
static void show_compare (const char *path, const char *a, size_t na, const char *b, size_t nb, const char *title) {
  if (diff_open_texts(path, a, na, b, nb, title) != 0) return;
  {
    int gg;
    for (gg = 0; gg < MAX_GRP; gg++) g_grp[gg].dtab = g_grp[gg].diff = 0;
  }
  G->dtab = 1;
  G->diff = 1;
  E.focus = F_EDITOR;
}


/*
** {==================================================================
** Quick diff and merge conflicts in the editor (equick.c has the data):
** the gutter's change bars, the dirty diff peek (Alt+F3), Stage / Revert
** Change and the Selected Ranges commands; the conflicts' colors and
** their Accept Current / Incoming / Both links
** ===================================================================
*/

#define QD_ADD	0x2EA043	/* editorGutter.addedBackground, Dark Modern's */
#define QD_MOD	0x0078D4	/* editorGutter.modifiedBackground */
#define QD_DEL	0xF85149	/* editorGutter.deletedBackground */

static struct {
  int open;
  size_t hi;	/* the change shown */
  size_t top;	/* its first line shown: a long one scrolls */
  int x, y, w, h;	/* where it was drawn */
  int stage_x, revert_x, prev_x, next_x, close_x;
} DP;


static const QHunk *qhunks (size_t *n) {
  *n = 0;
  if (!HAS_DOC || G->diff || T->page || T->md || T->real == NULL) return NULL;
  return quick_hunks(T->doc, T->real, n);
}


/* the gutter's bar of line y (draw_row calls it for a line's first row) */
static void draw_quick (int sy, size_t y, int gw, int is_cur) {
  int qm;
  uint32_t bg;
  if (!opt.scm_decor || T->real == NULL || T->page || gw < 3) return;
  if ((qm = quick_mark(T->doc, T->real, y)) == 0) return;
  bg = ui_color(is_cur ? C_LINE_BG : C_EDITOR_BG);
  if (qm & QM_ADD) scr_put_rgb(L.ed_x + gw - 1, sy, 0x258E, QD_ADD, bg, 0);	/* ▎ */
  else if (qm & QM_MOD) scr_put_rgb(L.ed_x + gw - 1, sy, 0x258E, QD_MOD, bg, 0);
  else scr_put_rgb(L.ed_x + gw - 1, sy, (qm & QM_DEL) ? 0x25E2 : 0x25E5, QD_DEL, bg, 0);	/* ◢ lines went below, ◥ above */
}


static size_t hunk_line (const QHunk *h) {	/* where a change is in the file now */
  return h->nn ? h->n0 : (h->n0 ? h->n0 - 1 : 0);
}


static size_t hunk_last (const QHunk *h) {
  return h->nn ? h->n0 + h->nn - 1 : hunk_line(h);
}


/* the change at line y (on it, or lines went just after it); -1: none */
static long hunk_at (const QHunk *h, size_t n, size_t y) {
  size_t i;
  for (i = 0; i < n; i++)
    if (y >= hunk_line(&h[i]) && y <= hunk_last(&h[i])) return (long)i;
  return -1;
}


/* the change after (d 1) or before (d -1) the cursor's line, around the ends */
static long hunk_next (const QHunk *h, size_t n, int d) {
  size_t y = T->cur.y, i;
  long at = hunk_at(h, n, y);
  if (n == 0) return -1;
  if (d > 0) {
    for (i = 0; i < n; i++)
      if (hunk_line(&h[i]) > y && (long)i != at) return (long)i;
    return 0;
  }
  for (i = n; i-- > 0;)
    if (hunk_line(&h[i]) < y && (long)i != at) return (long)i;
  return (long)n - 1;
}


static int peek_height (const QHunk *h) {
  int ph = (int)(h->on + h->nn) + 2, max = L.text_h * 45 / 100;
  if (max < 6) max = 6;
  if (ph > max) ph = max;
  if (ph > L.text_h) ph = L.text_h;
  return ph;
}


/* the dirty diff peek on change i, the cursor on it, room under it */
static void dirty_show (long i) {
  size_t n, y, last;
  const QHunk *h = qhunks(&n);
  Pos p;
  if (h == NULL || n == 0 || i < 0 || (size_t)i >= n) {
    DP.open = 0;
    toast(0, "There are no changes in this file");
    return;
  }
  DP.open = 1;
  DP.hi = (size_t)i;
  DP.top = 0;
  y = hunk_line(&h[i]);
  last = hunk_last(&h[i]);
  T->sel = 0;
  T->nmc = 0;
  p.y = y;
  p.x = 0;
  move_h(doc_clamp(T->doc, p), 0);
  {	/* the change and the peek under it in the view */
    size_t ph = (size_t)peek_height(&h[i]), th = (size_t)L.text_h;
    if (last + 1 + ph > T->top + th || y < T->top) T->top = last + 1 + ph > th ? last + 1 + ph - th : 0;
    if (T->top > y) T->top = y > 1 ? y - 1 : 0;
  }
  E.focus = F_EDITOR;
}


/* a line of text for a box: tabs as spaces, cut at w columns */
static void put_plain (int x, int y, int w, const char *s, size_t n, int st) {
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < n; i++) {
    if (s[i] == '\t') {
      int k;
      for (k = 0; k < TABW; k++) buf_putc(&b, ' ');
    }
    else buf_putc(&b, s[i]);
  }
  buf_putc(&b, '\0');
  scr_putsw(x, y, w, b.s, st);
  buf_free(&b);
}


/* VS Code's dirty diff widget: under the change, what it was (red) and is (green) */
static void draw_dirty_peek (void) {
  size_t n, total, k;
  const QHunk *hv, *h;
  int ph, r, y0, row, nw = 6;
  char title[400];
  Pos p;
  if (!DP.open) return;
  hv = qhunks(&n);
  if (hv == NULL || n == 0) {
    DP.open = 0;
    return;
  }
  if (DP.hi >= n) DP.hi = n - 1;
  h = &hv[DP.hi];
  total = h->on + h->nn;
  ph = peek_height(h);
  p.y = hunk_last(h);
  p.x = 0;
  r = vis_row(p);
  y0 = L.text_y + (r < 0 ? 0 : r + 1);
  if (y0 + ph > L.text_y + L.text_h) y0 = L.text_y + L.text_h - ph;
  DP.x = L.ed_x;
  DP.w = L.ed_w - L.sb_w;
  DP.y = y0;
  DP.h = ph;
  if (DP.top + (size_t)(ph - 2) > total) DP.top = total > (size_t)(ph - 2) ? total - (size_t)(ph - 2) : 0;
  scr_fill(DP.x, y0, DP.w, S_BOX);
  snprintf(title, sizeof(title), "%s \xE2\x80\x94 %lu of %lu change%s", path_basename(T->real),
           (unsigned long)DP.hi + 1, (unsigned long)n, n == 1 ? "" : "s");
  scr_putsw(DP.x + 2, y0, DP.w - 14, title, S_BOX_TITLE);
  DP.close_x = DP.x + DP.w - 2;
  DP.next_x = DP.close_x - 2;
  DP.prev_x = DP.next_x - 2;
  DP.revert_x = DP.prev_x - 2;
  DP.stage_x = DP.revert_x - 2;
  scr_put(DP.stage_x, y0, 0xEA60, S_BOX);	/* codicon add: Stage Change */
  scr_put(DP.revert_x, y0, 0xEAE2, S_BOX);	/* discard: Revert Change */
  scr_put(DP.prev_x, y0, 0xEAA1, S_BOX);	/* arrow-up: Previous Change */
  scr_put(DP.next_x, y0, 0xEA9A, S_BOX);	/* arrow-down: Next Change */
  scr_put(DP.close_x, y0, 0xEA76, S_BOX);	/* close */
  for (row = 0; row < ph - 2; row++) {
    int sy = y0 + 1 + row, st;
    char num[32];
    const char *s;
    size_t len = 0, line;
    k = DP.top + (size_t)row;
    if (k >= total) {
      scr_fill(DP.x, sy, DP.w, S_TEXT);
      continue;
    }
    if (k < h->on) {	/* the index's line, gone */
      line = h->o0 + k;
      s = quick_old(T->doc, T->real, line, &len);
      st = S_DIFF_DEL;
    }
    else {	/* the line as it is now */
      line = h->n0 + (k - h->on);
      s = line < T->doc->n ? row_at(line)->s : "";
      len = line < T->doc->n ? row_at(line)->len : 0;
      st = S_DIFF_ADD;
    }
    scr_fill(DP.x, sy, DP.w, st);
    snprintf(num, sizeof(num), "%*lu ", nw - 1, (unsigned long)line + 1);
    scr_puts(DP.x, sy, num, S_DIFF_NUM);
    scr_put(DP.x + nw, sy, k < h->on ? '-' : '+', st);
    put_plain(DP.x + nw + 2, sy, DP.w - nw - 3, s ? s : "", len, st);
  }
  for (k = 0; k < (size_t)DP.w; k++) scr_put(DP.x + (int)k, y0 + ph - 1, 0x2500, S_BOX_DIM);
}


/* a key while the peek is open: 1 when it was the peek's (Esc, scrolling) */
static int dirty_key (int k) {
  int code = KEY_CODE(k);
  size_t n, total, body;
  const QHunk *h;
  if (!DP.open) return 0;
  h = qhunks(&n);
  if (h == NULL || DP.hi >= n) {
    DP.open = 0;
    return 0;
  }
  total = h[DP.hi].on + h[DP.hi].nn;
  body = DP.h > 2 ? (size_t)DP.h - 2 : 1;
  switch (code) {
    case K_ESC: DP.open = 0; return 1;
    case K_UP: if (DP.top > 0) DP.top--; return 1;
    case K_DOWN: if (DP.top + body < total) DP.top++; return 1;
    case K_PGUP: DP.top = DP.top > body ? DP.top - body : 0; return 1;
    case K_PGDN: DP.top += body; return 1;
  }
  DP.open = 0;	/* another key: the peek goes, the key is the editor's */
  return 0;
}


static int dirty_click (Mouse *m) {
  if (!DP.open || m->y < DP.y || m->y >= DP.y + DP.h || m->x < DP.x || m->x >= DP.x + DP.w) return 0;
  if (m->wheel) {
    size_t st = (size_t)wheel_step(m->mods);
    if (m->wheel < 0) DP.top = DP.top > st ? DP.top - st : 0;
    else DP.top += st;
    return 1;
  }
  if (!(m->button == 0 && m->press && !m->drag) || m->y != DP.y) return 1;
  if (m->x == DP.stage_x) run_command(CMD_STAGE_CHANGE);
  else if (m->x == DP.revert_x) run_command(CMD_REVERT_CHANGE);
  else if (m->x == DP.prev_x) run_command(CMD_DIRTY_PREV);
  else if (m->x == DP.next_x) run_command(CMD_DIRTY_NEXT);
  else if (m->x == DP.close_x) DP.open = 0;
  return 1;
}


/* change i undone in the buffer: the index's lines back (one undo step with the caller's) */
static void revert_hunk (size_t i) {
  size_t n, k, len;
  const QHunk *hv = qhunks(&n);
  QHunk h;
  Buf b;
  Pos a, e;
  if (hv == NULL || i >= n) return;
  h = hv[i];
  buf_init(&b);
  for (k = 0; k < h.on; k++) {
    const char *s = quick_old(T->doc, T->real, h.o0 + k, &len);
    if (k) buf_putc(&b, '\n');
    if (s) buf_putn(&b, s, len);
  }
  T->sel = 0;
  T->nmc = 0;
  if (h.nn > 0) {
    a.y = h.n0;
    a.x = 0;
    e.y = h.n0 + h.nn - 1;
    e.x = row_at(e.y)->len;
    if (h.on == 0) {	/* lines that came: they go with their newline */
      if (e.y + 1 < T->doc->n) {
        e.y++;
        e.x = 0;
      }
      else if (a.y > 0) {
        a.y--;
        a.x = row_at(a.y)->len;
      }
    }
    ed_delete(a, e);
    if (h.on > 0) ed_insert(a, b.s ? b.s : "", b.len);
  }
  else if (h.n0 < T->doc->n) {	/* lines that went come back, before line n0 */
    a.y = h.n0;
    a.x = 0;
    buf_putc(&b, '\n');
    ed_insert(a, b.s, b.len);
  }
  else {	/* at the end */
    Buf c;
    buf_init(&c);
    buf_putc(&c, '\n');
    buf_putn(&c, b.s ? b.s : "", b.len);
    a.y = T->doc->n - 1;
    a.x = row_at(a.y)->len;
    ed_insert(a, c.s, c.len);
    buf_free(&c);
  }
  buf_free(&b);
  a.y = h.n0;
  a.x = 0;
  move_h(doc_clamp(T->doc, a), 0);
}


/* the changes the cursor's lines (the selection's) touch, into out; how many */
static size_t hunks_in_sel (const QHunk *h, size_t n, QHunk *out, size_t *idx) {
  size_t ly, hy, i, k = 0;
  sel_lines(&ly, &hy);
  for (i = 0; i < n; i++)
    if (hunk_last(&h[i]) >= ly && hunk_line(&h[i]) <= hy) {
      out[k] = h[i];
      idx[k++] = i;
    }
  return k;
}


/* line y of the buffer as a line of the index's copy (a changed one: the change's first) */
static size_t index_line (const QHunk *h, size_t n, size_t y) {
  size_t i;
  long off = 0;
  for (i = 0; i < n; i++) {
    if (y < h[i].n0) break;
    if (y < h[i].n0 + h[i].nn) return h[i].o0;
    off += (long)h[i].on - (long)h[i].nn;
  }
  return (size_t)((long)y + off);
}


/* the change at the cursor, the peek's when it is open; -1: none */
static long hunk_here (const QHunk *h, size_t n) {
  if (DP.open && DP.hi < n) return (long)DP.hi;
  return hunk_at(h, n, T->cur.y);
}


/* after a stage or a revert the peek shows what is left, or goes */
static void dirty_after (size_t was) {
  size_t n;
  if (!DP.open) return;
  qhunks(&n);
  if (n == 0) DP.open = 0;
  else dirty_show((long)(was < n ? was : n - 1));
}


static void quick_cmd (int cmd) {
  size_t n, k, i;
  const QHunk *h;
  long at;
  if (cmd == CMD_STAGE_RANGES || cmd == CMD_UNSTAGE_RANGES || cmd == CMD_REVERT_RANGES) {
    if (G->diff && HAS_DIFF) {	/* the diff editor's change at its bar */
      diff_range(cmd == CMD_STAGE_RANGES ? 0 : cmd == CMD_UNSTAGE_RANGES ? 1 : 2);
      if (!diff_active()) run_command(CMD_CLOSE);	/* nothing left of it: its tab goes */
      return;
    }
  }
  if (git_root() == NULL) {
    toast(1, "The folder currently open doesn't have a git repository.");
    return;
  }
  h = qhunks(&n);
  if (h == NULL) {
    if (HAS_DOC && !G->diff) toast(0, "%s is not in git's index", HAS_DOC && T->real ? path_basename(T->real) : "The file");
    return;
  }
  switch (cmd) {
    case CMD_DIRTY_NEXT: case CMD_DIRTY_PREV:
      if (n == 0) toast(0, "There are no changes in this file");
      else dirty_show(DP.open ? (long)((DP.hi + (cmd == CMD_DIRTY_NEXT ? 1 : n - 1)) % n)
                              : hunk_next(h, n, cmd == CMD_DIRTY_NEXT ? 1 : -1));
      return;
    case CMD_CHANGE_NEXT: case CMD_CHANGE_PREV: {
      Pos p;
      if (n == 0) return;
      at = hunk_next(h, n, cmd == CMD_CHANGE_NEXT ? 1 : -1);
      p.y = hunk_line(&h[at]);
      p.x = 0;
      move_h(doc_clamp(T->doc, p), 0);
      if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
      return;
    }
    case CMD_STAGE_CHANGE: {
      QHunk one;
      if ((at = hunk_here(h, n)) < 0) {
        toast(0, "There is no change at the cursor");
        return;
      }
      one = h[at];
      if (quick_stage(T->doc, T->real, &one, 1) == 0) {
        git_refresh();
        dirty_after((size_t)at);
      }
      return;
    }
    case CMD_REVERT_CHANGE:
      if ((at = hunk_here(h, n)) < 0) {
        toast(0, "There is no change at the cursor");
        return;
      }
      doc_group(T->doc);
      revert_hunk((size_t)at);
      doc_group(T->doc);
      dirty_after((size_t)at);
      return;
    case CMD_STAGE_RANGES: case CMD_REVERT_RANGES: {
      QHunk *sel = (QHunk *)xmalloc((n + 1) * sizeof(QHunk));
      size_t *idx = (size_t *)xmalloc((n + 1) * sizeof(size_t));
      k = hunks_in_sel(h, n, sel, idx);
      if (k == 0) toast(0, "There are no changes in the selection");
      else if (cmd == CMD_STAGE_RANGES) {
        if (quick_stage(T->doc, T->real, sel, k) == 0) git_refresh();
      }
      else {
        doc_group(T->doc);
        for (i = k; i-- > 0;) revert_hunk(idx[i]);	/* the last first: the lines before stay */
        doc_group(T->doc);
      }
      free(sel);
      free(idx);
      return;
    }
    case CMD_UNSTAGE_RANGES: {
      size_t ly, hy;
      int r;
      sel_lines(&ly, &hy);
      r = quick_unstage(T->real, index_line(h, n, ly), index_line(h, n, hy));
      if (r == 0) git_refresh();
      else if (r == 1) toast(0, "There are no staged changes in the selection");
      return;
    }
  }
}


/*
** Merge conflicts. Their colors are VS Code's merge.currentHeaderBackground
** and the rest, over the editor's background; after the <<<<<<< line the
** actions VS Code shows as a CodeLens above it.
*/
static struct {
  int sy[64], x0[64], x1[64], act[64];
  long ci[64];
  int n;
} CL;


static uint32_t tint (uint32_t bg, uint32_t c, int pct) {
  uint32_t r = 0;
  int sh;
  for (sh = 0; sh <= 16; sh += 8) {
    uint32_t a = (bg >> sh) & 255, b = (c >> sh) & 255;
    r |= ((a * (uint32_t)(100 - pct) + b * (uint32_t)pct) / 100) << sh;
  }
  return r;
}


/* s in its own colors; the columns it took */
static int put_colored (int x, int y, int end, const char *s, uint32_t fg, uint32_t bg) {
  int n = 0;
  for (; *s && x + n < end; s++, n++) scr_put_rgb(x + n, y, (unsigned char)*s, fg, bg, 0);
  return n;
}


static void conflict_row (int sy, size_t y, int gw, size_t from, int other) {
  long ci;
  const Conflict *c;
  size_t n, cur_end;
  uint32_t bg, col, hb;
  int pct, x, x0 = L.ed_x + gw, w = text_cols();
  if (!HAS_DOC || T->page || (ci = conflict_at(T->doc, y)) < 0) return;
  c = conflicts(T->doc, &n) + ci;
  cur_end = c->base != (size_t)-1 ? c->base : c->mid;
  bg = ui_color(C_EDITOR_BG);
  if (y == c->mid) return;	/* ======= keeps the editor's */
  if (y == c->start) col = 0x40C8AE, pct = 50;	/* the current change's header */
  else if (y < cur_end) col = 0x40C8AE, pct = 20;
  else if (y == c->base) col = 0x606060, pct = 40;	/* the common ancestors' (diff3) */
  else if (y < c->mid) col = 0x606060, pct = 20;
  else if (y < c->end) col = 0x40A6FF, pct = 20;	/* the incoming change */
  else col = 0x40A6FF, pct = 50;
  hb = tint(bg, col, pct);
  for (x = 0; x < w; x++) scr_set_bg(x0 + x, sy, hb);
  if (from == 0 && (y == c->start || y == c->end)) {	/* after the marker: what it is, and the actions */
    Pos e;
    int cx, cy, end = x0 + w;
    uint32_t dim = ui_color(C_DIM);
    e.y = y;
    e.x = row_at(y)->len;
    if (!screen_at(e, &cx, &cy) || cy != sy) return;
    cx += 1 + put_colored(cx + 1, sy, end, y == c->start ? "(Current Change)" : "(Incoming Change)", dim, hb);
    if (y == c->start) {
      static const char *const what[] = {"Accept Current Change", "Accept Incoming Change",
                                         "Accept Both Changes", "Compare Changes"};
      int i;
      cx += 2;
      for (i = 0; i < 4 && cx < end; i++) {
        int w0 = put_colored(cx, sy, end, what[i], ui_color(C_ACCENT), hb);
        if (!other && CL.n < 64) {	/* the group in front's, for clicks */
          CL.sy[CL.n] = sy;
          CL.x0[CL.n] = cx;
          CL.x1[CL.n] = cx + w0;
          CL.act[CL.n] = i;
          CL.ci[CL.n++] = ci;
        }
        cx += w0;
        if (i < 3 && cx + 3 < end) cx += put_colored(cx, sy, end, " | ", dim, hb);
      }
    }
  }
}


static void conflict_reset (void) {
  CL.n = 0;
}


/* conflict ci resolved: 0 its current side, 1 the incoming, 2 both */
static void conflict_accept (long ci, int which) {
  size_t n, y, cur_end;
  const Conflict *v = conflicts(T->doc, &n);
  Conflict c;
  Buf b;
  Pos a, e;
  int first = 1;
  if (ci < 0 || (size_t)ci >= n) return;
  c = v[ci];
  cur_end = c.base != (size_t)-1 ? c.base : c.mid;
  buf_init(&b);
  if (which != 1)
    for (y = c.start + 1; y < cur_end; y++) {
      if (!first) buf_putc(&b, '\n');
      buf_putn(&b, row_at(y)->s, row_at(y)->len);
      first = 0;
    }
  if (which != 0)
    for (y = c.mid + 1; y < c.end; y++) {
      if (!first) buf_putc(&b, '\n');
      buf_putn(&b, row_at(y)->s, row_at(y)->len);
      first = 0;
    }
  a.y = c.start;
  a.x = 0;
  if (c.end + 1 < T->doc->n) {
    e.y = c.end + 1;
    e.x = 0;
    if (!first) buf_putc(&b, '\n');
  }
  else {
    e.y = c.end;
    e.x = row_at(c.end)->len;
    if (first && a.y > 0) {
      a.y--;
      a.x = row_at(a.y)->len;
    }
  }
  T->sel = 0;
  T->nmc = 0;
  ed_delete(a, e);
  if (b.len) ed_insert(a, b.s, b.len);
  buf_free(&b);
  a.y = c.start;
  a.x = 0;
  move_h(doc_clamp(T->doc, a), 0);
}


/* the text of one side of conflict ci */
static char *conflict_side (long ci, int incoming, size_t *len) {
  size_t n, y, from, to;
  const Conflict *c = conflicts(T->doc, &n) + ci;
  Buf b;
  buf_init(&b);
  from = incoming ? c->mid + 1 : c->start + 1;
  to = incoming ? c->end : (c->base != (size_t)-1 ? c->base : c->mid);
  for (y = from; y < to; y++) {
    buf_putn(&b, row_at(y)->s, row_at(y)->len);
    buf_putc(&b, '\n');
  }
  buf_putc(&b, '\0');
  *len = b.len - 1;
  return buf_take(&b);
}


static void merge_cmd (int cmd) {
  size_t n, i;
  const Conflict *v;
  long ci;
  if (!HAS_DOC || G->diff || T->page) return;
  v = conflicts(T->doc, &n);
  if (n == 0) {
    toast(0, "No merge conflicts found in this file");
    return;
  }
  ci = conflict_at(T->doc, T->cur.y);
  switch (cmd) {
    case CMD_MERGE_NEXT: case CMD_MERGE_PREV: {
      Pos p;
      if (cmd == CMD_MERGE_NEXT) {
        for (i = 0; i < n && v[i].start <= T->cur.y; i++) ;
        if (i == n) i = 0;
      }
      else {
        for (i = n; i-- > 0 && v[i].start >= T->cur.y;) ;
        if (i == (size_t)-1) i = n - 1;
      }
      p.y = v[i].start;
      p.x = 0;
      move_h(doc_clamp(T->doc, p), 0);
      if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
      return;
    }
    case CMD_MERGE_ALL_CURRENT: case CMD_MERGE_ALL_INCOMING: case CMD_MERGE_ALL_BOTH:
      doc_group(T->doc);
      for (i = n; i-- > 0;)
        conflict_accept((long)i, cmd == CMD_MERGE_ALL_CURRENT ? 0 : cmd == CMD_MERGE_ALL_INCOMING ? 1 : 2);
      doc_group(T->doc);
      return;
  }
  if (ci < 0) {
    toast(0, "Editor cursor is not within a merge conflict");
    return;
  }
  if (cmd == CMD_MERGE_COMPARE) {
    size_t na, nb;
    char *a = conflict_side(ci, 0, &na), *b = conflict_side(ci, 1, &nb), title[300];
    snprintf(title, sizeof(title), "%s: Current Changes \xE2\x86\x94 Incoming Changes",
             T->real ? path_basename(T->real) : "Untitled");
    show_compare(T->real ? T->real : "", a, na, b, nb, title);
    free(a);
    free(b);
    return;
  }
  doc_group(T->doc);
  conflict_accept(ci, cmd == CMD_MERGE_CURRENT ? 0 : cmd == CMD_MERGE_INCOMING ? 1 : 2);
  doc_group(T->doc);
}


static int conflict_click (Mouse *m) {
  int i;
  if (!(m->button == 0 && m->press && !m->drag)) return 0;
  for (i = 0; i < CL.n; i++)
    if (m->y == CL.sy[i] && m->x >= CL.x0[i] && m->x < CL.x1[i]) {
      static const int cmd[] = {CMD_MERGE_CURRENT, CMD_MERGE_INCOMING, CMD_MERGE_BOTH, CMD_MERGE_COMPARE};
      size_t n;
      const Conflict *v = conflicts(T->doc, &n);
      Pos p;
      if (CL.ci[i] < 0 || (size_t)CL.ci[i] >= n) return 1;
      p.y = v[CL.ci[i]].start;	/* the cursor in it: the command takes that one */
      p.x = 0;
      move_h(doc_clamp(T->doc, p), 0);
      merge_cmd(cmd[CL.act[i]]);
      return 1;
    }
  return 0;
}

/* }================================================================== */


/* a file's text: an open tab's (edits and all), else the file's */
static char *file_text (const char *path, size_t *len) {
  int g, i;
  char *real = os_realpath(path), *s = NULL;
  Doc tmp;
  Pos a;
  a.y = a.x = 0;
  for (g = 0; g < g_ngrp && s == NULL; g++)
    for (i = 0; i < g_grp[g].ntab && s == NULL; i++) {
      Tab *t = g_grp[g].tab[i];
      if (!t->page && t->doc->path && (m_fncmp(t->doc->path, path) == 0 || (real && t->real && m_fncmp(t->real, real) == 0)))
        s = doc_text(t->doc, a, doc_end(t->doc), len);
    }
  free(real);
  if (s) return s;
  doc_init(&tmp);
  if (doc_load(&tmp, path) == 0) s = doc_text(&tmp, a, doc_end(&tmp), len);
  doc_free(&tmp);
  return s;
}


/* File: Compare Active File with Saved (Ctrl+K D): the file on disk, then the text */
static void compare_saved (void) {
  char *disk, *mine, title[512];
  size_t nd = 0, nm;
  Pos a;
  if (!HAS_DOC || T->page || G->diff || T->doc->path == NULL) {
    toast(0, "Open a saved file to compare it with its saved version");
    return;
  }
  a.y = a.x = 0;
  disk = doc_disk_text(T->doc, &nd);
  mine = doc_text(T->doc, a, doc_end(T->doc), &nm);
  snprintf(title, sizeof(title), "%s (on Disk) \xE2\x86\x94 %s", doc_name(), doc_name());
  show_compare(T->doc->path, disk, nd, mine, nm, title);
  free(disk);
  free(mine);
}


/* two files: left and right */
static void compare_files (const char *left, const char *right) {
  char *a, *b, title[512];
  size_t na = 0, nb = 0;
  a = file_text(left, &na);
  b = file_text(right, &nb);
  if (a == NULL || b == NULL) toast(1, "Unable to read '%s'", path_basename(a == NULL ? left : right));
  else {
    snprintf(title, sizeof(title), "%s \xE2\x86\x94 %s", path_basename(left), path_basename(right));
    show_compare(right, a, na, b, nb, title);
  }
  free(a);
  free(b);
}


/* File: Compare Active File With...: a file of the folder, picked like Ctrl+P */
static void compare_with (void) {
  Pick p;
  Vec paths, none;
  int r;
  if (!HAS_DOC || T->page || G->diff) return;
  pick_init(&p, "Select a file to compare with");
  vec_init(&paths);
  vec_init(&none);
  for (r = 0; r < ws_count(); r++)
    walk_files(ws_folder(r), ws_count() > 1 ? ws_folder_name(r) : "", &p, &paths, &none, 0);
  r = pick_run(&p);
  if (r >= 0) {
    char *full = xstrdup(paths.v[r]), *mine, *other, title[512];
    size_t nm, no = 0;
    Pos a;
    a.y = a.x = 0;
    mine = doc_text(T->doc, a, doc_end(T->doc), &nm);
    other = file_text(full, &no);
    snprintf(title, sizeof(title), "%s \xE2\x86\x94 %s", doc_name(), path_basename(full));
    if (other) show_compare(T->doc->path ? T->doc->path : full, mine, nm, other, no, title);
    free(mine);
    free(other);
    free(full);
  }
  pick_free(&p);
  vec_free(&paths);
}


/* File: Compare Active File with Clipboard (Ctrl+K C) */
static void compare_clipboard (void) {
  char *mine, title[512];
  size_t nm;
  Pos a;
  if (!HAS_DOC || T->page || G->diff) return;
  a.y = a.x = 0;
  mine = doc_text(T->doc, a, doc_end(T->doc), &nm);
  snprintf(title, sizeof(title), "Clipboard \xE2\x86\x94 %s", doc_name());
  show_compare(T->doc->path ? T->doc->path : "", E.clip ? E.clip : "", E.clip ? E.cliplen : 0, mine, nm, title);
  free(mine);
}


/* File: Revert File: the text as the file has it */
static void revert_file (void) {
  if (!HAS_DOC || T->page || G->diff || T->doc->path == NULL) return;
  reload_doc(T->doc);
}


/* the status bar's encoding: Reopen with Encoding, or Save with Encoding */
static void change_encoding (void) {
  Pick p;
  int r, how, e;
  if (!HAS_DOC || T->page || G->diff) return;
  pick_init(&p, "Select Action");
  p.keep_order = 1;
  pick_add(&p, "Reopen with Encoding", NULL, 0);
  pick_add(&p, "Save with Encoding", NULL, 0);
  how = pick_run(&p);
  pick_free(&p);
  if (how < 0) return;
  pick_init(&p, how == 0 ? "Select File Encoding to Reopen File" : "Select File Encoding to Save with");
  p.keep_order = 1;
  for (e = 0; e < ENC_N; e++) pick_add(&p, enc_name(e), e == T->doc->enc ? "Current Encoding" : enc_id(e), 0);
  p.start = T->doc->enc;
  r = pick_run(&p);
  pick_free(&p);
  if (r < 0) return;
  if (how == 1) {	/* the same text, other bytes */
    T->doc->enc = r;
    if (T->doc->path) save_front(SAVE_EXPLICIT);
    else T->doc->changes++;
    return;
  }
  if (T->doc->path == NULL) {
    T->doc->enc = r;
    return;
  }
  if (doc_dirty(T->doc)) {
    static const char *const bt[] = {"Reopen", "Cancel"};
    if (dialog("Reopening the file discards the changes not saved.", doc_name(), bt, 2) != 0) return;
  }
  T->doc->enc = r;
  T->doc->saved = T->doc->changes;	/* read again, whatever was typed */
  reload_doc(T->doc);
}

/* }================================================================== */


/*
** {==================================================================
** Editor groups: moving editors, the grid's layouts, sizes
** ===================================================================
*/

/*
** Tab i of group gs to group gd at index at (-1: after its active one).
** When gd shows the same text already, that one comes to the front and
** this one goes. gs left empty goes too, unless keep. The group of the
** tab in front after: its index.
*/
static int move_tab (int gs, int i, int gd, int at, int keep) {
  Tab *t;
  Group *d;
  int k, dup = -1;
  if (gs < 0 || gs >= g_ngrp || gd < 0 || gd >= g_ngrp || i < 0 || i >= g_grp[gs].ntab) return g_gcur;
  t = g_grp[gs].tab[i];
  d = &g_grp[gd];
  if (gs == gd) {	/* the same group: another place in its tab bar */
    if (at < 0 || at > d->ntab) at = d->ntab;
    if (at > i) at--;
    memmove(d->tab + i, d->tab + i + 1, (size_t)(d->ntab - i - 1) * sizeof(Tab *));
    memmove(d->tab + at + 1, d->tab + at, (size_t)(d->ntab - 1 - at) * sizeof(Tab *));
    d->tab[at] = t;
    focus_group(gd);
    focus_tab(at);
    return gd;
  }
  for (k = 0; k < d->ntab && !t->page && !t->md; k++)
    if (d->tab[k]->doc == t->doc && !d->tab[k]->md && !d->tab[k]->page) dup = k;
  focus_group(gs);
  if (dup >= 0) tab_free(i);	/* the text stays: the other group's tab shows it */
  else {
    memmove(G->tab + i, G->tab + i + 1, (size_t)(G->ntab - i - 1) * sizeof(Tab *));
    G->ntab--;
    if (G->ntab == 0) G->active = 0;
    else if (G->active > i || G->active >= G->ntab) G->active--;
    T = G->ntab ? G->tab[G->active] : &g_none;
    if (G->ntab == 0 && HAS_DIFF) G->diff = 1;
  }
  focus_group(gd);
  if (dup >= 0) focus_tab(dup);
  else {
    if (at < 0 || at > G->ntab) at = G->ntab ? G->active + 1 : 0;
    if (G->ntab == G->captab) {
      G->captab = G->captab ? G->captab * 2 : 8;
      G->tab = (Tab **)xrealloc(G->tab, (size_t)G->captab * sizeof(Tab *));
    }
    memmove(G->tab + at + 1, G->tab + at, (size_t)(G->ntab - at) * sizeof(Tab *));
    G->tab[at] = t;
    G->ntab++;
    focus_tab(at);
  }
  if (!keep && g_grp[gs].ntab == 0 && !g_grp[gs].dtab && g_ngrp > 1) {
    Tab *front = T;
    group_remove(gs);
    for (gd = 0; gd < g_ngrp; gd++)
      for (k = 0; k < g_grp[gd].ntab; k++)
        if (g_grp[gd].tab[k] == front) {
          focus_group(gd);
          focus_tab(k);
        }
  }
  return g_gcur;
}


/* View: Move Editor into Next / Previous Group: a new group beside when there is none */
static void move_to_group (int d) {
  int to, from = g_gcur;
  if (!HAS_DOC || G->diff) return;
  to = from + d;
  if (to < 0 || to >= g_ngrp) {
    if (G->ntab < 2) return;	/* it would only move itself */
    if ((to = group_add(from, d > 0 ? DROP_RIGHT : DROP_LEFT)) < 0) return;
    if (to <= from) from++;
  }
  move_tab(from, g_grp[from].active, to, -1, 0);
  E.focus = F_EDITOR;
}


/* View: Join Editor Group (with the next, or the one before the last); all: every one into the one in front */
static void join_groups (int all) {
  int g, into = g_gcur;
  if (g_ngrp < 2) return;
  if (!all) {
    int from = g_gcur;
    into = from + 1 < g_ngrp ? from + 1 : from - 1;
    while (g_grp[from].ntab > 0) move_tab(from, 0, into, -1, 1);
    focus_group(from);
    group_remove(from);
    if (into > from) into--;
    focus_group(into);
    return;
  }
  for (g = g_ngrp - 1; g >= 0; g--) {
    if (g == into) continue;
    while (g_grp[g].ntab > 0) move_tab(g, 0, into, -1, 1);
    focus_group(g);
    group_remove(g);
    if (into > g) into--;
  }
  focus_group(into);
}


/* the group next to the one in front (DROP_LEFT ...), -1: none; its place from the last picture */
static int group_toward (int d) {
  int g, best = -1, bd = 1 << 30, cx = L.gx[g_gcur] + L.gw[g_gcur] / 2, cy = L.gy[g_gcur] + L.gh[g_gcur] / 2;
  for (g = 0; g < g_ngrp; g++) {
    int gx = L.gx[g] + L.gw[g] / 2, gy = L.gy[g] + L.gh[g] / 2, dist;
    if (g == g_gcur) continue;
    if (d == DROP_LEFT && !(L.gx[g] + L.gw[g] <= L.gx[g_gcur])) continue;
    if (d == DROP_RIGHT && !(L.gx[g] >= L.gx[g_gcur] + L.gw[g_gcur])) continue;
    if (d == DROP_UP && !(L.gy[g] + L.gh[g] <= L.gy[g_gcur])) continue;
    if (d == DROP_DOWN && !(L.gy[g] >= L.gy[g_gcur] + L.gh[g_gcur])) continue;
    dist = abs(gx - cx) + abs(gy - cy) * 3;
    if (dist < bd) {
      bd = dist;
      best = g;
    }
  }
  return best;
}


/* Ctrl+1 .. Ctrl+4: group n; the one after the last is made (VS Code's focusSecondEditorGroup ...) */
static void focus_nth (int n) {
  if (n < g_ngrp) focus_group(n);
  else if (n == g_ngrp && HAS_DOC && !T->page) split_editor();
  E.focus = F_EDITOR;
}


/* View > Editor Layout: rows[c] groups in column c; editors of groups that go join the last one */
static void set_layout (int ncol, const int *rows) {
  int need = 0, c, r, k = 0;
  for (c = 0; c < ncol; c++) need += rows[c];
  while (g_ngrp > need) {
    int from = g_ngrp - 1;
    while (g_grp[from].ntab > 0) move_tab(from, 0, need - 1, -1, 1);
    g_ngrp--;
    free(g_grp[from].tab);
    memset(&g_grp[from], 0, sizeof(Group));
  }
  while (g_ngrp < need) memset(&g_grp[g_ngrp++], 0, sizeof(Group));
  for (c = 0; c < ncol; c++) {
    g_colw[c] = 100;
    for (r = 0; r < rows[c]; r++, k++) {
      g_grp[k].col = c;
      g_grp[k].hw = 100;
    }
  }
  E.grp_max = 0;
  focus_group(g_gcur < g_ngrp ? g_gcur : 0);
  E.focus = F_EDITOR;
}


/* View: Reset Editor Group Sizes */
static void reset_sizes (void) {
  int g;
  for (g = 0; g < MAX_GRP; g++) {
    g_colw[g] = 100;
    g_grp[g].hw = 100;
  }
  E.grp_max = 0;
}


/* View: Toggle Editor Group Sizes (Ctrl+K Ctrl+M): the group in front big, or all even again */
static void toggle_group_sizes (void) {
  int g;
  if (E.grp_max) {
    reset_sizes();
    return;
  }
  for (g = 0; g < MAX_GRP; g++) {
    g_colw[g] = 1;
    g_grp[g].hw = 1;
  }
  g_colw[G->col] = 1000;
  G->hw = 1000;
  E.grp_max = 1;
}


/* where group g's border is dragged to: the weights become the sizes, then the border moves */
static void group_border (int which, int x, int y) {
  int g = which % 1000, c, k, cw[MAX_GRP], ncol = grp_ncol();
  if (g < 0 || g >= g_ngrp) return;
  if (which >= 2000) {	/* its bottom: g and the one under it */
    int nb = g + 1, total, h;
    if (nb >= g_ngrp || g_grp[nb].col != g_grp[g].col) return;
    for (k = 0; k < g_ngrp; k++)
      if (g_grp[k].col == g_grp[g].col) g_grp[k].hw = L.gh[k] > 1 ? L.gh[k] : 1;
    total = L.gh[g] + L.gh[nb];
    h = y - L.gy[g];
    if (h < 4) h = 4;
    if (h > total - 4) h = total - 4;
    if (h < 1 || total - h < 1) return;
    g_grp[g].hw = h;
    g_grp[nb].hw = total - h;
  }
  else {	/* its right: its column and the next */
    int total, w;
    c = g_grp[g].col;
    if (c + 1 >= ncol) return;
    for (k = 0; k < ncol; k++) cw[k] = 1;
    for (k = 0; k < g_ngrp; k++) cw[g_grp[k].col] = L.gw[k] > 1 ? L.gw[k] : 1;
    for (k = 0; k < ncol; k++) g_colw[k] = cw[k];
    total = cw[c] + cw[c + 1];
    w = x - L.gx[g];
    if (w < 8) w = 8;
    if (w > total - 8) w = total - 8;
    if (w < 1 || total - w < 1) return;
    g_colw[c] = w;
    g_colw[c + 1] = total - w;
  }
  E.grp_max = 0;
}


/* the group at a cell: its index; 1000 + g on g's right border, 2000 + g on its bottom one; -1 none */
static int group_at (int x, int y) {
  int g;
  for (g = 0; g < g_ngrp; g++) {
    if (x >= L.gx[g] && x < L.gx[g] + L.gw[g] && y >= L.gy[g] && y < L.gy[g] + L.gh[g]) return g;
    if (g_ngrp > 1 && x == L.gx[g] + L.gw[g] && y >= L.gy[g] && y < L.gy[g] + L.gh[g] && x < L.area_x + L.area_w)
      return 1000 + g;
    if (g_ngrp > 1 && y == L.gy[g] + L.gh[g] && x >= L.gx[g] && x < L.gx[g] + L.gw[g] &&
        y < L.body_y + L.body_h - L.panel_h)
      return 2000 + g;
  }
  return -1;
}


/* a dragged tab over (x, y): where it would go, into E.tdrop_* */
static void drop_target (int x, int y) {
  int g = group_at(x, y);
  E.tdrop_zone = DROP_NONE;
  E.tdrop_g = -1;
  E.tdrop_at = -1;
  if (g < 0 || g >= 1000) return;
  E.tdrop_g = g;
  if (y == L.gy[g]) {	/* the tab bar: between two tabs */
    int gs = g_gcur, i;
    Group *g0 = G;
    Tab *t0 = T;
    G = &g_grp[g];
    T = G->ntab ? G->tab[G->active] : &g_none;
    use_group(g);
    draw_tabs();
    E.tdrop_at = G->ntab;
    for (i = 0; i < g_tabs.n && i < G->ntab; i++)
      if (g_tabs.x0[i] >= 0 && x < (g_tabs.x0[i] + g_tabs.x1[i]) / 2) {
        E.tdrop_at = i;
        break;
      }
    G = g0;
    T = t0;
    use_group(gs);
    E.tdrop_zone = DROP_IN;
    return;
  }
  {	/* the text: an edge splits (a quarter of it, like VS Code), the middle moves it there */
    int fx = (x - L.gx[g]) * 100 / (L.gw[g] > 0 ? L.gw[g] : 1), fy = (y - L.gy[g] - 1) * 100 / (L.gh[g] > 1 ? L.gh[g] - 1 : 1);
    int alone = E.tdrag_g == g && g_grp[g].ntab < 2;	/* its only tab: a split would leave the group empty */
    E.tdrop_zone = DROP_IN;
    if (!alone && g_ngrp < MAX_GRP) {
      if (fx < 25) E.tdrop_zone = DROP_LEFT;
      else if (fx >= 75) E.tdrop_zone = DROP_RIGHT;
      else if (fy < 25) E.tdrop_zone = DROP_UP;
      else if (fy >= 75) E.tdrop_zone = DROP_DOWN;
    }
    if (E.tdrop_zone == DROP_IN && g == E.tdrag_g) E.tdrop_zone = DROP_NONE;	/* where it is */
  }
}


/* the dragged tab dropped where drop_target said */
static void drop_tab (void) {
  int gs = E.tdrag_g, i = E.tdrag_i, gd = E.tdrop_g;
  if (E.tdrop_zone == DROP_NONE || gs < 0 || gs >= g_ngrp || i >= g_grp[gs].ntab || gd < 0 || gd >= g_ngrp) return;
  if (E.tdrop_zone != DROP_IN) {	/* a new group on that side */
    int ng = group_add(gd, E.tdrop_zone);
    if (ng < 0) return;
    if (gs >= ng) gs++;
    gd = ng;
  }
  move_tab(gs, i, gd, E.tdrop_at, 0);
  E.focus = F_EDITOR;
}

/* }================================================================== */


static void run_command (int cmd) {
  E.follow = 1;
  switch (cmd) {
    case CMD_NEW:
      tab_new();
      E.focus = F_EDITOR;
      break;
    case CMD_OPEN_FILE: {
      char *f = file_dialog("Open File", 0);
      if (f && open_file(f, 0) == 0) E.focus = F_EDITOR;
      free(f);
      break;
    }
    case CMD_OPEN_FOLDER: {
      char *d = file_dialog("Open Folder", 1);
      if (d) open_folder(d);
      free(d);
      break;
    }
    case CMD_OPEN_PROJECT: open_project(); break;
    case CMD_OPEN_WORKSPACE: {
      char *f = file_dialog("Open Workspace from File", 0);
      if (f && is_workspace_file(f)) open_workspace(f);
      else if (f) toast(1, "Select a .code-workspace file");
      free(f);
      break;
    }
    case CMD_ADD_FOLDER: {
      char *d = file_dialog("Add Folder to Workspace", 1);
      if (d) {
        ws_add_folder(d);
        apply_settings(0);
        E.side = 1;
        E.view = VIEW_FILES;
      }
      free(d);
      break;
    }
    case CMD_SAVE_WORKSPACE: {
      char *def, *f;
      Buf b;
      buf_init(&b);
      buf_printf(&b, "%s", ws_file() ? ws_file() : side_root());
      if (ws_file() == NULL) {	/* next to the first folder, named after it */
        char *j = path_join(side_root(), path_basename(side_root()));
        buf_free(&b);
        buf_init(&b);
        buf_printf(&b, "%s.code-workspace", j);
        free(j);
      }
      buf_putc(&b, '\0');
      def = buf_take(&b);
      f = ask_text("Save Workspace As", def);
      free(def);
      if (f && *f) {
        if (ws_save_as(f) == 0) {
          recent_add(ws_file());
          toast(0, "Saved %s", path_basename(f));
        }
        else toast(1, "Unable to write '%s'", f);
      }
      free(f);
      break;
    }
    case CMD_CLOSE_WORKSPACE:
      if (ws_active()) {
        ws_close();
        apply_settings(0);
        git_refresh();
      }
      break;
    case CMD_IMPORT_VSCODE: import_vscode(); break;
    case CMD_SAVE:
    case CMD_SAVE_AS:
      if (!HAS_DOC || T->page) return;
      if (cmd == CMD_SAVE_AS || T->doc->path == NULL) {
        char *was = T->doc->path ? xstrdup(T->doc->path) : NULL;
        save_as();
        files_index_stale();
        if (T->doc->path && (was == NULL || m_fncmp(was, T->doc->path) != 0)) doc_stamp(T->doc);	/* a new file: nothing newer */
        free(was);
      }
      if (T->doc->path == NULL) return;
      save_front(SAVE_EXPLICIT);
      break;
    case CMD_CLOSE:
      if (G->diff) {
        diff_close();
        G->diff = G->dtab = 0;
      }
      else if (HAS_DOC) close_tab(G->active);
      if (!HAS_DOC && !HAS_DIFF && g_ngrp > 1) close_group();	/* its last tab: the group goes */
      break;
    case CMD_QUIT: {	/* every group asks for its files, unless hot exit keeps them */
      int g, keep = g_gcur, ok = 1;
      if (!panel_confirm_exit()) break;	/* terminal.integrated.confirmOnExit */
      if (opt.hot_exit && g_session) {	/* files.hotExit: no questions, all comes back next time */
        session_write(1);
        E.quit = 1;
        break;
      }
      for (g = 0; g < g_ngrp && ok; g++) {
        focus_group(g);
        ok = save_all_changes();
      }
      if (ok) {
        focus_group(keep < g_ngrp ? keep : 0);
        if (g_session) session_write(0);
        E.quit = 1;
      }
      else focus_group(keep < g_ngrp ? keep : 0);
      break;
    }
    case CMD_UNDO: if (HAS_DOC) undo(0); break;
    case CMD_REDO: if (HAS_DOC) undo(1); break;
    case CMD_CUT: if (HAS_DOC) copy(1); break;
    case CMD_COPY: if (HAS_DOC) copy(0); break;
    case CMD_PASTE: if (HAS_DOC && E.clip) insert(E.clip, E.cliplen); break;
    case CMD_FIND:
      if (HAS_DOC && T->page == PAGE_SETTINGS && !G->diff) sui_search();
      else if (HAS_DOC && !G->diff && !T->page) {
        open_find();
        E.replacing = E.in_repl = 0;
      }
      break;
    case CMD_REPLACE:
      if (HAS_DOC && !G->diff) {
        open_find();
        E.replacing = 1;
        E.in_repl = E.find[0] != '\0';	/* something to look for: to the replace box */
      }
      break;
    case CMD_FIND_FILES:
    case CMD_REPLACE_FILES: {
      Pos a, b;
      char *t = NULL;
      sel_range(&a, &b);
      show_view(VIEW_SEARCH);
      if (HAS_DOC && T->sel && a.y == b.y) t = doc_text(T->doc, a, b, NULL);
      if (cmd == CMD_REPLACE_FILES) search_replace_mode(t);
      else if (t) search_set(t);
      free(t);
      break;
    }
    case CMD_SELECT_ALL:
      if (HAS_DOC) {
        Pos p;
        p.y = p.x = 0;
        move_to(p, 0);
        move_h(doc_end(T->doc), 1);
        E.follow = 0;
      }
      break;
    case CMD_LINE_UP: if (HAS_DOC) move_lines(0); break;
    case CMD_LINE_DOWN: if (HAS_DOC) move_lines(1); break;
    case CMD_EXPLORER: show_view(VIEW_FILES); break;
    case CMD_SEARCH: show_view(VIEW_SEARCH); break;
    case CMD_GIT: show_view(VIEW_GIT); break;
    case CMD_EXTENSIONS: show_view(VIEW_EXT); break;
    case CMD_TEST_VIEW: show_view(VIEW_TEST); break;
    case CMD_TEST_OUTPUT: {	/* the OUTPUT view, on Test Results */
      int i;
      for (i = 0; i < out_count() && strcmp(out_name(i), "Test Results") != 0; i++) ;
      if (i == out_count()) {
        toast(0, "No test has run yet.");
        break;
      }
      out_select(i);
      E.panel = 1;
      E.panel_view = 3;
      break;
    }
    case CMD_TEST_RUN_ALL: case CMD_TEST_RUN_CURSOR: case CMD_TEST_RUN_FILE: case CMD_TEST_RERUN:
    case CMD_TEST_DEBUG_CURSOR: case CMD_TEST_REFRESH: case CMD_TEST_CANCEL: case CMD_TEST_COLLAPSE:
      test_command(cmd);
      break;
    case CMD_PROBLEMS_FILTER:
      E.panel = 1;
      E.panel_view = 1;
      E.focus = F_PANEL;
      PB.in_filter = 1;
      break;
    case CMD_PROBLEMS_COLLAPSE: problems_collapse_all(); break;
    case CMD_PROBLEMS_ACTIVE: PB.active_only = !PB.active_only; break;
    case CMD_PROBLEMS_COPY: problems_copy(); break;
    case CMD_OUTLINE_FOLLOW: OL.nofollow = !OL.nofollow; break;
    case CMD_OUTLINE_SORT: {
      Pick p;
      int r;
      pick_init(&p, "Sort By");
      p.keep_order = 1;
      pick_add(&p, "Position", OL.sort == 0 ? "current" : NULL, 0);
      pick_add(&p, "Name", OL.sort == 1 ? "current" : NULL, 0);
      pick_add(&p, "Category", OL.sort == 2 ? "current" : NULL, 0);
      r = pick_run(&p);
      pick_free(&p);
      if (r >= 0) OL.sort = r;
      break;
    }
    case CMD_OUTLINE_COLLAPSE: outline_collapse_all(); break;
    case CMD_DIFF_WS: diff_toggle_trim(); break;
    case CMD_DIFF_HIDE: diff_toggle_hide(); break;
    case CMD_INSPECT_TOKENS: {	/* Developer: Inspect Editor Tokens and Scopes */
      char sc[1024];
      if (!HAS_DOC || G->diff || T->page) break;
      if (tm_scopes(T->doc, T->sx, T->cur.y, T->cur.x, sc, sizeof(sc))) toast(0, "%s", sc);
      else toast(0, "No TextMate grammar colors %s here", syntax_name(T->sx));
      break;
    }
    case CMD_EXT_VSIX: {
      char *f = file_dialog("Install from VSIX: the .vsix file", 0);
      if (f) ext_install_vsix(f);
      free(f);
      break;
    }
    case CMD_SIDEBAR:
      E.side = !E.side;
      if (!E.side) E.focus = F_EDITOR;
      break;
    case CMD_GOTO: if (HAS_DOC && !G->diff) goto_line(NULL); break;
    case CMD_NEXT_CHANGE: if (G->diff) diff_change(0); break;
    case CMD_PREV_CHANGE: if (G->diff) diff_change(1); break;
    case CMD_QUICK_OPEN: quick_open(); break;
    case CMD_PALETTE: palette(NULL); break;
    case CMD_KEYS: keyboard_shortcuts(); break;
    case CMD_CURSORS_LINE_ENDS: case CMD_CURSOR_UNDO: case CMD_CURSOR_REDO: case CMD_MOVE_SEL_NEXT:
    case CMD_CHANGE_ALL: case CMD_SELECT_ALL_FIND: case CMD_REINDENT: case CMD_REINDENT_SEL:
    case CMD_DEL_ALL_LEFT: case CMD_DEL_ALL_RIGHT: case CMD_TRANSPOSE: case CMD_PART_LEFT: case CMD_PART_RIGHT:
    case CMD_DEL_PART_LEFT: case CMD_DEL_PART_RIGHT: case CMD_ADD_COMMENT: case CMD_REMOVE_COMMENT:
    case CMD_SCROLL_PAGE_UP: case CMD_SCROLL_PAGE_DOWN:
      if (!HAS_DOC || G->diff || T->page || T->md) break;
      E.focus = F_EDITOR;
      edit_extra(cmd);
      break;
    case CMD_TAB_FOCUS:
      E.tab_focus = !E.tab_focus;
      toast(0, E.tab_focus ? "Pressing Tab will now move focus to the next focusable element."
                           : "Pressing Tab will now insert the tab character.");
      break;
    case CMD_TYPE: {	/* {"text": "..."}: typed where the keys go */
      const char *t = json_str(json_get(keys_args(), "text"), NULL);
      if (t == NULL) break;
      if (E.focus == F_PANEL && E.panel && E.panel_view == 0) panel_send(t, strlen(t));
      else if (HAS_DOC && !G->diff && !T->page && !T->md) {
        doc_group(T->doc);
        insert(t, strlen(t));
        doc_group(T->doc);
      }
      break;
    }
    case CMD_TERM_SEND: {	/* {"text": "\u001b[A"}: to the terminal as it is */
      const char *t = json_str(json_get(keys_args(), "text"), NULL);
      if (t && panel_count() > 0) panel_send(t, strlen(t));
      break;
    }
    case CMD_AUTO_SAVE:	/* File > Auto Save: afterDelay, or off */
      settings_put("files.autoSave", opt.auto_save != AUTO_OFF ? "off" : "afterDelay");
      opt.auto_save = opt.auto_save != AUTO_OFF ? AUTO_OFF : AUTO_DELAY;
      break;
    case CMD_COMPARE_SAVED: compare_saved(); break;
    case CMD_COMPARE_WITH: compare_with(); break;
    case CMD_COMPARE_CLIP: compare_clipboard(); break;
    case CMD_SELECT_COMPARE: {
      const char *f = files_selected();
      if (f == NULL && HAS_DOC && T->doc->path) f = T->doc->path;
      if (f) {
        free(g_cmp_sel);
        g_cmp_sel = xstrdup(f);
      }
      break;
    }
    case CMD_COMPARE_SELECTED: {
      const char *f = files_selected();
      if (g_cmp_sel && f) compare_files(g_cmp_sel, f);
      break;
    }
    case CMD_ENCODING: change_encoding(); break;
    case CMD_REVERT: revert_file(); break;
    case CMD_DIRTY_NEXT: case CMD_DIRTY_PREV: case CMD_CHANGE_NEXT: case CMD_CHANGE_PREV:
    case CMD_STAGE_CHANGE: case CMD_REVERT_CHANGE: case CMD_STAGE_RANGES: case CMD_UNSTAGE_RANGES:
    case CMD_REVERT_RANGES:
      quick_cmd(cmd);
      break;
    case CMD_MERGE_CURRENT: case CMD_MERGE_INCOMING: case CMD_MERGE_BOTH: case CMD_MERGE_ALL_CURRENT:
    case CMD_MERGE_ALL_INCOMING: case CMD_MERGE_ALL_BOTH: case CMD_MERGE_NEXT: case CMD_MERGE_PREV:
    case CMD_MERGE_COMPARE:
      merge_cmd(cmd);
      break;
    case CMD_GIT_INIT: git_init_repo(); break;
    case CMD_GIT_PUBLISH: git_publish(); break;
    case CMD_GIT_CLONE: {
      char *dir = git_clone();
      if (dir) open_folder(dir);
      free(dir);
      break;
    }
    case CMD_SIDEBAR_POS:
      opt.side_right = !opt.side_right;
      settings_put("workbench.sideBar.location", opt.side_right ? "right" : "left");
      break;
    case CMD_BREADCRUMBS: if (HAS_DOC && !G->diff && !T->page) crumb_open(CB.n - 1); break;
    case CMD_CLOSE_OTHERS: close_many(CLOSE_OTHERS); break;
    case CMD_CLOSE_RIGHT: close_many(CLOSE_RIGHT); break;
    case CMD_CLOSE_SAVED: close_many(CLOSE_SAVED); break;
    case CMD_CLOSE_GROUP_ALL:
      close_many(CLOSE_ALL);
      if (!HAS_DOC && !HAS_DIFF && g_ngrp > 1) close_group();
      break;
    case CMD_CLOSE_ALL: {	/* every group */
      int g;
      for (g = g_ngrp - 1; g >= 0; g--) {
        focus_group(g);
        close_many(CLOSE_ALL);
        if (!HAS_DOC && !HAS_DIFF && g_ngrp > 1) close_group();
      }
      break;
    }
    case CMD_PIN: pin_tab(1); break;
    case CMD_UNPIN: pin_tab(0); break;
    case CMD_KEEP_OPEN: if (HAS_DOC) T->preview = 0; break;
    case CMD_REVEAL: if (HAS_DOC) show_view(VIEW_FILES); break;
    case CMD_SWITCH_EDITOR: switch_editor(0, 0); break;
    case CMD_SHOW_EDITORS: switch_editor(1, 0); break;
    case CMD_MD_PREVIEW: md_preview(0); break;
    case CMD_HELP_KEYS: help_page("keyboard-shortcuts.md", help_keys_md); break;
    case CMD_HELP_TIPS: help_page("tips-and-tricks.md", help_tips_md); break;
    case CMD_HELP_COMMANDS: palette(NULL); break;
    case CMD_MD_SIDE: md_preview(1); break;
    case CMD_OPEN_EDITORS:
    case CMD_TIMELINE:
      show_view(VIEW_FILES);
      if (cmd == CMD_OPEN_EDITORS) OE.open = 1;
      else TL.open = TL.stale = 1;
      E.outline_focus = cmd == CMD_OPEN_EDITORS ? PANE_EDITORS : PANE_TIMELINE;
      break;
    case CMD_HISTORY_RESTORE: history_pick(1); break;
    case CMD_HISTORY_COMPARE: history_pick(0); break;
    case CMD_NAV_BACK: nav_go(-1); break;
    case CMD_NAV_FORWARD: nav_go(1); break;
    case CMD_GOTO_SYMBOL: goto_symbol(NULL); break;
    case CMD_REFERENCES: locations_ask(LOC_REFS); break;
    case CMD_IMPLEMENTATION: locations_ask(LOC_IMPL); break;
    case CMD_TYPE_DEF: locations_ask(LOC_TYPE); break;
    case CMD_PEEK_DEF: locations_ask(LOC_PEEK); break;
    case CMD_WORKSPACE_SYMBOL: workspace_symbols(NULL); break;
    case CMD_REOPEN: reopen_closed(); break;
    case CMD_LANGUAGE: change_language(); break;
    case CMD_EOL: change_eol(); break;
    case CMD_INDENTATION: change_indentation(); break;
    case CMD_INDENT_SPACES: indent_using(0); break;
    case CMD_INDENT_TABS: indent_using(1); break;
    case CMD_RENDER_WS:
      opt.render_ws = opt.render_ws == 0 ? 2 : 0;
      settings_put("editor.renderWhitespace", opt.render_ws ? "all" : "none");
      break;
    case CMD_STICKY:
      opt.sticky = !opt.sticky;
      settings_put("editor.stickyScroll.enabled", opt.sticky ? "true" : "false");
      break;
    case CMD_COLUMN_SELECT: toast(0, "Column selection: Shift+Alt+drag, or Ctrl+Shift+Alt+arrows"); break;
    case CMD_PARAM_HINTS:
      if (HAS_DOC && lsp_active(T->doc)) lsp_signature(T->doc, T->cur);
      break;
    case CMD_UPPER: case CMD_LOWER: case CMD_TITLE: case CMD_SORT_ASC: case CMD_SORT_DESC:
    case CMD_JOIN: case CMD_TRIM: case CMD_DEDUP: case CMD_DUP_SEL: case CMD_GOTO_BRACKET:
    case CMD_SELECT_BRACKET: case CMD_TO_SPACES: case CMD_TO_TABS: case CMD_DETECT_INDENT:
      if (!HAS_DOC || G->diff) break;
      E.focus = F_EDITOR;
      snip_end();
      if (cmd != CMD_GOTO_BRACKET && cmd != CMD_SELECT_BRACKET) T->nmc = 0;
      switch (cmd) {
        case CMD_UPPER: transform_case(1); break;
        case CMD_LOWER: transform_case(2); break;
        case CMD_TITLE: transform_case(3); break;
        case CMD_SORT_ASC: sort_lines(0, 0); break;
        case CMD_SORT_DESC: sort_lines(1, 0); break;
        case CMD_DEDUP: sort_lines(0, 1); break;
        case CMD_JOIN: join_lines(); break;
        case CMD_TRIM: trim_trailing(); break;
        case CMD_DUP_SEL: duplicate_selection(); break;
        case CMD_GOTO_BRACKET: bracket_cmd(0); break;
        case CMD_SELECT_BRACKET: bracket_cmd(1); break;
        case CMD_TO_SPACES: convert_indent(0); break;
        case CMD_TO_TABS: convert_indent(1); break;
        case CMD_DETECT_INDENT: doc_detect_indent(T->doc); break;
      }
      break;
    case CMD_FORMAT: format_doc(0); break;
    case CMD_INSERT_SNIPPET: insert_snippet_pick(); break;
    case CMD_SNIPPETS: configure_snippets(); break;
    case CMD_COMMENT: case CMD_BLOCK_COMMENT: case CMD_COPY_UP: case CMD_COPY_DOWN:
    case CMD_DELETE_LINE: case CMD_SELECT_LINE: case CMD_LINE_BELOW: case CMD_LINE_ABOVE:
    case CMD_INDENT: case CMD_OUTDENT:
      if (!HAS_DOC || G->diff) break;
      E.focus = F_EDITOR;
      snip_end();
      if (cmd == CMD_COMMENT) toggle_line_comment();
      else if (cmd == CMD_BLOCK_COMMENT) toggle_block_comment();
      else if (cmd == CMD_COPY_UP || cmd == CMD_COPY_DOWN) copy_lines(cmd == CMD_COPY_DOWN);
      else if (cmd == CMD_DELETE_LINE) delete_lines();
      else if (cmd == CMD_SELECT_LINE) select_line();
      else if (cmd == CMD_LINE_BELOW || cmd == CMD_LINE_ABOVE) insert_line(cmd == CMD_LINE_ABOVE);
      else {
        doc_group(T->doc);
        indent_lines(cmd == CMD_OUTDENT);
        doc_group(T->doc);
      }
      break;
    case CMD_DEBUG_VIEW:
      E.side = 1;
      E.view = VIEW_DEBUG;
      E.focus = F_SIDE;
      break;
    case CMD_DEBUG_CONSOLE:	/* show it and go there; from there: hide it */
      if (E.panel && E.panel_view == 2 && E.focus == F_PANEL) {
        E.panel = 0;
        E.focus = F_EDITOR;
        break;
      }
      E.panel = 1;
      E.panel_view = 2;
      E.focus = F_PANEL;
      break;
    case CMD_BREAKPOINT:
      if (HAS_DOC && !G->diff) dbg_toggle(T->real, T->cur.y);
      break;
    case CMD_BP_CONDITIONAL: case CMD_BP_LOG: case CMD_BP_EDIT:
      if (HAS_DOC && !G->diff && !T->page)
        dbg_edit_bp(T->real, T->cur.y, cmd == CMD_BP_CONDITIONAL ? 0 : cmd == CMD_BP_LOG ? 2 : -1);
      break;
    case CMD_BP_TOGGLE_ENABLE: if (HAS_DOC && !G->diff) dbg_enable_bp(T->real, T->cur.y); break;
    case CMD_RUN_TO_CURSOR: if (HAS_DOC && !G->diff) dbg_run_to(T->real, T->cur.y); break;
    case CMD_JUMP_TO_CURSOR: if (HAS_DOC && !G->diff) dbg_jump_to(T->real, T->cur.y); break;
    case CMD_DEBUG_ADD_WATCH:	/* the selection, else the name at the cursor */
      if (HAS_DOC && !G->diff && !T->page) {
        Pos a, b;
        char *e;
        size_t len;
        sel_range(&a, &b);
        if (!T->sel || a.y != b.y) {
          a = b = T->cur;
          a.x = word_start_at(T->cur);
          b.x = word_end_at(T->cur);
        }
        e = doc_text(T->doc, a, b, &len);
        if (len) dbg_add_watch(e);
        free(e);
        run_command(CMD_DEBUG_VIEW);
        E.focus = F_EDITOR;
      }
      break;
    case CMD_DEBUG_START: case CMD_DEBUG_RUN: case CMD_DEBUG_STOP: case CMD_DEBUG_RESTART:
    case CMD_DEBUG_STEP_OVER: case CMD_DEBUG_STEP_INTO: case CMD_DEBUG_STEP_OUT: case CMD_DEBUG_PAUSE:
    case CMD_DEBUG_CONFIG: case CMD_DEBUG_SELECT:
    case CMD_BP_REMOVE_ALL: case CMD_BP_ENABLE_ALL: case CMD_BP_DISABLE_ALL:
      dbg_command(cmd);
      break;
    case CMD_TASK_RUN: case CMD_TASK_BUILD: case CMD_TASK_CONFIGURE: task_command(cmd); break;
    case CMD_TERMINAL:	/* show it and go there; from there: hide it */
      if (E.panel && E.focus == F_PANEL && E.panel_view == 0) {
        E.panel = E.panel_max = 0;
        E.focus = F_EDITOR;
        break;
      }
      E.panel = 1;
      E.panel_view = 0;
      E.focus = F_PANEL;
      layout();
      if (!panel_alive() && panel_start(L.area_w - 2, L.panel_h - 1) != 0) {
        E.panel = 0;
        E.focus = F_EDITOR;
      }
      break;
    case CMD_TERM_FOCUS:
    case CMD_TERM_RUN_SEL:
    case CMD_TERM_RUN_FILE:
    case CMD_TERM_RECENT:
    case CMD_TERM_RECENT_DIR: {	/* the terminal shown (made when there is none), then ... */
      Pos a, b;
      char *s = NULL;
      size_t n = 0;
      if (cmd == CMD_TERM_RUN_SEL) {	/* the selection, else the cursor's line (VS Code's) */
        if (!HAS_DOC || G->diff || T->page) break;
        if (T->sel) sel_range(&a, &b);
        else {
          a.y = b.y = T->cur.y;
          a.x = 0;
          b.x = row_at(T->cur.y)->len;
        }
        s = doc_text(T->doc, a, b, &n);
      }
      if (cmd == CMD_TERM_RUN_FILE && (!HAS_DOC || G->diff || T->page || T->doc->path == NULL)) {
        toast(0, "There is no file to run: save it first");
        break;
      }
      E.panel = 1;
      E.panel_view = 0;
      layout();
      if (!panel_alive() && panel_start(L.area_w - 2, L.panel_h - 1) != 0) {
        E.panel = 0;
        free(s);
        break;
      }
      if (cmd == CMD_TERM_RUN_SEL) panel_run_text(s, n);	/* the keys stay in the editor, like VS Code */
      else if (cmd == CMD_TERM_RUN_FILE) panel_run_file(T->real ? T->real : T->doc->path);
      else {
        E.focus = F_PANEL;
        if (cmd == CMD_TERM_RECENT) panel_recent_command();
        else if (cmd == CMD_TERM_RECENT_DIR) panel_recent_dir();
      }
      free(s);
      break;
    }
    case CMD_TERM_PREV_CMD: panel_scroll_cmd(-1); break;
    case CMD_TERM_NEXT_CMD: panel_scroll_cmd(1); break;
    case CMD_TERM_SELECT_ALL: panel_select_all(); break;
    case CMD_TERM_COPY: panel_copy(); break;
    case CMD_TERM_PASTE: panel_paste_clip(); break;
    case CMD_TERM_COLOR: panel_change_color(); break;
    case CMD_TERM_ICON: panel_change_icon(); break;
    case CMD_TERMINAL_KILL:	/* the one in front; the panel goes with the last */
      if (!panel_confirm_kill()) break;
      panel_kill();
      if (!panel_alive()) {
        E.panel = E.panel_max = 0;
        if (E.focus == F_PANEL) E.focus = F_EDITOR;
      }
      break;
    case CMD_TERMINAL_NEW:
    case CMD_TERMINAL_NEW_PROFILE:
    case CMD_TERMINAL_SPLIT: {
      int prof = -1;
      if (cmd == CMD_TERMINAL_NEW_PROFILE && (prof = pick_profile("Select the terminal profile to create")) < 0) break;
      E.panel = 1;
      E.panel_view = 0;
      E.focus = F_PANEL;
      layout();
      if ((cmd == CMD_TERMINAL_SPLIT ? panel_split()
           : cmd == CMD_TERMINAL_NEW_PROFILE ? panel_new_profile(L.area_w - 2, L.panel_h - 1, prof)
           : panel_new(L.area_w - 2, L.panel_h - 1)) != 0 && !panel_alive()) {
        E.panel = 0;
        E.focus = F_EDITOR;
      }
      break;
    }
    case CMD_TERMINAL_PREV_PANE: case CMD_TERMINAL_NEXT_PANE:
      panel_focus_pane(cmd == CMD_TERMINAL_PREV_PANE ? -1 : 1);
      break;
    case CMD_TERMINAL_FIND:
      if (!panel_alive()) break;
      E.panel = 1;
      E.panel_view = 0;
      E.focus = F_PANEL;
      panel_find_open();
      break;
    case CMD_TERMINAL_CLEAR: panel_clear(); break;
    case CMD_TERMINAL_RENAME:
      if (panel_alive()) {
        char *name = ask_text("Enter terminal name", panel_name(panel_current()));
        if (name) panel_rename(name);
        free(name);
      }
      break;
    case CMD_TERMINAL_PROFILE: {	/* its name goes to settings.json */
      int r = pick_profile("Select your default terminal profile");
      if (r < 0) break;
      snprintf(opt.term_profile, sizeof(opt.term_profile), "%s", panel_profile_name(r));
      settings_put(term_profile_setting(), panel_profile_name(r));
      toast(0, "New terminals start %s", panel_profile_name(r));
      break;
    }
    case CMD_OUTPUT:	/* the panel with the output; again: back to where the keys were */
      if (E.panel && E.panel_view == 3 && E.focus == F_PANEL) {
        E.panel = 0;
        E.focus = F_EDITOR;
        break;
      }
      E.panel = 1;
      E.panel_view = 3;
      E.focus = F_PANEL;
      break;
    case CMD_OUTPUT_CHANNELS: pick_channel(); break;
    case CMD_OUTPUT_CLEAR: out_clear(); break;
    case CMD_PANEL_MAX:
      E.panel_max = !E.panel_max;
      if (E.panel_max) {
        E.panel = 1;
        E.focus = F_PANEL;
      }
      break;
    case CMD_MINIMAP: E.minimap = !E.minimap; break;
    case CMD_FOLD: case CMD_UNFOLD: case CMD_FOLD_ALL: case CMD_UNFOLD_ALL: case CMD_FOLD_REGIONS:
    case CMD_UNFOLD_REGIONS: case CMD_FOLD_COMMENTS: case CMD_FOLD_L1: case CMD_FOLD_L2: case CMD_FOLD_L3:
    case CMD_FOLD_L4: case CMD_FOLD_L5: case CMD_FOLD_L6: case CMD_FOLD_L7: fold_cmd(cmd); break;
    case CMD_FORMAT_SEL: format_selection(); break;
    case CMD_ORGANIZE_IMPORTS: organize_imports(0); break;
    case CMD_SOURCE_ACTION:
      if (!HAS_DOC || G->diff || T->page) break;
      if (!lsp_active(T->doc)) toast(0, "No source actions available");
      else lsp_source_action(T->doc, "source", 0);
      break;
    case CMD_EXPAND_SEL: expand_selection(); break;
    case CMD_SHRINK_SEL: shrink_selection(); break;
    case CMD_CALL_HIERARCHY: hierarchy_ask(LOC_CALLS_IN); break;
    case CMD_CALLS_OUT: hierarchy_ask(LOC_CALLS_OUT); break;
    case CMD_SUPERTYPES: hierarchy_ask(LOC_SUPER); break;
    case CMD_SUBTYPES: hierarchy_ask(LOC_SUB); break;
    case CMD_THEME: pick_theme(); break;
    case CMD_ZEN:
      E.zen = !E.zen;
      if (E.zen) toast(0, "Zen Mode: Esc Esc or Ctrl+K Z to leave");
      scr_redraw();
      break;
    case CMD_WORDWRAP:
      E.wrap = !E.wrap;
      if (HAS_DOC) {
        T->sub = 0;
        T->left = 0;
      }
      toast(0, "Word wrap: %s", E.wrap ? "on" : "off");
      break;
    case CMD_DEFINITION: if (HAS_DOC && !G->diff) lsp_define(T->doc, T->cur); break;
    case CMD_SUGGEST:	/* again while the list shows: its details, or not */
      if (CP.open && !CP.choice) g_comp_details = !g_comp_details;
      else if (HAS_DOC && !G->diff) comp_ask(1);
      break;
    case CMD_NEXT_PROBLEM: next_problem(0); break;
    case CMD_RENAME:	/* F2 in the Explorer: the file's name */
      if (E.focus == F_SIDE && E.side && E.view == VIEW_FILES && !E.outline_focus) explorer_cmd(FC_RENAME);
      else if (E.focus == F_SIDE && E.side && E.view == VIEW_DEBUG) {	/* Run and Debug: Edit Breakpoint, Set Value */
        SideAct act;
        memset(&act, 0, sizeof(act));
        debug_key(K_F2, &act);
      }
      else if (!T->page) rename_symbol();
      break;
    case CMD_QUICKFIX: quickfix(); break;
    case CMD_HOVER:
      if (HAS_DOC && !G->diff && !T->page) {
        char *dg = diag_text(T->cur);
        hover_close();
        HV.at = T->cur;
        HV.from_mouse = 0;
        if (lsp_active(T->doc)) {
          HV.diag = dg;
          lsp_hover(T->doc, T->cur);
        }
        else if (dg) HV.text = dg;
      }
      break;
    case CMD_PROBLEMS:	/* the panel, its Problems; again: back to where the keys were */
      if (E.panel && E.panel_view == 1 && E.focus == F_PANEL) {
        E.panel = 0;
        E.focus = F_EDITOR;
        break;
      }
      E.panel = 1;
      E.panel_view = 1;
      E.focus = F_PANEL;
      break;
    case CMD_PREV_PROBLEM: next_problem(1); break;
    case CMD_SPLIT: split_editor(); break;
    case CMD_GROUP1: focus_nth(0); break;
    case CMD_GROUP2: focus_nth(1); break;
    case CMD_GROUP3: focus_nth(2); break;
    case CMD_GROUP4: focus_nth(3); break;
    case CMD_SPLIT_DOWN: split_editor_to(DROP_DOWN); break;
    case CMD_SPLIT_LEFT: split_editor_to(DROP_LEFT); break;
    case CMD_SPLIT_UP: split_editor_to(DROP_UP); break;
    case CMD_FOCUS_LEFT_GROUP: case CMD_FOCUS_RIGHT_GROUP: case CMD_FOCUS_UP_GROUP: case CMD_FOCUS_DOWN_GROUP: {
      int g = group_toward(cmd == CMD_FOCUS_LEFT_GROUP ? DROP_LEFT : cmd == CMD_FOCUS_RIGHT_GROUP ? DROP_RIGHT
                           : cmd == CMD_FOCUS_UP_GROUP ? DROP_UP : DROP_DOWN);
      if (g >= 0) focus_group(g);
      E.focus = F_EDITOR;
      break;
    }
    case CMD_MOVE_NEXT_GROUP: move_to_group(1); break;
    case CMD_MOVE_PREV_GROUP: move_to_group(-1); break;
    case CMD_JOIN_GROUP: join_groups(0); break;
    case CMD_JOIN_ALL: join_groups(1); break;
    case CMD_GROUP_SIZES: toggle_group_sizes(); break;
    case CMD_RESET_SIZES: reset_sizes(); break;
    case CMD_LAYOUT_SINGLE: case CMD_LAYOUT_2COL: case CMD_LAYOUT_3COL: case CMD_LAYOUT_2ROW: case CMD_LAYOUT_GRID: {
      static const int one[] = {1}, cols2[] = {1, 1}, cols3[] = {1, 1, 1}, rows2[] = {2}, grid[] = {2, 2};
      if (cmd == CMD_LAYOUT_SINGLE) set_layout(1, one);
      else if (cmd == CMD_LAYOUT_2COL) set_layout(2, cols2);
      else if (cmd == CMD_LAYOUT_3COL) set_layout(3, cols3);
      else if (cmd == CMD_LAYOUT_2ROW) set_layout(1, rows2);
      else set_layout(2, grid);
      break;
    }
    case CMD_CENTERED: E.centered = !E.centered; break;
    case CMD_TOGGLE_MENUBAR:
      opt.menubar = !opt.menubar;
      settings_put("window.menuBarVisibility", opt.menubar ? "classic" : "hidden");
      break;
    case CMD_TOGGLE_STATUSBAR:
      opt.statusbar = !opt.statusbar;
      settings_put("workbench.statusBar.visible", opt.statusbar ? "true" : "false");
      break;
    case CMD_TOGGLE_ACTIVITYBAR:
      opt.activitybar = !opt.activitybar;
      settings_put("workbench.activityBar.location", opt.activitybar ? "default" : "hidden");
      break;
    case CMD_LAYOUT_MENU: case CMD_APPEARANCE_MENU: {	/* View > Editor Layout, View > Appearance: their entries */
      static const int lay[] = {CMD_SPLIT, CMD_SPLIT_DOWN, CMD_SPLIT_LEFT, CMD_SPLIT_UP, CMD_LAYOUT_SINGLE,
                                CMD_LAYOUT_2COL, CMD_LAYOUT_3COL, CMD_LAYOUT_2ROW, CMD_LAYOUT_GRID, CMD_JOIN_GROUP,
                                CMD_JOIN_ALL, CMD_MOVE_NEXT_GROUP, CMD_MOVE_PREV_GROUP, CMD_GROUP_SIZES, CMD_RESET_SIZES, 0};
      static const int look[] = {CMD_TOGGLE_MENUBAR, CMD_SIDEBAR, CMD_SIDEBAR_POS, CMD_TOGGLE_ACTIVITYBAR,
                                 CMD_TOGGLE_STATUSBAR, CMD_PANEL_MAX, CMD_ZEN, CMD_CENTERED, CMD_MINIMAP,
                                 CMD_STICKY, CMD_RENDER_WS, CMD_WORDWRAP, CMD_BREADCRUMBS, 0};
      const int *v = cmd == CMD_LAYOUT_MENU ? lay : look;
      Pick p;
      int n, r;
      pick_init(&p, cmd == CMD_LAYOUT_MENU ? "Editor Layout" : "Appearance");
      p.keep_order = 1;
      for (n = 0; v[n]; n++) pick_add(&p, cmd_name(v[n]), cmd_keys(v[n]), 0);
      r = pick_run(&p);
      pick_free(&p);
      if (r >= 0) run_command(v[r]);
      break;
    }
    case CMD_SETTINGS: page_open(PAGE_SETTINGS); break;	/* the Settings editor */
    case CMD_WELCOME: page_open(PAGE_WELCOME); break;
    case CMD_NOTIFICATIONS: note_center(); break;
    case CMD_EXP_NEW_FILE: explorer_cmd(FC_NEW_FILE); break;
    case CMD_EXP_OPEN_SIDE: explorer_cmd(FC_OPEN_SIDE); break;
    case CMD_EXP_FIND_FOLDER: explorer_cmd(FC_FIND_FOLDER); break;
    case CMD_EXP_NEW_FOLDER: explorer_cmd(FC_NEW_FOLDER); break;
    case CMD_EXP_REFRESH: files_index_stale(); explorer_cmd(FC_REFRESH); break;
    case CMD_EXP_COLLAPSE: explorer_cmd(FC_COLLAPSE); break;
    case CMD_COPY_PATH: copy_path(0); break;
    case CMD_COPY_REL_PATH: copy_path(1); break;
    case CMD_REVEAL_OS:
      if (E.focus == F_SIDE && E.side && E.view == VIEW_FILES) explorer_cmd(FC_REVEAL);
      else if (HAS_DOC && !T->page && T->doc->path) fs_reveal(T->real ? T->real : T->doc->path);
      else fs_reveal(side_root());
      break;
    case CMD_OPEN_IN_TERMINAL:
      if (E.focus == F_SIDE && E.side && E.view == VIEW_FILES) explorer_cmd(FC_TERMINAL);
      else {
        char *d = HAS_DOC && !T->page && T->doc->path ? path_dirname(T->doc->path) : NULL;
        panel_cwd(d);
        free(d);
        run_command(CMD_TERMINAL_NEW);
      }
      break;
    case CMD_SETTINGS_JSON: {	/* settings.json in a tab; saving it applies it */
      char *f = settings_path();
      if (f == NULL) break;
      settings_create();
      if (open_file(f, 0) == 0) E.focus = F_EDITOR;
      free(f);
      break;
    }
    case CMD_CURSOR_UP: editor_key(K_UP | KM_CTRL | KM_ALT); break;
    case CMD_CURSOR_DOWN: editor_key(K_DOWN | KM_CTRL | KM_ALT); break;
    case CMD_NEXT_MATCH: editor_key(CTRL('d')); break;
    case CMD_ALL_MATCHES: editor_key('l' | KM_CTRL | KM_SHIFT); break;
    case CMD_ABOUT: {
      static const char *const bt[] = {"OK"};
      dialog(MME_NAME " " MME_VERSION, "VS Code for the terminal, in C11. By Marjon Mangindo Cajocon.", bt, 1);
      break;
    }
    default:
      if (cmd >= CMD_GIT_CHECKOUT && cmd <= CMD_GIT_MORE) {	/* egitlog.c */
        SideAct act;
        memset(&act, 0, sizeof(act));
        git_command(cmd, G->diff ? diff_path() : (HAS_DOC ? T->real : NULL), &act);
        apply_act(&act);
      }
      break;
  }
}


/* git changed files (checkout, pull, stash ...): the open ones not edited are read again */
void on_disk_changed (void) {
  int g, i;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      Tab *t = g_grp[g].tab[i];
      Doc *d = t->doc;
      int seen = 0, gg, jj;
      for (gg = 0; gg <= g && !seen; gg++)	/* a text two tabs share is read once */
        for (jj = 0; jj < (gg == g ? i : g_grp[gg].ntab); jj++)
          if (g_grp[gg].tab[jj]->doc == d) seen = 1;
      if (!seen && d->path && !doc_dirty(d)) {
        char *path = xstrdup(d->path);
        int refs = d->refs;
        unsigned long edits = d->edits;
        lsp_close(d);
        doc_free(d);
        doc_init(d);
        doc_load(d, path);
        d->refs = refs;
        d->edits = edits + 1;	/* the views and the server see a new text */
        free(path);
        if (t->sx) lsp_open(d, syntax_name(t->sx));
      }
      t->cur = doc_clamp(d, t->cur);
      t->anchor = doc_clamp(d, t->anchor);
      if (t->top >= d->n) t->top = d->n - 1;
      t->nmc = 0;
      t->nfold = 0;
    }
}

/* }================================================================== */


/*
** {==================================================================
** Keys
** ===================================================================
*/

/* Ctrl+PgDn / Ctrl+PgUp: the next / previous tab, the diff's last */
static void next_tab (int d) {
  int n = G->ntab + (HAS_DIFF ? 1 : 0), on = G->diff ? G->ntab : G->active;
  if (n == 0) return;
  on = (on + d + n) % n;
  if (on < G->ntab) focus_tab(on);
  else G->diff = 1;
  E.focus = F_EDITOR;
}


/* the keys that work wherever the focus is; 1 when k was one */
static int global_key (int k) {
  int code = KEY_CODE(k);
  int cs = (k & (KM_CTRL | KM_SHIFT)) == (KM_CTRL | KM_SHIFT);
  static const struct {
    int key, cmd;
  } keys[] = {
    {CTRL('n'), CMD_NEW}, {CTRL('o'), CMD_OPEN_FILE}, {CTRL('r'), CMD_OPEN_PROJECT},
    {CTRL('s'), CMD_SAVE}, {CTRL('w'), CMD_CLOSE}, {CTRL('q'), CMD_QUIT},
    {CTRL('b'), CMD_SIDEBAR}, {CTRL('p'), CMD_QUICK_OPEN}, {CTRL('e'), CMD_QUICK_OPEN},
    {CTRL('g'), CMD_GOTO}, {CTRL('f'), CMD_FIND}, {CTRL('h'), CMD_REPLACE},
    {'[' | KM_CTRL | KM_SHIFT, CMD_FOLD}, {'{' | KM_CTRL | KM_SHIFT, CMD_FOLD},
    {']' | KM_CTRL | KM_SHIFT, CMD_UNFOLD}, {'}' | KM_CTRL | KM_SHIFT, CMD_UNFOLD}, {K_F1, CMD_PALETTE},
    {'1' | KM_ALT, CMD_EXPLORER}, {'2' | KM_ALT, CMD_SEARCH}, {'3' | KM_ALT, CMD_GIT},
    {',' | KM_CTRL, CMD_SETTINGS}, {K_F12, CMD_DEFINITION}, {' ' | KM_CTRL, CMD_SUGGEST},
    {K_F8, CMD_NEXT_PROBLEM}, {K_F8 | KM_SHIFT, CMD_PREV_PROBLEM},
    {K_F2, CMD_RENAME}, {'.' | KM_CTRL, CMD_QUICKFIX}, {'z' | KM_ALT, CMD_WORDWRAP}, {CTRL('\\'), CMD_SPLIT}, {'\\' | KM_CTRL, CMD_SPLIT},
    {'1' | KM_CTRL, CMD_GROUP1}, {'2' | KM_CTRL, CMD_GROUP2}, {'3' | KM_CTRL, CMD_GROUP3}, {'4' | KM_CTRL, CMD_GROUP4},
    {K_RIGHT | KM_CTRL | KM_ALT, CMD_MOVE_NEXT_GROUP}, {K_LEFT | KM_CTRL | KM_ALT, CMD_MOVE_PREV_GROUP},
    {'C' | KM_ALT, CMD_COPY_PATH}, {'c' | KM_ALT | KM_SHIFT, CMD_COPY_PATH}, {'C' | KM_ALT | KM_SHIFT, CMD_COPY_PATH},
    {'R' | KM_ALT, CMD_REVEAL_OS}, {'r' | KM_ALT | KM_SHIFT, CMD_REVEAL_OS}, {'R' | KM_ALT | KM_SHIFT, CMD_REVEAL_OS},
    {K_LEFT | KM_ALT, CMD_NAV_BACK}, {K_RIGHT | KM_ALT, CMD_NAV_FORWARD},
    {'.' | KM_CTRL | KM_SHIFT, CMD_BREADCRUMBS}, {'>' | KM_CTRL | KM_SHIFT, CMD_BREADCRUMBS},
    {K_F12 | KM_SHIFT, CMD_REFERENCES}, {K_F12 | KM_CTRL, CMD_IMPLEMENTATION}, {K_F12 | KM_ALT, CMD_PEEK_DEF},
    {CTRL('t'), CMD_WORKSPACE_SYMBOL}, {' ' | KM_CTRL | KM_SHIFT, CMD_PARAM_HINTS},
    {K_F3 | KM_ALT, CMD_DIRTY_NEXT}, {K_F3 | KM_ALT | KM_SHIFT, CMD_DIRTY_PREV},
    {K_F5 | KM_ALT, CMD_CHANGE_NEXT}, {K_F5 | KM_ALT | KM_SHIFT, CMD_CHANGE_PREV}
  };
  size_t i;
  if (E.chord) {	/* the second key of Ctrl+K ... */
    E.chord = 0;
    toast(0, "%s", "");	/* the chord's notice goes */
    if (k == ('m' | KM_CTRL)) {	/* the kitty keyboard's Ctrl+M: not Enter */
      run_command(CMD_GROUP_SIZES);
      return 1;
    }
    if ((k & (KM_CTRL | KM_ALT | KM_SHIFT)) == KM_CTRL && KEY_CODE(k) >= 'a' && KEY_CODE(k) <= 'z')
      k = CTRL(KEY_CODE(k));	/* Ctrl+J from the kitty keyboard: as the terminal's old code */
    if (k == K_TAB || k == 'i' || k == CTRL('i')) run_command(CMD_HOVER);
    else if (k == 'o' || k == CTRL('o')) run_command(CMD_OPEN_FOLDER);
    else if (k == 't' || k == CTRL('t')) run_command(CMD_THEME);
    else if (k == 'z' || k == 'Z') run_command(CMD_ZEN);
    else if (k == '[' || k == CTRL('[')) run_command(CMD_FOLD);
    else if (k == ']' || k == CTRL(']')) run_command(CMD_UNFOLD);
    else if (k == '0' || k == ('0' | KM_CTRL)) run_command(CMD_FOLD_ALL);
    else if (k == 'j' || k == CTRL('j')) run_command(CMD_UNFOLD_ALL);
    else if (k == 'f' || k == CTRL('f')) run_command(CMD_FORMAT_SEL);
    else if (k == '8' || k == ('8' | KM_CTRL)) run_command(CMD_FOLD_REGIONS);
    else if (k == '9' || k == ('9' | KM_CTRL)) run_command(CMD_UNFOLD_REGIONS);
    else if (k == '/' || k == ('/' | KM_CTRL) || k == 0x1F) run_command(CMD_FOLD_COMMENTS);
    else if ((KEY_CODE(k) >= '1' && KEY_CODE(k) <= '7') && (k & ~KM_CTRL) == KEY_CODE(k)) run_command(CMD_FOLD_L1 + (KEY_CODE(k) - '1'));
    else if (k == ('c' | KM_CTRL | KM_SHIFT) || k == ('C' | KM_CTRL | KM_SHIFT)) run_command(CMD_COPY_REL_PATH);
    else if (k == 's' || k == CTRL('s')) run_command(CMD_KEYS);
    else if (k == 'm' || k == 'M') run_command(CMD_LANGUAGE);
    else if (k == 'x' || k == CTRL('x')) run_command(CMD_TRIM);
    else if (k == (CTRL('s') | KM_ALT) || k == ('s' | KM_CTRL | KM_ALT)) run_command(CMD_STAGE_RANGES);
    else if (k == CTRL('n')) run_command(CMD_UNSTAGE_RANGES);
    else if (k == CTRL('r')) run_command(CMD_REVERT_RANGES);
    else if (k == CTRL('c')) run_command(CMD_ADD_COMMENT);
    else if (k == CTRL('u')) run_command(CMD_REMOVE_COMMENT);
    else if (k == CTRL('d')) run_command(CMD_MOVE_SEL_NEXT);
    else if (k == 'd' || k == 'D') run_command(CMD_COMPARE_SAVED);
    else if (k == 'c' || k == 'C') run_command(CMD_COMPARE_CLIP);
    else if (k == CTRL('\\') || k == ('\\' | KM_CTRL)) run_command(CMD_SPLIT_DOWN);
    else if (k == (K_LEFT | KM_CTRL)) run_command(CMD_FOCUS_LEFT_GROUP);
    else if (k == (K_RIGHT | KM_CTRL)) run_command(CMD_FOCUS_RIGHT_GROUP);
    else if (k == (K_UP | KM_CTRL)) run_command(CMD_FOCUS_UP_GROUP);
    else if (k == (K_DOWN | KM_CTRL)) run_command(CMD_FOCUS_DOWN_GROUP);
    else if (k == ('m' | KM_CTRL)) run_command(CMD_GROUP_SIZES);
    else if (k == 'w') run_command(CMD_CLOSE_GROUP_ALL);
    else if (k == CTRL('w')) run_command(CMD_CLOSE_ALL);
    else if (k == 'u') run_command(CMD_CLOSE_SAVED);
    else if (k == (K_ENTER | KM_SHIFT)) run_command(HAS_DOC && T->pinned ? CMD_UNPIN : CMD_PIN);
    else if (k == K_ENTER) run_command(CMD_KEEP_OPEN);
    else if (k == 'p' || k == CTRL('p')) run_command(CMD_COPY_PATH);
    else if (k == 'v' || k == CTRL('v')) run_command(CMD_MD_SIDE);
    else toast(0, "The key combination (Ctrl+K) is not a command.");
    return 1;
  }
  if (E.chord_test) {	/* the second key of Ctrl+; (Testing) */
    E.chord_test = 0;
    toast(0, "%s", "");
    if (k == 'a' || k == 'A') run_command(CMD_TEST_RUN_ALL);
    else if (k == 'c' || k == 'C') run_command(CMD_TEST_RUN_CURSOR);
    else if (k == CTRL('c')) run_command(CMD_TEST_DEBUG_CURSOR);
    else if (k == 'f' || k == 'F') run_command(CMD_TEST_RUN_FILE);
    else if (k == 'l' || k == 'L') run_command(CMD_TEST_RERUN);
    else if (k == CTRL('o') || k == 'o') run_command(CMD_TEST_OUTPUT);
    else if (k == CTRL('x') || k == 'x') run_command(CMD_TEST_CANCEL);
    else toast(0, "The key combination (Ctrl+;) is not a command.");
    return 1;
  }
  if (k == (';' | KM_CTRL) && E.focus != F_PANEL) {	/* Ctrl+; A, C, F, L: Testing, like VS Code */
    E.chord_test = 1;
    toast(0, "(Ctrl+;) was pressed. Waiting for second key of chord...");
    return 1;
  }
  if (k == CTRL('k') && E.focus != F_PANEL) {	/* Ctrl+K Ctrl+S: the shortcuts */
    E.chord = 1;
    toast(0, "(Ctrl+K) was pressed. Waiting for second key of chord...");
    return 1;
  }
  if ((code == K_PGDN || code == K_PGUP) && (k & KM_CTRL) && !(k & KM_SHIFT)) {
    next_tab(code == K_PGDN ? 1 : -1);
    return 1;
  }
  if (code == K_TAB && (k & KM_CTRL)) {	/* Ctrl+Tab, from terminals that send it: the editors by use */
    switch_editor(0, (k & KM_SHIFT) != 0);
    return 1;
  }
  if (cs) {	/* Ctrl+Shift+ letter: from terminals that tell it apart */
    switch (code) {
      case 'p': run_command(CMD_PALETTE); return 1;
      case 'e': run_command(CMD_EXPLORER); return 1;
      case 'f': run_command(CMD_FIND_FILES); return 1;
      case 'h': run_command(CMD_REPLACE_FILES); return 1;
      case 'v': run_command(CMD_MD_PREVIEW); return 1;
      case 'g': run_command(CMD_GIT); return 1;
      case 'x': run_command(CMD_EXTENSIONS); return 1;
      case 's': run_command(CMD_SAVE_AS); return 1;
      case 'z': run_command(CMD_REDO); return 1;
      case 'm': run_command(CMD_PROBLEMS); return 1;
      case 'u': run_command(CMD_OUTPUT); return 1;
      case '5': case '%': run_command(CMD_TERMINAL_SPLIT); return 1;
      case 'o': run_command(CMD_GOTO_SYMBOL); return 1;
      case 't': run_command(CMD_REOPEN); return 1;
      case '\\': case '|': run_command(CMD_GOTO_BRACKET); return 1;
      case 'd': run_command(CMD_DEBUG_VIEW); return 1;
      case 'y': run_command(CMD_DEBUG_CONSOLE); return 1;
      case 'b': run_command(CMD_TASK_BUILD); return 1;
    }
  }
  if ((code == K_F5 || code == K_F9 || ((code == K_F6 || code == K_F10 || code == K_F11) && dbg_active())) &&
      !(k & KM_ALT)) {
    int cs5 = k & (KM_CTRL | KM_SHIFT);	/* Run and Debug's keys, VS Code's */
    if (code == K_F5 && cs5 == 0 && E.focus == F_SIDE && E.view == VIEW_FILES && !dbg_active()) return 0;	/* the Explorer's refresh */
    if (code == K_F5) run_command(cs5 == (KM_CTRL | KM_SHIFT) ? CMD_DEBUG_RESTART : cs5 == KM_SHIFT ? CMD_DEBUG_STOP
                                  : cs5 == KM_CTRL ? CMD_DEBUG_RUN : CMD_DEBUG_START);
    else if (code == K_F9) run_command(CMD_BREAKPOINT);
    else if (code == K_F6) run_command(CMD_DEBUG_PAUSE);
    else if (code == K_F10) run_command(CMD_DEBUG_STEP_OVER);
    else run_command((k & KM_SHIFT) ? CMD_DEBUG_STEP_OUT : CMD_DEBUG_STEP_INTO);
    return 1;
  }
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    if (k == keys[i].key) {
      run_command(keys[i].cmd);
      return 1;
    }
  if (code == K_F10 && !(k & KM_SHIFT)) {
    run_command(menu_run(0));
    return 1;
  }
  if ((k & KM_ALT) && !(k & (KM_CTRL | KM_SHIFT))) {	/* Alt+F, Alt+E ...: a menu, like VS Code */
    static const char mn[] = "fesvgrth";
    const char *p = code < 128 ? strchr(mn, code) : NULL;
    int in_find = E.finding || (E.focus == F_SIDE && E.view == VIEW_SEARCH);
    if (p && *p && !(in_find && code == 'r')) {	/* Alt+R in a find box: its .* */
      run_command(menu_run((int)(p - mn)));
      return 1;
    }
  }
  return 0;
}


static void clip_text (const char *s);

/* is p dir, or in it? */
static int under_path (const char *p, const char *dir, size_t n) {
  return p && m_fnncmp(p, dir, n) == 0 && (p[n] == '\0' || path_is_sep(p[n]));
}


/* path was renamed to path2 (or a folder above it was): the tabs of its files follow */
static void tabs_renamed (const char *from, const char *to) {
  size_t n = strlen(from);
  int g, i;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      Tab *t = g_grp[g].tab[i];
      Doc *d = t->doc;
      const char *p = under_path(d->path, from, n) ? d->path : under_path(t->real, from, n) ? t->real : NULL;
      if (t->page || d->path == NULL) continue;
      if (p) {	/* its text (shared by a split editor's tab too) gets the new name */
        char *np = (char *)xmalloc(strlen(to) + strlen(p + n) + 1);
        strcpy(np, to);
        strcat(np, p + n);
        lsp_close(d);
        free(d->path);
        d->path = np;
      }
      if (under_path(d->path, to, strlen(to))) {
        free(t->real);
        t->real = os_realpath(d->path);
        t->sx = syntax_detect(d->path, d);
        if (t->sx) lsp_open(d, syntax_name(t->sx));
      }
    }
}


/* path is gone: the tabs of its files go too (those with changes stay, to be saved) */
static void tabs_deleted (const char *path) {
  size_t n = strlen(path);
  int g, i, keep = g_gcur;
  for (g = 0; g < g_ngrp; g++) {
    focus_group(g);
    for (i = G->ntab - 1; i >= 0; i--) {
      Tab *t = G->tab[i];
      const char *p = t->real ? t->real : t->doc->path;
      if (p == NULL || t->page || doc_dirty(t->doc)) continue;
      if (m_fnncmp(p, path, n) == 0 && (p[n] == '\0' || path_is_sep(p[n]))) tab_free(i);
    }
  }
  focus_group(keep < g_ngrp ? keep : 0);
}


void explorer_renamed (const char *from, const char *to) {
  tabs_renamed(from, to);
}


void explorer_deleted (const char *path) {
  tabs_deleted(path);
}


/*
** {==================================================================
** Run and Debug, tasks: what they ask of the editor
** ===================================================================
*/

const char *editor_file (void) {
  if (!HAS_DOC || G->diff) return NULL;
  return T->real ? T->real : T->doc->path;
}


size_t editor_line (void) {
  return HAS_DOC ? T->cur.y : 0;
}


/* testing.saveBeforeTest: every file with changes */
void editor_save_all (void) {
  int g, i;
  for (g = 0; g < g_ngrp; g++)
    for (i = 0; i < g_grp[g].ntab; i++) {
      Doc *d = g_grp[g].tab[i]->doc;
      if (d->path && doc_dirty(d) && !g_grp[g].tab[i]->page && doc_save(d) != 0)
        toast(1, "Failed to save '%s'", path_basename(d->path));
    }
}


void on_debug (int what, const char *path, size_t line) {
  int g, i;
  switch (what) {
    case DE_START:	/* the files are saved first, and the Run and Debug view and the console shown */
      for (g = 0; g < g_ngrp; g++)
        for (i = 0; i < g_grp[g].ntab; i++) {
          Doc *d = g_grp[g].tab[i]->doc;
          if (d->path && doc_dirty(d) && doc_save(d) != 0) toast(1, "Failed to save '%s'", path_basename(d->path));
        }
      E.side = 1;
      E.view = VIEW_DEBUG;
      E.panel = 1;
      E.panel_view = 2;
      if (E.focus == F_SIDE) E.focus = F_EDITOR;
      break;
    case DE_STOP:	/* paused: the file of the frame, its line in the middle */
    case DE_OPEN:
      if (path == NULL || open_file(path, 0) != 0) return;
      if (line > 0) {
        Pos p;
        p.y = line - 1;
        p.x = 0;
        move_h(doc_clamp(T->doc, p), 0);
        key_home(0);
        if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
      }
      E.focus = F_EDITOR;
      break;
  }
}


int on_task_terminal (const char *name, const char *cmd, const char *cwd, int id) {
  E.panel = 1;
  E.panel_view = 0;
  layout();
  if (panel_run(L.area_w - 2, L.panel_h - 1, name, cmd, cwd, id) != 0) {
    if (!panel_alive()) E.panel = 0;
    return -1;
  }
  return 0;
}


/* the expression under the mouse while paused: a name, with what it is in ("p.x") */
static char *hover_expr (Pos p) {
  const Row *r = row_at(p.y);
  Pos a = p;
  size_t e = word_end_at(p);
  a.x = word_start_at(p);
  if (a.x == e) return NULL;
  while (a.x >= 2 && r->s[a.x - 1] == '.' && char_class(r, a.x - 2) == 1) {
    Pos q = a;
    q.x -= 1;
    a.x = word_start_at(q);
  }
  return xstrndup(r->s + a.x, e - a.x);
}

/* }================================================================== */


static void apply_act (const SideAct *act) {
  switch (act->what) {
    case SA_RENAMED: tabs_renamed(act->path, act->path2); files_index_stale(); break;
    case SA_DELETED: tabs_deleted(act->path); files_index_stale(); break;
    case SA_CLIP: clip_text(act->path); break;
    case SA_TERMINAL:
      panel_cwd(act->path);
      run_command(CMD_TERMINAL_NEW);
      break;
    case SA_OPEN:
    case SA_GO:
      if (open_file(act->path, act->what == SA_OPEN) != 0) return;
      if (act->line > 0) {
        Pos p;
        p.y = act->line - 1;
        p.x = act->col;
        select_range(doc_clamp(T->doc, p), act->len);
        center_cursor();
      }
      if (act->what == SA_GO) E.focus = F_EDITOR;
      break;
    case SA_CMD: run_command(act->cmd); break;
    case SA_FOCUS_SCM: show_view(VIEW_GIT); break;
    case SA_OPEN_SIDE: {	/* Open to the Side: the group on the right (a new one when there is none) */
      int ng = g_gcur + 1 < g_ngrp ? g_gcur + 1 : group_add(g_gcur, DROP_RIGHT);
      focus_group(ng >= 0 ? ng : g_gcur);
      if (open_file(act->path, 0) == 0) E.focus = F_EDITOR;
      break;
    }
    case SA_FIND_FOLDER:	/* Find in Folder...: the Search view, "files to include" that folder */
      show_view(VIEW_SEARCH);
      search_scope(act->path);
      E.focus = F_SIDE;
      break;
    case SA_DIFF:
    case SA_SHOW_DIFF:
      if (act->what == SA_SHOW_DIFF || diff_open(act->path, act->staged) == 0) {
        {
          int gg;
          for (gg = 0; gg < MAX_GRP; gg++) g_grp[gg].dtab = g_grp[gg].diff = 0;
        }
        G->dtab = 1;
        G->diff = 1;
        if (act->go) E.focus = F_EDITOR;
      }
      break;
  }
}


/* is the selection a match of what is looked for? */
static int sel_is_match (void) {
  Pos a, b;
  if (!T->sel) return 0;
  sel_range(&a, &b);
  return a.y == b.y && b.x > a.x && b.x - a.x == match_at(a.y, a.x);
}


/* what a match at a (len bytes) is replaced with: E.repl, with $1 ... for a regex */
static char *repl_text (Pos a, size_t *len) {
  const Row *r = row_at(a.y);
  size_t cap[20], e;
  const Regex *re = E.find_regex ? find_re() : NULL;
  if (re && re_at(re, r->s, r->len, a.x, &e, cap)) return re_expand(E.repl, r->s, cap, len);
  *len = strlen(E.repl);
  return xstrdup(E.repl);
}


/* Replace: the match selected takes the replace text; then the next one */
static void replace_one (void) {
  size_t n;
  if (E.find[0] == '\0') return;
  if (sel_is_match()) {
    Pos a, b;
    char *t;
    sel_range(&a, &b);
    t = repl_text(a, &n);
    doc_group(T->doc);
    T->sel = 0;
    ed_delete(a, b);
    T->cur = ed_insert(a, t, n);
    doc_group(T->doc);
    free(t);
  }
  find_next(0);
}


/* Replace All: every match, the last first, one step of undo */
static void replace_all (void) {
  Pos *at = NULL;
  size_t n = 0, cap = 0, y, x, fl = strlen(E.find), i, *ml = NULL;
  char **txt = NULL;
  if (fl == 0) return;
  for (y = 0; y < T->doc->n; y++) {
    const Row *r = row_at(y);
    for (x = 0; x < r->len; x++) {
      size_t m = match_at(y, x);
      if (m) {
        if (n == cap) {
          cap = cap ? cap * 2 : 64;
          at = (Pos *)xrealloc(at, cap * sizeof(Pos));
          ml = (size_t *)xrealloc(ml, cap * sizeof(size_t));
          txt = (char **)xrealloc(txt, cap * sizeof(char *));
        }
        at[n].y = y;
        at[n].x = x;
        ml[n] = m;
        txt[n] = NULL;
        n++;
        x += m - 1;
      }
    }
  }
  if (n == 0) {
    toast(0, "No results");
    return;
  }
  for (i = 0; i < n; i++) {	/* the texts first: $1 needs the line as it was */
    size_t len;
    txt[i] = repl_text(at[i], &len);
  }
  doc_group(T->doc);
  T->sel = 0;
  for (i = n; i-- > 0;) {
    Pos b = at[i];
    b.x += ml[i];
    ed_delete(at[i], b);
    ed_insert(at[i], txt[i], strlen(txt[i]));
    free(txt[i]);
  }
  doc_group(T->doc);
  T->cur = doc_clamp(T->doc, T->cur);
  if (E.find_insel) E.find_insel = 0;	/* the selection's text changed */
  free(at);
  free(ml);
  free(txt);
  toast(0, "Replaced %lu occurrence%s", (unsigned long)n, n == 1 ? "" : "s");
}


/* a key in the replace box */
static void repl_key (int k) {
  size_t len = strlen(E.repl);
  int code = KEY_CODE(k);
  if (code == K_ENTER && (k & (KM_CTRL | KM_ALT))) replace_all();	/* Ctrl+Alt+Enter */
  else if (code == K_ENTER) replace_one();
  else if (code == K_BS) {
    while (len > 0 && ((unsigned char)E.repl[len - 1] & 0xC0) == 0x80) len--;
    if (len > 0) E.repl[len - 1] = '\0';
  }
  else if (code == K_PASTE) {
    Buf b;
    size_t i;
    buf_init(&b);
    term_paste(&b);
    for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < sizeof(E.repl); i++) E.repl[len++] = b.s[i];
    E.repl[len] = '\0';
    buf_free(&b);
  }
  else if (IS_TEXT(k) && len + 4 < sizeof(E.repl)) {
    len += (size_t)utf8_encode((uint32_t)k, E.repl + len);
    E.repl[len] = '\0';
  }
}


/* Alt+L: the matches only in the selection (the line, without one) */
static void find_in_selection (void) {
  if (E.find_insel) {
    E.find_insel = 0;
    return;
  }
  if (T->sel) sel_range(&E.fsel_a, &E.fsel_b);
  else {
    E.fsel_a.y = E.fsel_b.y = T->cur.y;
    E.fsel_a.x = 0;
    E.fsel_b.x = row_at(T->cur.y)->len;
  }
  E.find_insel = 1;
}


static void find_key (int k) {
  size_t len = strlen(E.find);
  int code = KEY_CODE(k);
  if (code == K_TAB && E.replacing) {	/* Tab: from one box to the other */
    E.in_repl = !E.in_repl;
    return;
  }
  if (code == K_ESC) {
    E.finding = E.find_open = E.replacing = E.in_repl = 0;
    return;
  }
  if (k == ('1' | KM_CTRL | KM_SHIFT) || k == ('!' | KM_CTRL | KM_SHIFT)) {	/* Ctrl+Shift+1 */
    replace_one();
    return;
  }
  if (E.replacing && E.in_repl && code != K_UP && code != K_DOWN && code != K_F3 &&
      k != ('c' | KM_ALT) && k != ('w' | KM_ALT) && k != ('r' | KM_ALT) && k != ('l' | KM_ALT)) {
    repl_key(k);
    return;
  }
  if (k == (K_ENTER | KM_ALT)) select_find_matches();	/* Alt+Enter: a cursor on every match */
  else if (code == K_ENTER || code == K_F3 || code == K_DOWN || code == K_UP)
    find_next(code == K_UP || (k & KM_SHIFT));
  else if (k == ('c' | KM_ALT)) E.find_case = !E.find_case;
  else if (k == ('w' | KM_ALT)) E.find_word = !E.find_word;
  else if (k == ('r' | KM_ALT)) E.find_regex = !E.find_regex;
  else if (k == ('l' | KM_ALT)) find_in_selection();
  else if (code == K_BS) {
    while (len > 0 && ((unsigned char)E.find[len - 1] & 0xC0) == 0x80) len--;
    if (len > 0) E.find[len - 1] = '\0';
  }
  else if (code == K_PASTE) {
    Buf b;
    size_t i;
    buf_init(&b);
    term_paste(&b);
    for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < sizeof(E.find); i++) E.find[len++] = b.s[i];
    E.find[len] = '\0';
    buf_free(&b);
  }
  else if (IS_TEXT(k) && len + 4 < sizeof(E.find)) {
    Pos a, b, f;
    len += (size_t)utf8_encode((uint32_t)k, E.find + len);
    E.find[len] = '\0';
    sel_range(&a, &b);	/* as you type: from where the match began */
    if (!E.find_insel && find_from(T->sel ? a : T->cur, 0, &f)) {
      select_range(f, match_at(f.y, f.x));
      if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
    }
  }
  else if (code == K_TAB) E.finding = 0;	/* back to the text, the widget stays */
}


/*
** VS Code's auto closing: ( [ { " ' ` put their pair after the cursor when
** what follows is space, the end or a closing bracket; typing the closing
** one over it just goes past; a selection is put between the two.
*/
static int auto_close (int code) {
  const Row *r = row_at(T->cur.y);
  int next = T->cur.x < r->len ? (unsigned char)r->s[T->cur.x] : 0;
  int prev = T->cur.x > 0 ? (unsigned char)r->s[T->cur.x - 1] : 0;
  int quote = code == '"' || code == '\'' || code == '`';
  char pair[2];
  if (!opt.auto_close || !T->sx) return 0;
  if ((is_close(code) || quote) && next == code && !T->sel) {	/* over the closing one */
    Pos p = T->cur;
    p.x++;
    move_h(p, 0);
    return 1;
  }
  if (!is_open(code) && !quote) return 0;
  pair[0] = (char)code;
  pair[1] = (char)(quote ? code : pair_of(code));
  if (T->sel) {	/* around the selection */
    Pos a, b;
    sel_range(&a, &b);
    T->sel = 0;
    ed_insert(b, pair + 1, 1);
    ed_insert(a, pair, 1);
    T->anchor = a;
    T->anchor.x++;
    T->cur = b;
    if (b.y == a.y) T->cur.x++;
    T->sel = 1;
    return 1;
  }
  if (next && !(next == ' ' || next == '\t' || is_close(next) || next == ';' || next == ',' ||
                (quote && next != code))) return 0;
  if (quote && T->cur.x > 0 && char_class(r, T->cur.x - 1) == 1) return 0;	/* don't */
  if (quote && prev == '\\') return 0;
  insert(pair, 2);
  move_h(doc_clamp(T->doc, (Pos){T->cur.y, T->cur.x - 1}), 0);
  return 1;
}


/* Backspace between a pair takes both; Enter between {} opens an indented line */
static int pair_key (int code) {
  const Row *r = row_at(T->cur.y);
  int next, prev;
  if (!opt.auto_close || T->sel || T->cur.x == 0 || T->cur.x >= r->len) return 0;
  next = (unsigned char)r->s[T->cur.x];
  prev = (unsigned char)r->s[T->cur.x - 1];
  if (code == K_BS && ((is_open(prev) && next == pair_of(prev)) ||
                       ((prev == '"' || prev == '\'' || prev == '`') && next == prev))) {
    Pos a = T->cur, b = T->cur;
    a.x--;
    b.x++;
    ed_delete(a, b);
    move_h(a, 0);
    return 1;
  }
  if (code == K_ENTER && is_open(prev) && next == pair_of(prev)) {
    size_t ind = indent_end(r), n;
    char *s;
    Pos mid;
    if (ind > T->cur.x) ind = T->cur.x;
    n = E.tab_unit_len;
    s = (char *)xmalloc(2 * ind + n + 3);
    s[0] = '\n';
    memcpy(s + 1, r->s, ind);
    memcpy(s + 1 + ind, E.tab_unit, n);
    s[1 + ind + n] = '\n';
    memcpy(s + 2 + ind + n, r->s, ind);
    insert(s, 2 * ind + n + 2);
    mid.y = T->cur.y - 1;
    mid.x = ind + n;
    move_h(mid, 0);
    free(s);
    return 1;
  }
  return 0;
}


/* html.autoClosingTags: the files where tags close themselves; 2: XML (no void tags) */
static int tag_lang (void) {
  static const char *const ids[] = {"html", "vue", "svelte", "php", "handlebars", "razor", NULL};
  const char *id = doc_lang_id(), *dot = T->doc->path ? strrchr(T->doc->path, '.') : NULL;
  int i;
  if (!opt.close_tags) return 0;
  if (strcmp(id, "xml") == 0) return 2;
  if (dot && (strcmp(dot, ".jsx") == 0 || strcmp(dot, ".tsx") == 0)) return 2;
  for (i = 0; ids[i]; i++)
    if (strcmp(ids[i], id) == 0) return 1;
  return 0;
}


static int void_tag (const char *s, size_t n) {
  static const char *const v[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link",
                                   "meta", "param", "source", "track", "wbr", "keygen", "!doctype", NULL};
  int i;
  for (i = 0; v[i]; i++)
    if (strlen(v[i]) == n && m_strnicmp(v[i], s, n) == 0) return 1;
  return 0;
}


static int tag_char (int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '_' || c == ':' || c == '.';
}


/* the tag still open at p: the one "</" closes (its name in buf), 0 none */
static int open_tag (Pos p, int lang, char *buf, size_t cap) {
  size_t y0 = p.y > 3000 ? p.y - 3000 : 0, y, depth = 0;
  char stack[64][64];
  int n = 0;
  for (y = y0; y <= p.y; y++) {	/* the tags from there to p, as a stack */
    const Row *r = row_at(y);
    size_t end = y == p.y ? p.x : r->len, x = 0;
    (void)depth;
    while (x < end) {
      size_t a, b;
      int close = 0;
      if (r->s[x] != '<') {
        x++;
        continue;
      }
      a = x + 1;
      if (a < end && r->s[a] == '/') {
        close = 1;
        a++;
      }
      if (a >= end || !((r->s[a] | 0x20) >= 'a' && (r->s[a] | 0x20) <= 'z')) {
        x++;
        continue;
      }
      b = a;
      while (b < end && tag_char((unsigned char)r->s[b])) b++;
      x = b;
      while (x < end && r->s[x] != '>') {	/* to its end, over "..." */
        if (r->s[x] == '"' || r->s[x] == '\'') {
          char q = r->s[x++];
          while (x < end && r->s[x] != q) x++;
        }
        if (x < end) x++;
      }
      if (x >= end) break;	/* not closed yet */
      if (close) {
        int k;
        for (k = n - 1; k >= 0; k--)
          if (strlen(stack[k]) == b - a && strncmp(stack[k], r->s + a, b - a) == 0) break;
        if (k >= 0) n = k;
      }
      else if (r->s[x - 1] != '/' && !(lang == 1 && void_tag(r->s + a, b - a)) && b - a < 64 && n < 64) {
        memcpy(stack[n], r->s + a, b - a);
        stack[n][b - a] = '\0';
        n++;
      }
      x++;
    }
  }
  if (n == 0 || strlen(stack[n - 1]) + 1 > cap) return 0;
  strcpy(buf, stack[n - 1]);
  return 1;
}


/* '>' of <div ...> puts </div> after the cursor; "</" gets the open tag's name; 1 when done */
static int close_tag (int code) {
  const Row *r = row_at(T->cur.y);
  size_t x = T->cur.x, a, b;
  int lang;
  char name[64];
  if ((code != '>' && code != '/') || T->sel || !(lang = tag_lang())) return 0;
  if (code == '/') {
    Buf t;
    if (x == 0 || r->s[x - 1] != '<' || (x < r->len && r->s[x] != ' ' && r->s[x] != '\n' && r->s[x] != '<'))
      return 0;
    if (!open_tag(T->cur, lang, name, sizeof(name))) return 0;
    buf_init(&t);
    buf_printf(&t, "/%s>", name);
    insert(t.s, t.len);
    buf_free(&t);
    return 1;
  }
  for (a = x; a > 0 && r->s[a - 1] != '<' && r->s[a - 1] != '>'; a--) ;
  if (a == 0 || r->s[a - 1] != '<' || a >= x) return 0;
  if (!((r->s[a] | 0x20) >= 'a' && (r->s[a] | 0x20) <= 'z')) return 0;	/* </x, <!-- , <? */
  if (r->s[x - 1] == '/') return 0;	/* <br/ */
  {	/* not in a quoted value */
    size_t i;
    int q = 0;
    for (i = a; i < x; i++)
      if (r->s[i] == '"' || r->s[i] == '\'') q = !q;
    if (q) return 0;
  }
  for (b = a; b < x && tag_char((unsigned char)r->s[b]); b++) ;
  if (b - a >= sizeof(name) || (lang == 1 && void_tag(r->s + a, b - a))) return 0;
  {
    Buf t;
    Pos p;
    buf_init(&t);
    buf_puts(&t, "></");
    buf_putn(&t, r->s + a, b - a);
    buf_putc(&t, '>');
    insert(t.s, t.len);
    buf_free(&t);
    p = T->cur;
    p.x -= b - a + 3;	/* between the two */
    T->cur = p;
    T->want = col_of(row_at(p.y), p.x);
  }
  return 1;
}


/* one key for the cursor in front (with many cursors: each in turn) */
static void key_one (int k) {
  int code = KEY_CODE(k), shift = (k & KM_SHIFT) != 0, ctrl = (k & KM_CTRL) != 0;
  Pos p;
  if (IS_TEXT(k) && close_tag(code)) return;
  if ((IS_TEXT(k) && auto_close(code)) || ((k == K_BS || k == K_ENTER) && pair_key(code))) return;
  if (IS_TEXT(k)) {
    insert_char((uint32_t)code);
    outdent_typed();	/* "}", "end", "else:" ...: back to their block's indent */
    return;
  }
  switch (code) {
    case K_UP:
      if (E.wrap && !(k & (KM_ALT | KM_CTRL)) && wrap_move(-1, shift)) break;
      if (k & KM_ALT) move_lines(0);
      else if (ctrl) {
        if (T->top > 0) T->top--;
        E.follow = 0;
      }
      else move_v(-1, shift);
      break;
    case K_DOWN:
      if (E.wrap && !(k & (KM_ALT | KM_CTRL)) && wrap_move(1, shift)) break;
      if (k & KM_ALT) move_lines(1);
      else if (ctrl) {
        if (T->top + 1 < T->doc->n) T->top++;
        E.follow = 0;
      }
      else move_v(1, shift);
      break;
    case K_LEFT:
      if (ctrl) move_h(word_left(T->cur), shift);
      else if (T->sel && !shift) {	/* a selection collapses to its start */
        Pos a, b;
        sel_range(&a, &b);
        move_h(a, 0);
      }
      else {
        p = T->cur;
        if (p.x > 0) p.x = prev_x(row_at(p.y), p.x);
        else if (p.y > 0) {
          p.y--;
          p.x = row_at(p.y)->len;
        }
        move_h(p, shift);
      }
      break;
    case K_RIGHT:
      if (ctrl) move_h(word_right(T->cur), shift);
      else if (T->sel && !shift) {
        Pos a, b;
        sel_range(&a, &b);
        move_h(b, 0);
      }
      else {
        p = T->cur;
        if (p.x < row_at(p.y)->len) p.x = next_x(row_at(p.y), p.x);
        else if (p.y + 1 < T->doc->n) {
          p.y++;
          p.x = 0;
        }
        move_h(p, shift);
      }
      break;
    case K_HOME:
      if (ctrl) {
        p.y = p.x = 0;
        move_h(p, shift);
      }
      else key_home(shift);
      break;
    case K_END:
      if (ctrl) move_h(doc_end(T->doc), shift);
      else {
        p = T->cur;
        p.x = row_at(p.y)->len;
        move_h(p, shift);
      }
      break;
    case K_PGUP:
    case K_PGDN: {
      long th = L.text_h;
      if (k & KM_ALT) {	/* Alt+PageUp / Down: scroll a page, the cursor stays */
        if (code == K_PGUP) T->top = T->top > (size_t)th ? T->top - (size_t)th : 0;
        else if (T->top + (size_t)th < T->doc->n) T->top += (size_t)th;
        E.follow = 0;
        break;
      }
      if (code == K_PGUP) T->top = T->top > (size_t)th ? T->top - (size_t)th : 0;
      else if (T->top + (size_t)th < T->doc->n) T->top += (size_t)th;
      move_v(code == K_PGUP ? -th : th, shift);
      break;
    }
    case K_ENTER: newline(); break;
    case K_TAB:
      if (shift) indent_lines(1);
      else tab();
      break;
    case K_BS: backspace(ctrl || (k & KM_ALT)); break;
    case K_DEL: delete_forward(ctrl); break;
    case K_F3: find_next(shift); break;
    case K_PASTE: {
      Buf b;
      buf_init(&b);
      term_paste(&b);
      paste_text(b.s ? b.s : "", b.len);
      buf_free(&b);
      break;
    }
    case K_ESC:
      if (E.find_open) E.find_open = E.finding = 0;
      else T->sel = 0;
      break;
    case CTRL('z'): undo(0); break;
    case CTRL('y'): undo(1); break;
    case CTRL('c'): copy(0); break;
    case CTRL('x'): copy(1); break;
    case CTRL('v'): if (E.clip) paste_text(E.clip, E.cliplen); break;
    case CTRL('a'): run_command(CMD_SELECT_ALL); break;
    case K_PASTE_TEXT: insert(E.ptext, E.plen); break;
    case K_PASTE_LINE:
      E.pline--;
      insert(E.plines[E.pline], E.plens[E.pline]);
      break;
    case K_CUT_SEL: delete_sel(); break;
    default: break;
  }
}


/* the keys every cursor does: typing, deleting, moving, selecting */
static int multi_key (int k) {
  int code = KEY_CODE(k);
  if (IS_TEXT(k)) return 1;
  if (k & KM_ALT) return 0;
  switch (code) {
    case K_ENTER: case K_BS: case K_DEL: case K_LEFT: case K_RIGHT: return 1;
    case K_TAB: return !(k & KM_SHIFT);
    case K_UP: case K_DOWN: case K_HOME: case K_END: return !(k & KM_CTRL);
  }
  return 0;
}


static const Cur *g_sort;	/* for cmp_cur */

static int cmp_cur (const void *a, const void *b) {	/* the last one in the text first */
  return -pos_cmp(g_sort[*(const int *)a].cur, g_sort[*(const int *)b].cur);
}


/* runs key_one for every cursor, the one lowest in the text first */
static void each_cursor (int k) {
  int n = T->nmc + 1, i, j;
  Cur *all = (Cur *)xmalloc((size_t)n * sizeof(Cur));
  int *ord = (int *)xmalloc((size_t)n * sizeof(int));
  cur_get(&all[0]);	/* the main cursor is all[0] */
  memcpy(all + 1, T->mc, (size_t)T->nmc * sizeof(Cur));
  for (i = 0; i < n; i++) ord[i] = i;
  g_sort = all;
  qsort(ord, (size_t)n, sizeof(int), cmp_cur);
  for (i = 0; i < n; i++) {	/* all[me] in front, the others are T->mc */
    int me = ord[i];
    T->nmc = 0;
    for (j = 0; j < n; j++)
      if (j != me) T->mc[T->nmc++] = all[j];
    cur_set(&all[me]);
    key_one(k);
    cur_get(&all[me]);
    for (j = 0, T->nmc = 0; j < n; j++)	/* the others moved with the edit */
      if (j != me) all[j] = T->mc[T->nmc++];
  }
  cur_set(&all[0]);
  for (j = 1, T->nmc = 0; j < n; j++) T->mc[T->nmc++] = all[j];
  free(ord);
  free(all);
  mc_merge();
}


/* the text the cursors paste: a line each when there are as many lines */
static void multi_paste (const char *s, size_t n) {
  size_t lines = 1, i;
  int j, total = T->nmc + 1;
  const char *p = s;
  for (i = 0; i < n; i++) lines += (s[i] == '\n');
  if (n > 0 && s[n - 1] == '\n') lines--;
  if ((int)lines != total || eopt.mc_paste_full) {	/* the whole text at every cursor */
    E.ptext = s;
    E.plen = n;
    each_cursor(K_PASTE_TEXT);
    return;
  }
  E.plines = (const char **)xmalloc((size_t)total * sizeof(char *));
  E.plens = (size_t *)xmalloc((size_t)total * sizeof(size_t));
  for (j = 0; j < total; j++) {
    const char *e = memchr(p, '\n', (size_t)(s + n - p));
    E.plines[j] = p;
    E.plens[j] = e ? (size_t)(e - p) : (size_t)(s + n - p);
    p = e ? e + 1 : s + n;
  }
  E.pline = total;	/* each_cursor starts from the last cursor: the last line */
  each_cursor(K_PASTE_LINE);
  free(E.plines);
  free(E.plens);
  E.plines = NULL;
}


static int cmp_cur_up (const void *a, const void *b) {	/* the first one in the text first */
  return pos_cmp(g_sort[*(const int *)a].cur, g_sort[*(const int *)b].cur);
}


/* Ctrl+C / Ctrl+X with many cursors: the selections, a line each */
static void multi_copy (int cut) {
  int n = T->nmc + 1, i, *ord;
  Cur *all = (Cur *)xmalloc((size_t)n * sizeof(Cur));
  Buf b;
  buf_init(&b);
  cur_get(&all[0]);
  memcpy(all + 1, T->mc, (size_t)T->nmc * sizeof(Cur));
  ord = (int *)xmalloc((size_t)n * sizeof(int));
  for (i = 0; i < n; i++) ord[i] = i;
  g_sort = all;
  qsort(ord, (size_t)n, sizeof(int), cmp_cur_up);
  for (i = 0; i < n; i++) {
    const Cur *c = &all[ord[i]];
    Pos a = c->anchor, e = c->cur;
    char *t;
    size_t len;
    if (!c->sel) continue;
    if (pos_cmp(a, e) > 0) {
      Pos x = a;
      a = e;
      e = x;
    }
    t = doc_text(T->doc, a, e, &len);
    if (b.len) buf_putc(&b, '\n');
    buf_putn(&b, t, len);
    free(t);
  }
  free(ord);
  free(all);
  if (b.len == 0) {
    buf_free(&b);
    return;
  }
  free(E.clip);
  E.cliplen = b.len;
  E.clip = buf_take(&b);
  osc52(E.clip, E.cliplen);
  if (cut) each_cursor(K_CUT_SEL);
}



/* the word under the cursor, selected (Ctrl+D the first time) */
static int select_word (void) {
  const Row *r = row_at(T->cur.y);
  size_t a = T->cur.x, b = T->cur.x;
  while (a > 0 && char_class(r, a - 1) == 1) a--;
  while (b < r->len && char_class(r, b) == 1) b++;
  if (a == b) return 0;
  T->anchor.y = T->cur.y;
  T->anchor.x = a;
  T->cur.x = b;
  T->sel = 1;
  T->want = col_of(r, b);
  return 1;
}


/* the next place the text s is, after p, round the end; 1 found */
static int next_exact (const char *s, size_t n, Pos p, Pos *f) {
  size_t i, y, x;
  for (i = 0; i <= T->doc->n; i++) {
    const Row *r;
    y = (p.y + i) % T->doc->n;
    r = row_at(y);
    for (x = (i == 0) ? p.x : 0; x + n <= r->len; x++)
      if (memcmp(r->s + x, s, n) == 0) {
        f->y = y;
        f->x = x;
        return 1;
      }
  }
  return 0;
}


static int has_cursor_at (Pos a) {
  int i;
  if (T->sel && pos_cmp(T->anchor, a) == 0) return 1;
  for (i = 0; i < T->nmc; i++)
    if (T->mc[i].sel && pos_cmp(T->mc[i].anchor, a) == 0) return 1;
  return 0;
}


/* Ctrl+D: the next place of the selection gets a cursor too; all: Ctrl+Shift+L */
static void add_next_match (int all) {
  Pos a, b, f;
  char *t;
  size_t n;
  if (!T->sel && !select_word()) return;
  sel_range(&a, &b);
  if (a.y != b.y) return;
  t = doc_text(T->doc, a, b, &n);
  do {
    if (!next_exact(t, n, b, &f) || has_cursor_at(f)) break;
    mc_push();
    T->anchor = f;
    T->cur = f;
    T->cur.x += n;
    T->sel = 1;
    T->want = col_of(row_at(f.y), T->cur.x);
    b = T->cur;
  } while (all);
  free(t);
  if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
}



/* Ctrl+Alt+Up / Down: a cursor on the line above the top one / below the last */
static void add_cursor_line (int down) {
  Pos p = T->cur;
  int i;
  for (i = 0; i < T->nmc; i++)
    if (down ? T->mc[i].cur.y > p.y : T->mc[i].cur.y < p.y) p = T->mc[i].cur;
  if (down ? p.y + 1 >= T->doc->n : p.y == 0) return;
  p.y = down ? p.y + 1 : p.y - 1;
  p.x = x_of_col(row_at(p.y), T->want);
  mc_push();
  T->cur = p;
  T->sel = 0;
}


/*
** {==================================================================
** Editing extras: more cursors, auto indent, word parts, cursor undo
** ===================================================================
*/

/* Shift+Alt+I: a cursor at the end of every line the selection has */
static void cursors_line_ends (void) {
  Pos a, b;
  size_t y, hy;
  if (!T->sel) {
    T->cur.x = row_at(T->cur.y)->len;
    T->want = col_of(row_at(T->cur.y), T->cur.x);
    return;
  }
  sel_range(&a, &b);
  hy = (b.x == 0 && b.y > a.y) ? b.y - 1 : b.y;
  T->nmc = 0;
  T->sel = 0;
  for (y = a.y; y <= hy; y++) {
    if (y > a.y) mc_push();
    T->cur.y = y;
    T->cur.x = row_at(y)->len;
    T->anchor = T->cur;
    T->want = col_of(row_at(y), T->cur.x);
  }
}


/* Ctrl+K Ctrl+D: the last selection goes to the next place of its text */
static void move_sel_next (void) {
  Pos a, b, f;
  char *t;
  size_t n;
  if (!T->sel) {
    select_word();
    return;
  }
  sel_range(&a, &b);
  if (a.y != b.y) return;
  t = doc_text(T->doc, a, b, &n);
  if (next_exact(t, n, b, &f) && !has_cursor_at(f)) {
    T->anchor = f;
    T->cur = f;
    T->cur.x += n;
    T->sel = 1;
    T->want = col_of(row_at(f.y), T->cur.x);
  }
  free(t);
  if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
}


/* Alt+Enter in the find widget: every match selected, a cursor each */
static void select_find_matches (void) {
  size_t y, x, m;
  int first = 1;
  if (E.find[0] == '\0') return;
  for (y = 0; y < T->doc->n; y++) {
    const Row *r = row_at(y);
    for (x = 0; x < r->len; x++)
      if ((m = match_at(y, x)) > 0) {
        if (first) T->nmc = 0;
        else mc_push();
        first = 0;
        T->anchor.y = T->cur.y = y;
        T->anchor.x = x;
        T->cur.x = x + m;
        T->sel = 1;
        T->want = col_of(r, T->cur.x);
        x += m - 1;
      }
  }
  if (first) {
    toast(0, "No results");
    return;
  }
  E.finding = 0;
  E.focus = F_EDITOR;
}


/*
** Ctrl+U (cursorUndo): the cursors as they were before. The main loop keeps
** the states the cursors of the editor in front went through.
*/
#define CUR_HIST	64

static struct {
  Tab *t;	/* whose */
  Cur *c[CUR_HIST];	/* a state: the main cursor, then the others */
  int nc[CUR_HIST];
  unsigned long edits[CUR_HIST];	/* the text's changes then: an edit moves cursors, it is no cursor step */
  int n, at;	/* how many; the one now */
} CH;


static int ch_same (int i) {
  int k;
  Cur now;
  cur_get(&now);
  if (CH.nc[i] != T->nmc + 1) return 0;
  for (k = 0; k <= T->nmc; k++) {
    const Cur *a = &CH.c[i][k], *b = k ? &T->mc[k - 1] : &now;
    if (pos_cmp(a->cur, b->cur) != 0 || a->sel != b->sel || (a->sel && pos_cmp(a->anchor, b->anchor) != 0)) return 0;
  }
  return 1;
}


static void cursor_record (void) {
  int i;
  if (!HAS_DOC || G->diff || T->page || T->md) return;
  if (CH.t != T) {	/* another editor: a history of its own */
    for (i = 0; i < CH.n; i++) free(CH.c[i]);
    CH.n = CH.at = 0;
    CH.t = T;
  }
  if (CH.n > 0 && ch_same(CH.at)) return;
  for (i = CH.at + 1; i < CH.n; i++) free(CH.c[i]);	/* a new way: the redo ones go */
  if (CH.n > 0) CH.n = CH.at + 1;
  if (CH.n > 0 && CH.edits[CH.at] != T->doc->edits) {	/* moved by typing: that state, now */
    free(CH.c[CH.at]);
    CH.n--;
  }
  if (CH.n == CUR_HIST) {
    free(CH.c[0]);
    memmove(CH.c, CH.c + 1, (CUR_HIST - 1) * sizeof(CH.c[0]));
    memmove(CH.nc, CH.nc + 1, (CUR_HIST - 1) * sizeof(CH.nc[0]));
    memmove(CH.edits, CH.edits + 1, (CUR_HIST - 1) * sizeof(CH.edits[0]));
    CH.n--;
  }
  CH.nc[CH.n] = T->nmc + 1;
  CH.edits[CH.n] = T->doc->edits;
  CH.c[CH.n] = (Cur *)xmalloc((size_t)(T->nmc + 1) * sizeof(Cur));
  cur_get(&CH.c[CH.n][0]);
  if (T->nmc) memcpy(CH.c[CH.n] + 1, T->mc, (size_t)T->nmc * sizeof(Cur));
  CH.at = CH.n++;
}


/* d -1: Cursor Undo, 1: Cursor Redo */
static void cursor_back (int d) {
  int to, i;
  cursor_record();
  to = CH.at + d;
  if (CH.t != T || to < 0 || to >= CH.n) return;
  CH.at = to;
  T->nmc = 0;
  for (i = 1; i < CH.nc[to]; i++) {
    Cur c = CH.c[to][i];
    if (T->nmc == T->capmc) {
      T->capmc = T->capmc ? T->capmc * 2 : 8;
      T->mc = (Cur *)xrealloc(T->mc, (size_t)T->capmc * sizeof(Cur));
    }
    c.cur = doc_clamp(T->doc, c.cur);
    c.anchor = doc_clamp(T->doc, c.anchor);
    T->mc[T->nmc++] = c;
  }
  cur_set(&CH.c[to][0]);
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
  if (T->cur.y < T->top || T->cur.y >= T->top + (size_t)L.text_h) center_cursor();
}


/* a word part starts at x (0 < x < len): camelCase humps, snake_case parts */
static int part_edge (const Row *r, size_t x) {
  int a = (unsigned char)r->s[x - 1], b = (unsigned char)r->s[x];
  int ca = char_class(r, x - 1), cb = char_class(r, x);
  if (ca != cb) return 1;
  if (ca != 1) return 0;
  if ((a == '_') != (b == '_')) return 1;
  if (a >= 'a' && a <= 'z' && b >= 'A' && b <= 'Z') return 1;
  if (a >= 'A' && a <= 'Z' && b >= 'A' && b <= 'Z' && x + 1 < r->len && r->s[x + 1] >= 'a' && r->s[x + 1] <= 'z')
    return 1;	/* "HTMLParser": before "Parser" */
  return 0;
}


static Pos part_right (Pos p) {
  const Row *r = row_at(p.y);
  if (p.x >= r->len) return word_right(p);
  p.x = next_x(r, p.x);
  while (p.x < r->len && !part_edge(r, p.x)) p.x = next_x(r, p.x);
  return p;
}


static Pos part_left (Pos p) {
  const Row *r = row_at(p.y);
  if (p.x == 0) return word_left(p);
  p.x = prev_x(r, p.x);
  while (p.x > 0 && !part_edge(r, p.x)) p.x = prev_x(r, p.x);
  return p;
}


/* Transpose Letters: the characters before and at the cursor trade places */
static void transpose (void) {
  const Row *r = row_at(T->cur.y);
  size_t x = T->cur.x, a, e;
  char buf[16];
  Pos pa, pe;
  if (r->len < 2 || x == 0) return;
  if (x >= r->len) x = prev_x(r, r->len);	/* at the end: the last two */
  a = prev_x(r, x);
  e = next_x(r, x);
  if (e - a > sizeof(buf)) return;
  memcpy(buf, r->s + x, e - x);
  memcpy(buf + (e - x), r->s + a, x - a);
  pa.y = pe.y = T->cur.y;
  pa.x = a;
  pe.x = e;
  doc_group(T->doc);
  ed_delete(pa, pe);
  ed_insert(pa, buf, e - a);
  doc_group(T->doc);
  T->sel = 0;
  T->cur.x = e;
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


/*
** editor.autoIndent: VS Code's indentation rules of the language (its
** increaseIndentPattern and decreaseIndentPattern), or its brackets.
*/
typedef struct IRule {
  const char *langs;	/* " python "; NULL: the rest */
  const char *inc, *dec;
} IRule;

static const IRule g_irule[] = {
  {" python ", "^.*:\\s*(#.*)?$", "^\\s*(elif|else|except|finally)\\b.*:\\s*(#.*)?$"},
  {" lua ", "^\\s*((local\\s+)?function\\b.*|.*\\bfunction\\s*\\(.*\\)\\s*|if\\b.*\\bthen|for\\b.*\\bdo|while\\b.*\\bdo|repeat|else|elseif\\b.*\\bthen)\\s*(--.*)?$|[{(\\[]\\s*$",
   "^\\s*(end\\b|else\\b|elseif\\b|until\\b|[})\\]])"},
  {" ruby ", "^\\s*(def|class|module|if|unless|while|until|for|case|begin|else|elsif|when|rescue|ensure)\\b.*$|\\bdo(\\s*\\|[^|]*\\|)?\\s*$|[{(\\[]\\s*$",
   "^\\s*(end\\b|else\\b|elsif\\b|when\\b|rescue\\b|ensure\\b|[})\\]])"},
  {" shellscript ", "(\\bthen|\\bdo|\\{|\\(|^\\s*else)\\s*$|^\\s*case\\b.*\\bin\\s*$", "^\\s*(fi\\b|done\\b|esac\\b|else\\b|elif\\b|[})])"},
  {" yaml ", ":\\s*(#.*)?$", NULL},
  {" html xml vue svelte ", "<[A-Za-z][^/>]*>\\s*$", "^\\s*</"},
  {" markdown plaintext log diff ", NULL, NULL},
  {NULL, "[{(\\[]\\s*(//.*|/\\*.*\\*/\\s*)?$", "^\\s*[})\\]]"}
};

#define NIRULE	(sizeof(g_irule) / sizeof(g_irule[0]))


/* the rules of the language in front; 0: none */
static int ind_rules (Regex **inc, Regex **dec) {
  static Regex *ri[NIRULE], *rd[NIRULE];
  static char done[NIRULE];
  const char *id = doc_lang_id();
  char key[64];
  size_t i;
  if (!T->sx || id == NULL) return 0;
  snprintf(key, sizeof(key), " %s ", id);
  for (i = 0; i + 1 < NIRULE && !strstr(g_irule[i].langs, key); i++) ;
  if (!done[i]) {
    const char *err;
    ri[i] = g_irule[i].inc ? re_compile(g_irule[i].inc, 0, &err) : NULL;
    rd[i] = g_irule[i].dec ? re_compile(g_irule[i].dec, 0, &err) : NULL;
    done[i] = 1;
  }
  *inc = ri[i];
  *dec = rd[i];
  return ri[i] || rd[i];
}


static int ir_match (const Regex *re, const char *s, size_t n) {
  size_t a, b;
  return re && re_find(re, s, n, 0, &a, &b);
}


/* does the text before the cursor (s[0..n)) open a block? */
static int indent_more (const char *s, size_t n) {
  Regex *inc, *dec;
  size_t i;
  for (i = 0; i < n && (s[i] == ' ' || s[i] == '\t'); i++) ;
  if (i == n || !ind_rules(&inc, &dec)) return 0;
  return ir_match(inc, s, n);
}


/* the indent of a line under line p: p's indent, one more when p opens a block */
static char *indent_after (size_t p, int more, int less, size_t *len) {
  const Row *r = row_at(p);
  size_t ind = indent_end(r), unit = T->doc->tabs ? 1 : (size_t)T->doc->indent, n = ind;
  char *s = (char *)xmalloc(ind + unit + 1);
  memcpy(s, r->s, ind);
  if (more && !less) {
    memset(s + n, T->doc->tabs ? '\t' : ' ', unit);
    n += unit;
  }
  else if (less && !more) {	/* one unit less: a tab, or up to indent spaces */
    if (n > 0 && s[n - 1] == '\t') n--;
    else {
      size_t k = 0;
      while (n > 0 && s[n - 1] == ' ' && k < unit) {
        n--;
        k++;
      }
    }
  }
  *len = n;
  return s;
}


static size_t lead_cols (const char *s, size_t n, size_t *bytes);

/* a typed character made the line a block's end ("}", "end", "else:"): its indent goes back */
static void outdent_typed (void) {
  Regex *inc, *dec;
  const Row *r;
  size_t y = T->cur.y, cut, ind, p = 0, tl;
  char *t;
  int was, found = 0;
  Pos a, b;
  if (eopt.auto_indent < 2 || T->nmc > 0 || !ind_rules(&inc, &dec) || dec == NULL || T->cur.x == 0) return;
  r = row_at(y);
  if (!ir_match(dec, r->s, r->len)) return;
  cut = prev_x(r, T->cur.x);	/* without the character typed: did it match? */
  t = (char *)xmalloc(r->len + 1);
  memcpy(t, r->s, cut);
  memcpy(t + cut, r->s + T->cur.x, r->len - T->cur.x);
  was = ir_match(dec, t, r->len - (T->cur.x - cut));
  free(t);
  if (was) return;
  ind = indent_end(r);
  for (p = y; p-- > 0;)
    if (indent_end(row_at(p)) < row_at(p)->len) {
      found = 1;
      break;
    }
  if (found) t = indent_after(p, ir_match(inc, row_at(p)->s, row_at(p)->len), 1, &tl);
  else {
    t = xstrdup("");
    tl = 0;
  }
  {
    size_t now = col_of(r, ind), want = lead_cols(t, tl, NULL);
    if (want < now) {	/* only ever back */
      a.y = b.y = y;
      a.x = 0;
      b.x = ind;
      ed_delete(a, b);
      ed_insert(a, t, tl);
      T->cur.x = T->cur.x - ind + tl;
      T->want = col_of(row_at(y), T->cur.x);
    }
  }
  free(t);
}


/* the columns of a line's leading white space */
static size_t lead_cols (const char *s, size_t n, size_t *bytes) {
  size_t i, c = 0;
  for (i = 0; i < n && (s[i] == ' ' || s[i] == '\t'); i++) c += s[i] == '\t' ? (size_t)TABW - c % (size_t)TABW : 1;
  if (bytes) *bytes = i;
  return c;
}


/* white space of c columns, as the file indents */
static void put_indent (Buf *b, size_t c) {
  if (T->doc->tabs) {
    for (; c >= (size_t)TABW; c -= (size_t)TABW) buf_putc(b, '\t');
  }
  for (; c > 0; c--) buf_putc(b, ' ');
}


/* lines ly..hy indented as the rules say, from the line above them; 1 something changed */
static int reindent_lines (size_t ly, size_t hy) {
  Regex *inc, *dec;
  size_t y, p, unit = T->doc->tabs ? (size_t)TABW : (size_t)T->doc->indent;
  long level = 0;
  int changed = 0;
  if (!ind_rules(&inc, &dec)) return 0;
  for (p = ly; p-- > 0;)	/* from the line above: its indent */
    if (indent_end(row_at(p)) < row_at(p)->len) {
      const Row *r = row_at(p);
      level = (long)(lead_cols(r->s, r->len, NULL) / unit) + (ir_match(inc, r->s, r->len) ? 1 : 0);
      break;
    }
  doc_group(T->doc);
  for (y = ly; y <= hy; y++) {
    const Row *r = row_at(y);
    size_t nb, have = lead_cols(r->s, r->len, &nb);
    long lev = level - (ir_match(dec, r->s, r->len) ? 1 : 0);
    Buf b;
    if (nb == r->len) continue;	/* a blank line stays */
    if (lev < 0) lev = 0;
    buf_init(&b);
    put_indent(&b, (size_t)lev * unit);
    if (have != (size_t)lev * unit || b.len != nb || memcmp(b.s ? b.s : "", r->s, nb) != 0) {
      Pos a, e;
      a.y = e.y = y;
      a.x = 0;
      e.x = nb;
      ed_delete(a, e);
      keep_del(a, e);
      if (b.len) keep_ins(a, ed_insert(a, b.s, b.len));
      changed = 1;
    }
    buf_free(&b);
    r = row_at(y);
    level = lev + (ir_match(inc, r->s, r->len) ? 1 : 0);
  }
  doc_group(T->doc);
  T->cur = doc_clamp(T->doc, T->cur);
  T->anchor = doc_clamp(T->doc, T->anchor);
  return changed;
}


/* Reindent Lines (all) / Reindent Selected Lines */
static void reindent (int sel_only) {
  Regex *inc, *dec;
  size_t ly = 0, hy = T->doc->n - 1;
  if (!ind_rules(&inc, &dec)) {
    toast(0, "There are no indentation rules for %s", syntax_name(T->sx));
    return;
  }
  if (sel_only) sel_lines(&ly, &hy);
  if (!reindent_lines(ly, hy)) toast(0, "The lines are indented already");
}


/* editor.autoIndentOnPaste: pasted lines moved to the indent of the cursor */
static void paste_text (const char *s, size_t n) {
  const Row *r;
  size_t i, base = (size_t)-1, target, first_nb;
  const char *p;
  Buf o;
  if (!eopt.indent_paste || eopt.auto_indent < 2 || memchr(s, '\n', n) == NULL || T->nmc > 0) {
    insert(s, n);
    return;
  }
  delete_sel();
  r = row_at(T->cur.y);
  if (indent_end(r) < T->cur.x) {	/* not in the indent: as it is */
    insert(s, n);
    return;
  }
  target = col_of(r, T->cur.x);
  for (p = s; p < s + n;) {	/* the least indent of its lines with text */
    const char *e = memchr(p, '\n', (size_t)(s + n - p));
    size_t ll = e ? (size_t)(e - p) : (size_t)(s + n - p), c = lead_cols(p, ll, &first_nb);
    if (first_nb < ll && c < base) base = c;
    p = e ? e + 1 : s + n;
  }
  if (base == (size_t)-1) {
    insert(s, n);
    return;
  }
  buf_init(&o);
  for (p = s, i = 0; p < s + n; i++) {
    const char *e = memchr(p, '\n', (size_t)(s + n - p));
    size_t ll = e ? (size_t)(e - p) : (size_t)(s + n - p), nb, c = lead_cols(p, ll, &nb);
    if (nb < ll || i == 0) put_indent(&o, (c > base ? c - base : 0) + (i ? target : 0));	/* the first: after the cursor */
    buf_putn(&o, p + nb, ll - nb);
    if (e) buf_putc(&o, '\n');
    p = e ? e + 1 : s + n;
  }
  {
    size_t y0 = T->cur.y;
    Regex *inc, *dec;
    insert(o.s ? o.s : "", o.len);
    if (ind_rules(&inc, &dec) && T->cur.y > y0) reindent_lines(y0 + 1, T->cur.y);	/* the language's rules, as VS Code: under the first line */
  }
  buf_free(&o);
}


/* editor.tabCompletion: a snippet's prefix before the cursor, then Tab, puts it in */
static int tab_complete (void) {
  const Row *r = row_at(T->cur.y);
  size_t ns, i, x0 = T->cur.x;
  const Snip *v;
  if (T->sel || T->nmc > 0 || T->cur.x == 0) return 0;
  while (x0 > 0 && r->s[x0 - 1] != ' ' && r->s[x0 - 1] != '\t') x0--;
  v = snip_list(doc_lang_id(), &ns);
  for (; x0 < T->cur.x; x0++)	/* "#ifndef", or the word part of "(for" */
    for (i = 0; i < ns; i++)
      if (strlen(v[i].prefix) == T->cur.x - x0 && memcmp(v[i].prefix, r->s + x0, T->cur.x - x0) == 0) {
        Pos a = T->cur;
        a.x = x0;
        comp_free();
        snippet_insert(a, T->cur, v[i].body);
        return 1;
      }
  return 0;
}


/* editor.suggestSelection: the suggestions taken lately, with what was typed for them */
#define RECENT_SUG	32

static struct {
  char *label[RECENT_SUG], *pre[RECENT_SUG];
  int n;
} RS;


static void comp_recent_add (const char *label, const char *pre, size_t n) {
  int i;
  if (eopt.suggest_sel == 0) return;
  if (RS.n == RECENT_SUG) {
    free(RS.label[RS.n - 1]);
    free(RS.pre[RS.n - 1]);
    RS.n--;
  }
  for (i = RS.n; i > 0; i--) {
    RS.label[i] = RS.label[i - 1];
    RS.pre[i] = RS.pre[i - 1];
  }
  RS.label[0] = xstrdup(label);
  RS.pre[0] = xstrndup(pre, n);
  RS.n++;
}


static void comp_recent_pick (const char *pre) {
  int i;
  size_t k;
  if (eopt.suggest_sel == 0 || !CP.open) return;
  for (i = 0; i < RS.n; i++) {
    if (eopt.suggest_sel == 2 && strcmp(RS.pre[i], pre) != 0) continue;	/* recentlyUsedByPrefix */
    for (k = 0; k < CP.nvis; k++)
      if (strcmp(CP.v[CP.vis[k]].label, RS.label[i]) == 0) {
        CP.sel = k;
        CP.top = k > 5 ? k - 5 : 0;
        return;
      }
  }
}


/* editor.quickSuggestions: may the list show up where the cursor is (other, comments, strings)? */
static int qs_allowed (void) {
  static unsigned char *tok;
  static size_t cap;
  const Row *r = row_at(T->cur.y);
  int t;
  if (!T->sx || T->cur.x == 0) return eopt.qs_other;
  if (r->len + 1 > cap) {
    cap = r->len + 256;
    tok = (unsigned char *)xrealloc(tok, cap);
  }
  syntax_line(T->doc, T->sx, T->cur.y, tok);
  t = tok[T->cur.x - 1];
  if (t == T_COMMENT) return eopt.qs_comments;
  if (t == T_STRING || t == T_ESCAPE) return eopt.qs_strings;
  return eopt.qs_other;
}


/*
** editor.unicodeHighlight: characters easy to take for ASCII ones (VS Code's
** confusables, the common ones) and the ones that only make room.
*/
static const struct {
  uint32_t cp, ascii;
} g_confuse[] = {
  {0x00D7, 'x'}, {0x0131, 'i'}, {0x01C0, 'l'}, {0x037E, ';'},
  {0x0391, 'A'}, {0x0392, 'B'}, {0x0395, 'E'}, {0x0396, 'Z'}, {0x0397, 'H'}, {0x0399, 'I'}, {0x039A, 'K'},
  {0x039C, 'M'}, {0x039D, 'N'}, {0x039F, 'O'}, {0x03A1, 'P'}, {0x03A4, 'T'}, {0x03A5, 'Y'}, {0x03A7, 'X'},
  {0x03B1, 'a'}, {0x03BD, 'v'}, {0x03BF, 'o'}, {0x03C1, 'p'},
  {0x0405, 'S'}, {0x0406, 'I'}, {0x0408, 'J'}, {0x0410, 'A'}, {0x0412, 'B'}, {0x0415, 'E'}, {0x041A, 'K'},
  {0x041C, 'M'}, {0x041D, 'H'}, {0x041E, 'O'}, {0x0420, 'P'}, {0x0421, 'C'}, {0x0422, 'T'}, {0x0425, 'X'},
  {0x0430, 'a'}, {0x0435, 'e'}, {0x043E, 'o'}, {0x0440, 'p'}, {0x0441, 'c'}, {0x0443, 'y'}, {0x0445, 'x'},
  {0x0455, 's'}, {0x0456, 'i'}, {0x0458, 'j'}, {0x0501, 'd'},
  {0x2010, '-'}, {0x2011, '-'}, {0x2012, '-'}, {0x2013, '-'}, {0x2014, '-'}, {0x2018, '\''}, {0x2019, '\''},
  {0x201A, ','}, {0x201C, '"'}, {0x201D, '"'}, {0x2044, '/'}, {0x2215, '/'}, {0x2212, '-'}, {0x2236, ':'}
};


/* 1 ambiguous (*ascii: the character it looks like), 2 invisible; 0 neither (or the setting is off) */
static uint32_t uni_flag (uint32_t cp, uint32_t *ascii) {
  size_t i;
  const char *id;
  if (cp < 0x80) return 0;
  if (eopt.uni_invisible && (cp == 0xA0 || cp == 0xAD || (cp >= 0x2000 && cp <= 0x200F) || cp == 0x2028 ||
                             cp == 0x2029 || cp == 0x202F || cp == 0x205F || (cp >= 0x2060 && cp <= 0x2064) ||
                             cp == 0x3000 || cp == 0xFEFF || cp == 0x180E))
    return 2;
  if (!eopt.uni_ambiguous) return 0;
  id = HAS_DOC ? doc_lang_id() : NULL;
  if (id && (strcmp(id, "markdown") == 0 || strcmp(id, "plaintext") == 0)) return 0;	/* VS Code's language defaults */
  if (cp >= 0xFF01 && cp <= 0xFF5E) {	/* fullwidth */
    if (ascii) *ascii = cp - 0xFF01 + 0x21;
    return 1;
  }
  for (i = 0; i < sizeof(g_confuse) / sizeof(g_confuse[0]); i++)
    if (g_confuse[i].cp == cp) {
      if (ascii) *ascii = g_confuse[i].ascii;
      return 1;
    }
  return 0;
}


/* the hover's words for the character at p (VS Code's), or NULL */
static char *uni_explain (Pos p) {
  const Row *r;
  size_t len;
  uint32_t cp, a = 0, f;
  char u[5], b[400];
  if (p.y >= T->doc->n) return NULL;
  r = row_at(p.y);
  if (p.x >= r->len) return NULL;
  cp = utf8_decode(r->s + p.x, r->len - p.x, &len);
  if ((f = uni_flag(cp, &a)) == 0) return NULL;
  u[utf8_encode(cp, u)] = '\0';
  if (f == 2) snprintf(b, sizeof(b), "The character U+%04X is invisible.", (unsigned)cp);
  else snprintf(b, sizeof(b), "The character U+%04X \"%s\" could be confused with the ASCII character U+%04X \"%c\", "
                              "which is more common in source code.", (unsigned)cp, u, (unsigned)a, (int)a);
  return xstrdup(b);
}


/* the commands of this part */
static void edit_extra (int cmd) {
  Pos a, b;
  const Row *r = row_at(T->cur.y);
  switch (cmd) {
    case CMD_CURSORS_LINE_ENDS: cursors_line_ends(); break;
    case CMD_CURSOR_UNDO: cursor_back(-1); break;
    case CMD_CURSOR_REDO: cursor_back(1); break;
    case CMD_MOVE_SEL_NEXT: move_sel_next(); break;
    case CMD_CHANGE_ALL: add_next_match(1); break;
    case CMD_SELECT_ALL_FIND: select_find_matches(); break;
    case CMD_REINDENT: reindent(0); break;
    case CMD_REINDENT_SEL: reindent(1); break;
    case CMD_ADD_COMMENT: comment_lines(1); break;
    case CMD_REMOVE_COMMENT: comment_lines(2); break;
    case CMD_TRANSPOSE: transpose(); break;
    case CMD_PART_LEFT: move_h(part_left(T->cur), 0); break;
    case CMD_PART_RIGHT: move_h(part_right(T->cur), 0); break;
    case CMD_DEL_ALL_LEFT: case CMD_DEL_ALL_RIGHT: case CMD_DEL_PART_LEFT: case CMD_DEL_PART_RIGHT:
      doc_group(T->doc);
      if (delete_sel()) {
        doc_group(T->doc);
        break;
      }
      a = b = T->cur;
      if (cmd == CMD_DEL_ALL_LEFT) a.x = 0;
      else if (cmd == CMD_DEL_ALL_RIGHT) {
        if (b.x < r->len) b.x = r->len;
        else if (b.y + 1 < T->doc->n) {	/* at the end: the line break */
          b.y++;
          b.x = 0;
        }
      }
      else if (cmd == CMD_DEL_PART_LEFT) a = part_left(T->cur);
      else b = part_right(T->cur);
      ed_delete(a, b);
      T->cur = a;
      T->want = col_of(row_at(a.y), a.x);
      doc_group(T->doc);
      break;
    case CMD_SCROLL_PAGE_UP: case CMD_SCROLL_PAGE_DOWN: {
      size_t th = (size_t)L.text_h;
      if (cmd == CMD_SCROLL_PAGE_UP) T->top = T->top > th ? T->top - th : 0;
      else if (T->top + th < T->doc->n) T->top += th;
      E.follow = 0;
      break;
    }
  }
}

/* }================================================================== */


static void editor_key_one (int k);


/* editor.linkedEditing: the name of the tag at the cursor, and of its pair */
typedef struct LinkTag {
  int on;
  Pos a;	/* the name at the cursor starts */
  size_t len;	/* its length */
  Pos pa;	/* its pair's name */
  char name[128];
  size_t lines;
  int del;	/* the key deletes */
} LinkTag;

/* the tag name the cursor is in or after: its start, length; *closing: "</x" */
static int tag_name_at (Pos p, Pos *a, size_t *len, int *closing) {
  const Row *r = row_at(p.y);
  size_t x = p.x, e;
  while (x > 0 && tag_char((unsigned char)r->s[x - 1])) x--;
  if (x == 0) return 0;
  if (r->s[x - 1] == '<') *closing = 0;
  else if (r->s[x - 1] == '/' && x >= 2 && r->s[x - 2] == '<') *closing = 1;
  else return 0;
  for (e = x; e < r->len && tag_char((unsigned char)r->s[e]); e++) ;
  a->y = p.y;
  a->x = x;
  *len = e - x;
  return 1;
}


/* the pair of the tag named nm at a (closing: look back for its opening) */
static int tag_pair (Pos a, const char *nm, size_t nl, int closing, Pos *pa) {
  long depth = 0;
  size_t y = a.y, lines = 0;
  if (!closing) {
    size_t x = a.x + nl;
    for (; y < T->doc->n && lines < 3000; y++, lines++, x = 0) {
      const Row *r = row_at(y);
      for (; x < r->len; x++) {
        int cl;
        if (r->s[x] != '<') continue;
        cl = x + 1 < r->len && r->s[x + 1] == '/';
        if (x + 1 + cl + nl <= r->len && memcmp(r->s + x + 1 + cl, nm, nl) == 0 &&
            (x + 1 + cl + nl == r->len || !tag_char((unsigned char)r->s[x + 1 + cl + nl]))) {
          if (!cl) {	/* <x ... /> does not open */
            size_t q = x + 1 + nl;
            while (q < r->len && r->s[q] != '>') q++;
            if (!(q < r->len && q > 0 && r->s[q - 1] == '/')) depth++;
          }
          else if (depth-- == 0) {
            pa->y = y;
            pa->x = x + 2;
            return 1;
          }
        }
      }
    }
    return 0;
  }
  for (;; lines++) {
    const Row *r = row_at(y);
    long x = y == a.y ? (long)a.x - 3 : (long)r->len - 1;
    for (; x >= 0; x--) {
      int cl;
      size_t ux = (size_t)x;
      if (r->s[ux] != '<') continue;
      cl = ux + 1 < r->len && r->s[ux + 1] == '/';
      if (ux + 1 + cl + nl <= r->len && memcmp(r->s + ux + 1 + cl, nm, nl) == 0 &&
          (ux + 1 + cl + nl == r->len || !tag_char((unsigned char)r->s[ux + 1 + cl + nl]))) {
        if (cl) depth++;
        else {
          size_t q = ux + 1 + nl;
          while (q < r->len && r->s[q] != '>') q++;
          if (q < r->len && q > 0 && r->s[q - 1] == '/') continue;
          if (depth-- == 0) {
            pa->y = y;
            pa->x = ux + 1;
            return 1;
          }
        }
      }
    }
    if (y == 0 || lines > 3000) return 0;
    y--;
  }
}


static int link_lang (void) {
  const char *n = T->sx ? syntax_name(T->sx) : "";
  return strcmp(n, "HTML") == 0 || strcmp(n, "XML") == 0 || strcmp(n, "Vue") == 0 || strcmp(n, "Svelte") == 0 ||
         strstr(n, "JavaScript") || strstr(n, "TypeScript");
}


/* before a key: the tag at the cursor and its pair */
static void link_before (LinkTag *lt, int k) {
  int closing;
  lt->on = 0;
  if (!opt.linked_edit || T->nmc || T->sel || !link_lang() || !(IS_TEXT(k) || k == K_BS || k == K_DEL)) return;
  if (!tag_name_at(T->cur, &lt->a, &lt->len, &closing) || lt->len == 0 || lt->len >= sizeof(lt->name)) return;
  memcpy(lt->name, row_at(lt->a.y)->s + lt->a.x, lt->len);
  lt->name[lt->len] = '\0';
  if (!tag_pair(lt->a, lt->name, lt->len, closing, &lt->pa)) return;
  lt->lines = T->doc->n;
  lt->del = k == K_BS || k == K_DEL;
  lt->on = 1;
}


/* after it: the pair's name made the same */
static void link_after (const LinkTag *lt) {
  Pos a, pa = lt->pa, pb;
  size_t len;
  int closing;
  char now[128];
  if (!lt->on || T->doc->n != lt->lines || T->cur.y != lt->a.y) return;
  if (!tag_name_at(T->cur, &a, &len, &closing)) {	/* all of it deleted: the name is empty */
    if (!lt->del) return;
    a = lt->a;
    len = 0;
  }
  if (a.x != lt->a.x || len >= sizeof(now)) return;
  memcpy(now, row_at(a.y)->s + a.x, len);
  now[len] = '\0';
  if (strcmp(now, lt->name) == 0) return;
  if (pa.y == a.y && pa.x > a.x) pa.x = pa.x + len - lt->len;	/* the pair after it on the line moved */
  pb = pa;
  pb.x += lt->len;
  if (pb.x > row_at(pa.y)->len || memcmp(row_at(pa.y)->s + pa.x, lt->name, lt->len) != 0) return;
  ed_delete(pa, pb);
  if (pa.y == T->cur.y && pa.x < T->cur.x) T->cur.x -= lt->len;
  ed_insert(pa, now, len);
  if (pa.y == T->cur.y && pa.x < T->cur.x) T->cur.x += len;
}

/* a key in the Markdown preview: it scrolls */
static void md_key (int k) {
  size_t th = (size_t)L.text_h;
  E.follow = 0;
  switch (KEY_CODE(k)) {
    case K_UP: if (T->top > 0) T->top--; break;
    case K_DOWN: T->top++; break;
    case K_PGUP: T->top = T->top > th ? T->top - th : 0; break;
    case K_PGDN: case ' ': T->top += th; break;
    case K_HOME: T->top = 0; break;
    case K_END: T->top = (size_t)-1 / 2; break;	/* md_draw keeps it inside */
  }
}


static int is_markdown (const Tab *t) {
  const char *p = t->doc->path, *dot = p ? strrchr(path_basename(p), '.') : NULL;
  return dot && (m_fncmp(dot, ".md") == 0 || m_fncmp(dot, ".markdown") == 0 || m_fncmp(dot, ".mdx") == 0);
}


/*
** Ctrl+Shift+V: the preview in a tab of this group; Ctrl+K V: in the group
** to the side (made when there is none). Its text is the file's: it follows.
*/
static void md_preview (int side) {
  Doc *d;
  Tab *t;
  int i, at;
  if (!HAS_DOC || G->diff) return;
  if (T->md) return;
  if (!is_markdown(T)) {
    toast(0, "Markdown: Open Preview works on Markdown files (.md).");
    return;
  }
  d = T->doc;
  if (side) {	/* the file keeps the keys, like VS Code's */
    int g0 = g_gcur;
    if (g_ngrp == 1) {	/* a new group with the file: that tab becomes the preview */
      split_editor();
      T->md = 1;
      T->sx = NULL;
      T->top = 0;
      free(T->real);
      T->real = NULL;
    }
    else {
      focus_group(g0 + 1 < g_ngrp ? g0 + 1 : g0 - 1);
      md_preview(0);
    }
    focus_group(g0);
    E.focus = F_EDITOR;
    return;
  }
  for (i = 0; i < G->ntab; i++)	/* there already */
    if (G->tab[i]->md && G->tab[i]->doc == d) {
      focus_tab(i);
      E.focus = F_EDITOR;
      return;
    }
  t = (Tab *)xmalloc(sizeof(Tab));
  memset(t, 0, sizeof(*t));
  t->doc = d;
  d->refs++;
  t->md = 1;
  if (G->ntab == G->captab) {
    G->captab = G->captab ? G->captab * 2 : 8;
    G->tab = (Tab **)xrealloc(G->tab, (size_t)G->captab * sizeof(Tab *));
  }
  at = G->ntab ? G->active + 1 : 0;
  memmove(G->tab + at + 1, G->tab + at, (size_t)(G->ntab - at) * sizeof(Tab *));
  G->tab[at] = t;
  G->ntab++;
  G->active = at;
  T = t;
  G->diff = 0;
  E.focus = F_EDITOR;
}


/*
** {==================================================================
** Help pages: made as Markdown in mme-data/help, shown in the preview
** ===================================================================
*/

static void help_row (Buf *b, int cmd) {
  const char *k = cmd_keys(cmd);
  if (k[0]) buf_printf(b, "| %s | `%s` |\n", cmd_name(cmd), k);
}


/* Help: Keyboard Shortcuts Reference: VS Code's sheet, with the keys as they are now (keybindings.json too) */
static void help_keys_md (Buf *b) {
  static const struct {
    const char *title;
    int cmd[24];
  } part[] = {
    {"General", {CMD_PALETTE, CMD_QUICK_OPEN, CMD_SETTINGS, CMD_KEYS, CMD_THEME, CMD_QUIT, 0}},
    {"Basic editing", {CMD_CUT, CMD_COPY, CMD_PASTE, CMD_UNDO, CMD_REDO, CMD_LINE_UP, CMD_LINE_DOWN, CMD_COPY_UP,
                       CMD_COPY_DOWN, CMD_DELETE_LINE, CMD_LINE_BELOW, CMD_LINE_ABOVE, CMD_GOTO_BRACKET, CMD_INDENT,
                       CMD_OUTDENT, CMD_COMMENT, CMD_BLOCK_COMMENT, CMD_WORDWRAP, CMD_FOLD, CMD_UNFOLD, CMD_FOLD_ALL,
                       CMD_UNFOLD_ALL, 0}},
    {"Navigation", {CMD_GOTO_SYMBOL, CMD_WORKSPACE_SYMBOL, CMD_GOTO, CMD_PROBLEMS, CMD_NEXT_PROBLEM, CMD_PREV_PROBLEM,
                    CMD_NAV_BACK, CMD_NAV_FORWARD, CMD_SWITCH_EDITOR, CMD_BREADCRUMBS, 0}},
    {"Search and replace", {CMD_FIND, CMD_REPLACE, CMD_FIND_FILES, CMD_REPLACE_FILES, 0}},
    {"Multi-cursor and selection", {CMD_CURSOR_UP, CMD_CURSOR_DOWN, CMD_NEXT_MATCH, CMD_ALL_MATCHES,
                                    CMD_SELECT_LINE, CMD_CURSORS_LINE_ENDS, CMD_CURSOR_UNDO, CMD_CHANGE_ALL,
                                    CMD_EXPAND_SEL, CMD_SHRINK_SEL, 0}},
    {"Rich languages editing", {CMD_SUGGEST, CMD_PARAM_HINTS, CMD_FORMAT, CMD_FORMAT_SEL, CMD_DEFINITION,
                                CMD_PEEK_DEF, CMD_REFERENCES, CMD_IMPLEMENTATION, CMD_QUICKFIX, CMD_RENAME,
                                CMD_ORGANIZE_IMPORTS, CMD_CALL_HIERARCHY, CMD_TRIM, CMD_LANGUAGE, 0}},
    {"Editor management", {CMD_CLOSE, CMD_SPLIT, CMD_SPLIT_DOWN, CMD_GROUP1, CMD_GROUP2, CMD_MOVE_NEXT_GROUP,
                           CMD_MOVE_PREV_GROUP, CMD_REOPEN, CMD_PIN, 0}},
    {"File management", {CMD_NEW, CMD_OPEN_FILE, CMD_SAVE, CMD_SAVE_AS, CMD_OPEN_PROJECT, CMD_COMPARE_SAVED,
                         CMD_COPY_PATH, CMD_REVEAL_OS, 0}},
    {"Display", {CMD_ZEN, CMD_SIDEBAR, CMD_EXPLORER, CMD_SEARCH, CMD_GIT, CMD_DEBUG_VIEW, CMD_EXTENSIONS,
                 CMD_OUTPUT, CMD_MD_PREVIEW, CMD_MD_SIDE, CMD_PANEL_MAX, 0}},
    {"Debug", {CMD_BREAKPOINT, CMD_DEBUG_START, CMD_DEBUG_RUN, CMD_DEBUG_STOP, CMD_DEBUG_STEP_OVER,
               CMD_DEBUG_STEP_INTO, CMD_DEBUG_STEP_OUT, CMD_DEBUG_CONSOLE, CMD_TASK_BUILD, 0}},
    {"Integrated terminal", {CMD_TERMINAL, CMD_TERMINAL_NEW, CMD_TERMINAL_SPLIT, CMD_TERMINAL_FIND, CMD_TERM_COPY,
                             CMD_TERM_PASTE, CMD_TERM_PREV_CMD, CMD_TERM_NEXT_CMD, CMD_TERM_RECENT, 0}}
  };
  size_t i, k;
  buf_puts(b, "# Keyboard Shortcuts\n\n"
              "The keys of mme, as VS Code's reference sheet groups them. Your own keys "
              "(Keyboard Shortcuts, Ctrl+K Ctrl+S) show as they are now. Ctrl+Shift+ keys and a "
              "few others need a terminal with the kitty keyboard protocol (mmc-term, kitty, WezTerm).\n\n");
  for (i = 0; i < sizeof(part) / sizeof(part[0]); i++) {
    buf_printf(b, "## %s\n\n| Command | Keys |\n|---|---|\n", part[i].title);
    for (k = 0; part[i].cmd[k]; k++) help_row(b, part[i].cmd[k]);
    buf_puts(b, "\n");
  }
}


static void help_tips_md (Buf *b) {
  buf_puts(b, "# Tips and Tricks\n\n"
              "## Find anything\n\n"
              "- **Ctrl+P** opens a file by name: type parts of its name or path (`src/ma` finds `src/main.c`), "
              "the letters matched are lit.\n"
              "- In the same box, **>** runs a command, **@** goes to a symbol of the file, **#** to one of the "
              "workspace, **:** to a line, **?** lists them.\n"
              "- **Ctrl+Shift+F** searches every file; **Alt+C**, **Alt+W**, **Alt+R** toggle case, whole word "
              "and regular expressions.\n\n"
              "## Edit faster\n\n"
              "- **Ctrl+D** selects the next place of the word; **Ctrl+Shift+L** all of them; **Alt+Click** "
              "adds a cursor.\n"
              "- **Alt+Up / Alt+Down** move lines, **Shift+Alt+Up / Down** copy them.\n"
              "- **Ctrl+/** comments lines; **Ctrl+Shift+[** folds the block.\n"
              "- Type a snippet's prefix (`for`, `if`, `main`) and press **Tab**; in HTML, Emmet: `ul>li*3` "
              "then **Tab**.\n\n"
              "## Understand code\n\n"
              "- **F12** goes to the definition, **Alt+F12** peeks it, **Shift+F12** lists the references.\n"
              "- **F2** renames a symbol everywhere; **Ctrl+.** shows the quick fixes (the lightbulb).\n"
              "- **Ctrl+Shift+O** lists the file's symbols; the Outline in the Explorer does too.\n\n"
              "## Work with git\n\n"
              "- **Ctrl+Shift+G**: stage (`+`), commit (**Ctrl+Enter**), the graph of the history.\n"
              "- The bars in the gutter are your changes: click one to see it, stage it or revert it.\n\n"
              "## Make it yours\n\n"
              "- **Ctrl+,** opens the Settings, **Ctrl+K Ctrl+T** the color themes, **Ctrl+K Ctrl+S** the "
              "keyboard shortcuts; Preferences: Import VS Code Settings brings yours over.\n"
              "- Everything mme keeps is in the folder `mme-data` next to the program.\n");
}


/* the page in mme-data/help/<name>, made again and shown in the Markdown preview */
static void help_page (const char *name, void (*make) (Buf *b)) {
  char *dir = data_path("help"), *f;
  Buf b;
  int fd, i;
  Tab *src;
  mkdir_p(dir);
  f = path_join(dir, name);
  free(dir);
  buf_init(&b);
  make(&b);
  if ((fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  buf_free(&b);
  for (i = 0; i < G->ntab; i++) {	/* its preview is open: to the front, with the new text */
    Tab *t = G->tab[i];
    if (t->md && t->doc->path && m_fncmp(t->doc->path, f) == 0) {
      focus_tab(i);
      doc_load(t->doc, f);
      free(f);
      E.focus = F_EDITOR;
      return;
    }
  }
  if (open_file(f, 0) != 0) {
    free(f);
    return;
  }
  free(f);
  src = T;
  md_preview(0);
  for (i = 0; i < G->ntab; i++)	/* the preview stays, the Markdown source goes */
    if (G->tab[i] == src && src != T) {
      int k, on = -1;
      Tab *keep = T;
      close_tab(i);
      for (k = 0; k < G->ntab; k++)
        if (G->tab[k] == keep) on = k;
      if (on >= 0) focus_tab(on);
      break;
    }
  E.focus = F_EDITOR;
}

/* }================================================================== */


static void editor_key (int k) {
  if (E.zen && k == K_ESC) {	/* Esc Esc: out of Zen mode */
    long long now = os_now_us();
    if (now - E.last_esc < 700000) {
      E.zen = 0;
      scr_redraw();
      E.last_esc = 0;
      return;
    }
    E.last_esc = now;
  }
  if (!HAS_DOC) return;
  if (T->page) {
    page_key(k);
    return;
  }
  if (T->md) {
    md_key(k);
    return;
  }
  if (PK.open && peek_key(k)) return;
  if (DP.open && dirty_key(k)) return;
  hover_close();
  if (SG.label && KEY_CODE(k) == K_ESC && !CP.open) {
    sig_close();
    return;
  }
  if (SN.t && SN.t != T) snip_end();
  if (SN.t && KEY_CODE(k) == K_TAB && !(k & (KM_CTRL | KM_ALT))) {	/* the next / previous tab stop (Enter takes a suggestion) */
    comp_free();
    snip_next((k & KM_SHIFT) ? -1 : 1);
    return;
  }
  if (CP.open && comp_key(k)) return;
  if (k == K_TAB && E.tab_focus) {	/* Toggle Tab Key Moves Focus: out of the editor */
    if (E.side) E.focus = F_SIDE;
    else if (E.panel) E.focus = F_PANEL;
    return;
  }
  if (k == K_TAB && eopt.tab_completion && tab_complete()) return;	/* editor.tabCompletion */
  if (KEY_CODE(k) == K_TAB && !(k & (KM_CTRL | KM_ALT | KM_SHIFT)) && emmet_tab()) return;	/* Emmet */
  if (SN.t && (KEY_CODE(k) == K_ESC || k == CTRL('z') || k == CTRL('y'))) snip_end();
  {	/* formatOnPaste, formatOnType; linked editing of an HTML tag's name */
    Pos pa, pb;
    int paste = (KEY_CODE(k) == K_PASTE || k == CTRL('v')) && T->nmc == 0;
    LinkTag lt;
    sel_range(&pa, &pb);
    if (!T->sel) pa = T->cur;
    link_before(&lt, k);
    editor_key_one(k);
    link_after(&lt);
    if (paste && opt.format_paste && lsp_active(T->doc) && pos_cmp(pa, T->cur) < 0) lsp_format_range(T->doc, pa, T->cur);
    if (opt.format_type && T->nmc == 0 && lsp_active(T->doc) && ((IS_TEXT(k) && k < 128) || k == K_ENTER)) {
      char ch[2];
      ch[0] = k == K_ENTER ? '\n' : (char)k;
      ch[1] = '\0';
      if (strchr(lsp_type_chars(T->doc), ch[0])) lsp_format_type(T->doc, T->cur, ch);
    }
  }
  if (SN.t && (pos_cmp(T->cur, SN.a) < 0 || pos_cmp(T->cur, SN.b) > 0)) snip_end();	/* out of it */
  comp_after_key(k);
}


static void editor_key_one (int k) {
  int code = KEY_CODE(k), kind = KIND_OTHER;
  E.tab_unit_len = T->doc->tabs ? 1 : (size_t)T->doc->indent;
  if (E.tab_unit_len > sizeof(E.tab_unit)) E.tab_unit_len = sizeof(E.tab_unit);
  memset(E.tab_unit, T->doc->tabs ? '\t' : ' ', E.tab_unit_len);
  if (IS_TEXT(k)) kind = KIND_TYPE;
  else if (code == K_BS || code == K_DEL) kind = KIND_DELETE;
  if (kind != T->last_kind || kind == KIND_OTHER || T->sel) doc_group(T->doc);
  if (kind == KIND_TYPE && (code == ' ' || code == '\t')) doc_group(T->doc);
  T->last_kind = kind;
  {	/* the editor's own commands */
    static const struct {
      int key, cmd;
    } ek[] = {
      {0x1F, CMD_COMMENT}, {'/' | KM_CTRL, CMD_COMMENT},
      {'A' | KM_ALT, CMD_BLOCK_COMMENT}, {'a' | KM_ALT | KM_SHIFT, CMD_BLOCK_COMMENT}, {'A' | KM_ALT | KM_SHIFT, CMD_BLOCK_COMMENT},
      {K_UP | KM_ALT | KM_SHIFT, CMD_COPY_UP}, {K_DOWN | KM_ALT | KM_SHIFT, CMD_COPY_DOWN},
      {'k' | KM_CTRL | KM_SHIFT, CMD_DELETE_LINE}, {'K' | KM_CTRL | KM_SHIFT, CMD_DELETE_LINE},
      {CTRL('l'), CMD_SELECT_LINE},
      {'F' | KM_ALT, CMD_FORMAT}, {'f' | KM_ALT | KM_SHIFT, CMD_FORMAT}, {'F' | KM_ALT | KM_SHIFT, CMD_FORMAT},
      {'i' | KM_CTRL | KM_SHIFT, CMD_FORMAT},
      {K_ENTER | KM_CTRL, CMD_LINE_BELOW}, {K_ENTER | KM_CTRL | KM_SHIFT, CMD_LINE_ABOVE},
      {CTRL(']'), CMD_INDENT}, {']' | KM_CTRL, CMD_INDENT}, {'[' | KM_CTRL, CMD_OUTDENT},
      {K_RIGHT | KM_ALT | KM_SHIFT, CMD_EXPAND_SEL}, {K_LEFT | KM_ALT | KM_SHIFT, CMD_SHRINK_SEL},
      {'O' | KM_ALT, CMD_ORGANIZE_IMPORTS}, {'o' | KM_ALT | KM_SHIFT, CMD_ORGANIZE_IMPORTS}, {'O' | KM_ALT | KM_SHIFT, CMD_ORGANIZE_IMPORTS},
      {'H' | KM_ALT, CMD_CALL_HIERARCHY}, {'h' | KM_ALT | KM_SHIFT, CMD_CALL_HIERARCHY}, {'H' | KM_ALT | KM_SHIFT, CMD_CALL_HIERARCHY},
      {'I' | KM_ALT, CMD_CURSORS_LINE_ENDS}, {'i' | KM_ALT | KM_SHIFT, CMD_CURSORS_LINE_ENDS}, {'I' | KM_ALT | KM_SHIFT, CMD_CURSORS_LINE_ENDS},
      {CTRL('u'), CMD_CURSOR_UNDO}, {'u' | KM_CTRL, CMD_CURSOR_UNDO}, {K_F2 | KM_CTRL, CMD_CHANGE_ALL},
      {'m' | KM_CTRL, CMD_TAB_FOCUS}
    };
    size_t i;
    for (i = 0; i < sizeof(ek) / sizeof(ek[0]); i++)
      if (k == ek[i].key) {
        run_command(ek[i].cmd);
        return;
      }
  }
  if (k == CTRL('d')) {
    if (!T->sel) select_word();
    else add_next_match(0);
    return;
  }
  if (k == ('l' | KM_CTRL | KM_SHIFT)) {
    add_next_match(1);
    return;
  }
  if ((k & (KM_CTRL | KM_SHIFT | KM_ALT)) == (KM_CTRL | KM_SHIFT | KM_ALT) &&
      (code == K_UP || code == K_DOWN || code == K_LEFT || code == K_RIGHT || code == K_PGUP || code == K_PGDN)) {
    column_key(code);	/* a column selection */
    return;
  }
  if ((code == K_UP || code == K_DOWN) && (k & KM_CTRL) && (k & KM_ALT)) {
    add_cursor_line(code == K_DOWN);
    return;
  }
  if (T->nmc > 0) {
    if (code == K_ESC && !E.find_open) {	/* back to one cursor */
      T->nmc = 0;
      return;
    }
    if (code == K_PASTE) {
      Buf b;
      buf_init(&b);
      term_paste(&b);
      multi_paste(b.s ? b.s : "", b.len);
      buf_free(&b);
      return;
    }
    if (k == CTRL('v')) {
      if (E.clip) multi_paste(E.clip, E.cliplen);
      return;
    }
    if (k == CTRL('c') || k == CTRL('x')) {	/* every selection, a line each */
      multi_copy(k == CTRL('x'));
      return;
    }
    if (multi_key(k)) {
      each_cursor(k);
      return;
    }
    if (k == CTRL('z') || k == CTRL('y') || k == CTRL('a')) T->nmc = 0;
  }
  key_one(k);
}


/* with the terminal in front most keys are the shell's; these stay VS Code's */
static int panel_passes (int k) {
  int code = KEY_CODE(k);
  if (code == K_F1 || code == K_F10) return 1;
  if ((k & (KM_CTRL | KM_SHIFT)) == (KM_CTRL | KM_SHIFT) && code < 128) return 1;
  if (k == ('1' | KM_ALT) || k == ('2' | KM_ALT) || k == ('3' | KM_ALT)) return 1;
  if (code == K_F5 || code == K_F9 || ((code == K_F6 || code == K_F11) && dbg_active())) return 1;	/* debugging */
  return (code == K_PGUP || code == K_PGDN) && (k & KM_CTRL);
}


/* a when clause's context key, as VS Code names it: its value, NULL for false */
const char *when_ctx (const char *key) {
  static char buf[512];
  int ed = E.focus == F_EDITOR && HAS_DOC && !G->diff && !T->page && !T->md;
#define B(x)	((x) ? "true" : NULL)
  if (strncmp(key, "config.", 7) == 0) {	/* a setting */
    const Json *v = settings_value(key + 7);
    if (v == NULL) return NULL;
    if (v->type == J_BOOL) return B(v->b);
    if (v->type == J_STR) return v->str;
    if (v->type == J_NUM) {
      snprintf(buf, sizeof(buf), "%g", v->num);
      return buf;
    }
    return "true";
  }
  if (strcmp(key, "editorTextFocus") == 0 || strcmp(key, "editorFocus") == 0) return B(ed);
  if (strcmp(key, "textInputFocus") == 0) return B(ed || E.finding || (E.focus == F_SIDE && E.view == VIEW_SEARCH));
  if (strcmp(key, "inputFocus") == 0) return B(ed || E.finding || (E.focus == F_SIDE && E.view == VIEW_SEARCH));
  if (strcmp(key, "editorIsOpen") == 0) return B(HAS_DOC);
  if (strcmp(key, "editorHasSelection") == 0) return B(HAS_DOC && T->sel);
  if (strcmp(key, "editorHasMultipleSelections") == 0) return B(HAS_DOC && T->nmc > 0);
  if (strcmp(key, "editorReadonly") == 0) return B(HAS_DOC && (G->diff || T->page || T->md));
  if (strcmp(key, "editorLangId") == 0 || strcmp(key, "resourceLangId") == 0) return HAS_DOC ? doc_lang_id() : NULL;
  if (strcmp(key, "resourceExtname") == 0 || strcmp(key, "resourceFilename") == 0 ||
      strcmp(key, "resource") == 0 || strcmp(key, "resourcePath") == 0) {
    const char *f = HAS_DOC && T->doc->path ? T->doc->path : NULL, *dot;
    if (f == NULL) return NULL;
    if (key[8] == 'E') return (dot = strrchr(path_basename(f), '.')) != NULL ? dot : NULL;
    if (key[8] == 'F') return path_basename(f);
    return f;
  }
  if (strcmp(key, "isInDiffEditor") == 0) return B(G->diff);
  if (strcmp(key, "suggestWidgetVisible") == 0) return B(CP.open);
  if (strcmp(key, "findWidgetVisible") == 0) return B(E.find_open);
  if (strcmp(key, "findInputFocussed") == 0) return B(E.finding && !E.in_repl);
  if (strcmp(key, "replaceInputFocussed") == 0) return B(E.finding && E.in_repl);
  if (strcmp(key, "inSnippetMode") == 0) return B(SN.t != NULL);
  if (strcmp(key, "hasNextTabstop") == 0) return B(SN.t != NULL && SN.at + 1 < SN.norder);
  if (strcmp(key, "hasPrevTabstop") == 0) return B(SN.t != NULL && SN.at > 0);
  if (strcmp(key, "inQuickOpen") == 0) return NULL;	/* the quick input takes its keys itself */
  if (strcmp(key, "inDebugMode") == 0) return B(dbg_active());
  if (strcmp(key, "terminalFocus") == 0) return B(E.focus == F_PANEL && E.panel && E.panel_view == 0);
  if (strcmp(key, "terminalIsOpen") == 0) return B(panel_count() > 0);
  if (strcmp(key, "panelFocus") == 0) return B(E.focus == F_PANEL && E.panel);
  if (strcmp(key, "panelVisible") == 0) return B(E.panel);
  if (strcmp(key, "sideBarFocus") == 0) return B(E.focus == F_SIDE && E.side);
  if (strcmp(key, "sideBarVisible") == 0) return B(E.side);
  if (strcmp(key, "explorerViewletFocus") == 0 || strcmp(key, "filesExplorerFocus") == 0 ||
      strcmp(key, "explorerViewletVisible") == 0)
    return B(E.side && E.view == VIEW_FILES && (key[8] == 'V' ? 1 : E.focus == F_SIDE));
  if (strcmp(key, "searchViewletFocus") == 0 || strcmp(key, "searchViewletVisible") == 0)
    return B(E.side && E.view == VIEW_SEARCH && (key[14] == 'V' ? 1 : E.focus == F_SIDE));
  if (strcmp(key, "scmViewletVisible") == 0) return B(E.side && E.view == VIEW_GIT);
  if (strcmp(key, "activeViewlet") == 0) {
    static const char *const v[VIEW_N] = {"workbench.view.explorer", "workbench.view.search",
                                          "workbench.view.scm", "workbench.view.debug", "workbench.view.extensions"};
    return E.side ? v[E.view] : NULL;
  }
  if (strcmp(key, "activePanel") == 0) {
    static const char *const v[] = {"terminal", "workbench.panel.markers", "workbench.panel.repl", "workbench.panel.output"};
    return E.panel && E.panel_view >= 0 && E.panel_view < 4 ? v[E.panel_view] : NULL;
  }
  if (strcmp(key, "multipleEditorGroups") == 0) return B(g_ngrp > 1);
  if (strcmp(key, "activeEditorGroupIndex") == 0) {
    snprintf(buf, sizeof(buf), "%d", g_gcur + 1);
    return buf;
  }
  if (strcmp(key, "workbenchState") == 0) return ws_count() > 1 ? "workspace" : "folder";
#ifdef _WIN32
  if (strcmp(key, "isWindows") == 0) return "true";
#elif defined(__APPLE__)
  if (strcmp(key, "isMac") == 0) return "true";
#else
  if (strcmp(key, "isLinux") == 0) return "true";
#endif
  return NULL;
#undef B
}


/* the command a keybinding found: its args (keys_args) go with it */
static void run_bound (int c) {
  run_command(c);
  keys_args_clear();
}


/* keybindings.json's keys, before mme's; 1 when k was one */
static int user_key (int k) {
  int c;
  if (E.focus == F_PANEL && E.panel && E.panel_view == 0 && !panel_passes(k)) return 0;	/* the shell's */
  if (E.uchord) {
    c = keys_find(E.uchord, k);
    E.uchord = 0;
    if (c > 0 || c == KEYS_BLOCK) {
      E.chord = 0;
      toast(0, "%s", "");
      if (c > 0) run_bound(c);
      return 1;
    }
    if (E.chord) return 0;	/* mme's Ctrl+K chord has it */
    toast(0, "The key combination is not a command.");
    return 1;
  }
  c = keys_find(0, k);
  if (c > 0) {
    run_bound(c);
    return 1;
  }
  if (c == KEYS_BLOCK) return !IS_TEXT(k);	/* "-command": mme's key does nothing (typing still types) */
  if (c == -1) {
    char name[48];
    E.uchord = k;
    if (k == CTRL('k') && E.focus != F_PANEL) E.chord = 1;	/* mme's chord goes on too */
    key_name(k, 1, name, sizeof(name));
    toast(0, "(%s) was pressed. Waiting for second key of chord...", name);
    return 1;
  }
  return 0;
}


static void on_key (int k) {
  int code = KEY_CODE(k);
  E.follow = 1;
  E.nav_quiet = IS_TEXT(k) || code == K_UP || code == K_DOWN || code == K_PGUP || code == K_PGDN ||
                code == K_ENTER || code == K_BS || code == K_DEL || code == K_PASTE || code == K_TAB;
  if (user_key(k)) goto done;
  /* Ctrl+` (NUL, or the kitty key) and Ctrl+J: VS Code's terminal and panel */
  if (k == ('`' | KM_CTRL | KM_SHIFT) || k == ('~' | KM_CTRL | KM_SHIFT)) {
    run_command(CMD_TERMINAL_NEW);
    goto done;
  }
  if (k == 0 || k == ('`' | KM_CTRL) || (k == CTRL('j') && !E.chord && !(E.focus == F_SIDE && E.view == VIEW_GIT))) {
    run_command(CMD_TERMINAL);
    goto done;
  }
  if (E.focus == F_PANEL && E.panel && E.panel_view == 2) {	/* the debug console */
    if (KEY_CODE(k) == K_PASTE) {
      Buf b;
      buf_init(&b);
      term_paste(&b);
      console_paste(b.s ? b.s : "", b.len);
      buf_free(&b);
    }
    else if (!global_key(k) && !console_key(k) && KEY_CODE(k) == K_ESC) E.focus = F_EDITOR;
    E.follow = 0;
    goto done;
  }
  if (E.focus == F_PANEL && E.panel && E.panel_view == 1) {	/* the problems' list, its filter */
    if (PB.in_filter || k == CTRL('f') || k == CTRL('c') || (IS_TEXT(k) && k != ' ')) problems_key(k);
    else if (!global_key(k)) problems_key(k);
    goto done;
  }
  if (E.focus == F_PANEL && E.panel && E.panel_view == 3) {	/* the output */
    if (KEY_CODE(k) == K_ESC) E.focus = F_EDITOR;
    else if (!global_key(k)) out_key(k);
    goto done;
  }
  if (E.focus == F_PANEL && E.panel && panel_finding() && !panel_passes(k) && panel_find_key(k)) goto done;
  if (E.focus == F_PANEL && E.panel && k == CTRL('f')) {	/* Find, over the terminal */
    panel_find_open();
    goto done;
  }
  if (E.focus == F_PANEL && E.panel && (k == (K_LEFT | KM_ALT) || k == (K_RIGHT | KM_ALT)) && panel_group_size() > 1) {
    panel_focus_pane(k == (K_LEFT | KM_ALT) ? -1 : 1);	/* the split next to it */
    goto done;
  }
  if (E.focus == F_PANEL && E.panel && E.panel_view == 0 && panel_key_cmd(k)) {	/* copy, paste, Ctrl+Up ... */
    E.follow = 0;
    goto done;
  }
  if (E.focus == F_PANEL && E.panel) {
    if (panel_passes(k)) global_key(k);
    else if (KEY_CODE(k) == K_PASTE) {
      Buf b;
      buf_init(&b);
      term_paste(&b);
      panel_paste(b.s ? b.s : "", b.len);
      buf_free(&b);
    }
    else panel_key(k);
    E.follow = 0;
    goto done;
  }
  if (global_key(k)) goto done;
  if (E.focus == F_SIDE && E.side && E.view == VIEW_FILES && E.outline_focus) {	/* a pane under the folders */
    if (E.outline_focus == PANE_OUTLINE && OL.h > 0) outline_key(k);
    else if (E.outline_focus == PANE_EDITORS && OE.h > 0) oe_key(k);
    else if (E.outline_focus == PANE_TIMELINE && TL.h > 0) tl_key(k);
    else E.outline_focus = PANE_TREE;
    E.follow = 0;
    goto done;
  }
  if (E.focus == F_SIDE && E.side) {
    SideAct act;
    memset(&act, 0, sizeof(act));
    if (KEY_CODE(k) == K_TAB && E.view == VIEW_FILES && (OL.h > 0 || OE.h > 0 || TL.h > 0)) next_pane();	/* to the panes */
    else if (side_key(E.view, k, &act)) apply_act(&act);
    else if (KEY_CODE(k) == K_ESC || KEY_CODE(k) == K_TAB) E.focus = F_EDITOR;
    E.follow = 0;
    goto done;
  }
  if (G->diff && HAS_DIFF) {
    switch (diff_key(k)) {
      case DIFF_CLOSE: run_command(CMD_CLOSE); break;
      case DIFF_EDIT: diff_edit_file(); break;
    }
    E.follow = 0;
    goto done;
  }
  if (E.finding) {
    find_key(k);
    goto done;
  }
  editor_key(k);
  if (HAS_DOC && T->preview && doc_dirty(T->doc)) T->preview = 0;	/* edited: it stays */
done:
  fold_reveal();
  if (E.follow && HAS_DOC && !G->diff && !T->md) scroll_to_cursor();
}

/* }================================================================== */


/*
** {==================================================================
** Mouse
** ===================================================================
*/

/* where the mouse is in the text */
static Pos mouse_pos (Mouse *m) {
  size_t y, col;
  Pos p;
  mouse_line_col(m, &y, &col);
  p.y = y;
  p.x = x_of_col(row_at(y), col);
  return doc_clamp(T->doc, p);
}


/* the word at p (a..b); a == b when there is none */
static void word_at (Pos p, Pos *a, Pos *b) {
  const Row *r = row_at(p.y);
  *a = *b = p;
  while (a->x > 0 && char_class(r, a->x - 1) == 1) a->x--;
  while (b->x < r->len && char_class(r, b->x) == 1) b->x++;
}


/* a double click dragged: by words; a triple one: by lines */
static void unit_drag (Mouse *m) {
  Pos p = mouse_pos(m), a, b;
  if (E.drag_unit == 2) word_at(p, &a, &b);
  else {
    a.y = b.y = p.y;
    a.x = 0;
    if (p.y + 1 < T->doc->n) {
      b.y = p.y + 1;
      b.x = 0;
    }
    else b.x = row_at(p.y)->len;
  }
  if (pos_cmp(a, E.drag_a) < 0) {
    T->anchor = E.drag_b;
    T->cur = a;
  }
  else {
    T->anchor = E.drag_a;
    T->cur = pos_cmp(b, E.drag_b) > 0 ? b : E.drag_b;
  }
  T->sel = pos_cmp(T->anchor, T->cur) != 0;
  T->want = col_of(row_at(T->cur.y), T->cur.x);
}


/* the selection dropped at p: moved there (copied with Ctrl), like editor.dragAndDrop */
static void drop_text (Pos p, int copy) {
  Pos a, b;
  char *t;
  size_t len;
  sel_range(&a, &b);
  if (pos_cmp(p, a) >= 0 && pos_cmp(p, b) <= 0) return;	/* on itself */
  t = doc_text(T->doc, a, b, &len);
  doc_group(T->doc);
  if (!copy) {
    if (pos_cmp(p, b) > 0) {	/* after it: the place moves back as it goes */
      if (p.y == b.y) {
        p.x -= b.x - a.x;
        p.y = a.y;
        if (b.y != a.y) p.x += a.x;
      }
      else p.y -= b.y - a.y;
      if (p.y == a.y && b.y != a.y) p.x = a.x + (p.x - a.x);
    }
    ed_delete(a, b);
  }
  T->anchor = p;
  T->cur = ed_insert(p, t, len);
  T->sel = 1;
  doc_group(T->doc);
  free(t);
}


static void text_click (Mouse *m, int extend) {
  int gw = gutter_width();
  long cx = m->x - L.ed_x - gw;
  Pos p;
  long ry = m->y - L.text_y;
  if (ry < 0) {	/* dragged above the text: scroll up */
    if (T->top > 0) T->top--;
    ry = 0;
  }
  if (ry >= L.text_h) {
    if (T->top + (size_t)L.text_h < T->doc->n) T->top++;
    ry = L.text_h - 1;
  }
  {
    size_t from, to, left;
    if (!vis_goto((int)ry, &p.y, &from, &to, &left)) {
      move_h(doc_end(T->doc), extend);
      return;
    }
    p.x = x_of_vcol(p.y, cx < 0 ? left : left + (size_t)cx);
    if (p.x < from) p.x = from;
    if (p.x >= to && to < row_at(p.y)->len) p.x = prev_x(row_at(p.y), to);	/* its row, not the next */
  }
  move_h(p, extend);
}


static void find_click (Mouse *m) {
  if (m->y == g_fw.y + 1 && E.replacing) {	/* the replace row */
    if (m->x == g_fw.one_x) replace_one();
    else if (m->x == g_fw.all_x) replace_all();
    else {
      E.finding = 1;
      E.in_repl = 1;
    }
    E.focus = F_EDITOR;
    return;
  }
  E.in_repl = 0;
  if (m->x == g_fw.chev_x) E.replacing = !E.replacing;
  else if (m->x == g_fw.close_x) E.find_open = E.finding = E.replacing = 0;
  else if (m->x == g_fw.up_x) find_next(1);
  else if (m->x == g_fw.down_x) find_next(0);
  else if (m->x == g_fw.case_x) E.find_case = !E.find_case;
  else if (m->x == g_fw.word_x) E.find_word = !E.find_word;
  else if (m->x == g_fw.re_x) E.find_regex = !E.find_regex;
  else if (m->x == g_fw.sel_x) find_in_selection();
  else E.finding = 1;
  E.focus = F_EDITOR;
}


/*
** The pointer's shape where the mouse is: an I beam over text a click puts
** the cursor in, a hand over what a click does something with (the menus,
** the tabs, the side bar, the panel's tabs, the status bar, the pages).
** Terminals that do not know OSC 22 keep their own pointer.
*/
static int pointer_at (const Mouse *m) {
  int in_editor = m->x >= L.area_x && m->y >= L.text_y && m->y < L.text_y + L.text_h &&
                  !(L.panel_h > 0 && m->y >= L.panel_y);
  if (L.panel_h > 0 && m->y > L.panel_y && E.panel_view == 0) return PTR_TEXT;	/* the terminal's text */
  if (in_editor && HAS_DOC && !T->page && !G->diff) {
    int gw = gutter_width();
    if (m->x >= L.ed_x + gw && m->x < L.ed_x + gw + text_cols()) return E.link_y ? PTR_POINTER : PTR_TEXT;
    return PTR_POINTER;	/* the gutter: folding, breakpoints, the changes' bars */
  }
  return PTR_POINTER;
}


static void on_mouse (void) {
  Mouse *m = &term_mouse;
  int press = m->button == 0 && m->press && !m->drag;
  E.follow = 0;
  scr_pointer(pointer_at(m));
  g_mm_hover = HAS_DOC && mm_width() > 0 && m->x >= L.mm_x && m->x < L.mm_x + mm_width() &&
               m->y >= L.text_y && m->y < L.text_y + L.text_h;	/* editor.minimap.showSlider "mouseover" */
  panel_hover(m->x, m->y);	/* a link under it is underlined */
  if (panel_dragging()) {	/* a selection in the terminal: the mouse is its until the button comes up */
    PanelLink lk;
    panel_mouse(m, 1, &lk);
    return;
  }
  if (m->button == 3 && m->drag && !m->wheel) {	/* it moved, no button down: hover later */
    E.link_y = 0;
    if ((m->mods & (eopt.mc_ctrl ? KM_ALT : KM_CTRL)) && HAS_DOC && !G->diff && !T->page && lsp_active(T->doc) &&
        m->y >= L.text_y && m->y < L.text_y + L.text_h && m->x >= L.ed_x + gutter_width() &&
        m->x < L.ed_x + gutter_width() + text_cols()) {	/* Ctrl+hover: a link to the definition */
      Pos p = mouse_pos(m), a, b;
      word_at(p, &a, &b);
      if (a.x < b.x) {
        E.link_y = (int)p.y + 1;
        E.link_x0 = a.x;
        E.link_x1 = b.x;
      }
    }
    if (HV.text && HV.from_mouse && (m->y != HV.my || abs(m->x - HV.mx) > 3)) hover_close();
    HV.mx = m->x;
    HV.my = m->y;
    HV.mt = os_now_us();
    HV.waiting = 1;
    return;
  }
  if (press && HAS_DOC && !G->diff && LN.d == T->doc) {	/* a code lens: its command */
    int k;
    for (k = 0; k < LN.nhit; k++)
      if (m->y == LN.hy[k] && m->x >= LN.hx0[k] && m->x < LN.hx1[k] && LN.hidx[k] < LN.n) {
        Pos at;
        at.y = LN.v[LN.hidx[k]].y;
        at.x = 0;
        hover_close();
        move_h(doc_clamp(T->doc, at), 0);
        key_home(0);
        lsp_lens_run(LN.hidx[k]);
        return;
      }
  }
  if (press && HV.text && HV.qf_y >= 0 && m->y == HV.qf_y && m->x >= HV.qf_x0 && m->x < HV.qf_x1) {
    Pos at = HV.at;	/* the hover's Quick Fix...: the actions there */
    hover_close();
    move_h(doc_clamp(T->doc, at), 0);
    quickfix();
    return;
  }
  hover_close();
  if (((m->press && !m->drag) || m->wheel) && !E.tdrag && !E.grp_resize && m->x >= L.area_x &&
      m->x < L.area_x + L.area_w && m->y >= L.body_y && m->y < L.body_y + L.body_h - L.panel_h) {
    int g = group_at(m->x, m->y);	/* the group under the mouse comes to the front; a border drags */
    if (g >= 1000) {
      if (press) E.grp_resize = g;
      return;
    }
    if (g >= 0 && g != g_gcur) {
      focus_group(g);
      layout();
    }
  }
  if (HAS_DOC && !G->diff && peek_click(m)) return;
  if (HAS_DOC && !G->diff && (dirty_click(m) || conflict_click(m))) return;
  if (press && dbg_active()) {	/* the debug toolbar */
    int c = dbg_toolbar_hit(m->x, m->y);
    if (c > 0) run_command(c);
    if (c != 0) return;
  }
  if (m->button == 2 && m->press && !m->drag && HAS_DOC && !G->diff && !T->page && m->x == L.ed_x &&
      m->y >= L.text_y && m->y < L.text_y + L.text_h) {	/* right click in the glyph margin: the breakpoint's menu */
    size_t y, from, to, left;
    if (vis_goto(m->y - L.text_y, &y, &from, &to, &left)) {
      static const int add[] = {CMD_BREAKPOINT, CMD_BP_CONDITIONAL, CMD_BP_LOG, CMD_RUN_TO_CURSOR, -1};
      static const char *const add_l[] = {"Add Breakpoint", "Add Conditional Breakpoint...", "Add Logpoint...",
                                          "Run to Line"};
      static const int has[] = {CMD_BREAKPOINT, CMD_BP_EDIT, CMD_BP_TOGGLE_ENABLE, -1};
      int dm = dbg_mark(T->real, y), c;
      const char *has_l[3];
      Pos q;
      has_l[0] = "Remove Breakpoint";
      has_l[1] = (dm & DM_LOG) ? "Edit Logpoint..." : "Edit Breakpoint...";
      has_l[2] = (dm & DM_DISABLED) ? "Enable Breakpoint" : "Disable Breakpoint";
      q.y = y;
      q.x = 0;
      move_h(doc_clamp(T->doc, q), 0);
      c = (dm & DM_BP) ? menu_popup(m->x, m->y, has, has_l) : menu_popup(m->x, m->y, add, add_l);
      if (c > 0) run_command(c);
      return;
    }
  }
  if (press && HAS_DOC && !G->diff && m->x == L.ed_x && m->y >= L.text_y && m->y < L.text_y + L.text_h &&
      m->x >= L.area_x && !(lsp_active(T->doc) && m->y == L.text_y + vis_row(T->cur))) {	/* the glyph margin: a breakpoint */
    size_t y, from, to, left;
    if (vis_goto(m->y - L.text_y, &y, &from, &to, &left)) {
      if (T->real && test_mark(T->real, y) && !dbg_mark(T->real, y)) test_gutter(T->real, y);	/* ▷: the test runs */
      else dbg_toggle(T->real, y);
      return;
    }
  }
  if (press && HAS_DOC && lsp_active(T->doc) && m->x == L.ed_x && m->y == L.text_y + vis_row(T->cur)) {
    quickfix();	/* the lightbulb */
    return;
  }
  if (E.fdrag) {	/* the Explorer's rows dragged: into a folder of the tree, or into an editor group (opened) */
    int over_side = L.side_w > 0 && E.view == VIEW_FILES && (opt.side_right ? m->x > L.edge_x : m->x < L.edge_x);
    if (m->drag && m->button == 0) {
      if (E.fdrag == 1 && (abs(m->x - E.fdrag_x) >= 2 || m->y != E.fdrag_y)) E.fdrag = 2;
      if (E.fdrag == 2) files_drag_over(over_side ? m->y - L.body_y : -1);
    }
    else if (!m->press) {
      if (E.fdrag == 2 && over_side) {
        SideAct act;
        memset(&act, 0, sizeof(act));
        files_drop(m->y - L.body_y, (m->mods & KM_CTRL) != 0, &act);	/* Ctrl: copied */
        apply_act(&act);
      }
      else if (E.fdrag == 2 && m->x >= L.area_x && m->x < L.area_x + L.area_w && m->y >= L.body_y &&
               m->y < L.body_y + L.body_h - L.panel_h) {
        size_t i;
        int g = group_at(m->x, m->y);
        if (g >= 0 && g < 1000) focus_group(g);
        for (i = 0; i < files_drag_count(); i++) {
          OsStat st;
          const char *f = files_drag_path(i);
          if (os_stat(f, &st) == 0 && !st.is_dir && open_file(f, 0) == 0) E.focus = F_EDITOR;
        }
        files_drag_end();
      }
      else files_drag_end();
      E.fdrag = 0;
    }
    return;
  }
  if (E.grp_resize) {	/* a border between editor groups follows the mouse */
    if (m->drag) group_border(E.grp_resize, m->x, m->y);
    else if (!m->press) E.grp_resize = 0;
    return;
  }
  if (E.tdrag) {	/* a tab pressed: dragged to another place, group or side */
    if (m->drag && m->button == 0) {
      if (E.tdrag == 1 && (abs(m->x - E.tdrag_x) >= 2 || m->y != E.tdrag_y)) E.tdrag = 2;
      if (E.tdrag == 2) drop_target(m->x, m->y);
    }
    else if (!m->press) {
      if (E.tdrag == 2) drop_tab();
      E.tdrag = 0;
    }
    return;
  }
  if (E.resizing) {	/* the sidebar's edge follows the mouse */
    if (m->drag) E.side_w = opt.side_right ? L.act_x - m->x : m->x - L.act_w + 1;
    else if (!m->press) E.resizing = 0;
    if (E.side_w < 12) E.side_w = 12;
    return;
  }
  if (E.drag_col) {	/* the column selection follows the mouse */
    if (m->drag && HAS_DOC && !G->diff) column_mouse(m, 0);
    else if (!m->press) E.drag_col = 0;
    return;
  }
  if (E.drag_drop) {	/* the selection being dragged: where it would go, dropped at the release */
    if (m->drag && HAS_DOC && !G->diff) {
      E.drop = mouse_pos(m);
      E.drop_moved = 1;
    }
    else if (!m->press) {
      E.drag_drop = 0;
      if (HAS_DOC && !G->diff) {
        if (E.drop_moved) drop_text(E.drop, (m->mods & KM_CTRL) != 0);
        else {	/* only a click in the selection: the cursor there */
          T->nmc = 0;
          move_h(E.drop, 0);
        }
      }
      E.drop_moved = 0;
    }
    return;
  }
  if (E.drag_text) {
    if (m->drag && HAS_DOC && !G->diff) {
      if (E.drag_unit >= 2) unit_drag(m);
      else text_click(m, 1);
    }
    else if (!m->press) E.drag_text = 0;
    return;
  }
  if (E.resizing_panel) {	/* the panel's top follows the mouse */
    if (m->drag) E.panel_h = L.body_y + L.body_h - m->y;
    else if (!m->press) E.resizing_panel = 0;
    return;
  }
  if (E.drag_mm && !m->drag && !m->press) E.drag_mm = 0;
  if (E.drag_hsb) {	/* the same for the horizontal one */
    if (m->drag) hscrollbar_mouse(m);
    else if (!m->press) E.drag_hsb = 0;
    return;
  }
  if (L.hsb && press && m->y == L.text_y + L.text_h && m->x >= L.ed_x + gutter_width() &&
      m->x < L.ed_x + gutter_width() + text_cols()) {
    hscrollbar_mouse(m);
    return;
  }
  if (E.drag_sb) {	/* the thumb follows the mouse until the button comes up */
    if (m->drag) scrollbar_mouse(m);
    else if (!m->press) E.drag_sb = 0;
    return;
  }
  if (L.sb_w > 0 && press && m->x == L.ed_x + L.ed_w - 1 && m->y >= L.text_y &&
      m->y < L.text_y + L.text_h && m->x >= L.area_x) {
    scrollbar_mouse(m);
    return;
  }
  if (L.panel_h > 0 && m->x >= L.area_x && m->x < L.area_x + L.area_w && m->y >= L.panel_y &&
      m->y < L.body_y + L.body_h) {
    if (m->wheel && E.panel_view == 1) {
      size_t st = (size_t)wheel_step(m->mods);
      if (m->wheel < 0) PB.sel = PB.sel > st ? PB.sel - st : 0;
      else PB.sel = PB.sel + st < PB.n ? PB.sel + st : (PB.n ? PB.n - 1 : 0);
    }
    else if (m->wheel && E.panel_view == 2) console_wheel(m->wheel);
    else if (m->wheel && E.panel_view == 3) out_wheel(m->wheel);
    else if (m->wheel) panel_wheel(m->wheel);
    else if (E.panel_view == 0 && m->y > L.panel_y && (m->button != 3 || !m->drag)) {	/* a terminal, the tabs list, a link */
      PanelLink lk;
      int r = panel_mouse(m, E.focus == F_PANEL, &lk);
      if (m->press && !m->drag) E.focus = F_PANEL;
      if (r == 2) open_link(&lk);
      else if (r == 3) {
        E.panel = E.panel_max = 0;
        E.focus = F_EDITOR;
      }
      else if (r >= 100) run_command(r - 100);	/* the right-click menu */
    }
    else if (press && E.panel_view == 1 && m->y > L.panel_y) problems_click(PB.top + (size_t)(m->y - L.panel_y - 1));	/* a problem: there */
    else if (press && m->y == L.panel_y) {	/* its title row: the icons, or drag it */
      if (m->x == g_pn.close_x) {
        E.panel = E.panel_max = 0;
        E.focus = F_EDITOR;
      }
      else if (m->x == g_pn.max_x) run_command(CMD_PANEL_MAX);
      else if (E.panel_view == 1 && problems_title_click(m->x)) ;	/* the filter box, the funnel, Collapse All */
      else if (m->x >= g_pn.prob_x0 && m->x < g_pn.prob_x1) {
        E.panel_view = 1;
        E.focus = F_PANEL;
      }
      else if (m->x >= g_pn.out_x0 && m->x < g_pn.out_x1) {
        E.panel_view = 3;
        E.focus = F_PANEL;
      }
      else if (m->x >= g_pn.dbg_x0 && m->x < g_pn.dbg_x1) {
        E.panel_view = 2;
        E.focus = F_PANEL;
      }
      else if (m->x == g_pn.clear_x) out_clear();
      else if (m->x >= g_pn.chan_x0 && m->x < g_pn.chan_x1) pick_channel();
      else if (m->x == g_pn.split_x) run_command(CMD_TERMINAL_SPLIT);
      else if (m->x == g_pn.prof_x) run_command(CMD_TERMINAL_NEW_PROFILE);
      else if (m->x >= g_pn.term_x0 && m->x < g_pn.term_x1) run_command(CMD_TERMINAL), E.panel = 1;
      else if (m->x == g_pn.kill_x) run_command(CMD_TERMINAL_KILL);
      else if (m->x == g_pn.add_x) run_command(CMD_TERMINAL_NEW);
      else {
        int k;
        for (k = 0; k < g_pn.ntab; k++)
          if (m->x >= g_pn.tab_x0[k] && m->x < g_pn.tab_x1[k]) break;
        if (k < g_pn.ntab) {
          panel_select(k);
          E.focus = F_PANEL;
        }
        else E.resizing_panel = 1;
      }
    }
    else if (press) E.focus = F_PANEL;
    return;
  }
  if (mm_width() > 0 && (E.drag_mm || (m->x >= L.mm_x && m->x < L.mm_x + mm_width() && m->y >= L.text_y &&
                                       m->y < L.text_y + L.text_h))) {
    if (m->wheel) {	/* over the minimap */
      size_t st = (size_t)wheel_step(m->mods);
      if (m->wheel < 0) T->top = T->top > st ? T->top - st : 0;
      else if (T->doc->n > (size_t)L.text_h) {
        T->top += st;
        if (T->top > T->doc->n - (size_t)L.text_h) T->top = T->doc->n - (size_t)L.text_h;
      }
    }
    else if (m->button == 0 && (m->press || m->drag)) minimap_mouse(m);
    return;
  }
  if (m->y == 0 && SHOW_MENU) {	/* the menu bar */
    if (press) {
      int menu = menubar_hit(m->x);
      if (menu >= 0) run_command(menu_run(menu));
    }
    return;
  }
  if (m->y == E.rows - 1 && SHOW_STATUS) {	/* the status bar */
    if (press && m->x >= g_sb.branch_x0 && m->x < g_sb.branch_x1) run_command(CMD_GIT_CHECKOUT);
    else if (press && m->x >= g_sb.sync_x0 && m->x < g_sb.sync_x1) run_command(CMD_GIT_SYNC);
    else if (press && m->x >= g_sb.pos_x0 && m->x < g_sb.pos_x1) run_command(CMD_GOTO);
    else if (press && m->x >= g_sb.prob_x0 && m->x < g_sb.prob_x1) run_command(CMD_PROBLEMS);
    else if (press && m->x >= g_sb.ind_x0 && m->x < g_sb.ind_x1) run_command(CMD_INDENTATION);
    else if (press && m->x >= g_sb.eol_x0 && m->x < g_sb.eol_x1) run_command(CMD_EOL);
    else if (press && m->x >= g_sb.enc_x0 && m->x < g_sb.enc_x1) run_command(CMD_ENCODING);
    else if (press && m->x >= g_sb.lang_x0 && m->x < g_sb.lang_x1) run_command(CMD_LANGUAGE);
    else if (press && m->x >= g_sb.bell_x - 1) run_command(CMD_NOTIFICATIONS);
    return;
  }
  if (L.act_w > 0 && m->x >= L.act_x && m->x < L.act_x + L.act_w && m->y >= L.body_y) {	/* the activity bar: a view, or hide the one shown */
    int v = act_hit(m->y - L.body_y);
    if (!press || v < 0) return;
    if (E.side && E.view == v) {
      E.side = 0;
      E.focus = F_EDITOR;
    }
    else show_view(v);
    return;
  }
  if (E.bar_drag) {	/* the side bar's scrollbar, held */
    if (m->button == 0 && !m->press && !m->drag) E.bar_drag = 0;
    else files_bar_to(m->y - files_bar_y(), files_bar_rows());
    return;
  }
  if (L.side_w > 0 && (opt.side_right ? m->x >= L.edge_x : m->x <= L.edge_x)) {	/* the sidebar */
    SideAct act;
    if (m->x == L.edge_x && press) {
      E.resizing = 1;
      return;
    }
    if (press && E.view == VIEW_FILES && files_bar_rows() > 0 && m->x == L.side_x + side_width() - 1 &&
        m->y >= files_bar_y() && m->y < files_bar_y() + files_bar_rows()) {	/* its scrollbar */
      E.bar_drag = 1;
      files_bar_to(m->y - files_bar_y(), files_bar_rows());
      return;
    }
    if (E.view == VIEW_FILES && OE.h > 0 && m->y >= OE.y0 && m->y < OE.y0 + OE.h) {	/* OPEN EDITORS */
      if (m->wheel) {	/* OPEN EDITORS */
        size_t st = (size_t)wheel_step(m->mods);
        OE.top = m->wheel < 0 ? (OE.top > st ? OE.top - st : 0) : OE.top + st;
      }
      else if (press) {
        E.focus = F_SIDE;
        oe_click(m->y - OE.y0, m->x - L.side_x);
      }
      return;
    }
    if (E.view == VIEW_FILES && TL.h > 0 && m->y >= TL.y0 && m->y < TL.y0 + TL.h) {	/* TIMELINE */
      if (m->wheel) {	/* TIMELINE */
        size_t st = (size_t)wheel_step(m->mods);
        TL.top = m->wheel < 0 ? (TL.top > st ? TL.top - st : 0) : TL.top + st;
      }
      else if (m->press && !m->drag && (m->button == 0 || m->button == 2)) {
        E.focus = F_SIDE;
        tl_click(m->y - TL.y0, m->x, m->button);
      }
      return;
    }
    if (m->wheel) {
      side_wheel(E.view, m->wheel);
      return;
    }
    if (m->button == 2 && m->press && !m->drag && E.view == VIEW_FILES && !(OL.h > 0 && m->y >= OL.y0)) {
      E.focus = F_SIDE;	/* the right button: the Explorer's menu */
      E.outline_focus = 0;
      memset(&act, 0, sizeof(act));
      files_menu(m->y - L.body_y, m->x, m->y + 1, &act);
      apply_act(&act);
      return;
    }
    if (!press) return;
    E.focus = F_SIDE;
    if (E.view == VIEW_FILES && OL.h > 0 && m->y >= OL.y0 && m->y < OL.y0 + OL.h) {	/* the Outline: there */
      int col = m->x - L.side_x, w = side_width();
      E.outline_focus = PANE_OUTLINE;
      if (m->y <= OL.y0 + 1 && (col <= 1 || !OL.open)) OL.open = !OL.open;	/* its chevron */
      else if (!OL.open) ;
      else if (m->y == OL.y0 + 1 && col >= w - 4) outline_menu();
      else if (m->y == OL.y0 + 1 && col >= w - 6) outline_collapse_all();
      else if (m->y >= OL.y0 + 2) {
        size_t n, k = OL.top + (size_t)(m->y - OL.y0 - 2);
        const Sym *v = outline_rows(&n);
        if (k < OL.nvis) {
          size_t i = OL.vis[k];
          OL.sel = k;
          if (sym_end(v, n, i) > i + 1 && col <= 2 + v[i].depth * 2) ol_close(&v[i], !ol_closed(&v[i]));	/* the chevron */
          else outline_go(1);
        }
      }
      return;
    }
    E.outline_focus = 0;
    memset(&act, 0, sizeof(act));
    if (E.view == VIEW_FILES) files_mods(m->mods);	/* Ctrl / Shift: more rows selected, Alt: to the side */
    side_click(E.view, m->y - L.body_y, m->x - L.side_x, &act);
    if (E.view == VIEW_FILES) {
      files_mods(0);
      if (!(m->mods & (KM_CTRL | KM_SHIFT)) && files_drag_start(m->y - L.body_y)) {	/* it may be dragged */
        E.fdrag = 1;
        E.fdrag_x = m->x;
        E.fdrag_y = m->y;
      }
    }
    apply_act(&act);
    return;
  }
  if (m->x >= L.area_x && m->x < L.area_x + L.area_w && (m->x < L.ed_x - L.mml_w || m->x >= L.ed_x + L.ed_w ||
      m->y < L.ed_y || m->y >= L.ed_y + L.ed_h)) return;	/* not a group: a border, the centered layout's sides */
  if (press && m->y == L.ed_y + 1 && HAS_DOC && !G->diff && !T->page) {	/* the breadcrumbs */
    int i;
    for (i = 0; i < CB.n; i++)
      if (m->x >= CB.x0[i] && m->x < CB.x1[i]) {
        crumb_open(i);
        return;
      }
    return;
  }
  if (m->y == L.ed_y) {	/* the tabs */
    draw_tabs_full();	/* where this group's tabs are */
    if (m->wheel) {	/* the tabs scroll, like VS Code's */
      int n = G->ntab + (HAS_DIFF ? 1 : 0);
      G->first += m->wheel;
      if (G->first < 0) G->first = 0;
      if (G->first >= n) G->first = n ? n - 1 : 0;
      return;
    }
    if (!m->press || m->drag || m->button > 2) return;
    if (G->diff && HAS_DIFF && m->button == 0) {	/* the diff's icons */
      int r = diff_title_click(m->x);
      if (r == DIFF_EDIT) diff_edit_file();
      if (r != DIFF_NO) {
        E.focus = F_EDITOR;
        return;
      }
    }
    {
      int i;
      for (i = 0; i < g_tabs.n; i++) {
        if (g_tabs.x0[i] < 0 || m->x < g_tabs.x0[i] || m->x >= g_tabs.x1[i]) continue;
        if (m->button == 2) {	/* its menu */
          if (i < G->ntab) tab_menu(i, m->x);
        }
        else if (m->x == g_tabs.close[i] && i < G->ntab && G->tab[i]->pinned && !doc_dirty(G->tab[i]->doc)) {
          focus_tab(i);	/* the pin: unpinned */
          pin_tab(0);
        }
        else if (m->x == g_tabs.close[i] || m->button == 1) {	/* x, or the middle button */
          if (i < G->ntab) close_tab(i);
          else run_command(CMD_CLOSE);
        }
        else if (i < G->ntab) {
          focus_tab(i);
          if (m->button == 0) {	/* it may be dragged: another place, group or side */
            E.tdrag = 1;
            E.tdrag_g = g_gcur;
            E.tdrag_i = i;
            E.tdrag_x = m->x;
            E.tdrag_y = m->y;
            E.tdrop_zone = DROP_NONE;
          }
        }
        else G->diff = 1;
        break;
      }
    }
    E.focus = F_EDITOR;
    return;
  }
  if (G->diff && HAS_DIFF) {
    if (m->wheel) diff_wheel(m->wheel);
    else if (press || (m->button == 0 && m->drag)) {
      if (press) E.focus = F_EDITOR;
      if (diff_click(m->x, m->y) == DIFF_REVERT && press) run_command(CMD_REVERT_RANGES);	/* its arrow: Revert Block */
    }
    return;
  }
  if (!HAS_DOC) {
    if (press) E.focus = F_EDITOR;
    return;
  }
  if (T->page) {
    if (press || m->wheel || (m->button == 2 && m->press && !m->drag)) {
      if (!m->wheel) E.focus = F_EDITOR;
      page_mouse(m);
    }
    return;
  }
  if (T->md) {	/* the Markdown preview */
    size_t step = (size_t)wheel_step(m->mods);
    if (m->wheel < 0) T->top = T->top > step ? T->top - step : 0;
    else if (m->wheel > 0) T->top += step;
    else if (press) E.focus = F_EDITOR;
    return;
  }
  if (m->wheel && (m->mods & KM_SHIFT) && !E.wrap) {	/* Shift+wheel: to the side, like VS Code */
    size_t width = doc_width() + 1, tc = (size_t)text_cols(), step = (size_t)(2 * wheel_step(0));
    if (m->wheel < 0) T->left = T->left > step ? T->left - step : 0;
    else if (width > tc) {
      T->left += step;
      if (T->left > width - tc) T->left = width - tc;
    }
    return;
  }
  if (m->wheel) {
    size_t th = (size_t)L.text_h, step = (size_t)wheel_step(m->mods);
    if (m->wheel < 0) T->top = T->top > step ? T->top - step : 0;
    else {	/* editor.scrollBeyondLastLine: until the last line is on top */
      size_t lim = eopt.beyond_last ? (T->doc->n ? T->doc->n - 1 : 0) : (T->doc->n > th ? T->doc->n - th : 0);
      if (T->top < lim) T->top = T->top + step < lim ? T->top + step : lim;
    }
    return;
  }
  if (E.find_open && m->y >= g_fw.y && m->y < g_fw.y + g_fw.h && m->x >= g_fw.x && m->x < g_fw.x + g_fw.w) {
    if (press) find_click(m);
    return;
  }
  if (press && m->y >= L.text_y && m->y < L.text_y + L.text_h && HAS_DOC && !G->diff && !T->page &&
      m->x == L.ed_x + gutter_width() - 1) {	/* a change's bar: its dirty diff peek */
    size_t y, from, to, left, n;
    const QHunk *h = qhunks(&n);
    long i;
    if (h && vis_goto(m->y - L.text_y, &y, &from, &to, &left) && (i = hunk_at(h, n, y)) >= 0) {
      if (DP.open && DP.hi == (size_t)i) DP.open = 0;
      else dirty_show(i);
      return;
    }
  }
  if (press && m->y >= L.text_y && m->y < L.text_y + L.text_h && gutter_width() >= 4 &&
      m->x == L.ed_x + gutter_width() - 2) {	/* a fold's chevron */
    size_t y, from, to, left;
    if (vis_goto(m->y - L.text_y, &y, &from, &to, &left) && from == 0) {
      if (is_folded(y)) fold_del(y);
      else if (fold_end(y) > y) {
        fold_add(y);
        if (hidden_in(T->cur.y) != (size_t)-1) {
          T->cur.y = y;
          T->cur = doc_clamp(T->doc, T->cur);
        }
      }
      E.focus = F_EDITOR;
      return;
    }
  }
  if (press && m->y >= L.text_y) {
    E.focus = F_EDITOR;
    E.finding = 0;
    if ((m->mods & (eopt.mc_ctrl ? KM_ALT : KM_CTRL)) && !(m->mods & KM_SHIFT) && lsp_active(T->doc)) {	/* Ctrl+Click: the definition */
      text_click(m, 0);
      lsp_define(T->doc, T->cur);
      return;
    }
    if (sticky_click(m)) return;
    if ((m->mods & (KM_SHIFT | KM_ALT)) == (KM_SHIFT | KM_ALT)) {	/* Shift+Alt+drag: a column */
      column_mouse(m, 1);
      E.drag_col = 1;
      return;
    }
    if (m->mods & (eopt.mc_ctrl ? KM_CTRL : KM_ALT)) {	/* Alt+Click (editor.multiCursorModifier): one more cursor */
      mc_push();
      text_click(m, 0);
      mc_merge();
      return;
    }
    {	/* 2 or 3 clicks in the same place: a word, a line */
      static long long last;
      static int lx, ly, count;
      long long now = os_now_us();
      count = (now - last < 500000 && m->x == lx && m->y == ly) ? (count % 3) + 1 : 1;
      last = now;
      lx = m->x;
      ly = m->y;
      E.drag_unit = (m->mods & KM_SHIFT) ? 1 : count;
    }
    if (E.drag_unit == 1 && !(m->mods & KM_SHIFT) && T->sel && T->nmc == 0 && opt.drag_drop) {
      Pos p = mouse_pos(m), a, b;
      sel_range(&a, &b);
      if (pos_cmp(p, a) >= 0 && pos_cmp(p, b) < 0) {	/* in the selection: maybe a drag */
        E.drag_drop = 1;
        E.drop_moved = 0;
        E.drop = p;
        return;
      }
    }
    T->nmc = 0;
    E.drag_text = 1;
    text_click(m, (m->mods & KM_SHIFT) != 0);
    if (E.drag_unit >= 2) {
      Pos a, b;
      if (E.drag_unit == 2) {
        word_at(T->cur, &a, &b);
        if (a.x == b.x) b.x = a.x < row_at(a.y)->len ? next_x(row_at(a.y), a.x) : a.x;
      }
      else {
        a.y = b.y = T->cur.y;
        a.x = 0;
        if (b.y + 1 < T->doc->n) {
          b.y++;
          b.x = 0;
        }
        else b.x = row_at(b.y)->len;
      }
      E.drag_a = a;
      E.drag_b = b;
      T->anchor = a;
      T->cur = b;
      T->sel = pos_cmp(a, b) != 0;
      T->want = col_of(row_at(b.y), b.x);
    }
  }
  else if (m->button == 1 && m->press && !m->drag && m->y >= L.text_y && m->y < L.text_y + L.text_h &&
           m->x >= L.ed_x + gutter_width()) {	/* the middle button dragged: a column */
    E.focus = F_EDITOR;
    column_mouse(m, 1);
    E.drag_col = 1;
  }
}

/* }================================================================== */


static void usage (void) {
  fd_puts(1, "usage: " MME_NAME " [folder | file[:line]]\n"
             "  VS Code for the terminal\n"
             "  mme .          open this folder\n"
             "  mme main.c:12  open a file at a line\n"
             "  -h, --help     this text\n"
             "  -v, --version  the version\n");
}


/* "file:12" opens file at line 12, unless a file has that very name */
static long split_line (char *arg) {
  char *c = strrchr(arg, ':');
  long long n;
  OsStat st;
  if (c == NULL || c[1] == '\0') return 0;
  if (os_stat(arg, &st) == 0 && st.exists) return 0;
  if (str_to_ll(c + 1, &n) != 0 || n <= 0) return 0;
  *c = '\0';
  return (long)n;
}


static void cleanup (void) {
  test_shutdown();
  dbg_shutdown();
  lsp_shutdown();
  panel_kill_all();
  term_close();
  os_shutdown();
}


int main (int argc, char **argv) {
  long line = 0;
  OsStat st;
  os_init();
  os_args(&argc, &argv);
  data_init(argv[0]);
  g_none.doc = &g_none_doc;
  doc_init(g_none.doc);
  E.side_w = 30;
  E.panel_h = 12;
  OL.open = 1;	/* the Outline starts open, OPEN EDITORS and TIMELINE folded, like VS Code */
  apply_settings(0);
  keys_load();
  E.view = VIEW_FILES;
  if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    usage();
    return 0;
  }
  if (argc > 1 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
    fd_puts(1, MME_NAME " " MME_VERSION "\n");
    return 0;
  }
  if (argc > 1) line = split_line(argv[1]);
  if (argc > 1 && is_workspace_file(argv[1])) {	/* mme x.code-workspace */
    if (ws_open(argv[1]) != 0) {
      fd_printf(2, MME_NAME ": %s is not a workspace\n", argv[1]);
      return 1;
    }
    recent_add(ws_file());
    E.side = opt.sidebar;
    E.focus = F_SIDE;
  }
  else if (argc > 1 && os_stat(argv[1], &st) == 0 && st.is_dir) {	/* mme . */
    side_open(argv[1]);
    recent_add(side_root());
    g_session = 1;
    E.side = opt.sidebar;
    E.focus = F_SIDE;
  }
  else if (argc > 1) {	/* mme file: its folder, the sidebar closed */
    char *real = os_realpath(argv[1]), *dir;
    dir = real ? path_dirname(real) : os_getcwd();
    side_open(dir);
    free(dir);
    free(real);
    tab_new();
    if (doc_load(T->doc, argv[1]) < 0) {
      fd_printf(2, MME_NAME ": cannot open %s\n", argv[1]);
      return 1;
    }
    T->sx = syntax_detect(argv[1], T->doc);
    set_real();
    if (T->sx) lsp_open(T->doc, syntax_name(T->sx));
    recent_file_add(argv[1]);
  }
  else {	/* mme: this folder */
    char *cwd = os_getcwd();
    side_open(cwd);
    free(cwd);
    g_session = 1;
    E.side = opt.sidebar;
    E.focus = F_SIDE;
  }
  apply_settings(0);	/* the folder's .vscode/settings.json, a workspace's settings */
  git_refresh();
  if (term_open() != 0) {
    fd_puts(2, MME_NAME ": not a terminal\n");
    return 1;
  }
  atexit(cleanup);
  ui_background = background;
  check_size();
  layout();
  if (g_session && opt.restore) session_restore();	/* window.restoreWindows: the folder's editors again */
  if (!HAS_DOC && opt.startup_welcome) {	/* workbench.startupEditor: the Welcome page */
    int f = E.focus;
    page_open(PAGE_WELCOME);
    E.focus = f;
  }
  check_size();
  layout();
  if (line > 0 && HAS_DOC) {
    Pos p;
    p.y = (size_t)line - 1;
    p.x = 0;
    move_h(doc_clamp(T->doc, p), 0);
    center_cursor();
  }
  while (!E.quit) {
    int k;
    lsp_poll();
    dbg_poll();
    nav_track();
    cursor_record();	/* Ctrl+U: the cursors as they were */
    mru_track();
    if (panel_poll() == 2 && E.panel_view == 0) {	/* the shell ended: its panel closes, like VS Code's */
      E.panel = 0;
      if (E.focus == F_PANEL) E.focus = F_EDITOR;
    }
    draw();
    k = term_key(search_busy() ? 1 : (panel_alive() || dbg_active() || (HAS_DOC && lsp_active(T->doc))) ? 20 : 100);	/* the size, the shell, the servers */
    quickfix_idle();
    if (k == K_NONE) {
      hover_idle();
      hl_idle();
      bulb_idle();
      extras_idle();
      side_idle(E.view);
      work_idle();
      continue;
    }
    if (k == K_MOUSE) on_mouse();
    else on_key(k);
    autosave_focus();
  }
  return 0;
}
