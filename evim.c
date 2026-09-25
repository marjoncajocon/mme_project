/*
** evim.c - Vim mode: the keys of VSCodeVim in the text editor
**
** With vim.enable on, the text editor has Vim's modes: Normal, Insert,
** Visual (by characters, lines and blocks) and Replace. Normal mode reads
** a command as it is typed - a register, a count, an operator and its
** motion or text object - and runs it when it is whole; the keys waiting
** show in the status bar, and so does the mode, "-- NORMAL --". Insert mode
** is the editor as it always is: the keys go on to mme.c, and are kept for
** "." and for a count. : and / ask on the status bar, as VSCodeVim does.
**
** The text is changed through ved_insert and ved_delete (mme.c), so the
** other cursors, the split views, folds and the language server all follow;
** a command is one step of undo, and so is everything typed in one visit to
** Insert mode. VS Code's keys with Ctrl stay VS Code's, but for the ones Vim
** is known by (Ctrl+D, Ctrl+U, Ctrl+V ...): vim.useCtrlKeys, vim.handleKeys.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { VM_NORMAL, VM_INSERT, VM_VISUAL, VM_VLINE, VM_VBLOCK, VM_REPLACE };
enum { RK_CHAR, RK_LINE, RK_BLOCK };	/* what a register holds */
enum { MK_EXCL, MK_INCL, MK_LINE };	/* a motion: exclusive, inclusive, by lines */
enum { CK_MOTION, CK_OBJECT, CK_LINES, CK_SEARCH, CK_ACTION };	/* what a command is */
enum { VP_NONE, VP_EX, VP_SEARCH };	/* the line typed on the status bar */
enum { RW_YANK, RW_SMALL, RW_BIG };	/* how a register is written: y, a delete in a line, a bigger one */

#define WANT_END	((size_t)1 << 30)	/* $: j and k go to the ends of the lines */
#define OP_G(c)		(0x100 | (c))	/* gu gU g~ g? gc */
#define IS_VISUAL	(V.mode == VM_VISUAL || V.mode == VM_VLINE || V.mode == VM_VBLOCK)
#define DOC		(V.v.doc)
#define NPEND		32

typedef struct Reg {
  char *s;
  size_t n;
  int kind;	/* RK_* */
} Reg;

typedef struct VCmd {	/* a command of Normal or Visual mode, as it was typed */
  int reg;	/* "x, 0: none named */
  long count;	/* the counts multiplied, 0: none typed */
  int op;	/* d c y < > = or OP_G(u U ~ ? c); 0: none */
  int kind;	/* CK_* */
  int key[3];	/* the motion, the text object or the command: "w", "iw", "gg", "x" */
  int nkey;
  int arg;	/* the character after f t r m ' ` q @ */
} VCmd;

typedef struct Mark {
  Doc *d;
  Pos p;
} Mark;

typedef struct Keys {	/* keys kept: a macro, what was typed in Insert mode */
  int *k;
  size_t n, cap;
} Keys;

typedef struct Mot {	/* where a motion went */
  Pos to;
  int kind;	/* MK_* */
  int jump;	/* G, n, %, ...: the place left is remembered (``) */
} Mot;

static struct {
  int enable;	/* vim.enable */
  int clip;	/* vim.useSystemClipboard */
  int ctrl;	/* vim.useCtrlKeys */
  signed char hk[27];	/* vim.handleKeys: Ctrl+a .. Ctrl+z, Ctrl+[; 1 Vim's, 0 VS Code's, -1 not said */
  int hls, incs, icase, scase, gdef;	/* vim.hlsearch, incsearch, ignorecase, smartcase, gdefault */
  int insert_first;	/* vim.startInInsertMode */
} VO;

static struct {
  VimEd v;	/* the editor, while a key is handled */
  int mode;	/* VM_* */
  Doc *doc;	/* the text the mode is for: another tab starts over */
  int pend[NPEND], np;	/* the keys of a command not whole yet */
  Pos vs, vc;	/* Visual mode: where it began, its cursor */
  size_t vs_col;	/* Visual block: the column it began at */
  int vdollar;	/* Visual block: $ took it to the ends of the lines */
  Pos seen_cur, seen_anchor;	/* how the editor was left: a click changes it */
  int seen_sel, seen;
  Reg reg[128];	/* by name: " 0-9 a-z - . : / */
  char *clip_s;	/* the text last put on the clipboard, and its kind */
  size_t clip_n;
  int clip_kind;
  Mark mark[26];	/* m{a-z} */
  Mark mk_lt, mk_gt, mk_jump, mk_change, mk_ins;	/* '< '> '' '. '^ */
  int last_vmode;	/* gv */
  Pos last_vs, last_vc;
  int f_key, f_ch;	/* the last f t F T, for ; and , */
  char pat[256];	/* the last search, as typed */
  int pat_back;	/* it went up (?) */
  int pat_nosmart;	/* * and #: vim.smartcase does not apply */
  int lit;	/* its matches are lit */
  int prompt;	/* VP_* */
  char line[256];	/* what is typed there */
  int p_kind;	/* ':' '/' '?' */
  Pos p_cur;	/* the cursor when it opened: a search starts there, Esc goes back */
  size_t p_top;
  VCmd p_cmd;	/* the operator a search is the motion of (d/foo) */
  int p_op, p_vis;
  char last_ex[256];	/* @: */
  char sub_pat[256], sub_rep[256];	/* the last :s */
  int sub_g;
  int ins_cmd;	/* what began Insert mode: i a o O R ... (Esc repeats it for a count) */
  long ins_count;
  Keys ins;	/* what was typed since */
  long ig;	/* the undo group everything typed joins */
  size_t iu;	/* the undo list's length when it began */
  int ins_dot;	/* Esc makes it the change "." repeats */
  int ctrl_r;	/* Ctrl+R in Insert mode: the next key names a register */
  int oneshot;	/* Ctrl+O in Insert mode: one command, then back */
  int blk;	/* Visual block's I A c: Esc puts the cursor at blk_home */
  Pos blk_home;
  Buf rbuf;	/* Replace mode: the characters typed over, for Backspace (\1: none) */
  VCmd dot;	/* the last change */
  int dot_set;
  Keys dot_ins;	/* and what was typed with it */
  int dot_vmode;	/* a change made in Visual mode: the mode, its size */
  long dot_vlines, dot_vcols;
  int dotting;	/* "." is replaying: nothing is kept */
  int rec;	/* q{a-z}: the macro being recorded, 0 none */
  Keys mac[26];
  int last_mac;
  int playing;	/* a macro (or :tabnext) is pressing keys: they are not recorded */
  char msg[200];	/* a message in the place of the mode, until the next key */
} V;


/*
** {==================================================================
** Settings
** ===================================================================
*/

void vim_settings (const Json *j) {
  const Json *hk = json_get(j, "vim\\.handleKeys");
  int was = VO.enable;
  size_t i;
  VO.enable = json_bool(json_get(j, "vim\\.enable"), 0);
  VO.clip = json_bool(json_get(j, "vim\\.useSystemClipboard"), 0);
  VO.ctrl = json_bool(json_get(j, "vim\\.useCtrlKeys"), 1);
  VO.hls = json_bool(json_get(j, "vim\\.hlsearch"), 0);
  VO.incs = json_bool(json_get(j, "vim\\.incsearch"), 1);
  VO.icase = json_bool(json_get(j, "vim\\.ignorecase"), 1);
  VO.scase = json_bool(json_get(j, "vim\\.smartcase"), 1);
  VO.gdef = json_bool(json_get(j, "vim\\.gdefault"), 0);
  VO.insert_first = json_bool(json_get(j, "vim\\.startInInsertMode"), 0);
  memset(VO.hk, -1, sizeof(VO.hk));
  VO.hk['s' - 'a'] = VO.hk['z' - 'a'] = 0;	/* VSCodeVim's defaults: Ctrl+S saves, Ctrl+Z undoes */
  for (i = 0; hk && hk->type == J_OBJ && i < hk->n; i++) {	/* {"<C-d>": true} */
    const char *k = hk->kid[i]->key;
    if (k && strlen(k) == 5 && k[0] == '<' && (k[1] == 'C' || k[1] == 'c') && k[2] == '-' && k[4] == '>') {
      int c = k[3] >= 'A' && k[3] <= 'Z' ? k[3] + 32 : k[3];
      if (c >= 'a' && c <= 'z') VO.hk[c - 'a'] = (signed char)json_bool(hk->kid[i], 1);
      else if (c == '[') VO.hk[26] = (signed char)json_bool(hk->kid[i], 1);
    }
  }
  if (VO.enable != was) {	/* on or off: it starts over */
    V.mode = VO.insert_first ? VM_INSERT : VM_NORMAL;
    V.np = 0;
    V.prompt = VP_NONE;
    V.lit = 0;
    V.doc = NULL;
  }
}


/* Vim: Toggle Vim Mode */
void vim_toggle (void) {
  VimEd v;
  VO.enable = !VO.enable;
  settings_put("vim.enable", VO.enable ? "true" : "false");
  V.mode = VO.insert_first ? VM_INSERT : VM_NORMAL;
  V.np = 0;
  V.prompt = VP_NONE;
  V.lit = 0;
  V.doc = NULL;
  if (!VO.enable && ved_get(&v)) {
    *v.sel = 0;
    if (v.nmc) ved_cursors(v.cur, v.cur, 1);
  }
}

/* }================================================================== */


/*
** {==================================================================
** The text
** ===================================================================
*/

static Pos pos (size_t y, size_t x) {
  Pos p;
  p.y = y;
  p.x = x;
  return p;
}


static const Row *row (size_t y) {
  return &DOC->row[y < DOC->n ? y : DOC->n - 1];
}


static size_t llen (size_t y) {
  return row(y)->len;
}


static size_t last_y (void) {
  return DOC->n - 1;
}


/* the byte at p, 0 at the end of its line */
static int ch (Pos p) {
  const Row *r = row(p.y);
  return p.x < r->len ? (unsigned char)r->s[p.x] : 0;
}


/* the character after (before) byte x of line y */
static size_t nextx (size_t y, size_t x) {
  const Row *r = row(y);
  if (x >= r->len) return r->len;
  x++;
  while (x < r->len && ((unsigned char)r->s[x] & 0xC0) == 0x80) x++;
  return x;
}


static size_t prevx (size_t y, size_t x) {
  const Row *r = row(y);
  if (x == 0) return 0;
  if (x > r->len) x = r->len;
  x--;
  while (x > 0 && ((unsigned char)r->s[x] & 0xC0) == 0x80) x--;
  return x;
}


/* the last character of a line, where Normal mode's cursor may go at most */
static size_t lastx (size_t y) {
  return prevx(y, llen(y));
}


static size_t first_nb (size_t y) {
  const Row *r = row(y);
  size_t x = 0;
  while (x < r->len && (r->s[x] == ' ' || r->s[x] == '\t')) x++;
  return x;
}


static int blank_line (size_t y) {
  return first_nb(y) == llen(y);
}


/* the character after p, where an inclusive range ends; the next line at the end of one */
static Pos after (Pos p) {
  if (p.x < llen(p.y)) p.x = nextx(p.y, p.x);
  else if (p.y < last_y()) p = pos(p.y + 1, 0);
  return p;
}


static Pos cur (void) {
  return *V.v.cur;
}


/* the cursor to p; the column j and k aim for with it */
static void go (Pos p) {
  if (p.y > last_y()) p.y = last_y();
  if (p.x > llen(p.y)) p.x = llen(p.y);
  *V.v.cur = p;
  *V.v.sel = 0;
  *V.v.want = ved_col(p.y, p.x);
}


static char *text (Pos a, Pos b, size_t *n) {
  return doc_text(DOC, a, b, n);
}


/* the lines y0 .. y1, each with its \n */
static char *lines_text (size_t y0, size_t y1, size_t *n) {
  Buf b;
  size_t y;
  buf_init(&b);
  for (y = y0; y <= y1 && y < DOC->n; y++) {
    buf_putn(&b, row(y)->s, row(y)->len);
    buf_putc(&b, '\n');
  }
  *n = b.len;
  if (b.s == NULL) return xstrdup("");
  return buf_take(&b);
}


/* the next change is a step of undo of its own: from here to Esc if it goes on in Insert mode */
static void grp (void) {
  doc_group(DOC);
  V.ig = DOC->group;
  V.iu = DOC->undo.n;
}


/* everything since grp is one step of undo */
static void undo_join (void) {
  size_t i;
  if (V.doc != DOC) return;
  for (i = V.iu < DOC->undo.n ? V.iu : DOC->undo.n; i < DOC->undo.n; i++)
    if (DOC->undo.v[i].group > V.ig) DOC->undo.v[i].group = V.ig;
  DOC->group = V.ig;
  doc_group(DOC);
}


static Pos ins (Pos at, const char *s, size_t n) {
  return ved_insert(at, s, n);
}


static void del (Pos a, Pos b) {
  if (pos_cmp(a, b) < 0) ved_delete(a, b);
}


static void kadd (Keys *k, int key) {
  if (k->n == k->cap) {
    k->cap = k->cap ? k->cap * 2 : 32;
    k->k = (int *)xrealloc(k->k, k->cap * sizeof(int));
  }
  k->k[k->n++] = key;
}


static void kcopy (Keys *to, const Keys *from) {
  size_t i;
  to->n = 0;
  for (i = 0; i < from->n; i++) kadd(to, from->k[i]);
}


static void say (const char *fmt, const char *arg) {
  snprintf(V.msg, sizeof(V.msg), fmt, arg ? arg : "");
}

/* }================================================================== */


/*
** {==================================================================
** Registers
** ===================================================================
*/

static int reg_ok (int r) {
  return (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') ||
         (r != 0 && strchr("\"-+*_.:/%", r) != NULL);
}


static void reg_put (int r, const char *s, size_t n, int kind) {
  Reg *g = &V.reg[r];
  free(g->s);
  g->s = (char *)xmalloc(n + 1);
  memcpy(g->s, s, n);
  g->s[n] = '\0';
  g->n = n;
  g->kind = kind;
}


static void clip_put (const char *s, size_t n, int kind) {
  clip_set(s, n);
  free(V.clip_s);
  V.clip_s = xstrndup(s, n);
  V.clip_n = n;
  V.clip_kind = kind;
}


/* a yank or a delete into register r (0: none named), as Vim fills them */
static void reg_set (int r, const char *s, size_t n, int kind, int how) {
  int i;
  if (r == '_') return;
  if (r >= 'A' && r <= 'Z') {	/* appended to */
    Reg *g = &V.reg[r + 32];
    Buf b;
    buf_init(&b);
    if (g->s) buf_putn(&b, g->s, g->n);
    if (kind == RK_LINE && b.len && b.s[b.len - 1] != '\n') buf_putc(&b, '\n');
    buf_putn(&b, s, n);
    reg_put(r + 32, b.s ? b.s : "", b.len, g->s && g->kind == RK_LINE ? RK_LINE : kind);
    buf_free(&b);
    r += 32;
    s = V.reg[r].s;
    n = V.reg[r].n;
    kind = V.reg[r].kind;
  }
  else if (r != 0 && r != '"') reg_put(r, s, n, kind);
  if (r == '+' || r == '*' || ((r == 0 || r == '"') && VO.clip)) clip_put(s, n, kind);
  if (r == 0 && how == RW_YANK) reg_put('0', s, n, kind);
  if (r == 0 && how == RW_BIG) {	/* "1 .. "9 move up */
    free(V.reg['9'].s);
    for (i = '9'; i > '1'; i--) V.reg[i] = V.reg[i - 1];
    memset(&V.reg['1'], 0, sizeof(Reg));
    reg_put('1', s, n, kind);
  }
  if (r == 0 && how == RW_SMALL) reg_put('-', s, n, kind);
  reg_put('"', s, n, kind);
}


/* what register r holds; the clipboard for "+ "* (and " with vim.useSystemClipboard) */
static const Reg *reg_get (int r) {
  static Reg tmp;
  if (r == 0) r = '"';
  if (r >= 'A' && r <= 'Z') r += 32;
  if (r == '+' || r == '*' || (r == '"' && VO.clip)) {
    size_t n;
    const char *s = clip_get(&n);
    if (s == NULL) return NULL;
    tmp.s = (char *)s;
    tmp.n = n;
    if (V.clip_s && V.clip_n == n && memcmp(V.clip_s, s, n) == 0) tmp.kind = V.clip_kind;	/* ours: its kind is known */
    else tmp.kind = n > 0 && s[n - 1] == '\n' ? RK_LINE : RK_CHAR;
    return &tmp;
  }
  if (r == '%') {	/* the file's name */
    tmp.s = (char *)(V.v.path ? V.v.path : "");
    tmp.n = strlen(tmp.s);
    tmp.kind = RK_CHAR;
    return &tmp;
  }
  if (r < 0 || r >= 128 || V.reg[r].s == NULL) return NULL;
  return &V.reg[r];
}

/* }================================================================== */


/*
** {==================================================================
** Marks
** ===================================================================
*/

static void mark_set (Mark *m, Pos p) {
  m->d = DOC;
  m->p = p;
}


static Mark *mark_of (int c) {
  if (c >= 'a' && c <= 'z') return &V.mark[c - 'a'];
  switch (c) {
    case '<': return &V.mk_lt;
    case '>': return &V.mk_gt;
    case '\'': case '`': return &V.mk_jump;
    case '.': return &V.mk_change;
    case '^': return &V.mk_ins;
  }
  return NULL;
}


/* where mark c is in this text; 0: not set */
static int mark_get (int c, Pos *p) {
  Mark *m = mark_of(c);
  if (m == NULL || m->d != DOC) return 0;
  *p = m->p;
  if (p->y > last_y()) p->y = last_y();
  if (p->x > llen(p->y)) p->x = llen(p->y);
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Words, the way Vim moves over them (search.c's fwd_word, bck_word ...)
** ===================================================================
*/

static int g_big;	/* W B E: a WORD is anything that is not blank */

/* 0 blank (and the end of a line), 1 punctuation, 2 a word's character */
static int cls (Pos p) {
  int c = ch(p);
  if (c == ' ' || c == '\t' || c == 0) return 0;
  if (g_big) return 1;
  if (c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z')) return 2;
  return 1;
}


/* one character on: 0 moved, 2 onto the end of the line, 1 to the next line, -1 at the end */
static int inc (Pos *p) {
  size_t n = llen(p->y);
  if (p->x < n) {
    p->x = nextx(p->y, p->x);
    return p->x < n ? 0 : 2;
  }
  if (p->y < last_y()) {
    p->y++;
    p->x = 0;
    return 1;
  }
  return -1;
}


/* one back: 0 moved, 1 to the end of the line before, -1 at the start */
static int dec (Pos *p) {
  if (p->x > 0) {
    p->x = prevx(p->y, p->x);
    return 0;
  }
  if (p->y > 0) {
    p->y--;
    p->x = llen(p->y);
    return 1;
  }
  return -1;
}


static int empty_at (Pos p) {
  return p.x == 0 && llen(p.y) == 0;
}


static int skip_cls (Pos *p, int c, int fwd) {
  while (cls(*p) == c)
    if ((fwd ? inc(p) : dec(p)) == -1) return 1;
  return 0;
}


/* w W; eol: an operator's, it stops at the end of the line of its last word */
static int fwd_word (Pos *p, long count, int eol) {
  while (--count >= 0) {
    int sc = cls(*p), last = p->y == last_y(), i = inc(p);
    if (i == -1 || (i >= 1 && last)) return 0;
    if (i >= 1 && eol && count == 0) return 1;
    if (sc != 0)
      while (cls(*p) == sc) {
        i = inc(p);
        if (i == -1 || (i >= 1 && eol && count == 0)) return 1;
      }
    while (cls(*p) == 0) {
      if (empty_at(*p)) break;
      i = inc(p);
      if (i == -1 || (i >= 1 && eol && count == 0)) return 1;
    }
  }
  return 1;
}


/* b B */
static int bck_word (Pos *p, long count) {
  while (--count >= 0) {
    if (dec(p) == -1) return 0;
    while (cls(*p) == 0) {
      if (empty_at(*p)) goto next;
      if (dec(p) == -1) return 1;
    }
    if (skip_cls(p, cls(*p), 0)) return 1;
    inc(p);
next:
    ;
  }
  return 1;
}


/* e E; stop: cw on the last character of a word changes that one */
static int end_word (Pos *p, long count, int stop) {
  while (--count >= 0) {
    int sc = cls(*p);
    if (inc(p) == -1) return 0;
    if (cls(*p) == sc && sc != 0) {
      if (skip_cls(p, sc, 1)) return 0;
    }
    else if (!stop || sc == 0) {
      while (cls(*p) == 0)
        if (inc(p) == -1) return 0;
      if (skip_cls(p, cls(*p), 1)) return 0;
    }
    dec(p);
    stop = 0;
  }
  return 1;
}


/* ge gE */
static int bckend_word (Pos *p, long count) {
  while (--count >= 0) {
    int sc = cls(*p);
    if (dec(p) == -1) return 0;
    if (sc != 0)
      while (cls(*p) == sc)
        if (dec(p) == -1) return 1;
    while (cls(*p) == 0) {
      if (empty_at(*p)) break;
      if (dec(p) == -1) return 1;
    }
  }
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Searching: / ? n N * #, with the regular expressions of eregex.c
** ===================================================================
*/

/* Vim's pattern (magic) as eregex.c's: \( \| \+ \< ... ; *icase from \c \C */
static void re_of_vim (const char *v, char *out, size_t cap, int *icase) {
  size_t n = 0;
  const char *lit = "(){}|+?";	/* plain in Vim, special in eregex.c */
  for (; *v && n + 3 < cap; v++) {
    if (*v == '\\' && v[1]) {
      int c = *++v;
      if (c == 'c' || c == 'C') {
        *icase = c == 'c';
        continue;
      }
      if (c == '<' || c == '>') {
        out[n++] = '\\';
        out[n++] = 'b';
      }
      else if (c == '=') out[n++] = '?';
      else if (strchr(lit, c)) out[n++] = (char)c;
      else {
        out[n++] = '\\';
        out[n++] = (char)c;
      }
    }
    else if (strchr(lit, *v)) {
      out[n++] = '\\';
      out[n++] = *v;
    }
    else out[n++] = *v;
  }
  out[n] = '\0';
}


/* vim.ignorecase and vim.smartcase for a pattern typed */
static int case_off (const char *pat, int nosmart) {
  const char *p;
  if (!VO.icase) return 0;
  if (VO.scase && !nosmart)
    for (p = pat; *p; p++) {
      if (*p == '\\' && p[1]) {
        p++;
        continue;
      }
      if (*p >= 'A' && *p <= 'Z') return 0;
    }
  return 1;
}


/* the pattern goes to Find (and so is lit) */
static void find_set (const char *pat, int nosmart) {
  char re[300];
  int ic = case_off(pat, nosmart);
  re_of_vim(pat, re, sizeof(re), &ic);
  ved_find(re, ic);
}


/* the count'th match from p, up or down; 0: none */
static int search_from (Pos p, int back, long count, Pos *at) {
  size_t len;
  Pos f;
  if (count < 1) count = 1;
  while (count-- > 0) {
    Pos from = back ? p : (p.x < llen(p.y) ? pos(p.y, nextx(p.y, p.x)) : (p.y < last_y() ? pos(p.y + 1, 0) : pos(0, 0)));
    if (!ved_find_next(from, back, &f, &len)) return 0;
    p = f;
  }
  *at = p;
  return 1;
}


/* n N: the last search again (back: the other way) */
static int search_again (int back, long count, Pos *at) {
  if (V.pat[0] == '\0') {
    say("E35: No previous regular expression", NULL);
    return 0;
  }
  find_set(V.pat, V.pat_nosmart);
  if (VO.hls) V.lit = 1;
  if (!search_from(cur(), V.pat_back ^ back, count, at)) {
    say("E486: Pattern not found: %s", V.pat);
    return 0;
  }
  return 1;
}


/* * #: the word under the cursor, whole */
static int search_word (int back, int whole, long count, Pos *at) {
  Pos p = cur(), a;
  const Row *r = row(p.y);
  size_t n;
  int c;
  g_big = 0;
  while (p.x < r->len && cls(p) != 2) p.x = nextx(p.y, p.x);	/* the first word at or after the cursor */
  if (p.x >= r->len) {
    say("E348: No string under cursor", NULL);
    return 0;
  }
  c = cls(p);
  a = p;
  while (a.x > 0 && cls(pos(a.y, prevx(a.y, a.x))) == c) a.x = prevx(a.y, a.x);
  while (p.x < r->len && cls(p) == c) p.x = nextx(p.y, p.x);
  n = 0;
  if (whole) n += (size_t)snprintf(V.pat, sizeof(V.pat), "\\<");
  for (; a.x < p.x && n + 4 < sizeof(V.pat); a.x++) {
    if (strchr("\\/.*$^~[]", r->s[a.x])) V.pat[n++] = '\\';
    V.pat[n++] = r->s[a.x];
  }
  V.pat[n] = '\0';
  if (whole) snprintf(V.pat + n, sizeof(V.pat) - n, "\\>");
  V.pat_back = back;
  V.pat_nosmart = 1;
  reg_put('/', V.pat, strlen(V.pat), RK_CHAR);
  return search_again(0, count, at);
}

/* }================================================================== */


/*
** {==================================================================
** Motions
** ===================================================================
*/

static int is_bracket (int c) {
  return c && strchr("(){}[]", c) != NULL;
}


/* the bracket that pairs with the one at p (%) */
static int match_pair (Pos p, Pos *m) {
  int c = ch(p), o, cl, d, depth = 1;
  const char *pairs = "(){}[]";
  const char *at = strchr(pairs, c);
  if (!is_bracket(c) || at == NULL) return 0;
  d = ((at - pairs) & 1) ? -1 : 1;
  o = c;
  cl = d > 0 ? at[1] : at[-1];
  for (;;) {
    int k = d > 0 ? inc(&p) : dec(&p);
    if (k == -1) return 0;
    if (ch(p) == o) depth++;
    else if (ch(p) == cl && --depth == 0) {
      *m = p;
      return 1;
    }
  }
}


/* the lines of the screen: H M L */
static size_t screen_line (int which, long n) {
  size_t top = *V.v.top, bot = top + (size_t)V.v.rows - 1, y;
  if (bot > last_y()) bot = last_y();
  if (top > bot) top = bot;
  if (which == 'H') y = top + (size_t)(n - 1);
  else if (which == 'L') y = bot >= (size_t)(n - 1) + top ? bot - (size_t)(n - 1) : top;
  else y = top + (bot - top) / 2;
  if (y > bot) y = bot;
  if (y < top) y = top;
  return y;
}


/* f F t T: the count'th c in the line; again: ; , (t does not stop right before it) */
static int find_char (Pos *p, int key, int c, long count, int again) {
  const Row *r = row(p->y);
  size_t x = p->x;
  int fwd = key == 'f' || key == 't', till = key == 't' || key == 'T';
  while (count-- > 0) {
    size_t start = x;
    for (;;) {
      if (fwd) {
        if (x >= r->len || (x = nextx(p->y, x)) >= r->len) return 0;
      }
      else {
        if (x == 0) return 0;
        x = prevx(p->y, x);
      }
      if ((unsigned char)r->s[x] == c) {
        if (till && again && count == 0 && (fwd ? nextx(p->y, start) == x : nextx(p->y, x) == start)) continue;
        break;
      }
    }
  }
  if (till) x = fwd ? prevx(p->y, x) : nextx(p->y, x);
  p->x = x;
  return 1;
}


/* { }: the next empty line (a paragraph's edge) */
static int paragraph (Pos *p, long count, int dir, int *incl) {
  size_t y = p->y;
  *incl = 0;
  while (count-- > 0) {
    int skipped = 0, first;
    for (first = 1;; first = 0) {
      if (llen(y) != 0) skipped = 1;
      if (!first && skipped && llen(y) == 0) break;
      if ((dir < 0 && y == 0) || (dir > 0 && y >= last_y())) {
        if (count) return 0;
        break;
      }
      y = dir > 0 ? y + 1 : y - 1;
    }
  }
  p->y = y;
  p->x = 0;
  if (y == last_y() && dir > 0 && llen(y) > 0) {
    p->x = lastx(y);
    *incl = 1;
  }
  return 1;
}


/* line n (from 1), for G gg :n */
static size_t line_nr (long n) {
  if (n < 1) return 0;
  return (size_t)n - 1 > last_y() ? last_y() : (size_t)n - 1;
}


/* j k: the column they aim for stays */
static int vertical (const VCmd *c) {
  int k = c->key[0];
  return k == 'j' || k == 'k' || k == K_UP || k == K_DOWN || (k == 'g' && (c->key[1] == 'j' || c->key[1] == 'k'));
}


/* where the motion of c takes p; 0: nowhere (the operator does nothing) */
static int motion (const VCmd *c, Pos p, Mot *m) {
  long n = c->count ? c->count : 1, i;
  int k = c->key[0], k1 = c->nkey > 1 ? c->key[1] : 0, incl, op = c->op != 0;
  size_t y;
  m->kind = MK_EXCL;
  m->jump = 0;
  g_big = 0;
  switch (k) {
    case 'h': case K_LEFT:
      if (p.x == 0) return 0;
      for (i = 0; i < n && p.x > 0; i++) p.x = prevx(p.y, p.x);
      break;
    case K_BS:
      for (i = 0; i < n; i++)
        if (dec(&p) == -1) break;
        else if (p.x == llen(p.y) && p.x > 0) p.x = prevx(p.y, p.x);
      break;
    case 'l': case K_RIGHT:
      if (p.x >= llen(p.y) || (!op && nextx(p.y, p.x) >= llen(p.y))) return 0;
      for (i = 0; i < n && p.x < llen(p.y); i++) p.x = nextx(p.y, p.x);
      break;
    case ' ':
      for (i = 0; i < n; i++) {
        if (p.x < llen(p.y) && nextx(p.y, p.x) < llen(p.y)) p.x = nextx(p.y, p.x);
        else if (p.y < last_y()) p = pos(p.y + 1, 0);
        else break;
      }
      break;
    case 'j': case 'k': case K_DOWN: case K_UP: {
      int d = (k == 'j' || k == K_DOWN) ? 1 : -1;
      size_t y0 = p.y;
      for (i = 0; i < n; i++) {
        y = ved_line_step(p.y, d);
        if (y > last_y() || y == p.y) break;
        p.y = y;
      }
      if (p.y == y0) return 0;
      p.x = *V.v.want >= WANT_END ? llen(p.y) : ved_x(p.y, *V.v.want);
      m->kind = MK_LINE;
      break;
    }
    case '+': case '-': case K_ENTER: case '_': {
      int d = k == '-' ? -1 : 1;
      long steps = k == '_' ? n - 1 : n;
      for (i = 0; i < steps; i++) {
        y = ved_line_step(p.y, d);
        if (y > last_y() || y == p.y) {
          if (i == 0 && steps > 0) return 0;
          break;
        }
        p.y = y;
      }
      p.x = first_nb(p.y);
      m->kind = MK_LINE;
      break;
    }
    case '0': case K_HOME: p.x = 0; break;
    case '^': p.x = first_nb(p.y); break;
    case '$': case K_END:
      for (i = 1; i < n && p.y < last_y(); i++) p.y++;
      p.x = op ? lastx(p.y) : llen(p.y);
      if (op && llen(p.y) == 0) p.x = 0;
      m->kind = MK_INCL;
      break;
    case '|':
      p.x = ved_x(p.y, (size_t)(n - 1));
      if (p.x >= llen(p.y) && llen(p.y) > 0) p.x = lastx(p.y);
      break;
    case 'w': case 'W':
      g_big = k == 'W';
      if (op && c->op == 'c' && cls(p) != 0) {	/* cw is ce, but for the last character of a word */
        end_word(&p, n, 1);
        m->kind = MK_INCL;
        break;
      }
      fwd_word(&p, n, op);
      break;
    case 'b': case 'B':
      g_big = k == 'B';
      if (!bck_word(&p, n) && pos_cmp(p, cur()) == 0) return 0;
      break;
    case 'e': case 'E':
      g_big = k == 'E';
      end_word(&p, n, 0);
      m->kind = MK_INCL;
      break;
    case 'G':
      p.y = c->count ? line_nr(c->count) : last_y();
      p.x = first_nb(p.y);
      m->kind = MK_LINE;
      m->jump = 1;
      break;
    case 'H': case 'M': case 'L':
      p.y = screen_line(k, n);
      p.x = first_nb(p.y);
      m->kind = MK_LINE;
      m->jump = 1;
      break;
    case '{': case '}':
      if (!paragraph(&p, n, k == '}' ? 1 : -1, &incl)) return 0;
      if (incl) m->kind = MK_INCL;
      m->jump = 1;
      break;
    case '%':
      m->jump = 1;
      if (c->count) {	/* 50%: half way down */
        if (c->count > 100) return 0;
        p.y = line_nr((c->count * (long)DOC->n + 99) / 100);
        p.x = first_nb(p.y);
        m->kind = MK_LINE;
        break;
      }
      while (p.x < llen(p.y) && !is_bracket(ch(p))) p.x = nextx(p.y, p.x);
      if (!match_pair(p, &p)) return 0;
      m->kind = MK_INCL;
      break;
    case 'f': case 'F': case 't': case 'T':
      V.f_key = k;
      V.f_ch = c->arg;
      if (!find_char(&p, k, c->arg, n, 0)) return 0;
      if (k == 'f' || k == 't') m->kind = MK_INCL;
      break;
    case ';': case ',': {
      int fk = V.f_key;
      if (fk == 0) return 0;
      if (k == ',') fk = fk == 'f' ? 'F' : fk == 'F' ? 'f' : fk == 't' ? 'T' : 't';
      if (!find_char(&p, fk, V.f_ch, n, 1)) return 0;
      if (fk == 'f' || fk == 't') m->kind = MK_INCL;
      break;
    }
    case 'n': case 'N':
      if (!search_again(k == 'N', n, &p)) return 0;
      m->jump = 1;
      break;
    case '*': case '#':
      if (!search_word(k == '#', 1, n, &p)) return 0;
      m->jump = 1;
      break;
    case '`': case '\'':
      if (!mark_get(c->arg, &p)) {
        say("E20: Mark not set", NULL);
        return 0;
      }
      if (k == '\'') {
        p.x = first_nb(p.y);
        m->kind = MK_LINE;
      }
      m->jump = 1;
      break;
    case 'g':
      switch (k1) {
        case 'g':
          p.y = c->count ? line_nr(c->count) : 0;
          p.x = first_nb(p.y);
          m->kind = MK_LINE;
          m->jump = 1;
          break;
        case 'e': case 'E':
          g_big = k1 == 'E';
          bckend_word(&p, n);
          m->kind = MK_INCL;
          break;
        case '_':
          for (i = 1; i < n && p.y < last_y(); i++) p.y++;
          p.x = llen(p.y);
          while (p.x > 0 && (row(p.y)->s[p.x - 1] == ' ' || row(p.y)->s[p.x - 1] == '\t')) p.x--;
          p.x = p.x > 0 ? prevx(p.y, p.x) : 0;
          m->kind = MK_INCL;
          break;
        case 'j': case 'k': {
          VCmd j = *c;
          j.key[0] = k1;
          j.nkey = 1;
          return motion(&j, p, m);
        }
        case '0': p.x = 0; break;
        case '$': p.x = op ? lastx(p.y) : llen(p.y); m->kind = MK_INCL; break;
        case 'm': p.x = ved_x(p.y, ved_col(p.y, llen(p.y)) / 2); break;
        case '*': case '#':
          if (!search_word(k1 == '#', 0, n, &p)) return 0;
          m->jump = 1;
          break;
        default: return 0;
      }
      break;
    default: return 0;
  }
  m->to = p;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Text objects: iw aw iW aW i" a" i( a( ib i{ iB i[ i< it at ip ap
** ===================================================================
*/

/* the whole text, lines joined by \n, and the way between its bytes and places */
static char *flat (size_t *n) {
  Pos e = pos(last_y(), llen(last_y()));
  return text(pos(0, 0), e, n);
}


static size_t off_of (Pos p) {
  size_t y, o = 0;
  for (y = 0; y < p.y && y < DOC->n; y++) o += llen(y) + 1;
  return o + p.x;
}


static Pos pos_of (size_t o) {
  size_t y = 0;
  while (y < last_y() && o > llen(y)) {
    o -= llen(y) + 1;
    y++;
  }
  return pos(y, o > llen(y) ? llen(y) : o);
}


/* iw aw iW aW */
static int obj_word (long n, int around, Pos *a, Pos *b) {
  Pos p = cur(), s, e;
  size_t len = llen(p.y);
  int c;
  if (len == 0) {
    *a = *b = p;
    return around ? 0 : 1;
  }
  if (p.x >= len) p.x = lastx(p.y);
  c = cls(p);
  s = e = p;
  while (s.x > 0 && cls(pos(s.y, prevx(s.y, s.x))) == c) s.x = prevx(s.y, s.x);
  while (e.x < len && cls(e) == c) e.x = nextx(e.y, e.x);
  if (around) {
    if (c == 0) {	/* on blanks: they and the word after */
      if (e.x < len) {
        int c2 = cls(e);
        while (e.x < len && cls(e) == c2) e.x = nextx(e.y, e.x);
      }
    }
    else if (e.x < len && cls(e) == 0) {	/* the word and the blanks after it */
      while (e.x < len && cls(e) == 0) e.x = nextx(e.y, e.x);
    }
    else	/* none after: the blanks before it */
      while (s.x > 0 && cls(pos(s.y, prevx(s.y, s.x))) == 0) s.x = prevx(s.y, s.x);
  }
  while (--n > 0 && e.x < len) {	/* a count: the next ones too */
    int c2 = cls(e);
    while (e.x < len && cls(e) == c2) e.x = nextx(e.y, e.x);
    if (around && c2 != 0)
      while (e.x < len && cls(e) == 0) e.x = nextx(e.y, e.x);
  }
  *a = s;
  *b = e;
  return 1;
}


static int escaped (const Row *r, size_t x) {
  return x > 0 && r->s[x - 1] == '\\';
}


/* i" a" i' a' i` a`: in the line */
static int obj_quote (int q, int around, Pos *a, Pos *b) {
  Pos p = cur();
  const Row *r = row(p.y);
  size_t qs[256], nq = 0, x, i, s, e;
  for (x = 0; x < r->len && nq < 256; x++)
    if (r->s[x] == q && !escaped(r, x)) qs[nq++] = x;
  if (nq < 2) return 0;
  s = e = (size_t)-1;
  for (i = 0; i < nq; i++)
    if (qs[i] == p.x) {	/* on a quote: opening or closing, by how many come before */
      if (i % 2 == 0 && i + 1 < nq) s = qs[i], e = qs[i + 1];
      else if (i % 2 == 1) s = qs[i - 1], e = qs[i];
      break;
    }
  if (s == (size_t)-1) {
    if (p.x < qs[0]) s = qs[0], e = qs[1];	/* before them all: the first string */
    else
      for (i = 0; i + 1 < nq; i++)
        if (qs[i] < p.x && p.x < qs[i + 1]) {
          s = qs[i], e = qs[i + 1];
          break;
        }
  }
  if (s == (size_t)-1) return 0;
  if (!around) {
    *a = pos(p.y, s + 1);
    *b = pos(p.y, e);
    return 1;
  }
  e++;
  if (e < r->len && (r->s[e] == ' ' || r->s[e] == '\t'))
    while (e < r->len && (r->s[e] == ' ' || r->s[e] == '\t')) e++;
  else
    while (s > 0 && (r->s[s - 1] == ' ' || r->s[s - 1] == '\t')) s--;
  *a = pos(p.y, s);
  *b = pos(p.y, e);
  return 1;
}


/* i( a( i{ i[ i<: the count'th pair around the cursor */
static int obj_bracket (int o, int cl, long n, int around, Pos *a, Pos *b, int *kind) {
  size_t len, at, i, s = 0, e = 0;
  char *t = flat(&len);
  long depth;
  int found = 0;
  at = off_of(cur());
  if (at < len && t[at] == cl) {	/* on the closing one: its opening one */
    depth = 0;
    for (i = at + 1; i-- > 0;) {
      if (t[i] == cl) depth++;
      else if (t[i] == o && --depth == 0) break;
    }
    if (i == (size_t)-1) goto none;
    at = i;
  }
  while (n-- > 0) {
    depth = 0;
    for (i = at + (t[at] == o && !found ? 1 : 0); i-- > 0;) {
      if (t[i] == cl) depth++;
      else if (t[i] == o && depth-- == 0) break;
    }
    if (i == (size_t)-1) goto none;
    s = i;
    found = 1;
    at = s;
  }
  depth = 0;
  for (i = s + 1; i < len; i++) {
    if (t[i] == o) depth++;
    else if (t[i] == cl && depth-- == 0) break;
  }
  if (i >= len) goto none;
  e = i;
  free(t);
  if (around) {
    *a = pos_of(s);
    *b = pos_of(e + 1);
    return 1;
  }
  *a = pos_of(s + 1);
  *b = pos_of(e);
  if (a->x == llen(a->y) && a->y < b->y && first_nb(b->y) == b->x) {	/* { ends its line, } starts one: the lines between */
    if (b->y - a->y < 2) return 0;
    *a = pos(a->y + 1, 0);
    *b = pos(b->y - 1, llen(b->y - 1));
    *kind = MK_LINE;
    return 1;
  }
  if (a->x == llen(a->y) && a->y < b->y) *a = pos(a->y + 1, 0);	/* ( ends its line: from the next one */
  return pos_cmp(*a, *b) <= 0;
none:
  free(t);
  return 0;
}


/* it at: the count'th element around the cursor */
static int obj_tag (long n, int around, Pos *a, Pos *b) {
  size_t len, at, i, nt = 0, cap = 64;
  char *t = flat(&len);
  struct Tg {
    size_t s, e;	/* <name ...> or </name>: from its < to after its > */
    size_t ns, nn;	/* its name */
    int close;
    long pair;
  } *tg = (struct Tg *)xmalloc(cap * sizeof(*tg));
  long *stack = NULL, sp = 0, best = -1;
  int ok = 0;
  at = off_of(cur());
  for (i = 0; i < len; i++) {	/* every tag, paired with a stack */
    size_t j = i + 1, ns;
    int close = 0;
    if (t[i] != '<') continue;
    if (j < len && t[j] == '/') close = 1, j++;
    ns = j;
    while (j < len && (t[j] == '-' || t[j] == ':' || t[j] == '.' || t[j] == '_' || (t[j] >= '0' && t[j] <= '9') ||
                       ((t[j] | 32) >= 'a' && (t[j] | 32) <= 'z')))
      j++;
    if (j == ns) continue;
    while (j < len && t[j] != '>' && t[j] != '<') j++;
    if (j >= len || t[j] != '>') continue;
    if (!close && t[j - 1] == '/') continue;	/* <br/> */
    if (nt == cap) tg = xrealloc(tg, (cap *= 2) * sizeof(*tg));
    tg[nt].s = i;
    tg[nt].e = j + 1;
    tg[nt].ns = ns;
    tg[nt].nn = (ns < j ? j : ns) - ns;
    {
      size_t k = ns;
      while (k < j && t[k] != ' ' && t[k] != '\t' && t[k] != '\n' && t[k] != '/') k++;
      tg[nt].nn = k - ns;
    }
    tg[nt].close = close;
    tg[nt].pair = -1;
    nt++;
  }
  stack = (long *)xmalloc((nt + 1) * sizeof(long));
  for (i = 0; i < nt; i++) {
    if (!tg[i].close) stack[sp++] = (long)i;
    else {
      long k;
      for (k = sp - 1; k >= 0; k--)
        if (tg[stack[k]].nn == tg[i].nn && memcmp(t + tg[stack[k]].ns, t + tg[i].ns, tg[i].nn) == 0) break;
      if (k < 0) continue;
      tg[stack[k]].pair = (long)i;
      tg[i].pair = stack[k];
      sp = k;
    }
  }
  while (n-- > 0) {	/* the innermost around the cursor, then the one around that */
    long k, pick = -1;
    size_t lo = best >= 0 ? tg[best].s : at, hi = best >= 0 ? tg[tg[best].pair].e : at + 1;
    for (k = 0; k < (long)nt; k++) {
      if (tg[k].close || tg[k].pair < 0) continue;
      if (tg[k].s <= lo && tg[tg[k].pair].e >= hi && k != best && (tg[k].s < lo || tg[tg[k].pair].e > hi || best < 0))
        if (pick < 0 || tg[k].s > tg[pick].s) pick = k;
    }
    if (pick < 0) break;
    best = pick;
    ok = 1;
  }
  if (ok) {
    size_t cl = (size_t)tg[best].pair;
    *a = pos_of(around ? tg[best].s : tg[best].e);
    *b = pos_of(around ? tg[cl].e : tg[cl].s);
  }
  free(stack);
  free(tg);
  free(t);
  return ok;
}


/* ip ap: lines, a block of text or of blank lines */
static int obj_par (long n, int around, size_t *y0, size_t *y1) {
  size_t y = cur().y, s = y, e = y;
  int w = blank_line(y);
  while (s > 0 && blank_line(s - 1) == w) s--;
  while (e < last_y() && blank_line(e + 1) == w) e++;
  while (--n > 0 && e < last_y()) {
    int w2 = blank_line(e + 1);
    e++;
    while (e < last_y() && blank_line(e + 1) == w2) e++;
  }
  if (around) {
    if (e < last_y()) {	/* the blank lines after it (or the text, after blank lines) */
      int w2 = blank_line(e + 1);
      e++;
      while (e < last_y() && blank_line(e + 1) == w2) e++;
    }
    else if (!w)	/* none after: the blank lines before */
      while (s > 0 && blank_line(s - 1)) s--;
  }
  *y0 = s;
  *y1 = e;
  return 1;
}


/* the text object of c: a to b (exclusive), or lines; 0 none there */
static int object (const VCmd *c, Pos *a, Pos *b, int *kind) {
  long n = c->count ? c->count : 1;
  int around = c->key[0] == 'a', o = c->key[1];
  *kind = MK_EXCL;
  g_big = o == 'W';
  switch (o) {
    case 'w': case 'W': return obj_word(n, around, a, b);
    case '"': case '\'': case '`': return obj_quote(o, around, a, b);
    case '(': case ')': case 'b': return obj_bracket('(', ')', n, around, a, b, kind);
    case '{': case '}': case 'B': return obj_bracket('{', '}', n, around, a, b, kind);
    case '[': case ']': return obj_bracket('[', ']', n, around, a, b, kind);
    case '<': case '>': return obj_bracket('<', '>', n, around, a, b, kind);
    case 't': return obj_tag(n, around, a, b);
    case 'p': {
      size_t y0, y1;
      obj_par(n, around, &y0, &y1);
      *a = pos(y0, 0);
      *b = pos(y1, llen(y1));
      *kind = MK_LINE;
      return 1;
    }
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** Operators
** ===================================================================
*/

static void mode_insert (int cmd, long count);

/* the indent of one level: a tab, or spaces */
static void shift_lines (size_t y0, size_t y1, long times, int right) {
  size_t y, unit = DOC->tabs ? (size_t)TABW : (size_t)(DOC->indent > 0 ? DOC->indent : 4);
  for (y = y0; y <= y1 && y < DOC->n; y++) {
    const Row *r = row(y);
    size_t nb = first_nb(y), have = ved_col(y, nb), want;
    Buf b;
    if (r->len == 0) continue;	/* an empty line stays empty */
    if (right) want = have + unit * (size_t)times;
    else want = have > unit * (size_t)times ? have - unit * (size_t)times : 0;
    want -= want % unit;
    if (right && want < have + unit * (size_t)times) want += unit;
    buf_init(&b);
    if (DOC->tabs) {
      size_t k;
      for (k = 0; k + (size_t)TABW <= want; k += (size_t)TABW) buf_putc(&b, '\t');
      for (; k < want; k++) buf_putc(&b, ' ');
    }
    else {
      size_t k;
      for (k = 0; k < want; k++) buf_putc(&b, ' ');
    }
    if (b.len != nb || memcmp(b.s ? b.s : "", r->s, nb) != 0) {
      del(pos(y, 0), pos(y, nb));
      if (b.len) ins(pos(y, 0), b.s, b.len);
    }
    buf_free(&b);
  }
}


/* gu gU g~ g?: the case of a to b */
static void case_range (Pos a, Pos b, int how) {
  size_t n, i;
  char *t = text(a, b, &n);
  int changed = 0;
  for (i = 0; i < n; i++) {
    int c = (unsigned char)t[i], d = c;
    int up = c >= 'a' && c <= 'z', lo = c >= 'A' && c <= 'Z';
    if (how == 'u' && lo) d = c + 32;
    else if (how == 'U' && up) d = c - 32;
    else if (how == '~' && (up || lo)) d = c ^ 32;
    else if (how == '?' && (up || lo)) d = ((c | 32) - 'a' + 13) % 26 + (up ? 'a' : 'A');
    if (d != c) t[i] = (char)d, changed = 1;
  }
  if (changed) {
    del(a, b);
    ins(a, t, n);
  }
  free(t);
}


/* lines y0 .. y1 go: the lines around close up */
static void del_lines (size_t y0, size_t y1) {
  if (y1 < last_y()) del(pos(y0, 0), pos(y1 + 1, 0));
  else if (y0 > 0) del(pos(y0 - 1, llen(y0 - 1)), pos(y1, llen(y1)));
  else del(pos(0, 0), pos(y1, llen(y1)));
}


/* an editor command over lines y0 .. y1 selected (gc = :sort); the cursor to the first */
static void lines_command (size_t y0, size_t y1, int cmd) {
  *V.v.anchor = pos(y0, 0);
  *V.v.cur = pos(y1, llen(y1));
  *V.v.sel = 1;
  ved_command(cmd);
  if (!ved_get(&V.v)) return;
  *V.v.sel = 0;
  go(pos(y0, first_nb(y0 < DOC->n ? y0 : last_y())));
}


static int big_motion (const VCmd *c) {
  int k = c->key[0];
  return c->kind == CK_SEARCH || (c->kind == CK_MOTION && k != 0 && strchr("%(){}`nN/?*#", k) != NULL);
}


/* operator op of c over a to b (exclusive), or the lines of a to b */
static void apply (const VCmd *c, Pos a, Pos b, int kind) {
  int op = c->op;
  long times = c->count ? c->count : 1;
  size_t n, y0 = a.y, y1 = b.y;
  char *t;
  if (pos_cmp(a, b) > 0) {
    Pos s = a;
    a = b;
    b = s;
    y0 = a.y;
    y1 = b.y;
  }
  if (op != 'y') {
    grp();
    mark_set(&V.mk_change, a);
  }
  switch (op) {
    case 'y':
      t = kind == MK_LINE ? lines_text(y0, y1, &n) : text(a, b, &n);
      reg_set(c->reg, t, n, kind == MK_LINE ? RK_LINE : RK_CHAR, RW_YANK);
      free(t);
      if (kind == MK_LINE) {
        if (cur().y > y0) go(pos(y0, cur().x < llen(y0) ? cur().x : lastx(y0)));
      }
      else go(a);
      break;
    case 'd': case 'c':
      if (kind == MK_LINE) {
        t = lines_text(y0, y1, &n);
        reg_set(c->reg, t, n, RK_LINE, RW_BIG);
        free(t);
        if (op == 'c') {	/* one line is left, with the first one's indent */
          size_t ind = first_nb(y0);
          del(pos(y0, ind), pos(y1, llen(y1)));
          go(pos(y0, ind));
          mode_insert('c', 1);
          break;
        }
        del_lines(y0, y1);
        y0 = y0 > last_y() ? last_y() : y0;
        go(pos(y0, first_nb(y0)));
        break;
      }
      t = text(a, b, &n);
      reg_set(c->reg, t, n, RK_CHAR, a.y != b.y || big_motion(c) ? RW_BIG : RW_SMALL);
      free(t);
      del(a, b);
      go(a);
      if (op == 'c') mode_insert('c', 1);
      break;
    case '>': case '<':
      if (kind != MK_LINE && b.x == 0 && b.y > a.y) y1--;
      shift_lines(y0, y1, c->kind == CK_ACTION ? times : 1, op == '>');
      go(pos(y0, first_nb(y0)));
      break;
    case '=':
      if (kind != MK_LINE && b.x == 0 && b.y > a.y) y1--;
      lines_command(y0, y1, CMD_REINDENT_SEL);
      if (ved_get(&V.v)) go(pos(y0, first_nb(y0)));
      break;
    case OP_G('c'):
      if (kind != MK_LINE && b.x == 0 && b.y > a.y) y1--;
      lines_command(y0, y1, CMD_COMMENT);
      break;
    default:	/* gu gU g~ g? */
      if (kind == MK_LINE) {
        case_range(pos(y0, 0), pos(y1, llen(y1)), op & 0xFF);
        go(pos(y0, cur().x));
      }
      else {
        case_range(a, b, op & 0xFF);
        go(a);
      }
      break;
  }
}


/* the motion or text object of c, then its operator */
static void do_op (const VCmd *c) {
  Pos a = cur(), b = a;
  int kind = MK_EXCL;
  long n = c->count ? c->count : 1;
  if (c->kind == CK_LINES) {	/* dd 3yy >> gcc */
    size_t y1 = a.y + (size_t)(n - 1);
    if (y1 > last_y()) y1 = last_y();
    if (c->op == '>' || c->op == '<') {
      VCmd s = *c;
      s.count = 1;
      a = pos(a.y, 0);
      b = pos(y1, 0);
      apply(&s, a, b, MK_LINE);
      return;
    }
    apply(c, pos(a.y, 0), pos(y1, 0), MK_LINE);
    return;
  }
  if (c->kind == CK_OBJECT) {
    if (!object(c, &a, &b, &kind)) return;
    apply(c, a, b, kind);
    return;
  }
  {
    Mot m;
    if (!motion(c, a, &m)) return;
    b = m.to;
    kind = m.kind;
  }
  if (pos_cmp(b, a) < 0) {
    Pos s = a;
    a = b;
    b = s;
  }
  if (kind == MK_INCL) b.x = nextx(b.y, b.x);	/* not past the line's end: d$ on an empty line keeps it */
  else if (kind == MK_EXCL && b.x == 0 && b.y > a.y && pos_cmp(a, b) != 0) {	/* :h exclusive */
    int line = a.x <= first_nb(a.y);
    b = pos(b.y - 1, llen(b.y - 1));
    if (line) kind = MK_LINE;
  }
  apply(c, a, b, kind);
}

/* }================================================================== */


/*
** {==================================================================
** Put: p P gp gP
** ===================================================================
*/

static void put (int r, long count, int before, int gp) {
  const Reg *g = reg_get(r);
  Pos p = cur(), e;
  Buf b;
  long i;
  char name[2];
  if (g == NULL || g->n == 0) {
    name[0] = (char)(r ? r : '"');
    name[1] = '\0';
    say("E353: Nothing in register %s", name);
    return;
  }
  if (count < 1) count = 1;
  grp();
  buf_init(&b);
  if (g->kind == RK_LINE) {
    size_t first, nl = 0;
    for (i = 0; i < count; i++) {
      buf_putn(&b, g->s, g->n);
      if (g->s[g->n - 1] != '\n') buf_putc(&b, '\n');
    }
    for (i = 0; i < (long)b.len; i++) nl += b.s[i] == '\n';
    if (before) {
      first = p.y;
      ins(pos(p.y, 0), b.s, b.len);
    }
    else {
      first = p.y + 1;
      ins(pos(p.y, llen(p.y)), "\n", 1);
      ins(pos(p.y + 1, 0), b.s, b.len - 1);
    }
    mark_set(&V.mk_change, pos(first, 0));
    if (gp) go(pos(first + nl <= last_y() ? first + nl : last_y(), 0));
    else go(pos(first, first_nb(first)));
  }
  else if (g->kind == RK_BLOCK) {
    size_t col = ved_col(p.y, p.x), y = p.y, k = 0, s = 0;
    if (!before && llen(p.y) > 0) col = ved_col(p.y, nextx(p.y, p.x));
    for (k = 0; k <= g->n; k++) {
      if (k < g->n && g->s[k] != '\n') continue;
      {
        size_t x, have;
        Buf piece;
        if (y > last_y()) ins(pos(last_y(), llen(last_y())), "\n", 1);
        x = ved_x(y, col);
        have = ved_col(y, llen(y));
        buf_init(&piece);
        for (; have < col && x >= llen(y); have++) buf_putc(&piece, ' ');
        for (i = 0; i < count; i++) buf_putn(&piece, g->s + s, k - s);
        if (piece.len) ins(pos(y, x), piece.s, piece.len);
        buf_free(&piece);
      }
      s = k + 1;
      y++;
    }
    go(pos(p.y, ved_x(p.y, col)));
  }
  else {
    Pos at = p;
    for (i = 0; i < count; i++) buf_putn(&b, g->s, g->n);
    if (!before && llen(p.y) > 0) at.x = nextx(p.y, p.x);
    e = ins(at, b.s, b.len);
    mark_set(&V.mk_change, at);
    if (gp) go(e);
    else if (memchr(b.s, '\n', b.len)) go(at);	/* more than a line: its start */
    else go(pos(e.y, prevx(e.y, e.x)));
  }
  buf_free(&b);
}

/* }================================================================== */


/*
** {==================================================================
** Insert and Replace modes
** ===================================================================
*/

static void mode_insert (int cmd, long count) {
  V.mode = cmd == 'R' ? VM_REPLACE : VM_INSERT;
  V.ins_cmd = cmd;
  V.ins_count = count < 1 ? 1 : count;
  V.ins.n = 0;
  V.ins_dot = !V.dotting;
  V.oneshot = 0;
  V.rbuf.len = 0;
}


/* R: a character typed over the one at the cursor */
static void replace_char (int k) {
  Pos p = cur();
  char u[4];
  int n = utf8_encode((uint32_t)k, u);
  if (p.x < llen(p.y)) {
    size_t e = nextx(p.y, p.x);
    buf_putn(&V.rbuf, row(p.y)->s + p.x, e - p.x);
    buf_putc(&V.rbuf, '\2');	/* the end of one */
    del(p, pos(p.y, e));
  }
  else buf_putc(&V.rbuf, '\1');	/* nothing was there */
  p = ins(p, u, (size_t)n);
  go(p);
}


/* R: Backspace gives the character back */
static void replace_back (void) {
  Pos p = cur();
  size_t k;
  if (V.rbuf.len == 0) {
    if (p.x > 0) go(pos(p.y, prevx(p.y, p.x)));
    return;
  }
  p.x = prevx(p.y, p.x);
  del(p, pos(p.y, nextx(p.y, p.x)));
  if (V.rbuf.s[V.rbuf.len - 1] == '\1') V.rbuf.len--;
  else {
    V.rbuf.len--;
    for (k = V.rbuf.len; k > 0 && V.rbuf.s[k - 1] != '\2' && V.rbuf.s[k - 1] != '\1'; k--) ;
    ins(p, V.rbuf.s + k, V.rbuf.len - k);
    V.rbuf.len = k;
  }
  go(p);
}


/* Ctrl+U in Insert mode: to the start of the line's text */
static void ins_kill_line (void) {
  Pos p = cur();
  size_t s = first_nb(p.y) < p.x ? first_nb(p.y) : 0;
  del(pos(p.y, s), p);
  go(pos(p.y, s));
}


/* a key of Insert (Replace) mode; live: typed now, else "." or a count replays it. 0: mme.c's */
static int ins_key (int k, int live) {
  int code = KEY_CODE(k);
  if (V.ctrl_r) {	/* Ctrl+R x: register x typed in */
    const Reg *g;
    V.ctrl_r = 0;
    kadd(&V.ins, k);
    if ((g = reg_get(k == '"' ? 0 : k)) != NULL && g->n) go(ins(cur(), g->s, g->n));
    return 1;
  }
  if (live && (code == K_UP || code == K_DOWN || code == K_LEFT || code == K_RIGHT || code == K_HOME ||
               code == K_END || code == K_PGUP || code == K_PGDN)) {
    V.ins.n = 0;	/* moved: "." repeats what is typed from here */
    undo_join();
    grp();
    return 0;
  }
  if (!IS_TEXT(k) && code != K_ENTER && code != K_TAB && code != K_BS && code != K_DEL && k != CTRL('r') &&
      k != CTRL('w') && k != CTRL('u') && k != CTRL('t') && k != CTRL('d') && k != CTRL('o'))
    return 0;	/* not typing: VS Code's */
  if (k != CTRL('o')) kadd(&V.ins, k);
  if (k == CTRL('r')) V.ctrl_r = 1;
  else if (k == CTRL('w')) ved_key(K_BS | KM_CTRL);
  else if (k == CTRL('u')) ins_kill_line();
  else if (k == CTRL('t') || k == CTRL('d')) {	/* the line one level in or out, the cursor with its text */
    Pos p = cur();
    size_t before = llen(p.y);
    shift_lines(p.y, p.y, 1, k == CTRL('t'));
    p.x = llen(p.y) + p.x >= before ? llen(p.y) + p.x - before : 0;
    go(p);
  }
  else if (k == CTRL('o')) {
    V.oneshot = 1;
    V.mode = VM_NORMAL;
  }
  else if (V.mode == VM_REPLACE && IS_TEXT(k)) replace_char(k);
  else if (V.mode == VM_REPLACE && code == K_BS) replace_back();
  else if (!live) ved_key(k);
  else return 0;
  return 1;
}


/* o O: a line to type in, with its indent */
static void open_line (int above) {
  ved_command(above ? CMD_LINE_ABOVE : CMD_LINE_BELOW);
  ved_get(&V.v);
}


/* Esc: what was typed again for a count, one step of undo, back to Normal mode */
static void ins_end (void) {
  long i;
  size_t k, typed = V.ins.n;
  Keys again;
  Pos p;
  memset(&again, 0, sizeof(again));
  kcopy(&again, &V.ins);
  for (i = 1; i < V.ins_count; i++) {
    if (V.ins_cmd == 'o' || V.ins_cmd == 'O') open_line(V.ins_cmd == 'O');
    for (k = 0; k < again.n; k++) ins_key(again.k[k], 0);
  }
  free(again.k);
  V.ins.n = typed;
  undo_join();
  if (V.v.nmc > 0) {	/* Visual block's I and A: back to one cursor */
    Pos c = cur();
    ved_cursors(&c, &c, 1);
  }
  p = cur();
  if (V.blk) {	/* ... at the block's corner */
    V.blk = 0;
    p = V.blk_home;
    if (p.y > last_y()) p.y = last_y();
    if (p.x < llen(p.y)) p.x = nextx(p.y, p.x);	/* the step left below undoes this */
  }
  mark_set(&V.mk_ins, p);
  mark_set(&V.mk_change, p);
  if (V.ins_dot) kcopy(&V.dot_ins, &V.ins);
  V.mode = VM_NORMAL;
  V.oneshot = 0;
  V.ctrl_r = 0;
  if (p.x > 0) p.x = prevx(p.y, p.x);
  go(p);
}

/* }================================================================== */


/*
** {==================================================================
** Visual mode
** ===================================================================
*/

static void visual_start (int mode) {
  V.mode = mode;
  V.vs = V.vc = cur();
  V.vs_col = ved_col(V.vc.y, V.vc.x);
  V.vdollar = 0;
}


/* Visual block: the column of its cursor's side; after $ it goes to the ends of the lines */
static size_t block_col (void) {
  if (*V.v.want < WANT_END) return *V.v.want;
  V.vdollar = 1;
  return V.vs_col;
}


/* the selection mme.c draws: Vim's includes the character under the cursor */
static void paint (void) {
  Pos a = V.vs, c = V.vc;
  if (V.mode == VM_VISUAL) {
    if (pos_cmp(c, a) >= 0) c = after(c);
    else a = after(a);
    ved_cursors(&a, &c, 1);
    *V.v.sel = 1;
  }
  else if (V.mode == VM_VLINE) {
    if (c.y >= a.y) {
      a = pos(a.y, 0);
      c = pos(c.y, llen(c.y));
    }
    else {
      a = pos(a.y, llen(a.y));
      c = pos(c.y, 0);
    }
    ved_cursors(&a, &c, 1);
    *V.v.sel = 1;
  }
  else {	/* a cursor on each line, the one Vim's cursor is on first */
    size_t y0 = a.y < c.y ? a.y : c.y, y1 = a.y < c.y ? c.y : a.y, y, cc = block_col();
    size_t c0 = V.vs_col < cc ? V.vs_col : cc, c1 = V.vs_col < cc ? cc : V.vs_col;
    int right = cc >= V.vs_col, n = 1;
    Pos *an = (Pos *)xmalloc((y1 - y0 + 2) * sizeof(Pos)), *cu = (Pos *)xmalloc((y1 - y0 + 2) * sizeof(Pos));
    for (y = y0; y <= y1; y++) {
      size_t x0 = ved_x(y, c0), x1 = V.vdollar ? llen(y) : ved_x(y, c1);
      int i;
      if (x1 < llen(y) && !V.vdollar) x1 = nextx(y, x1);
      if (x0 >= llen(y) && y != c.y) continue;
      i = y == c.y ? 0 : n++;
      an[i] = pos(y, right ? x0 : x1);
      cu[i] = pos(y, right ? x1 : x0);
    }
    ved_cursors(an, cu, n);
    *V.v.sel = 1;
    free(an);
    free(cu);
  }
}


/* the selection: a to b (exclusive), and its lines */
static void vis_range (Pos *a, Pos *b) {
  Pos s = V.vs, e = V.vc;
  if (pos_cmp(s, e) > 0) {
    s = V.vc;
    e = V.vs;
  }
  if (V.mode == VM_VLINE) {
    *a = pos(s.y, 0);
    *b = pos(e.y, llen(e.y));
    return;
  }
  *a = s;
  *b = after(e);
}


/* Visual block: its lines and columns */
static void vis_block (size_t *y0, size_t *y1, size_t *c0, size_t *c1) {
  size_t cc = block_col();
  *y0 = V.vs.y < V.vc.y ? V.vs.y : V.vc.y;
  *y1 = V.vs.y < V.vc.y ? V.vc.y : V.vs.y;
  *c0 = V.vs_col < cc ? V.vs_col : cc;
  *c1 = V.vs_col < cc ? cc : V.vs_col;
}


/* the bytes of line y in the block (x1 exclusive) */
static void block_x (size_t y, size_t c0, size_t c1, size_t *x0, size_t *x1) {
  *x0 = ved_x(y, c0);
  *x1 = V.vdollar ? llen(y) : ved_x(y, c1);
  if (*x1 < llen(y) && !V.vdollar) *x1 = nextx(y, *x1);
  if (*x0 > *x1) *x0 = *x1;
}


static void visual_end (void) {
  Pos a, b, c = V.vc;
  vis_range(&a, &b);
  V.last_vmode = V.mode;
  V.last_vs = V.vs;
  V.last_vc = V.vc;
  mark_set(&V.mk_lt, a);
  mark_set(&V.mk_gt, pos(b.y, b.x > 0 ? prevx(b.y, b.x) : 0));
  if (V.mode == VM_VLINE) mark_set(&V.mk_gt, pos(b.y, llen(b.y)));
  V.mode = VM_NORMAL;
  ved_cursors(&c, &c, 1);
  go(c);
}


/* d y c ~ u U r J > < = gc in Visual block mode */
static void block_op (int op, int arg, VCmd *c) {
  size_t y0, y1, c0, c1, y, x0, x1;
  Buf b;
  vis_block(&y0, &y1, &c0, &c1);
  if (op == 'D' || op == 'C' || op == 'X' || op == 'Y') V.vdollar = 1;
  if (op == 'y' || op == 'Y' || op == 'd' || op == 'x' || op == 'D' || op == 'X' || op == 'c' || op == 'C' || op == 's') {
    buf_init(&b);
    for (y = y0; y <= y1; y++) {
      block_x(y, c0, c1, &x0, &x1);
      buf_putn(&b, row(y)->s + x0, x1 - x0);
      if (y < y1) buf_putc(&b, '\n');
    }
    reg_set(c->reg, b.s ? b.s : "", b.len, RK_BLOCK, op == 'y' || op == 'Y' ? RW_YANK : RW_BIG);
    buf_free(&b);
    if (op != 'y' && op != 'Y') {
      grp();
      for (y = y1 + 1; y-- > y0;) {
        block_x(y, c0, c1, &x0, &x1);
        del(pos(y, x0), pos(y, x1));
      }
    }
  }
  else if (op == 'r' || op == '~' || op == 'u' || op == 'U') {
    grp();
    for (y = y0; y <= y1; y++) {
      block_x(y, c0, c1, &x0, &x1);
      if (op != 'r') case_range(pos(y, x0), pos(y, x1), op);
      else if (x1 > x0) {
        size_t chars = utf8_count(row(y)->s + x0, x1 - x0), k;
        char u[4];
        int w = utf8_encode((uint32_t)arg, u);
        buf_init(&b);
        for (k = 0; k < chars; k++) buf_putn(&b, u, (size_t)w);
        del(pos(y, x0), pos(y, x1));
        ins(pos(y, x0), b.s, b.len);
        buf_free(&b);
      }
    }
  }
  else if (op == '>' || op == '<') {
    grp();
    shift_lines(y0, y1, c->count ? c->count : 1, op == '>');
  }
  V.mode = VM_NORMAL;
  ved_cursors(V.v.cur, V.v.cur, 1);
  go(pos(y0, ved_x(y0, c0)));
  if (op == 'c' || op == 'C' || op == 's') {	/* typed on every line at once */
    Pos an[512];
    int k = 0;
    for (y = y0; y <= y1 && k < 512; y++)
      if (ved_col(y, llen(y)) >= c0 || y == y0) an[k++] = pos(y, ved_x(y, c0));
    ved_cursors(an, an, k);
    mode_insert('I', 1);
    V.ins_dot = 0;
    V.blk_home = an[0];
    V.blk = 1;
  }
}


/* I A in Visual block (and line) mode: a cursor on every line */
static void block_insert (int append) {
  size_t y0, y1, c0, c1, y;
  Pos an[512];
  int k = 0;
  grp();
  if (V.mode == VM_VLINE) {
    y0 = V.vs.y < V.vc.y ? V.vs.y : V.vc.y;
    y1 = V.vs.y < V.vc.y ? V.vc.y : V.vs.y;
    for (y = y0; y <= y1 && k < 512; y++) an[k++] = pos(y, append ? llen(y) : first_nb(y));
  }
  else {
    vis_block(&y0, &y1, &c0, &c1);
    for (y = y0; y <= y1 && k < 512; y++) {
      size_t x0, x1;
      block_x(y, c0, c1, &x0, &x1);
      if (!append) {
        if (ved_col(y, llen(y)) < c0 && y != y0) continue;	/* too short: not this one */
        an[k++] = pos(y, x0);
      }
      else if (V.vdollar) an[k++] = pos(y, llen(y));
      else {
        size_t have = ved_col(y, llen(y));
        if (have <= c1 && c1 < WANT_END) {	/* too short: spaces up to the block's right edge */
          Buf b;
          buf_init(&b);
          for (; have <= c1; have++) buf_putc(&b, ' ');
          ins(pos(y, llen(y)), b.s, b.len);
          buf_free(&b);
          x1 = llen(y);
        }
        an[k++] = pos(y, x1);
      }
    }
  }
  V.mode = VM_NORMAL;
  if (k == 0) return;
  ved_cursors(an, an, k);
  mode_insert(append ? 'A' : 'I', 1);
  V.blk_home = an[0];
  V.blk = 1;
  V.ins_dot = 0;
}


/* p P in Visual mode: the selection's place takes the register's text */
static void vis_put (VCmd *c) {
  const Reg *g = reg_get(c->reg);
  Reg keep;
  Pos a, b;
  int line = V.mode == VM_VLINE;
  if (g == NULL) {
    visual_end();
    say("E353: Nothing in register", NULL);
    return;
  }
  keep.s = xstrndup(g->s, g->n);
  keep.n = g->n;
  keep.kind = g->kind;
  vis_range(&a, &b);
  V.mode = VM_NORMAL;
  ved_cursors(V.v.cur, V.v.cur, 1);
  grp();
  {
    size_t n;
    char *t = line ? lines_text(a.y, b.y, &n) : text(a, b, &n);
    reg_set(c->key[0] == 'P' ? '_' : 0, t, n, line ? RK_LINE : RK_CHAR, line || a.y != b.y ? RW_BIG : RW_SMALL);
    free(t);
  }
  if (line) {
    size_t y = a.y;
    del_lines(a.y, b.y);
    if (keep.kind == RK_LINE) {
      if (y > last_y()) {
        ins(pos(last_y(), llen(last_y())), "\n", 1);
        y = last_y();
        ins(pos(y, 0), keep.s, keep.n - (keep.n && keep.s[keep.n - 1] == '\n'));
      }
      else ins(pos(y, 0), keep.s, keep.n);
    }
    else {
      if (y > last_y()) {
        ins(pos(last_y(), llen(last_y())), "\n", 1);
        y = last_y();
      }
      else ins(pos(y, 0), "\n", 1);
      ins(pos(y, 0), keep.s, keep.n);
    }
    go(pos(y, first_nb(y)));
  }
  else {
    del(a, b);
    if (keep.kind == RK_LINE) {	/* the lines go between the two halves */
      Pos e = ins(a, "\n", 1);
      e = ins(e, keep.s, keep.n);
      go(pos(a.y + 1, first_nb(a.y + 1)));
    }
    else {
      Pos e = ins(a, keep.s, keep.n);
      go(pos(e.y, e.x > 0 ? prevx(e.y, e.x) : 0));
    }
  }
  free(keep.s);
}

/* }================================================================== */


/*
** {==================================================================
** Normal mode's own commands
** ===================================================================
*/

static void join (size_t y, long n, int spaces) {
  long i;
  if (n < 2) n = 2;
  if (y >= last_y()) return;
  grp();
  for (i = 1; i < n && y < last_y(); i++) {
    size_t l0 = llen(y), ws = spaces ? first_nb(y + 1) : 0;
    int sp = 0;
    const Row *r = row(y);
    int endc = l0 ? (unsigned char)r->s[l0 - 1] : 0;
    int nextc = ws < llen(y + 1) ? (unsigned char)row(y + 1)->s[ws] : 0;
    del(pos(y, l0), pos(y + 1, ws));
    if (spaces && nextc && nextc != ')' && l0 > 0 && endc != ' ' && endc != '\t') {
      ins(pos(y, l0), " ", 1);
      sp = 1;
    }
    go(pos(y, l0 > 0 && !sp && nextc == 0 ? prevx(y, l0) : l0));
  }
}


/* u Ctrl+R: the cursor goes to where the step changed the text first, as Vim's */
static void undo_steps (int redo, long n) {
  while (n-- > 0) {
    UndoList *u = redo ? &DOC->redo : &DOC->undo;
    Pos at;
    size_t i;
    long g;
    if (u->n == 0) {
      say(redo ? "Already at newest change" : "Already at oldest change", NULL);
      return;
    }
    g = u->v[u->n - 1].group;
    at = u->v[u->n - 1].a;
    for (i = u->n; i-- > 0 && u->v[i].group == g;)
      if (pos_cmp(u->v[i].a, at) < 0) at = u->v[i].a;
    ved_command(redo ? CMD_REDO : CMD_UNDO);
    if (!ved_get(&V.v)) return;
    go(at);
  }
}


/* ~: the case of the characters under the cursor, and on */
static void tilde (long n) {
  Pos p = cur(), e = p;
  long i;
  if (llen(p.y) == 0) return;
  for (i = 0; i < n && e.x < llen(e.y); i++) e.x = nextx(e.y, e.x);
  grp();
  case_range(p, e, '~');
  go(pos(p.y, e.x < llen(p.y) ? e.x : lastx(p.y)));
}


/* r{c} */
static int replace_n (int c, long n) {
  Pos p = cur(), e = p;
  long i;
  Buf b;
  for (i = 0; i < n; i++) {
    if (e.x >= llen(e.y)) return 0;
    e.x = nextx(e.y, e.x);
  }
  grp();
  del(p, e);
  if (c == K_ENTER) {
    ins(p, "\n", 1);
    go(pos(p.y + 1, 0));
    return 1;
  }
  buf_init(&b);
  for (i = 0; i < n; i++) {
    char u[4];
    buf_putn(&b, u, (size_t)utf8_encode((uint32_t)c, u));
  }
  e = ins(p, b.s, b.len);
  buf_free(&b);
  go(pos(e.y, prevx(e.y, e.x)));
  return 1;
}


/* Ctrl+A Ctrl+X: the number at or after the cursor */
static void add_number (long d) {
  Pos p = cur();
  const Row *r = row(p.y);
  size_t x = p.x, s, e;
  long long v = 0;
  int hex = 0, neg = 0;
  char buf[48];
  while (x < r->len && x > 0 && r->s[x] >= '0' && r->s[x] <= '9' && r->s[x - 1] >= '0' && r->s[x - 1] <= '9') x--;
  while (x < r->len && !(r->s[x] >= '0' && r->s[x] <= '9')) x++;
  if (x >= r->len) return;
  s = x;
  if (r->s[x] == '0' && x + 1 < r->len && (r->s[x + 1] | 32) == 'x') {
    hex = 1;
    for (e = x + 2; e < r->len && strchr("0123456789abcdefABCDEF", r->s[e]); e++)
      v = v * 16 + (r->s[e] <= '9' ? r->s[e] - '0' : (r->s[e] | 32) - 'a' + 10);
  }
  else {
    if (s > 0 && r->s[s - 1] == '-') neg = 1, s--;
    for (e = x; e < r->len && r->s[e] >= '0' && r->s[e] <= '9'; e++) v = v * 10 + (r->s[e] - '0');
    if (neg) v = -v;
  }
  v += d;
  if (hex) snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
  else snprintf(buf, sizeof(buf), "%lld", v);
  grp();
  del(pos(p.y, s), pos(p.y, e));
  ins(pos(p.y, s), buf, strlen(buf));
  go(pos(p.y, s + strlen(buf) - 1));
}


/* Ctrl+D Ctrl+U Ctrl+F Ctrl+B Ctrl+E Ctrl+Y, zz zt zb */
static void scroll (int how, long n) {
  size_t rows = (size_t)V.v.rows, *top = V.v.top, maxtop = DOC->n > rows ? DOC->n - rows : 0;
  Pos p = cur();
  size_t half = rows / 2 ? rows / 2 : 1, page = rows > 2 ? rows - 2 : 1, d;
  switch (how) {
    case 'd': case 'u':
      d = n > 0 ? (size_t)n : half;
      if (how == 'd') {
        if (p.y >= last_y()) return;
        *top = *top + d < maxtop ? *top + d : maxtop;
        p.y = p.y + d < last_y() ? p.y + d : last_y();
      }
      else {
        if (p.y == 0) return;
        *top = *top > d ? *top - d : 0;
        p.y = p.y > d ? p.y - d : 0;
      }
      go(pos(p.y, first_nb(p.y)));
      break;
    case 'f': case 'b':
      d = page * (size_t)(n > 0 ? n : 1);
      if (how == 'f') {
        *top = *top + d < maxtop ? *top + d : maxtop;
        if (p.y < *top) p.y = *top;
        if (*top == maxtop) p.y = p.y + d < last_y() ? (p.y > *top ? p.y : *top) : last_y();
      }
      else {
        *top = *top > d ? *top - d : 0;
        if (p.y >= *top + rows) p.y = *top + rows - 1;
      }
      go(pos(p.y, first_nb(p.y)));
      break;
    case 'e': case 'y':
      d = n > 0 ? (size_t)n : 1;
      if (how == 'e') *top = *top + d < last_y() ? *top + d : last_y();
      else *top = *top > d ? *top - d : 0;
      if (p.y < *top) p.y = *top;
      if (p.y >= *top + rows) p.y = *top + rows - 1;
      if (p.y != cur().y) go(pos(p.y, ved_x(p.y, *V.v.want)));
      break;
    case 'z': *top = p.y > rows / 2 ? p.y - rows / 2 : 0; break;
    case 't': *top = p.y; break;
    case 'B': *top = p.y + 1 > rows ? p.y + 1 - rows : 0; break;
  }
}


/* za zc zo zR zM ... */
static void fold (int k) {
  size_t i;
  int folded = 0;
  for (i = 0; i < V.v.nfold; i++)
    if (V.v.fold[i] == cur().y) folded = 1;
  switch (k) {
    case 'a': case 'A': ved_command(folded ? CMD_UNFOLD : CMD_FOLD); break;
    case 'c': case 'C': ved_command(CMD_FOLD); break;
    case 'o': case 'O': case 'v': ved_command(CMD_UNFOLD); break;
    case 'R': ved_command(CMD_UNFOLD_ALL); break;
    case 'M': ved_command(CMD_FOLD_ALL); break;
  }
}


/* @{r}: the keys of a macro, pressed again */
static void play (int r, long count) {
  static int depth;
  Keys k;
  long i;
  size_t j;
  if (r == '@') r = V.last_mac;
  if (r >= 'A' && r <= 'Z') r += 32;
  if (r < 'a' || r > 'z' || depth > 20) return;
  V.last_mac = r;
  memset(&k, 0, sizeof(k));
  kcopy(&k, &V.mac[r - 'a']);
  depth++;
  V.playing++;
  for (i = 0; i < count; i++)
    for (j = 0; j < k.n; j++) ved_feed(k.k[j]);
  V.playing--;
  depth--;
  free(k.k);
  ved_get(&V.v);
}


/* q{r} and q: a macro starts, or ends */
static void record (int r) {
  if (V.rec) {
    Keys *m = &V.mac[V.rec - 'a'];
    Buf b;
    size_t i;
    if (m->n > 0) m->n--;	/* the q that stopped it */
    buf_init(&b);
    for (i = 0; i < m->n; i++)
      if (IS_TEXT(m->k[i])) {
        char u[4];
        buf_putn(&b, u, (size_t)utf8_encode((uint32_t)m->k[i], u));
      }
    reg_put(V.rec, b.s ? b.s : "", b.len, RK_CHAR);
    buf_free(&b);
    V.rec = 0;
    return;
  }
  if (r >= 'A' && r <= 'Z') r += 32;	/* q{A-Z} goes on with it */
  else if (r >= 'a' && r <= 'z') V.mac[r - 'a'].n = 0;
  if (r < 'a' || r > 'z') return;
  V.rec = r;
}

/* }================================================================== */


/*
** {==================================================================
** Ex commands: the : line
** ===================================================================
*/

static void run_ex (const char *s);

/* a line address: . $ 12 'a '< /pat/ with +n -n after it; 0 none there */
static int ex_addr (const char **sp, size_t *y) {
  const char *s = *sp;
  long v;
  int got = 1;
  if (*s == '.') v = (long)cur().y, s++;
  else if (*s == '$') v = (long)last_y(), s++;
  else if (*s >= '0' && *s <= '9') {
    v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    v--;
  }
  else if (*s == '\'' && s[1]) {
    Pos p;
    if (!mark_get(s[1], &p)) return -1;
    v = (long)p.y;
    s += 2;
  }
  else if (*s == '+' || *s == '-') v = (long)cur().y;
  else got = 0;
  while (got && (*s == '+' || *s == '-')) {
    int sign = *s++ == '+' ? 1 : -1;
    long k = 0;
    if (*s >= '0' && *s <= '9')
      while (*s >= '0' && *s <= '9') k = k * 10 + (*s++ - '0');
    else k = 1;
    v += sign * k;
  }
  if (!got) return 0;
  if (v < 0) v = 0;
  *y = (size_t)v > last_y() ? last_y() : (size_t)v;
  *sp = s;
  return 1;
}


/* the text of a pattern or replacement up to delimiter d: *sp past it */
static void ex_part (const char **sp, int d, char *out, size_t cap) {
  const char *s = *sp;
  size_t n = 0;
  while (*s && *s != d) {
    if (*s == '\\' && s[1] == d) s++;
    else if (*s == '\\' && s[1] && n + 2 < cap) out[n++] = *s++;
    if (n + 1 < cap) out[n++] = *s;
    s++;
  }
  out[n] = '\0';
  if (*s == d) s++;
  *sp = s;
}


/* :s's replacement for one match: & \0 \1 .. \9 \r \n \t \u \U \l \L \E */
static void sub_expand (Buf *b, const char *rep, const char *s, const size_t *cap) {
  int once = 0, all = 0;
  for (; *rep; rep++) {
    const char *t = NULL;
    size_t n = 0, i;
    char one;
    if (*rep == '\\' && rep[1]) {
      int c = *++rep;
      if (c >= '0' && c <= '9') {
        size_t g = (size_t)(c - '0');
        if (cap[2 * g] != (size_t)-1) t = s + cap[2 * g], n = cap[2 * g + 1] - cap[2 * g];
      }
      else if (c == 'r' || c == 'n') one = '\n', t = &one, n = 1;
      else if (c == 't') one = '\t', t = &one, n = 1;
      else if (c == 'u' || c == 'l') {
        once = c;
        continue;
      }
      else if (c == 'U' || c == 'L') {
        all = c;
        continue;
      }
      else if (c == 'E' || c == 'e') {
        all = 0;
        continue;
      }
      else one = (char)c, t = &one, n = 1;
    }
    else if (*rep == '&') t = s + cap[0], n = cap[1] - cap[0];
    else one = *rep, t = &one, n = 1;
    for (i = 0; i < n; i++) {
      int c = (unsigned char)t[i];
      int how = once ? once : all;
      if ((how == 'u' || how == 'U') && c >= 'a' && c <= 'z') c -= 32;
      if ((how == 'l' || how == 'L') && c >= 'A' && c <= 'Z') c += 32;
      buf_putc(b, (char)c);
      once = 0;
    }
  }
}


/* :s/pat/rep/flags over lines y0 .. y1 */
static void ex_sub (const char *s, size_t y0, size_t y1) {
  char pat[256], rep[256], re[300];
  int d, g = VO.gdef, icase, nosmart = 0, count_only = 0;
  size_t y, subs = 0, lines = 0, lasty = 0;
  const char *err;
  Regex *rx;
  if (*s && !(*s >= 'a' && *s <= 'z') && *s != ' ' && *s != '"' && *s != '|' && *s != '\\') {
    d = *s++;
    ex_part(&s, d, pat, sizeof(pat));
    ex_part(&s, d, rep, sizeof(rep));
    if (strcmp(rep, "~") == 0) snprintf(rep, sizeof(rep), "%s", V.sub_rep);
    if (pat[0] == '\0') snprintf(pat, sizeof(pat), "%s", V.pat[0] ? V.pat : V.sub_pat);
  }
  else {	/* :s alone, :&: the last one again */
    snprintf(pat, sizeof(pat), "%s", V.sub_pat);
    snprintf(rep, sizeof(rep), "%s", V.sub_rep);
    if (*s == '&') s++, g = V.sub_g;
  }
  if (pat[0] == '\0') {
    say("E35: No previous regular expression", NULL);
    return;
  }
  icase = case_off(pat, 0);
  for (; *s; s++) {
    if (*s == 'g') g = !g;
    else if (*s == 'i') icase = 1, nosmart = 1;
    else if (*s == 'I') icase = 0, nosmart = 1;
    else if (*s == 'n') count_only = 1;
    else if (*s == '&') g = V.sub_g;
  }
  (void)nosmart;
  snprintf(V.sub_pat, sizeof(V.sub_pat), "%s", pat);
  snprintf(V.sub_rep, sizeof(V.sub_rep), "%s", rep);
  snprintf(V.pat, sizeof(V.pat), "%s", pat);	/* n goes on with it */
  V.pat_back = 0;
  V.pat_nosmart = 0;
  V.sub_g = g;
  re_of_vim(pat, re, sizeof(re), &icase);
  rx = re_compile(re, icase, &err);
  if (rx == NULL) {
    say("E486: Pattern not found: %s", pat);
    return;
  }
  grp();
  for (y = y0; y <= y1 && y < DOC->n; y++) {
    const Row *r = row(y);
    size_t at = 0, a, b = 0, done = 0, nl, k;
    size_t cap[20];
    Buf out;
    int hit = 0;
    buf_init(&out);
    while (at <= r->len) {	/* an empty match counts too: :s/^/# / */
      for (a = at; a <= r->len; a = a < r->len ? nextx(y, a) : a + 1)
        if (re_at(rx, r->s, r->len, a, &b, cap)) break;
      if (a > r->len) break;
      buf_putn(&out, r->s + done, a - done);
      sub_expand(&out, rep, r->s, cap);
      done = b;
      hit = 1;
      subs++;
      if (!g || a >= r->len) break;
      at = b > a ? b : nextx(y, a);
    }
    if (!hit) {
      buf_free(&out);
      continue;
    }
    lines++;
    lasty = y;
    if (count_only) {
      buf_free(&out);
      continue;
    }
    buf_putn(&out, r->s + done, r->len - done);
    for (nl = 0, k = 0; k < out.len; k++) nl += out.s[k] == '\n';
    del(pos(y, 0), pos(y, r->len));
    if (out.len) ins(pos(y, 0), out.s, out.len);
    buf_free(&out);
    y += nl;
    y1 += nl;
    lasty = y;
  }
  re_free(rx);
  if (subs == 0) {
    say("E486: Pattern not found: %s", pat);
    return;
  }
  if (!count_only) go(pos(lasty, first_nb(lasty)));
  if (count_only || lines > 2) {
    snprintf(V.msg, sizeof(V.msg), "%lu match%s on %lu line%s", (unsigned long)subs, subs == 1 ? "" : "es",
             (unsigned long)lines, lines == 1 ? "" : "s");
    if (!count_only)
      snprintf(V.msg, sizeof(V.msg), "%lu substitution%s on %lu line%s", (unsigned long)subs, subs == 1 ? "" : "s",
               (unsigned long)lines, lines == 1 ? "" : "s");
  }
}


/* :set nu, :set nornu ... */
static void ex_set (const char *s) {
  char name[64];
  while (*s) {
    size_t n = 0;
    int no = 0, val = -1;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && n + 1 < sizeof(name)) name[n++] = *s++;
    name[n] = '\0';
    if (n == 0) break;
    if (strchr(name, '=')) {
      char *eq = strchr(name, '=');
      *eq = '\0';
      val = atoi(eq + 1);
    }
    if (strncmp(name, "no", 2) == 0 && strcmp(name, "number") != 0) no = 1;
    if (name[strlen(name) - 1] == '!') name[strlen(name) - 1] = '\0', no = 2;
#define IS(a, b)	(strcmp(name + (no == 1 ? 2 : 0), a) == 0 || strcmp(name + (no == 1 ? 2 : 0), b) == 0)
    if (IS("nu", "number")) {
      int on = no == 2 ? !opt.line_numbers : !no;
      opt.line_numbers = on || eopt.line_nums == 2;
      if (eopt.line_nums != 2) eopt.line_nums = 1;
    }
    else if (IS("rnu", "relativenumber")) {
      int on = no == 2 ? eopt.line_nums != 2 : !no;
      eopt.line_nums = on ? 2 : 1;
      if (on) opt.line_numbers = 1;
    }
    else if (IS("hls", "hlsearch")) {
      VO.hls = no == 2 ? !VO.hls : !no;
      V.lit = VO.hls && V.pat[0];
    }
    else if (IS("ic", "ignorecase")) VO.icase = no == 2 ? !VO.icase : !no;
    else if (IS("scs", "smartcase")) VO.scase = no == 2 ? !VO.scase : !no;
    else if (IS("is", "incsearch")) VO.incs = no == 2 ? !VO.incs : !no;
    else if (IS("gd", "gdefault")) VO.gdef = no == 2 ? !VO.gdef : !no;
    else if (IS("et", "expandtab")) DOC->tabs = no == 2 ? !DOC->tabs : no;
    else if (IS("ts", "tabstop") && val > 0 && val <= 16) opt.tab_size = val;
    else if (IS("sw", "shiftwidth") && val > 0 && val <= 16) DOC->indent = val;
    else {
      say("E518: Unknown option: %s", name);
      return;
    }
#undef IS
  }
}


/* a file named on the : line: from the folder of the file in front */
static char *ex_path (const char *arg) {
  char *dir, *p;
  if (path_is_sep(arg[0]) || (arg[0] && arg[1] == ':') || V.v.path == NULL) return xstrdup(arg);	/* already whole */
  dir = path_dirname(V.v.path);
  p = path_join(dir, arg);
  free(dir);
  return p;
}


/* :w file: the text written there, the file in front stays what it was */
static void write_to (const char *arg) {
  char *f = ex_path(arg);
  size_t n;
  char *t = flat(&n);
  int fd = os_open(f, OS_WRITE);
  if (fd < 0) say("E212: Can't open file for writing", NULL);
  else {
    os_write(fd, t, n);
    if (n == 0 || t[n - 1] != '\n') os_write(fd, "\n", 1);
    os_close(fd);
    snprintf(V.msg, sizeof(V.msg), "\"%s\" %luL written", arg, (unsigned long)DOC->n);
  }
  free(t);
  free(f);
}


static int is_cmd (const char *name, const char *full, size_t min) {
  size_t n = strlen(name);
  return n >= min && n <= strlen(full) && strncmp(name, full, n) == 0;
}


static void run_ex (const char *s) {
  size_t y0 = cur().y, y1 = y0, n;
  int ranged = 0, bang = 0;
  char name[32], arg[256];
  while (*s == ':' || *s == ' ') s++;
  if (*s == '%') {
    y0 = 0;
    y1 = last_y();
    ranged = 2;
    s++;
  }
  else {
    int r = ex_addr(&s, &y0);
    if (r < 0) {
      say("E20: Mark not set", NULL);
      return;
    }
    if (r > 0) {
      y1 = y0;
      ranged = 1;
      if (*s == ',' || *s == ';') {
        s++;
        if (ex_addr(&s, &y1) < 0) {
          say("E20: Mark not set", NULL);
          return;
        }
        ranged = 2;
      }
    }
  }
  if (y1 < y0) {
    size_t t = y0;
    y0 = y1;
    y1 = t;
  }
  while (*s == ' ') s++;
  n = 0;
  if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))
    while (((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) && n + 1 < sizeof(name)) {
      name[n++] = *s++;
      if (n == 1 && name[0] == 's' && !((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))) break;	/* :s/ */
    }
  else if (*s && strchr("&<>", *s)) name[n++] = *s++;
  name[n] = '\0';
  if (*s == '!') bang = 1, s++;
  while (*s == ' ') s++;
  snprintf(arg, sizeof(arg), "%s", s);
  n = strlen(arg);
  while (n > 0 && arg[n - 1] == ' ') arg[--n] = '\0';
  if (name[0] == '\0' && *s == '\0') {	/* :12 */
    if (ranged) {
      mark_set(&V.mk_jump, cur());
      go(pos(y1, first_nb(y1)));
    }
    return;
  }
  if (is_cmd(name, "write", 1)) {
    if (arg[0]) write_to(arg);
    else ved_command(CMD_SAVE);
  }
  else if (is_cmd(name, "wall", 2)) editor_save_all();
  else if (is_cmd(name, "quit", 1) || is_cmd(name, "close", 3)) {
    if (bang && V.v.path) ved_command(CMD_REVERT);
    ved_command(CMD_CLOSE);
  }
  else if (is_cmd(name, "qall", 2) || is_cmd(name, "quitall", 5)) ved_command(CMD_QUIT);
  else if (is_cmd(name, "wq", 2) || is_cmd(name, "xit", 1) || is_cmd(name, "exit", 3)) {
    if (name[0] == 'w' || doc_dirty(DOC)) ved_command(CMD_SAVE);
    if (ved_get(&V.v) && !doc_dirty(DOC)) ved_command(CMD_CLOSE);
  }
  else if (is_cmd(name, "wqall", 3) || is_cmd(name, "xall", 2)) {
    editor_save_all();
    ved_command(CMD_QUIT);
  }
  else if (is_cmd(name, "edit", 1)) {
    if (arg[0]) {
      char *f = ex_path(arg);
      ved_open(f);
      free(f);
    }
    else if (bang) ved_command(CMD_REVERT);
  }
  else if (is_cmd(name, "enew", 3)) ved_command(CMD_NEW);
  else if (is_cmd(name, "split", 2) || is_cmd(name, "new", 3) || is_cmd(name, "vsplit", 2) || is_cmd(name, "vnew", 3)) {
    int vert = name[0] == 'v';
    ved_command(vert ? CMD_SPLIT : CMD_SPLIT_DOWN);
    if (name[strlen(name) - 1] == 'w' && strcmp(name, "new") == 0) ved_command(CMD_NEW);
    else if (name[0] == 'v' && name[1] == 'n') ved_command(CMD_NEW);
    else if (arg[0] && ved_get(&V.v)) {
      char *f = ex_path(arg);
      ved_open(f);
      free(f);
    }
  }
  else if (is_cmd(name, "tabnext", 4) || is_cmd(name, "bnext", 2) || is_cmd(name, "tabprevious", 4) ||
           is_cmd(name, "tabNext", 4) || is_cmd(name, "bprevious", 2) || is_cmd(name, "bNext", 2)) {
    int back = name[1] == 'p' || name[1] == 'N' || name[3] == 'p' || name[3] == 'N';
    V.playing++;
    ved_feed((back ? K_PGUP : K_PGDN) | KM_CTRL);
    V.playing--;
  }
  else if (is_cmd(name, "tabedit", 4) || is_cmd(name, "tabnew", 4)) {
    if (arg[0]) {
      char *f = ex_path(arg);
      ved_open(f);
      free(f);
    }
    else ved_command(CMD_NEW);
  }
  else if (is_cmd(name, "tabclose", 4)) ved_command(CMD_CLOSE);
  else if (is_cmd(name, "nohlsearch", 3)) V.lit = 0;
  else if (is_cmd(name, "set", 2)) ex_set(arg);
  else if (is_cmd(name, "substitute", 1) || strcmp(name, "&") == 0) {
    if (strcmp(name, "&") == 0) arg[0] = '\0';
    ex_sub(strcmp(name, "&") == 0 && bang ? "&" : arg, y0, y1);
  }
  else if (is_cmd(name, "delete", 1) || is_cmd(name, "yank", 1)) {
    VCmd c;
    memset(&c, 0, sizeof(c));
    c.op = name[0] == 'd' ? 'd' : 'y';
    c.kind = CK_LINES;
    if (arg[0] && reg_ok(arg[0])) c.reg = arg[0];
    apply(&c, pos(y0, 0), pos(y1, 0), MK_LINE);
  }
  else if (is_cmd(name, "join", 1)) join(y0, ranged == 2 ? (long)(y1 - y0 + 1) : 2, !bang);
  else if (strcmp(name, ">") == 0 || strcmp(name, "<") == 0) {
    grp();
    shift_lines(y0, y1, 1, name[0] == '>');
  }
  else if (is_cmd(name, "undo", 1)) ved_command(CMD_UNDO);
  else if (is_cmd(name, "redo", 3)) ved_command(CMD_REDO);
  else if (is_cmd(name, "sort", 3)) {
    if (!ranged) y0 = 0, y1 = last_y();
    lines_command(y0, y1, bang ? CMD_SORT_DESC : CMD_SORT_ASC);
  }
  else if (is_cmd(name, "copy", 2) || strcmp(name, "t") == 0 || is_cmd(name, "move", 1)) {
    const char *a = arg;
    size_t to;
    char *t;
    int mv = name[0] == 'm';
    if (strcmp(arg, "0") == 0) to = (size_t)-1;
    else if (ex_addr(&a, &to) <= 0) {
      say("E14: Invalid address", NULL);
      return;
    }
    if (mv && to != (size_t)-1 && to >= y0 && to <= y1) return;
    grp();
    t = lines_text(y0, y1, &n);
    if (to == (size_t)-1) ins(pos(0, 0), t, n);
    else if (to == last_y()) {
      ins(pos(to, llen(to)), "\n", 1);
      ins(pos(to + 1, 0), t, n - 1);
    }
    else ins(pos(to + 1, 0), t, n);
    if (mv) {
      size_t k = y1 - y0 + 1;
      if (to == (size_t)-1 || to < y0) del_lines(y0 + k, y1 + k);
      else del_lines(y0, y1);
      to = to == (size_t)-1 ? k - 1 : (to < y0 ? to + k : to);
      go(pos(to, first_nb(to)));
    }
    else go(pos(to == (size_t)-1 ? y1 - y0 : to + (y1 - y0) + 1, 0));
    free(t);
  }
  else {
    say("E492: Not an editor command: %s", name[0] ? name : s);
  }
}

/* }================================================================== */


/*
** {==================================================================
** The line on the status bar: : / ?
** ===================================================================
*/

static void prompt_open (int kind, const char *init) {
  V.prompt = kind == ':' ? VP_EX : VP_SEARCH;
  V.p_kind = kind;
  snprintf(V.line, sizeof(V.line), "%s", init ? init : "");
  V.p_cur = cur();
  V.p_top = *V.v.top;
  V.p_vis = IS_VISUAL;
}


/* incsearch: where the search typed so far would go */
static void search_preview (void) {
  Pos f;
  if (V.line[0] == '\0') {
    V.lit = 0;
    *V.v.cur = V.p_cur;
    *V.v.top = V.p_top;
    return;
  }
  find_set(V.line, 0);
  V.lit = 1;
  if (search_from(V.p_cur, V.p_kind == '?', 1, &f)) *V.v.cur = f;
  else *V.v.cur = V.p_cur;
  if (V.p_vis) V.vc = *V.v.cur;
}


static void exec (VCmd *c);
static void vis_exec (VCmd *c);

/* Enter on the search line: the cursor to the match, or the operator to it */
static void search_done (void) {
  Pos f;
  long count = V.p_cmd.count ? V.p_cmd.count : 1;
  if (V.line[0]) {
    snprintf(V.pat, sizeof(V.pat), "%s", V.line);
    reg_put('/', V.pat, strlen(V.pat), RK_CHAR);
  }
  V.pat_back = V.p_kind == '?';
  V.pat_nosmart = 0;
  *V.v.cur = V.p_cur;
  if (V.pat[0] == '\0') {
    say("E35: No previous regular expression", NULL);
    return;
  }
  find_set(V.pat, 0);
  V.lit = VO.hls;
  if (!search_from(V.p_cur, V.pat_back, count, &f)) {
    say("E486: Pattern not found: %s", V.pat);
    return;
  }
  if (V.p_op) {	/* d/foo: exclusive, up to the match */
    VCmd c = V.p_cmd;
    Pos a = V.p_cur, b = f;
    if (pos_cmp(b, a) < 0) {
      Pos t = a;
      a = b;
      b = t;
    }
    c.kind = CK_SEARCH;
    apply(&c, a, b, MK_EXCL);
    return;
  }
  mark_set(&V.mk_jump, V.p_cur);
  if (V.p_vis) V.vc = f;
  else go(f);
}


static void prompt_key (int k) {
  size_t n = strlen(V.line);
  int code = KEY_CODE(k);
  if (code == K_ESC || k == CTRL('c') || (code == K_BS && n == 0)) {
    V.prompt = VP_NONE;
    if (V.p_kind != ':') {
      V.lit = VO.hls && V.pat[0];
      if (V.lit) find_set(V.pat, V.pat_nosmart);
      *V.v.cur = V.p_cur;
      *V.v.top = V.p_top;
      if (V.p_vis) V.vc = V.p_cur;
    }
    return;
  }
  if (code == K_ENTER) {
    char line[256];
    int kind = V.p_kind;
    V.prompt = VP_NONE;
    snprintf(line, sizeof(line), "%s", V.line);
    if (kind == ':') {
      if (line[0]) snprintf(V.last_ex, sizeof(V.last_ex), "%s", line);
      reg_put(':', line, strlen(line), RK_CHAR);
      if (V.p_vis) {	/* '<,'> is the selection's lines */
        visual_end();
      }
      run_ex(line);
    }
    else search_done();
    return;
  }
  if (code == K_UP && V.p_kind == ':') snprintf(V.line, sizeof(V.line), "%s", V.last_ex);
  else if (code == K_UP && V.p_kind != ':') snprintf(V.line, sizeof(V.line), "%s", V.pat);
  else if (code == K_BS) {
    while (n > 0 && ((unsigned char)V.line[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) V.line[n - 1] = '\0';
  }
  else if (k == CTRL('u')) V.line[0] = '\0';
  else if (k == CTRL('w')) {
    while (n > 0 && V.line[n - 1] == ' ') n--;
    while (n > 0 && V.line[n - 1] != ' ') n--;
    V.line[n] = '\0';
  }
  else if (code == K_PASTE) {
    Buf b;
    size_t i;
    buf_init(&b);
    term_paste(&b);
    for (i = 0; i < b.len && b.s[i] != '\n' && n + 1 < sizeof(V.line); i++) V.line[n++] = b.s[i];
    V.line[n] = '\0';
    buf_free(&b);
  }
  else if (IS_TEXT(k) && n + 5 < sizeof(V.line)) {
    n += (size_t)utf8_encode((uint32_t)k, V.line + n);
    V.line[n] = '\0';
  }
  else return;
  if (V.p_kind != ':' && VO.incs) search_preview();
}

/* }================================================================== */


/*
** {==================================================================
** Reading a command as it is typed
** ===================================================================
*/

/* a count: digits (the first not 0); i after them */
static int count_at (const int *k, int n, int i, long *count) {
  long v = 0;
  if (i < n && k[i] >= '1' && k[i] <= '9')
    while (i < n && k[i] >= '0' && k[i] <= '9') {
      if (v < 100000) v = v * 10 + (k[i] - '0');
      i++;
    }
  if (v) *count = *count ? *count * v : v;
  return i;
}


/* an operator at i: the keys it took, 0 more wanted, -1 not one */
static int op_at (const int *k, int n, int i, int *op) {
  if (k[i] < 128 && k[i] > 0 && strchr("dcy<>=", k[i])) {
    *op = k[i];
    return 1;
  }
  if (k[i] == 'g') {
    if (i + 1 == n) return 0;
    if (k[i + 1] > 0 && k[i + 1] < 128 && strchr("uU~?c", k[i + 1])) {
      *op = OP_G(k[i + 1]);
      return 2;
    }
  }
  return -1;
}


/* a motion at i: 1 whole, 0 more keys wanted, -1 not one */
static int mot_at (const int *k, int n, int i, VCmd *c) {
  int a = k[i];
  c->kind = CK_MOTION;
  c->key[0] = a;
  c->nkey = 1;
  if (a == K_LEFT || a == K_RIGHT || a == K_UP || a == K_DOWN || a == K_HOME || a == K_END || a == K_ENTER || a == K_BS)
    return 1;
  if (a <= 0 || a >= 128) return -1;
  if (strchr("hjklwbeWBE0^$GHML{}%;,nN*#_+-| ", a)) return 1;
  if (strchr("fFtT`'", a)) {
    if (i + 1 == n) return 0;
    if (!IS_TEXT(k[i + 1]) && k[i + 1] != K_TAB) return -1;
    c->arg = k[i + 1] == K_TAB ? '\t' : k[i + 1];
    return 1;
  }
  if (a == 'g') {
    if (i + 1 == n) return 0;
    if (k[i + 1] > 0 && k[i + 1] < 128 && strchr("geE_jk0$m*#", k[i + 1])) {
      c->key[1] = k[i + 1];
      c->nkey = 2;
      return 1;
    }
  }
  return -1;
}


/* the keys typed so far: 1 a whole command in c, 0 more wanted, -1 none */
static int parse (const int *k, int n, VCmd *c, int visual) {
  int i = 0, r, op = 0;
  memset(c, 0, sizeof(*c));
  if (k[0] == '"') {
    if (n < 2) return 0;
    if (!reg_ok(k[1])) return -1;
    c->reg = k[1];
    i = 2;
  }
  i = count_at(k, n, i, &c->count);
  if (i == n) return 0;
  if (!visual) {
    r = op_at(k, n, i, &op);
    if (r == 0) return 0;
    if (r > 0) {
      c->op = op;
      i = count_at(k, n, i + r, &c->count);
      if (i == n) return 0;
      if ((op < 0x100 && k[i] == op) || (op >= 0x100 && k[i] == (op & 0xFF))) {	/* dd gcc gUU */
        c->kind = CK_LINES;
        c->key[0] = k[i];
        c->nkey = 1;
        return i + 1 == n ? 1 : -1;
      }
      if (op >= 0x100 && k[i] == 'g') {	/* gUgU */
        if (i + 1 == n) return 0;
        if (k[i + 1] == (op & 0xFF)) {
          c->kind = CK_LINES;
          return 1;
        }
      }
      if (k[i] == 'i' || k[i] == 'a') {
        if (i + 1 == n) return 0;
        if (k[i + 1] <= 0 || k[i + 1] >= 128 || !strchr("wWp\"'`()b{}B[]<>t", k[i + 1])) return -1;
        c->kind = CK_OBJECT;
        c->key[0] = k[i];
        c->key[1] = k[i + 1];
        c->nkey = 2;
        return 1;
      }
      if (k[i] == '/' || k[i] == '?') {
        c->kind = CK_SEARCH;
        c->key[0] = k[i];
        c->nkey = 1;
        return 1;
      }
      r = mot_at(k, n, i, c);
      return r;
    }
  }
  else if (k[i] == 'i' || k[i] == 'a') {	/* a text object grows the selection */
    if (i + 1 == n) return 0;
    if (k[i + 1] > 0 && k[i + 1] < 128 && strchr("wWp\"'`()b{}B[]<>t", k[i + 1])) {
      c->kind = CK_OBJECT;
      c->key[0] = k[i];
      c->key[1] = k[i + 1];
      c->nkey = 2;
      return 1;
    }
    return -1;
  }
  r = mot_at(k, n, i, c);
  if (r >= 0) return r;
  c->kind = CK_ACTION;
  c->key[0] = k[i];
  c->nkey = 1;
  if (k[i] > 0 && k[i] < 128 && strchr("rmq@zZg", k[i]) && !(k[i] == 'q' && V.rec)) {	/* two keys */
    if (i + 1 == n) return 0;
    c->key[1] = k[i + 1];
    c->nkey = 2;
    c->arg = k[i + 1];
    if (k[i] == 'g' && (k[i + 1] <= 0 || k[i + 1] >= 128 || !strchr("dDhtTJvipPIc~uU?", k[i + 1]))) return -1;
    if (k[i] == 'z' && k[i + 1] != K_ENTER && (k[i + 1] <= 0 || k[i + 1] >= 128 || !strchr("ztbacoCOAvRM.-", k[i + 1])))
      return -1;
    if (k[i] == 'Z' && k[i + 1] != 'Z' && k[i + 1] != 'Q') return -1;
    if ((k[i] == 'q' || k[i] == '@') && !(reg_ok(k[i + 1]) || k[i + 1] == '@' || k[i + 1] == ':')) return -1;
    if (k[i] == 'm' && !((k[i + 1] >= 'a' && k[i + 1] <= 'z') || k[i + 1] == '<' || k[i + 1] == '>' || k[i + 1] == '\'' || k[i + 1] == '`'))
      return -1;
    if (k[i] == 'r' && !IS_TEXT(k[i + 1]) && k[i + 1] != K_ENTER && k[i + 1] != K_TAB) return -1;
    if (k[i] == 'r' && k[i + 1] == K_TAB) c->arg = '\t';
    return i + 2 == n ? 1 : -1;
  }
  return i + 1 == n ? 1 : -1;
}

/* }================================================================== */


/*
** {==================================================================
** Running a command
** ===================================================================
*/

/* the change "." repeats */
static void dot_keep (const VCmd *c) {
  if (V.dotting) return;
  V.dot = *c;
  V.dot_set = 1;
  V.dot_vmode = 0;
  V.dot_ins.n = 0;
}


static void dot (long count) {
  VCmd c;
  size_t k;
  if (!V.dot_set) return;
  c = V.dot;
  if (count) c.count = count;
  V.dotting = 1;
  if (V.dot_vmode) {	/* the same size of selection, from the cursor */
    Pos p = cur();
    V.mode = V.dot_vmode;
    V.vs = p;
    if (V.dot_vmode == VM_VLINE) V.vc = pos(p.y + (size_t)V.dot_vlines <= last_y() ? p.y + (size_t)V.dot_vlines : last_y(), 0);
    else {
      Pos e = p;
      long i;
      for (i = 0; i < V.dot_vcols && e.x < lastx(e.y); i++) e.x = nextx(e.y, e.x);
      V.vc = e;
    }
    V.vs_col = ved_col(V.vs.y, V.vs.x);
    vis_exec(&c);
  }
  else exec(&c);
  if (V.mode == VM_INSERT || V.mode == VM_REPLACE) {	/* what was typed with it */
    for (k = 0; k < V.dot_ins.n; k++) ins_key(V.dot_ins.k[k], 0);
    ins_end();
  }
  V.dotting = 0;
}


static void exec (VCmd *c) {
  long n = c->count ? c->count : 1;
  int k0 = c->key[0], k1 = c->nkey > 1 ? c->key[1] : 0;
  Pos p = cur();
  if (c->kind == CK_SEARCH) {	/* d/foo: the search line first ("." searches for it again) */
    prompt_open(k0, NULL);
    V.p_cmd = *c;
    V.p_op = 1;
    if (V.dotting) {
      V.prompt = VP_NONE;
      search_done();
      return;
    }
    dot_keep(c);
    return;
  }
  if (c->op) {
    if (c->op != 'y') dot_keep(c);
    do_op(c);
    return;
  }
  if (c->kind == CK_MOTION) {
    Mot m;
    size_t w = *V.v.want;
    if (!motion(c, p, &m)) return;
    if (m.jump) mark_set(&V.mk_jump, p);
    go(m.to);
    if (vertical(c)) *V.v.want = w;	/* j k keep the column they aim for */
    if (k0 == '$' || k0 == K_END) *V.v.want = WANT_END;
    return;
  }
  switch (k0) {
    case 'x': case K_DEL: case 'X': case 's': case 'D': case 'C': case 'S': case 'Y': {
      VCmd o = *c;
      static const char from[] = "xXsDCSY", op[] = "ddcdccy", mot[] = "lh$$$$$";
      const char *at = k0 == K_DEL ? from : strchr(from, k0);
      o.op = op[at - from];
      o.key[0] = mot[at - from];
      o.nkey = 1;
      o.kind = CK_MOTION;
      if (k0 == 'S' || k0 == 'Y') o.kind = CK_LINES, o.key[0] = o.op;
      if ((k0 == 'x' || k0 == K_DEL || k0 == 's') && llen(p.y) == 0) {
        if (k0 == 's') {
          grp();
          mode_insert('s', 1);
          dot_keep(c);
        }
        return;
      }
      if (k0 != 'Y') dot_keep(c);
      do_op(&o);
      return;
    }
    case 'i': case 'a': case 'I': case 'A': case K_INS:
      grp();
      if (k0 == 'a' && llen(p.y) > 0) go(pos(p.y, nextx(p.y, p.x)));
      else if (k0 == 'I') go(pos(p.y, first_nb(p.y)));
      else if (k0 == 'A') go(pos(p.y, llen(p.y)));
      dot_keep(c);
      mode_insert(k0 == K_INS ? 'i' : k0, n);
      return;
    case 'o': case 'O':
      grp();
      open_line(k0 == 'O');
      dot_keep(c);
      mode_insert(k0, n);
      return;
    case 'R':
      grp();
      dot_keep(c);
      mode_insert('R', 1);
      return;
    case 'p': case 'P':
      dot_keep(c);
      put(c->reg, n, k0 == 'P', 0);
      return;
    case 'u': case CTRL('r'): undo_steps(k0 != 'u', n); return;
    case 'J': dot_keep(c); join(p.y, n, 1); return;
    case '~': dot_keep(c); tilde(n); return;
    case '.': dot(c->count); return;
    case 'r': if (replace_n(c->arg, n)) dot_keep(c); return;
    case 'v': visual_start(VM_VISUAL); return;
    case 'V': visual_start(VM_VLINE); return;
    case CTRL('v'): visual_start(VM_VBLOCK); return;
    case ':': prompt_open(':', c->count ? (c->count > 1 ? ".,.+" : ".") : NULL);
      if (c->count > 1) snprintf(V.line + strlen(V.line), sizeof(V.line) - strlen(V.line), "%ld", c->count - 1);
      return;
    case '/': case '?':
      prompt_open(k0, NULL);
      memset(&V.p_cmd, 0, sizeof(V.p_cmd));
      V.p_cmd.count = c->count;
      V.p_op = 0;
      return;
    case '&': run_ex("s"); return;
    case 'm':
      if (mark_of(k1)) mark_set(mark_of(k1), p);
      return;
    case 'q': record(k1); return;
    case '@':
      if (k1 == ':') {
        if (V.last_ex[0]) run_ex(V.last_ex);
        return;
      }
      play(k1, n);
      return;
    case 'z':
      switch (k1) {
        case 'z': case '.': scroll('z', 0); break;
        case 't': case K_ENTER: scroll('t', 0); break;
        case 'b': case '-': scroll('B', 0); break;
        default: fold(k1); return;
      }
      if (k1 == '.' || k1 == K_ENTER || k1 == '-') go(pos(p.y, first_nb(p.y)));
      return;
    case 'Z': run_ex(k1 == 'Z' ? "x" : "q!"); return;
    case 'g':
      switch (k1) {
        case 'd': case 'D': ved_command(CMD_DEFINITION); break;
        case 'h': ved_command(CMD_HOVER); break;
        case 't': case 'T':
          V.playing++;
          ved_feed((k1 == 'T' ? K_PGUP : K_PGDN) | KM_CTRL);
          V.playing--;
          break;
        case 'J': dot_keep(c); join(p.y, n, 0); break;
        case 'v':
          if (V.last_vmode) {
            V.mode = V.last_vmode;
            V.vs = V.last_vs;
            V.vc = V.last_vc;
            if (V.vs.y > last_y()) V.vs.y = last_y();
            if (V.vc.y > last_y()) V.vc.y = last_y();
            V.vs_col = ved_col(V.vs.y, V.vs.x);
            *V.v.want = ved_col(V.vc.y, V.vc.x);
            *V.v.cur = V.vc;
          }
          break;
        case 'i': {
          Pos q;
          grp();
          if (mark_get('^', &q)) go(q);
          dot_keep(c);
          mode_insert('i', n);
          break;
        }
        case 'I':
          grp();
          go(pos(p.y, 0));
          dot_keep(c);
          mode_insert('i', n);
          break;
        case 'p': case 'P':
          dot_keep(c);
          put(c->reg, n, k1 == 'P', 1);
          break;
      }
      return;
    case CTRL('d'): case CTRL('u'): scroll(k0 == CTRL('d') ? 'd' : 'u', c->count); return;
    case CTRL('f'): case K_PGDN: scroll('f', c->count); return;
    case CTRL('b'): case K_PGUP: scroll('b', c->count); return;
    case CTRL('e'): scroll('e', c->count); return;
    case CTRL('y'): scroll('y', c->count); return;
    case CTRL('a'): dot_keep(c); add_number(n); return;
    case CTRL('x'): dot_keep(c); add_number(-n); return;
    case CTRL('o'): ved_command(CMD_NAV_BACK); return;
    case K_TAB: ved_command(CMD_NAV_FORWARD); return;
  }
}


/* a command in Visual mode: a motion moves its end, the rest act on it */
static void vis_exec (VCmd *c) {
  int k0 = c->key[0], k1 = c->nkey > 1 ? c->key[1] : 0, vm = V.mode;
  Pos a, b;
  if (c->kind == CK_MOTION) {
    Mot m;
    VCmd t = *c;
    *V.v.cur = V.vc;
    t.op = 0;
    if (!motion(&t, V.vc, &m)) return;
    if (m.jump) mark_set(&V.mk_jump, V.vc);
    V.vc = m.to;
    if (V.mode != VM_VBLOCK && V.vc.x >= llen(V.vc.y) && llen(V.vc.y) > 0 && k0 != '$' && k0 != K_END)
      V.vc.x = lastx(V.vc.y);
    if (!vertical(c)) *V.v.want = ved_col(V.vc.y, V.vc.x);
    V.vdollar = V.vdollar && vertical(c);
    if (k0 == '$' || k0 == K_END) {
      *V.v.want = WANT_END;
      if (V.mode == VM_VBLOCK) V.vdollar = 1;
      else if (llen(V.vc.y) > 0) V.vc.x = lastx(V.vc.y);
    }
    return;
  }
  if (c->kind == CK_OBJECT) {
    int kind;
    Pos s, e;
    *V.v.cur = V.vc;
    if (!object(c, &s, &e, &kind)) return;
    if (kind == MK_LINE) {
      if (V.mode == VM_VISUAL) V.mode = VM_VLINE;
      V.vs = pos(pos_cmp(V.vs, V.vc) != 0 && V.vs.y < s.y ? V.vs.y : s.y, 0);
      V.vc = pos(e.y, 0);
    }
    else {
      if (pos_cmp(V.vs, V.vc) == 0) V.vs = s;
      V.vc = pos_cmp(e, s) > 0 ? (e.x > 0 ? pos(e.y, prevx(e.y, e.x)) : pos(e.y - 1, llen(e.y - 1))) : s;
    }
    *V.v.want = ved_col(V.vc.y, V.vc.x);
    return;
  }
  if (!V.dotting && strchr("dxXDcsCSRy~uUJr<>=p", k0 < 128 ? k0 : ' ') && k0 != 'y') {	/* "." does it again, the same size */
    Pos s = V.vs, e = V.vc;
    if (pos_cmp(s, e) > 0) s = V.vc, e = V.vs;
    dot_keep(c);
    V.dot_vmode = vm;
    V.dot_vlines = (long)(e.y - s.y);
    V.dot_vcols = s.y == e.y ? (long)utf8_count(row(s.y)->s + s.x, e.x - s.x) : 0;
  }
  if (k0 == 'g' && k1 == 'c') {	/* gc: the lines' comments */
    size_t y0 = V.vs.y < V.vc.y ? V.vs.y : V.vc.y, y1 = V.vs.y < V.vc.y ? V.vc.y : V.vs.y;
    visual_end();
    lines_command(y0, y1, CMD_COMMENT);
    go(pos(y0, first_nb(y0)));
    return;
  }
  if (vm == VM_VBLOCK && k0 < 128 && k0 > 0 && strchr("dxXDyYcCsr~uU<>", k0)) {
    visual_end();
    V.mode = VM_VBLOCK;	/* block_op reads the block as it was */
    block_op(k0, c->arg, c);
    return;
  }
  if (vm == VM_VBLOCK && k0 == 'g' && (k1 == 'u' || k1 == 'U' || k1 == '~')) {
    visual_end();
    V.mode = VM_VBLOCK;
    block_op(k1, 0, c);
    return;
  }
  switch (k0) {
    case K_ESC: visual_end(); return;
    case 'v': case 'V': case CTRL('v'): {
      int want = k0 == 'v' ? VM_VISUAL : k0 == 'V' ? VM_VLINE : VM_VBLOCK;
      if (want == V.mode) visual_end();
      else V.mode = want;
      return;
    }
    case 'o': case 'O': {
      Pos t = V.vs;
      if (k0 == 'O' && V.mode == VM_VBLOCK) {	/* the other corner, in this line */
        size_t cc = block_col();
        *V.v.want = V.vs_col;
        V.vs_col = cc;
        V.vc.x = ved_x(V.vc.y, *V.v.want);
        V.vs.x = ved_x(V.vs.y, V.vs_col);
        return;
      }
      V.vs = V.vc;
      V.vc = t;
      if (V.mode == VM_VBLOCK) {
        size_t cc = block_col();
        *V.v.want = V.vs_col;
        V.vs_col = cc;
      }
      else *V.v.want = ved_col(V.vc.y, V.vc.x);
      return;
    }
    case ':':
      prompt_open(':', "'<,'>");
      return;
    case '/': case '?':
      prompt_open(k0, NULL);
      memset(&V.p_cmd, 0, sizeof(V.p_cmd));
      V.p_op = 0;
      return;
    case 'I': case 'A':
      if (V.mode == VM_VISUAL) {
        vis_range(&a, &b);
        visual_end();
        grp();
        go(k0 == 'I' ? a : b);
        mode_insert(k0 == 'I' ? 'i' : 'a', 1);
        V.ins_dot = 0;
        return;
      }
      block_insert(k0 == 'A');
      return;
    case 'p': case 'P': vis_put(c); return;
    case 'J': {
      size_t y0 = V.vs.y < V.vc.y ? V.vs.y : V.vc.y, y1 = V.vs.y < V.vc.y ? V.vc.y : V.vs.y;
      visual_end();
      join(y0, (long)(y1 - y0 + 1), 1);
      return;
    }
    case 'g':
      if (k1 == 'J') {
        size_t y0 = V.vs.y < V.vc.y ? V.vs.y : V.vc.y, y1 = V.vs.y < V.vc.y ? V.vc.y : V.vs.y;
        visual_end();
        join(y0, (long)(y1 - y0 + 1), 0);
        return;
      }
      if (k1 == 'v') {	/* the one before, and back */
        int m = V.last_vmode;
        Pos s = V.last_vs, e = V.last_vc;
        if (!m) return;
        V.last_vmode = V.mode;
        V.last_vs = V.vs;
        V.last_vc = V.vc;
        V.mode = m;
        V.vs = s;
        V.vc = e;
        V.vs_col = ved_col(s.y, s.x);
        *V.v.want = ved_col(e.y, e.x);
        return;
      }
      if (k1 == 'u' || k1 == 'U' || k1 == '~' || k1 == '?') {
        VCmd o = *c;
        int line = V.mode == VM_VLINE;
        vis_range(&a, &b);
        visual_end();
        o.op = OP_G(k1);
        apply(&o, a, line ? pos(b.y, 0) : b, line ? MK_LINE : MK_EXCL);
        return;
      }
      return;
    case 'r': {
      Pos s, e, q;
      vis_range(&s, &e);
      visual_end();
      grp();
      for (q = s; pos_cmp(q, e) < 0 && q.y <= last_y();) {	/* every character, line by line */
        size_t x1 = q.y == e.y ? e.x : llen(q.y);
        if (x1 > q.x) {
          size_t chars = utf8_count(row(q.y)->s + q.x, x1 - q.x), k;
          char u[4];
          int w = utf8_encode((uint32_t)c->arg, u);
          Buf t;
          buf_init(&t);
          for (k = 0; k < chars; k++) buf_putn(&t, u, (size_t)w);
          del(q, pos(q.y, x1));
          ins(q, t.s, t.len);
          buf_free(&t);
        }
        if (q.y == e.y || q.y == last_y()) break;
        q = pos(q.y + 1, 0);
      }
      go(s);
      return;
    }
    case '~': case 'u': case 'U': {
      VCmd o = *c;
      int line = V.mode == VM_VLINE;
      vis_range(&a, &b);
      visual_end();
      o.op = OP_G(k0);
      apply(&o, a, line ? pos(b.y, 0) : b, line ? MK_LINE : MK_EXCL);
      return;
    }
  }
  if (k0 > 0 && k0 < 128 && strchr("dxXDyYcsCSR<>=", k0)) {
    VCmd o = *c;
    int line = V.mode == VM_VLINE || (k0 != 'x' && k0 != 'd' && k0 != 'y' && k0 != 'c' && k0 != 's' &&
                                      k0 != '<' && k0 != '>' && k0 != '=');
    static const char from[] = "dxXDyYcsCSR<>=", op[] = "ddddyyccccc<>=";
    vis_range(&a, &b);
    visual_end();
    o.op = op[strchr(from, k0) - from];
    o.kind = CK_ACTION;
    if (line || k0 == '<' || k0 == '>' || k0 == '=') apply(&o, pos(a.y, 0), pos(b.y, 0), MK_LINE);
    else apply(&o, a, b, MK_EXCL);
  }
}

/* }================================================================== */


/*
** {==================================================================
** The keys
** ===================================================================
*/

/* is Ctrl+c (c: a .. z, [) Vim's, in this mode? */
static int vim_ctrl (int c) {
  int i = c == '[' ? 26 : c - 'a';
  int ins = V.mode == VM_INSERT || V.mode == VM_REPLACE;
  if (i < 0 || i > 26) return 0;
  if (!VO.ctrl) return 0;
  if (VO.hk[i] >= 0) return VO.hk[i];
  return strchr(ins ? "orwutd" : "abdefioruvxy", c) != NULL;
}


/* the keys Normal and Visual mode want; the rest go on to VS Code's */
static int wanted (int k) {
  int code = KEY_CODE(k);
  if (k >= 1 && k <= 26 && k != K_TAB && k != K_ENTER && k != 10) return vim_ctrl(k + 'a' - 1);
  if (IS_TEXT(k)) return 1;
  if (k & (KM_CTRL | KM_ALT | KM_SHIFT)) return 0;
  return code == K_ESC || code == K_ENTER || code == K_BS || code == K_DEL || code == K_TAB || code == K_UP ||
         code == K_DOWN || code == K_LEFT || code == K_RIGHT || code == K_HOME || code == K_END ||
         code == K_PGUP || code == K_PGDN || code == K_INS;
}


/* a key of Normal or Visual mode */
static int cmd_key (int k) {
  VCmd c;
  int r, vis = IS_VISUAL;
  if (V.np > 0 && (k == K_ENTER || k == K_TAB)) ;	/* r<Enter>, f<Tab> */
  else if (!wanted(k)) {
    V.np = 0;
    return 0;
  }
  if (k == K_ESC || k == CTRL('c')) {
    if (V.np > 0) V.np = 0;
    else if (vis) visual_end();
    else ved_key(K_ESC);	/* the find widget, the other cursors, a hover: they go */
    return 1;
  }
  if (V.np == NPEND) V.np = 0;
  V.pend[V.np++] = k;
  r = parse(V.pend, V.np, &c, vis);
  if (r == 0) return 1;
  V.np = 0;
  if (r < 0) return 1;
  if (vis) vis_exec(&c);
  else exec(&c);
  if (ved_get(&V.v) && V.oneshot && V.mode == VM_NORMAL && V.prompt == VP_NONE) {	/* Ctrl+O's one command is done */
    V.oneshot = 0;
    V.mode = VM_INSERT;
  }
  return 1;
}


/* the editor as Vim left it: the mouse, another tab or VS Code's keys may have changed it since */
static void sync (void) {
  if (V.doc != V.v.doc) {
    V.doc = V.v.doc;
    V.mode = VO.insert_first ? VM_INSERT : VM_NORMAL;
    V.np = 0;
    V.prompt = VP_NONE;
    V.seen = 0;
    if (V.mode == VM_INSERT) mode_insert('i', 1), grp();
  }
  if (!V.seen) return;
  if (IS_VISUAL) {
    if (pos_cmp(*V.v.cur, V.seen_cur) != 0 || pos_cmp(*V.v.anchor, V.seen_anchor) != 0 || *V.v.sel != V.seen_sel) {
      if (!*V.v.sel) {	/* clicked: out of Visual mode */
        V.mode = VM_NORMAL;
        if (V.v.nmc) ved_cursors(V.v.cur, V.v.cur, 1);
        return;
      }
      V.mode = VM_VISUAL;	/* dragged: the selection is Visual mode's */
      V.vs = *V.v.anchor;
      V.vc = *V.v.cur;
      if (pos_cmp(V.vc, V.vs) > 0) V.vc = V.vc.x > 0 ? pos(V.vc.y, prevx(V.vc.y, V.vc.x)) : V.vc;
      return;
    }
    *V.v.cur = V.vc;
  }
  else if (V.mode == VM_NORMAL && *V.v.sel && V.v.nmc == 0 && !V.oneshot) {	/* a selection made: Visual mode */
    V.mode = VM_VISUAL;
    V.vs = *V.v.anchor;
    V.vc = *V.v.cur;
    if (pos_cmp(V.vc, V.vs) > 0 && V.vc.x > 0) V.vc = pos(V.vc.y, prevx(V.vc.y, V.vc.x));
    V.vs_col = ved_col(V.vs.y, V.vs.x);
  }
}


/* after a key: Normal mode's cursor on a character, Visual mode's selection drawn */
static void settle (void) {
  if (!ved_get(&V.v)) return;
  if (V.mode == VM_NORMAL && !V.oneshot && !*V.v.sel && V.prompt == VP_NONE) {
    Pos p = *V.v.cur;
    if (p.x >= llen(p.y) && llen(p.y) > 0) {
      p.x = lastx(p.y);
      *V.v.cur = p;
    }
  }
  if (IS_VISUAL) {
    if (V.vs.y > last_y()) V.vs = pos(last_y(), 0);
    if (V.vc.y > last_y()) V.vc = pos(last_y(), 0);
    paint();
  }
  V.seen_cur = *V.v.cur;
  V.seen_anchor = *V.v.anchor;
  V.seen_sel = *V.v.sel;
  V.seen = 1;
}


int vim_key (int k) {
  int r = 1;
  if (!VO.enable || !ved_get(&V.v) || !V.v.keys) return 0;
  if (k == ('[' | KM_CTRL)) k = K_ESC;
  sync();
  if (V.rec && !V.playing) kadd(&V.mac[V.rec - 'a'], k);
  V.msg[0] = '\0';
  if (V.prompt) prompt_key(k);
  else if (V.mode == VM_INSERT || V.mode == VM_REPLACE) {
    if (k == K_ESC) {
      ved_key(K_ESC);	/* a suggestion, a hover: they go too */
      if (!ved_get(&V.v)) return 1;
      ins_end();
    }
    else if (KEY_CODE(k) < 32 && k >= 1 && k <= 26 && k != K_TAB && k != K_ENTER && !vim_ctrl(k + 'a' - 1)) r = 0;
    else r = ins_key(k, 1);
    if (!r) return 0;	/* mme.c types it */
  }
  else r = cmd_key(k);
  if (r) settle();
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** The status bar, the cursor
** ===================================================================
*/

void vim_status (void) {
  VimEd v;
  char t[320];
  static const char *const name[] = {"-- NORMAL --", "-- INSERT --", "-- VISUAL --", "-- VISUAL LINE --",
                                     "-- VISUAL BLOCK --", "-- REPLACE --"};
  if (!VO.enable || !ved_get(&v)) return;
  if (v.doc != V.doc) snprintf(t, sizeof(t), "%s", name[VO.insert_first ? VM_INSERT : VM_NORMAL]);
  else if (V.prompt) snprintf(t, sizeof(t), "%c%s|", V.p_kind, V.line);
  else if (V.msg[0]) snprintf(t, sizeof(t), "%s", V.msg);
  else if (V.oneshot) snprintf(t, sizeof(t), "-- (insert) --");
  else snprintf(t, sizeof(t), "%s", name[V.mode]);
  if (V.rec && !V.prompt) snprintf(t + strlen(t), sizeof(t) - strlen(t), " recording @%c", V.rec);
  status_add("status.vim", "Vim", 0, 88, t, "Vim mode", CMD_NONE);
  if (V.np > 0 && !V.prompt) {	/* the keys of the command typed so far */
    char keys[NPEND * 4 + 1];
    size_t n = 0;
    int i;
    for (i = 0; i < V.np; i++)
      if (IS_TEXT(V.pend[i])) n += (size_t)utf8_encode((uint32_t)V.pend[i], keys + n);
      else if (V.pend[i] >= 1 && V.pend[i] <= 26) keys[n++] = '^', keys[n++] = (char)('@' + V.pend[i]);
    keys[n] = '\0';
    status_add("status.vim.keys", "Vim Keys", 0, 87, keys, "The keys of a command not whole yet", CMD_NONE);
  }
}


int vim_shape (int shape) {
  VimEd v;
  if (!VO.enable || !ved_get(&v) || !v.keys) return shape;
  if (v.doc == V.doc && (V.mode == VM_INSERT || (V.mode == VM_NORMAL && V.oneshot))) return shape;
  if (v.doc != V.doc && VO.insert_first) return shape;
  if (v.doc == V.doc && V.mode == VM_REPLACE) return (shape & 1) ? 3 : 4;	/* an underline */
  return (shape & 1) ? 1 : 2;	/* a block */
}


int vim_lit (void) {
  return VO.enable && V.lit;
}

/* }================================================================== */
