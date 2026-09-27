/*
** erefactor.c - the code actions' menu and the Refactor Preview
**
** Quick Fix (Ctrl+.), Refactor... (Ctrl+Shift+R) and Source Action... list
** the server's code actions grouped by their kind, as VS Code's widget
** does: Quick Fix, Extract, Inline, Rewrite, Move, Surround With, Source
** Action, More Actions. A disabled one shows why and does nothing.
**
** A rename or a code action whose WorkspaceEdit touches several files (or
** any rename, with Shift+Enter in its box) is shown first in the Refactor
** Preview: the files, each with its edits - the line as it will be, what
** goes red, what comes green - each file and each edit with a check box,
** then Apply or Discard. Only what stays checked is made.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** The code actions, grouped
** ===================================================================
*/

static const struct {
  const char *kind;	/* this kind, or one under it ("refactor.extract.function") */
  const char *title;
  int icon;
} g_cat[] = {
  {"quickfix", "Quick Fix", 0xEA61},	/* codicon lightbulb */
  {"refactor.extract", "Extract", 0xEB65},	/* wrench */
  {"refactor.inline", "Inline", 0xEB65},
  {"refactor.rewrite", "Rewrite", 0xEB65},
  {"refactor.move", "Move", 0xEB65},
  {"refactor.surround", "Surround With", 0xEB65},
  {"source", "Source Action", 0xEA61},
  {"", "More Actions...", 0xEA61}	/* anything else */
};

#define NCAT	(sizeof(g_cat) / sizeof(g_cat[0]))


/* kind is k or one under it: "refactor.extract.constant" is a "refactor.extract" */
static int kind_in (const char *kind, const char *k) {
  size_t n = strlen(k);
  return strncmp(kind, k, n) == 0 && (kind[n] == '\0' || kind[n] == '.');
}


static int category (const char *kind) {
  size_t c;
  for (c = 0; c + 1 < NCAT; c++)
    if (kind_in(kind, g_cat[c].kind)) return (int)c;
  return (int)NCAT - 1;
}


void actions_menu (int what, char *const *titles, size_t n) {
  static const char *const head[] = {"Quick Fix", "Refactor", "Source Action"};
  static const char *const none[] = {"No code actions available", "No refactorings available",
                                     "No source actions available"};
  Pick p;
  size_t i, *of, m = 0;
  int c, r;
  if (what < ACT_QUICKFIX || what > ACT_SOURCE) what = ACT_QUICKFIX;
  of = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  pick_init(&p, head[what]);
  for (c = 0; c < (int)NCAT; c++)	/* by category, in VS Code's order */
    for (i = 0; i < n; i++) {
      const char *kind = lsp_action_kind(i), *why = lsp_action_disabled(i);
      if (category(kind) != c) continue;
      if (what == ACT_REFACTOR && !kind_in(kind, "refactor")) continue;	/* what the server sent past "only" */
      if (what == ACT_SOURCE && !kind_in(kind, "source")) continue;
      p.group = c;
      p.group_label[c] = g_cat[c].title;
      pick_add(&p, titles[i], why, g_cat[c].icon);	/* disabled: why, dim after it */
      of[m++] = i;
    }
  if (m == 0) {
    toast(0, "%s", none[what]);
    pick_free(&p);
    free(of);
    return;
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && (size_t)r < m) {
    const char *why = lsp_action_disabled(of[r]);
    if (why) toast(0, "%s", why);
    else lsp_action_run(of[r]);
  }
  free(of);
}

/* }================================================================== */


/*
** {==================================================================
** Refactor Preview
** ===================================================================
*/

typedef struct PEdit {
  size_t e;	/* its TextEdit */
  size_t line;	/* from 0 */
  char *pre, *old, *new_, *post;	/* the line: before it, what goes, what comes, after it */
  int on;
} PEdit;

typedef struct PFile {
  const char *path;
  size_t first, n;	/* its edits in the list */
  int open;
} PFile;

static struct {
  const TextEdit *v;
  PEdit *ed;
  size_t ned;
  PFile *f;
  size_t nf;
  size_t *row;	/* what each row shows: 2 * file, or 2 * edit + 1 */
  size_t nrow, sel, top;
  int focus;	/* 0 the tree, 1 Apply, 2 Discard */
  int x, y, w, h, list_y, list_h, apply_x, discard_x;
} RP;


/* the byte of UTF-16 column u in s (n bytes) */
static size_t u16_byte (const char *s, size_t n, size_t u) {
  size_t i = 0, k = 0, len;
  while (i < n && k < u) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    k += cp >= 0x10000 ? 2 : 1;
    i += len;
  }
  return i;
}


/* line y of the text t (n bytes): where it starts, its length (no \r) */
static const char *line_of (const char *t, size_t n, size_t y, size_t *len) {
  size_t i = 0, k;
  while (y > 0 && i < n) {
    const char *nl = (const char *)memchr(t + i, '\n', n - i);
    if (nl == NULL) {
      i = n;
      break;
    }
    i = (size_t)(nl - t) + 1;
    y--;
  }
  if (y > 0) {
    *len = 0;
    return t + n;
  }
  for (k = i; k < n && t[k] != '\n'; k++) ;
  if (k > i && t[k - 1] == '\r') k--;
  *len = k - i;
  return t + i;
}


/* one line of what is shown: tabs as a space, a newline as ⏎ (VS Code's multi-line edits too) */
static char *one_line (const char *s, size_t n) {
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < n; i++) {
    if (s[i] == '\n') buf_puts(&b, "\xE2\x8F\x8E");
    else if (s[i] == '\r') continue;
    else buf_putc(&b, s[i] == '\t' ? ' ' : s[i]);
  }
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* edit e of the file whose text is t: the line it is on, what it takes out and puts in */
static void pedit_fill (PEdit *pe, const TextEdit *e, const char *t, size_t n) {
  size_t la, lb, a, b;
  const char *sa = line_of(t, n, e->l0, &la), *sb = line_of(t, n, e->l1, &lb);
  a = e->utf16 ? u16_byte(sa, la, e->c0) : (e->c0 < la ? e->c0 : la);
  b = e->utf16 ? u16_byte(sb, lb, e->c1) : (e->c1 < lb ? e->c1 : lb);
  pe->line = e->l0;
  pe->pre = one_line(sa, a);
  if (e->l1 == e->l0) pe->old = one_line(sa + a, b > a ? b - a : 0);
  else {	/* several lines go: from a to the end of b's, joined */
    size_t start = (size_t)(sa + a - t), end = (size_t)(sb + b - t);
    pe->old = one_line(t + start, end > start ? end - start : 0);
  }
  pe->new_ = one_line(e->text, strlen(e->text));
  pe->post = one_line(sb + b, lb - b);
  pe->on = 1;
}


static void rows (void) {
  size_t i, j;
  RP.nrow = 0;
  for (i = 0; i < RP.nf; i++) {
    RP.row[RP.nrow++] = 2 * i;
    if (RP.f[i].open)
      for (j = 0; j < RP.f[i].n; j++) RP.row[RP.nrow++] = 2 * (RP.f[i].first + j) + 1;
  }
  if (RP.sel >= RP.nrow) RP.sel = RP.nrow ? RP.nrow - 1 : 0;
}


/* a file's check box: 2 all its edits, 1 some, 0 none */
static int file_on (const PFile *f) {
  size_t j, on = 0;
  for (j = 0; j < f->n; j++) on += RP.ed[f->first + j].on != 0;
  return on == f->n ? 2 : on ? 1 : 0;
}


static void toggle (size_t row) {
  size_t k = RP.row[row];
  if (k % 2 == 0) {	/* a file: all of its edits, or none */
    PFile *f = &RP.f[k / 2];
    int to = file_on(f) != 2;
    size_t j;
    for (j = 0; j < f->n; j++) RP.ed[f->first + j].on = to;
  }
  else RP.ed[k / 2].on = !RP.ed[k / 2].on;
}


/* a folder as VS Code shows it: from the folder open ("" for the folder itself), else whole */
static const char *in_root (const char *dir) {
  const char *root = side_root();
  size_t i;
  for (i = 0; root[i] && dir[i]; i++) {
    int a = (unsigned char)root[i], b = (unsigned char)dir[i];
    if (path_is_sep('\\') && a < 128 && b < 128) {	/* Windows: its names in any case, either slash */
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a == '/') a = '\\';
      if (b == '/') b = '\\';
    }
    if (a != b) return dir;
  }
  if (root[i]) return dir;
  while (dir[i] && path_is_sep(dir[i])) i++;
  return dir + i;
}


static int put (int x, int y, int end, const char *s, int st) {
  if (x >= end) return x;
  return x + scr_putsw(x, y, end - x, s, st);
}


static void draw (void) {
  int cols = scr_cols(), rows_ = scr_rows(), i, bx;
  size_t on = 0, k;
  char head[128];
  RP.w = cols - 4 < 140 ? cols - 4 : 140;
  RP.h = rows_ * 2 / 3 < rows_ - 2 ? rows_ * 2 / 3 : rows_ - 2;
  if (RP.h < 8) RP.h = rows_ < 8 ? rows_ : 8;
  RP.x = (cols - RP.w) / 2;
  RP.y = (rows_ - RP.h) / 2;
  RP.list_y = RP.y + 2;
  RP.list_h = RP.h - 6;
  if (RP.list_h < 1) RP.list_h = 1;
  ui_background();
  scr_box(RP.x, RP.y, RP.w, RP.h, S_BOX);
  for (k = 0; k < RP.ned; k++) on += RP.ed[k].on != 0;
  scr_puts(RP.x + 2, RP.y, "REFACTOR PREVIEW", S_BOX_TITLE);
  snprintf(head, sizeof(head), "%lu of %lu changes in %lu file%s", (unsigned long)on, (unsigned long)RP.ned,
           (unsigned long)RP.nf, RP.nf == 1 ? "" : "s");
  scr_putsw(RP.x + 20, RP.y, RP.w - 22, head, S_BOX_DIM);
  if (RP.sel < RP.top) RP.top = RP.sel;
  if (RP.sel >= RP.top + (size_t)RP.list_h) RP.top = RP.sel - (size_t)RP.list_h + 1;
  for (i = 0; i < RP.list_h && RP.top + (size_t)i < RP.nrow; i++) {
    size_t r = RP.top + (size_t)i, id = RP.row[r];
    int y = RP.list_y + i, x = RP.x + 2, end = RP.x + RP.w - 2, sel = r == RP.sel && RP.focus == 0;
    int st = sel ? S_BOX_SEL : S_BOX, dim = sel ? S_BOX_SEL : S_BOX_DIM;
    scr_fill(RP.x + 1, y, RP.w - 2, st);
    if (id % 2 == 0) {	/* a file: its chevron, check box, icon, name, folder, count */
      const PFile *f = &RP.f[id / 2];
      int fo = file_on(f), ist;
      uint32_t ic = file_icon(path_basename(f->path), &ist);
      char cnt[32], *dir = xstrdup(f->path), *slash;
      scr_put(x, y, f->open ? 0xEAB4 : 0xEAB6, st);	/* chevron down / right */
      scr_put(x + 2, y, fo == 2 ? 0x2611 : fo ? 0x25A3 : 0x2610, st);	/* ☑ ▣ ☐ */
      scr_put(x + 4, y, ic, sel ? st : ist);
      x = put(x + 6, y, end, path_basename(f->path), st);
      slash = (char *)path_basename(dir);
      if (slash > dir) slash[-1] = '\0';
      else dir[0] = '\0';
      x = put(x + 1, y, end, in_root(dir), dim);
      snprintf(cnt, sizeof(cnt), "  %lu", (unsigned long)f->n);
      put(x, y, end, cnt, dim);
      free(dir);
    }
    else {	/* an edit: its check box, the line as it will be, what goes red and what comes green */
      const PEdit *e = &RP.ed[id / 2];
      char ln[32];
      scr_put(x + 4, y, e->on ? 0x2611 : 0x2610, st);
      x = put(x + 6, y, end, e->pre, st);
      if (e->old[0]) x = put(x, y, end, e->old, S_DIFF_DEL_HI);
      if (e->new_[0]) x = put(x, y, end, e->new_, S_DIFF_ADD_HI);
      x = put(x, y, end, e->post, st);
      snprintf(ln, sizeof(ln), "  %lu", (unsigned long)e->line + 1);
      put(x, y, end, ln, dim);
    }
  }
  {	/* the edit selected, before and after */
    size_t id = RP.nrow ? RP.row[RP.sel] : 0;
    int y = RP.y + RP.h - 4;
    for (i = 1; i < RP.w - 1; i++) scr_put(RP.x + i, y - 1 + 0, 0x2500, S_MENU_LINE);
    if (RP.nrow && id % 2 == 1) {
      const PEdit *e = &RP.ed[id / 2];
      int x = RP.x + 2, end = RP.x + RP.w - 2;
      scr_fill(RP.x + 1, y, RP.w - 2, S_DIFF_DEL);
      scr_fill(RP.x + 1, y + 1, RP.w - 2, S_DIFF_ADD);
      x = put(x, y, end, "- ", S_DIFF_DEL);
      x = put(x, y, end, e->pre, S_DIFF_DEL);
      x = put(x, y, end, e->old, S_DIFF_DEL_HI);
      put(x, y, end, e->post, S_DIFF_DEL);
      x = put(RP.x + 2, y + 1, end, "+ ", S_DIFF_ADD);
      x = put(x, y + 1, end, e->pre, S_DIFF_ADD);
      x = put(x, y + 1, end, e->new_, S_DIFF_ADD_HI);
      put(x, y + 1, end, e->post, S_DIFF_ADD);
    }
    else scr_putsw(RP.x + 2, y, RP.w - 4, "Space: check or uncheck, Enter: open or check, Tab: the buttons, "
                   "Ctrl+Enter: Apply, Esc: Discard", S_BOX_DIM);
  }
  bx = RP.x + RP.w - 2;	/* the buttons, from the right */
  RP.discard_x = bx - 11;
  RP.apply_x = RP.discard_x - 10;
  for (i = 0; i < 2; i++) {
    int st = RP.focus == 1 + i || (RP.focus == 0 && i == 0) ? S_STATUS : S_INPUT;
    int bxx = i == 0 ? RP.apply_x : RP.discard_x, bw = i == 0 ? 9 : 11;
    scr_fill(bxx, RP.y + RP.h - 1, bw, st);
    if (st == S_STATUS) scr_round(bxx, RP.y + RP.h - 1, bw, 1, RC_ALL, RR_SMALL);
    scr_puts(bxx + 2, RP.y + RP.h - 1, i == 0 ? "Apply" : "Discard", st);
  }
  scr_cursor(0, -1);
  scr_flush();
}


static void say (void) {
  size_t id;
  if (RP.focus) {
    acc_say(RP.focus == 1 ? "Apply button" : "Discard button");
    return;
  }
  if (RP.nrow == 0) return;
  id = RP.row[RP.sel];
  if (id % 2 == 0) {
    const PFile *f = &RP.f[id / 2];
    acc_sayf("%s, %lu changes, %s", path_basename(f->path), (unsigned long)f->n,
             file_on(f) == 2 ? "checked" : file_on(f) ? "partly checked" : "not checked");
  }
  else {
    const PEdit *e = &RP.ed[id / 2];
    acc_sayf("line %lu, %s replaced by %s, %s", (unsigned long)e->line + 1, e->old, e->new_,
             e->on ? "checked" : "not checked");
  }
}


/* the preview's own key loop: 1 Apply, 0 Discard */
static int run (void) {
  size_t said = (size_t)-1;
  int said_focus = -1;
  for (;;) {
    int k, code;
    draw();
    if (RP.sel != said || RP.focus != said_focus) {
      say();
      said = RP.sel;
      said_focus = RP.focus;
    }
    k = term_key(200);
    if (k == K_NONE) continue;
    code = KEY_CODE(k);
    if (code == K_ESC) return 0;
    if (code == K_ENTER && (k & KM_CTRL)) return 1;	/* Apply, wherever the focus is */
    if (code == K_TAB) {
      RP.focus = (RP.focus + ((k & KM_SHIFT) ? 2 : 1)) % 3;
      continue;
    }
    if (RP.focus) {
      if (code == K_ENTER || code == ' ') return RP.focus == 1;
      if (code == K_LEFT || code == K_RIGHT) RP.focus = RP.focus == 1 ? 2 : 1;
    }
    else if (code == K_UP && RP.sel > 0) RP.sel--;
    else if (code == K_DOWN && RP.sel + 1 < RP.nrow) RP.sel++;
    else if (code == K_PGUP) RP.sel = RP.sel > (size_t)RP.list_h ? RP.sel - (size_t)RP.list_h : 0;
    else if (code == K_PGDN) RP.sel = RP.nrow && RP.sel + (size_t)RP.list_h < RP.nrow ? RP.sel + (size_t)RP.list_h : (RP.nrow ? RP.nrow - 1 : 0);
    else if (code == K_HOME) RP.sel = 0;
    else if (code == K_END) RP.sel = RP.nrow ? RP.nrow - 1 : 0;
    else if (code == ' ' && RP.nrow) toggle(RP.sel);
    else if ((code == K_LEFT || code == K_RIGHT || code == K_ENTER) && RP.nrow) {
      size_t id = RP.row[RP.sel];
      if (id % 2 == 0 && code != K_ENTER) RP.f[id / 2].open = code == K_RIGHT;
      else if (id % 2 == 0) RP.f[id / 2].open = !RP.f[id / 2].open;
      else if (code == K_ENTER) toggle(RP.sel);
      else if (code == K_LEFT) {	/* an edit: to its file */
        while (RP.sel > 0 && RP.row[RP.sel] % 2 == 1) RP.sel--;
      }
      rows();
    }
    if (code == K_MOUSE && term_mouse.press && !term_mouse.drag && term_mouse.button == 0) {
      const Mouse *m = &term_mouse;
      if (m->y == RP.y + RP.h - 1 && m->x >= RP.apply_x && m->x < RP.apply_x + 9) return 1;
      if (m->y == RP.y + RP.h - 1 && m->x >= RP.discard_x && m->x < RP.discard_x + 11) return 0;
      if (m->y >= RP.list_y && m->y < RP.list_y + RP.list_h && RP.top + (size_t)(m->y - RP.list_y) < RP.nrow) {
        size_t r = RP.top + (size_t)(m->y - RP.list_y), id = RP.row[r];
        int cx = m->x - (RP.x + 2);
        RP.sel = r;
        RP.focus = 0;
        if (id % 2 == 0 && cx >= 0 && cx < 2) {	/* the chevron */
          RP.f[id / 2].open = !RP.f[id / 2].open;
          rows();
        }
        else if ((id % 2 == 0 && cx >= 2 && cx < 4) || (id % 2 == 1 && cx >= 4 && cx < 6)) toggle(r);	/* a check box */
      }
      else if (m->x < RP.x || m->x >= RP.x + RP.w || m->y < RP.y || m->y >= RP.y + RP.h) return 0;	/* outside */
    }
    else if (code == K_MOUSE && term_mouse.wheel) {
      if (term_mouse.wheel < 0 && RP.sel > 0) RP.sel--;
      else if (term_mouse.wheel > 0 && RP.sel + 1 < RP.nrow) RP.sel++;
    }
  }
}


static void preview (const TextEdit *v, size_t n) {
  size_t i, j, k = 0;
  char *done = (char *)calloc(n + 1, 1);
  TextEdit *keep;
  size_t m = 0;
  memset(&RP, 0, sizeof(RP));
  RP.v = v;
  RP.ed = (PEdit *)xmalloc(n * sizeof(PEdit));
  RP.f = (PFile *)xmalloc(n * sizeof(PFile));
  RP.row = (size_t *)xmalloc(2 * n * sizeof(size_t) + sizeof(size_t));
  for (i = 0; i < n; i++) {	/* by file, in the order they came; each file's edits by line */
    size_t len = 0, first = k;
    char *t;
    if (done[i]) continue;
    t = open_doc_text(v[i].path, &len);
    if (t == NULL) t = read_file(v[i].path, &len);
    for (j = i; j < n; j++)
      if (!done[j] && m_fncmp(v[j].path, v[i].path) == 0) {
        size_t q = k;
        done[j] = 1;
        while (q > first && (RP.ed[q - 1].line > v[j].l0 ||
                             (RP.ed[q - 1].line == v[j].l0 && v[RP.ed[q - 1].e].c0 > v[j].c0))) {
          RP.ed[q] = RP.ed[q - 1];
          q--;
        }
        pedit_fill(&RP.ed[q], &v[j], t ? t : "", t ? len : 0);
        RP.ed[q].e = j;
        k++;
      }
    free(t);
    RP.f[RP.nf].path = v[i].path;
    RP.f[RP.nf].first = first;
    RP.f[RP.nf].n = k - first;
    RP.f[RP.nf].open = 1;
    RP.nf++;
  }
  RP.ned = k;
  free(done);
  rows();
  if (run()) {
    keep = (TextEdit *)xmalloc((n + 1) * sizeof(TextEdit));
    for (i = 0; i < RP.ned; i++)
      if (RP.ed[i].on) keep[m++] = v[RP.ed[i].e];
    if (m) on_edit(keep, m);
    free(keep);
  }
  for (i = 0; i < RP.ned; i++) {
    free(RP.ed[i].pre);
    free(RP.ed[i].old);
    free(RP.ed[i].new_);
    free(RP.ed[i].post);
  }
  free(RP.ed);
  free(RP.f);
  free(RP.row);
  memset(&RP, 0, sizeof(RP));
}


/* a rename's or a code action's edits: previewed when they touch several files (always: whatever they touch) */
void on_edit_confirm (const TextEdit *v, size_t n, int always) {
  size_t i, j, files = 0;
  for (i = 0; i < n; i++) {
    for (j = 0; j < i && m_fncmp(v[j].path, v[i].path) != 0; j++) ;
    if (j == i) files++;
  }
  if (n == 0) {
    if (always) toast(0, "No edits to preview");
    return;
  }
  if (files <= 1 && !always) on_edit(v, n);
  else preview(v, n);
}

/* }================================================================== */
