/*
** emd.c - the Markdown preview (Ctrl+Shift+V, Ctrl+K V), like VS Code's
**
** The text is drawn as a page for the terminal: headings bold in color
** (the first two with a rule under them), paragraphs wrapped, lists with
** bullets, task boxes, quotes with a bar, code blocks on a darker ground
** with their language's colors, inline `code`, **bold**, *italic*, links
** underlined, tables in boxes and --- as a rule. It is drawn from the
** text each time, so it follows the typing.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Cell {
  uint32_t cp, fg, bg;
  int at;	/* RGB_* */
} Cell;

typedef struct Cells {
  Cell *v;
  size_t n, cap;
} Cells;

static struct {
  int x, y, w, h;	/* the page on the screen */
  size_t top;	/* the first row shown */
  size_t row;	/* rows made so far */
  uint32_t fg, bg, dim, head, code_fg, code_bg, link, border, quote;
} M;


static void cells_add (Cells *c, uint32_t cp, uint32_t fg, uint32_t bg, int at) {
  if (c->n == c->cap) {
    c->cap = c->cap ? c->cap * 2 : 128;
    c->v = (Cell *)xrealloc(c->v, c->cap * sizeof(Cell));
  }
  c->v[c->n].cp = cp;
  c->v[c->n].fg = fg;
  c->v[c->n].bg = bg;
  c->v[c->n].at = at;
  c->n++;
}


static void cells_str (Cells *c, const char *s, uint32_t fg, uint32_t bg, int at) {
  size_t n = strlen(s), i = 0, len;
  while (i < n) {
    cells_add(c, utf8_decode(s + i, n - i, &len), fg, bg, at);
    i += len ? len : 1;
  }
}


/*
** {==================================================================
** Rows
** ===================================================================
*/

/* one row of the page: drawn when it is in the part shown; fill: the rest of it */
static void row_out (const Cell *c, size_t n, uint32_t fill) {
  if (M.row >= M.top && M.row < M.top + (size_t)M.h) {
    int y = M.y + (int)(M.row - M.top), x = M.x;
    size_t i;
    for (i = 0; i < n && x < M.x + M.w; i++) x += scr_put_rgb(x, y, c[i].cp, c[i].fg, c[i].bg, c[i].at);
    for (; x < M.x + M.w; x++) scr_put_rgb(x, y, ' ', M.fg, fill, 0);
  }
  M.row++;
}


static void blank_row (void) {
  row_out(NULL, 0, M.bg);
}


/* cells wrapped at the page's width, at spaces; pre1 before the first row, pre2 before the others */
static void flow (const Cell *c, size_t n, const Cells *pre1, const Cells *pre2, uint32_t fill) {
  Cells row = {NULL, 0, 0};
  size_t i = 0;
  int first = 1, width = M.w - 2;	/* a margin at the right */
  if (n == 0) {
    row_out(pre1 ? pre1->v : NULL, pre1 ? pre1->n : 0, fill);
    return;
  }
  while (i < n) {
    const Cells *pre = first ? pre1 : pre2;
    size_t j, fit, lastsp = 0, k;
    int cols = pre ? (int)pre->n : 0;
    row.n = 0;
    for (k = 0; pre && k < pre->n; k++) cells_add(&row, pre->v[k].cp, pre->v[k].fg, pre->v[k].bg, pre->v[k].at);
    for (j = i; j < n && c[j].cp != '\n'; j++) {
      if (cols + 1 > width) break;
      if (c[j].cp == ' ') lastsp = j;
      cols++;
    }
    fit = j;
    if (j < n && c[j].cp != '\n' && lastsp > i) fit = lastsp;	/* break at the last space */
    for (k = i; k < fit; k++) cells_add(&row, c[k].cp, c[k].fg, c[k].bg, c[k].at);
    row_out(row.v, row.n, fill);
    i = fit;
    while (i < n && (c[i].cp == ' ' || c[i].cp == '\n')) {	/* the space (or the hard break) between */
      if (c[i].cp == '\n') {
        i++;
        break;
      }
      i++;
    }
    first = 0;
  }
  free(row.v);
}

/* }================================================================== */


/*
** {==================================================================
** Inline: **bold** *italic* `code` [link](url)
** ===================================================================
*/

static void inline_cells (Cells *out, const char *s, size_t n, uint32_t fg, uint32_t bg, int at0) {
  size_t i = 0, len;
  int bold = 0, ital = 0;
  while (i < n) {
    char c = s[i];
    if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|<>~", s[i + 1])) {	/* an escaped sign */
      cells_add(out, (unsigned char)s[i + 1], fg, bg, at0 | (bold ? RGB_BOLD : 0) | (ital ? RGB_ITALIC : 0));
      i += 2;
      continue;
    }
    if (c == '`') {	/* `code`: to the next one */
      size_t e = i + 1;
      while (e < n && s[e] != '`') e++;
      if (e < n) {
        size_t k = i + 1;
        while (k < e) {
          cells_add(out, utf8_decode(s + k, e - k, &len), M.code_fg, M.code_bg, 0);
          k += len ? len : 1;
        }
        i = e + 1;
        continue;
      }
    }
    if ((c == '*' || c == '_') && i + 1 < n && s[i + 1] == c) {	/* ** or __ */
      bold = !bold;
      i += 2;
      continue;
    }
    if ((c == '*' || c == '_') && (ital || (i + 1 < n && s[i + 1] != ' '))) {
      if (!(c == '_' && i > 0 && ((s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= '0' && s[i - 1] <= '9')))) {
        ital = !ital;	/* snake_case stays */
        i++;
        continue;
      }
    }
    if (c == '~' && i + 1 < n && s[i + 1] == '~') {	/* ~~strike~~: no strike in a terminal cell; left out */
      i += 2;
      continue;
    }
    if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {	/* [text](url), ![alt](url) */
      int img = c == '!';
      size_t a = i + (img ? 2 : 1), b = a, e;
      int depth = 1;
      while (b < n && depth) {
        if (s[b] == '[') depth++;
        else if (s[b] == ']') depth--;
        if (depth) b++;
      }
      if (b < n && b + 1 < n && s[b + 1] == '(' && (e = b + 2) < n) {
        while (e < n && s[e] != ')') e++;
        if (e < n) {
          if (img) cells_str(out, "\xF0\x9F\x96\xBC ", M.dim, bg, 0);	/* the picture's place */
          inline_cells(out, s + a, b - a, img ? M.dim : M.link, bg, img ? RGB_ITALIC : RGB_UNDER);
          i = e + 1;
          continue;
        }
      }
    }
    if (c == '<' && i + 1 < n && (strncmp(s + i + 1, "http", 4) == 0)) {	/* <https://...> */
      size_t e = i + 1;
      while (e < n && s[e] != '>') e++;
      if (e < n) {
        inline_cells(out, s + i + 1, e - i - 1, M.link, bg, RGB_UNDER);
        i = e + 1;
        continue;
      }
    }
    cells_add(out, utf8_decode(s + i, n - i, &len), fg, bg,
              at0 | (bold ? RGB_BOLD : 0) | (ital ? RGB_ITALIC : 0));
    i += len ? len : 1;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Blocks
** ===================================================================
*/

static int is_blank (const Row *r) {
  size_t i;
  for (i = 0; i < r->len; i++)
    if (r->s[i] != ' ' && r->s[i] != '\t') return 0;
  return 1;
}


static size_t indent_of (const Row *r) {
  size_t i = 0, col = 0;
  while (i < r->len && (r->s[i] == ' ' || r->s[i] == '\t')) col += r->s[i++] == '\t' ? 4 : 1;
  return col;
}


static size_t skip_ws (const Row *r) {
  size_t i = 0;
  while (i < r->len && (r->s[i] == ' ' || r->s[i] == '\t')) i++;
  return i;
}


/* ---, ***, ___ (three or more, spaces between allowed) */
static int is_rule (const Row *r) {
  size_t i = skip_ws(r), n = 0;
  char c = i < r->len ? r->s[i] : 0;
  if (c != '-' && c != '*' && c != '_') return 0;
  for (; i < r->len; i++) {
    if (r->s[i] == c) n++;
    else if (r->s[i] != ' ') return 0;
  }
  return n >= 3;
}


static int is_fence (const Row *r, char *ch) {
  size_t i = skip_ws(r);
  if (i + 3 <= r->len && (memcmp(r->s + i, "```", 3) == 0 || memcmp(r->s + i, "~~~", 3) == 0)) {
    *ch = r->s[i];
    return 1;
  }
  return 0;
}


/* a list item: its marker's length (after the indent), 0: not one; *num: the number, -1 a bullet */
static size_t list_mark (const Row *r, long *num) {
  size_t i = skip_ws(r), j;
  if (i + 1 < r->len && (r->s[i] == '-' || r->s[i] == '*' || r->s[i] == '+') && r->s[i + 1] == ' ') {
    *num = -1;
    return 2;
  }
  for (j = i; j < r->len && r->s[j] >= '0' && r->s[j] <= '9' && j - i < 9; j++) ;
  if (j > i && j + 1 < r->len && (r->s[j] == '.' || r->s[j] == ')') && r->s[j + 1] == ' ') {
    *num = strtol(r->s + i, NULL, 10);
    return j - i + 2;
  }
  return 0;
}


static int is_table_row (const Row *r) {
  size_t i = skip_ws(r);
  return i < r->len && r->s[i] == '|';
}


/* "| a | b |" split in cells (trimmed); returns how many */
static int table_split (const Row *r, const char **cs, size_t *cl, int max) {
  size_t i = skip_ws(r), end = r->len;
  int n = 0;
  if (i < end && r->s[i] == '|') i++;
  while (end > i && (r->s[end - 1] == ' ' || r->s[end - 1] == '\t')) end--;
  if (end > i && r->s[end - 1] == '|') end--;
  while (i <= end && n < max) {
    size_t a = i, b;
    while (i < end && !(r->s[i] == '|' && (i == 0 || r->s[i - 1] != '\\'))) i++;
    b = i;
    while (a < b && r->s[a] == ' ') a++;
    while (b > a && r->s[b - 1] == ' ') b--;
    cs[n] = r->s + a;
    cl[n] = b - a;
    n++;
    if (i >= end) break;
    i++;
  }
  return n;
}


static size_t cells_cols (const char *s, size_t n) {
  Cells c = {NULL, 0, 0};
  size_t k;
  inline_cells(&c, s, n, 0, 0, 0);
  k = c.n;
  free(c.v);
  return k;
}


/* rows a..b-1 are a table: boxes, the first row bold, columns as wide as they must be */
static void draw_table (const Doc *d, size_t a, size_t b) {
  enum { MAXC = 16 };
  size_t wcol[MAXC], y, i;
  int ncol = 0, c;
  Cells row = {NULL, 0, 0};
  memset(wcol, 0, sizeof(wcol));
  for (y = a; y < b; y++) {
    const char *cs[MAXC];
    size_t cl[MAXC];
    int n = table_split(&d->row[y], cs, cl, MAXC);
    if (y == a + 1) continue;	/* |---|---| */
    if (n > ncol) ncol = n;
    for (c = 0; c < n; c++) {
      size_t k = cells_cols(cs[c], cl[c]);
      if (k > wcol[c]) wcol[c] = k;
    }
  }
  {	/* too wide: the widest columns give way */
    size_t total = 1;
    for (c = 0; c < ncol; c++) total += wcol[c] + 3;
    while (total > (size_t)M.w - 1) {
      int wide = 0;
      for (c = 1; c < ncol; c++)
        if (wcol[c] > wcol[wide]) wide = c;
      if (wcol[wide] <= 3) break;
      wcol[wide]--;
      total--;
    }
  }
#define BORDER_ROW(l, m, r)	do { \
    row.n = 0; \
    cells_add(&row, l, M.border, M.bg, 0); \
    for (c = 0; c < ncol; c++) { \
      for (i = 0; i < wcol[c] + 2; i++) cells_add(&row, 0x2500, M.border, M.bg, 0); \
      cells_add(&row, c + 1 < ncol ? (m) : (r), M.border, M.bg, 0); \
    } \
    row_out(row.v, row.n, M.bg); } while (0)
  BORDER_ROW(0x250C, 0x252C, 0x2510);
  for (y = a; y < b; y++) {
    const char *cs[MAXC];
    size_t cl[MAXC];
    int n;
    if (y == a + 1) {
      BORDER_ROW(0x251C, 0x253C, 0x2524);
      continue;
    }
    n = table_split(&d->row[y], cs, cl, MAXC);
    row.n = 0;
    cells_add(&row, 0x2502, M.border, M.bg, 0);
    for (c = 0; c < ncol; c++) {
      Cells cell = {NULL, 0, 0};
      size_t k;
      if (c < n) inline_cells(&cell, cs[c], cl[c], y == a ? M.head : M.fg, M.bg, y == a ? RGB_BOLD : 0);
      cells_add(&row, ' ', M.fg, M.bg, 0);
      for (k = 0; k < wcol[c]; k++)
        if (k < cell.n) cells_add(&row, cell.v[k].cp, cell.v[k].fg, cell.v[k].bg, cell.v[k].at);
        else cells_add(&row, ' ', M.fg, M.bg, 0);
      cells_add(&row, ' ', M.fg, M.bg, 0);
      cells_add(&row, 0x2502, M.border, M.bg, 0);
      free(cell.v);
    }
    row_out(row.v, row.n, M.bg);
  }
  BORDER_ROW(0x2514, 0x2534, 0x2518);
#undef BORDER_ROW
  free(row.v);
}


/* a code block's line: two spaces in, its language's colors, the dark ground to the edge */
static int code_line (const Row *r, const Syntax *sx, int state) {
  Cells row = {NULL, 0, 0};
  unsigned char *tok = NULL;
  size_t i = 0, len;
  if (sx) {
    tok = (unsigned char *)xmalloc(r->len + 1);
    state = syntax_scan(sx, r->s, r->len, state, tok);
  }
  cells_add(&row, ' ', M.fg, M.code_bg, 0);
  cells_add(&row, ' ', M.fg, M.code_bg, 0);
  while (i < r->len) {
    uint32_t fg = M.fg, bg;
    uint32_t cp = utf8_decode(r->s + i, r->len - i, &len);
    if (tok) theme_tok(TOK(tok[i], B_EDITOR), &fg, &bg);
    if (cp == '\t') {
      int k;
      for (k = 0; k < 4; k++) cells_add(&row, ' ', fg, M.code_bg, 0);
    }
    else cells_add(&row, cp, fg, M.code_bg, 0);
    i += len ? len : 1;
  }
  if (row.n > (size_t)M.w) row.n = (size_t)M.w;	/* long lines are cut, as in a code block */
  row_out(row.v, row.n, M.code_bg);
  free(row.v);
  free(tok);
  return state;
}


/* the language of a fence's info: "```go" -> Go's syntax */
static const Syntax *fence_syntax (const Row *r) {
  static const char *const alias[][2] = {
    {"python", "py"}, {"javascript", "js"}, {"typescript", "ts"}, {"rust", "rs"}, {"shell", "sh"},
    {"bash", "sh"}, {"console", "sh"}, {"c++", "cpp"}, {"golang", "go"}, {"powershell", "ps1"},
    {"yml", "yaml"}, {"markdown", "md"}, {"batch", "bat"}
  };
  char lang[32], name[48];
  size_t i = skip_ws(r) + 3, n = 0, k;
  while (i < r->len && (r->s[i] == '`' || r->s[i] == '~' || r->s[i] == ' ')) i++;
  while (i < r->len && r->s[i] != ' ' && r->s[i] != '{' && n + 1 < sizeof(lang)) {
    char c = r->s[i++];
    lang[n++] = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
  }
  lang[n] = '\0';
  if (n == 0) return NULL;
  for (k = 0; k < sizeof(alias) / sizeof(alias[0]); k++)
    if (strcmp(lang, alias[k][0]) == 0) snprintf(lang, sizeof(lang), "%s", alias[k][1]);
  snprintf(name, sizeof(name), "x.%s", lang);
  return syntax_for(name);
}


/* the page, from the text's line y0 on */
static void blocks (const Doc *d) {
  size_t y = 0, n = d->n;
  int gap = 0;	/* a blank row before the next block */
  Cells para = {NULL, 0, 0}, pre1 = {NULL, 0, 0}, pre2 = {NULL, 0, 0};
  while (y < n) {
    const Row *r = &d->row[y];
    size_t i = skip_ws(r);
    long num;
    size_t mk;
    char fch;
    if (is_blank(r)) {
      y++;
      continue;
    }
    if (gap) blank_row();
    gap = 1;
    pre1.n = pre2.n = para.n = 0;
    if (is_fence(r, &fch)) {	/* ``` code ``` */
      const Syntax *sx = fence_syntax(r);
      int state = 0;
      row_out(NULL, 0, M.code_bg);
      for (y++; y < n; y++) {
        char c2;
        if (is_fence(&d->row[y], &c2) && c2 == fch) {
          y++;
          break;
        }
        state = code_line(&d->row[y], sx, state);
      }
      row_out(NULL, 0, M.code_bg);
      continue;
    }
    if (r->s[i] == '#' ) {	/* # Heading */
      size_t lv = 0, e = r->len;
      while (i + lv < r->len && r->s[i + lv] == '#') lv++;
      if (lv <= 6 && (i + lv == r->len || r->s[i + lv] == ' ')) {
        size_t a = i + lv;
        while (a < e && r->s[a] == ' ') a++;
        while (e > a && (r->s[e - 1] == '#' || r->s[e - 1] == ' ')) e--;
        inline_cells(&para, r->s + a, e - a, M.head, M.bg, RGB_BOLD | (lv >= 5 ? RGB_ITALIC : 0));
        flow(para.v, para.n, NULL, NULL, M.bg);
        if (lv <= 2) {	/* the rule under the first two */
          Cells rule = {NULL, 0, 0};
          int k;
          for (k = 0; k < M.w - 2; k++) cells_add(&rule, lv == 1 ? 0x2501 : 0x2500, M.border, M.bg, 0);
          row_out(rule.v, rule.n, M.bg);
          free(rule.v);
        }
        y++;
        continue;
      }
    }
    if (is_rule(r)) {	/* --- */
      Cells rule = {NULL, 0, 0};
      int k;
      for (k = 0; k < M.w - 2; k++) cells_add(&rule, 0x2500, M.border, M.bg, 0);
      row_out(rule.v, rule.n, M.bg);
      free(rule.v);
      y++;
      continue;
    }
    if (is_table_row(r) && y + 1 < n && is_table_row(&d->row[y + 1]) &&
        strspn(d->row[y + 1].s + skip_ws(&d->row[y + 1]), "|-: ") >= d->row[y + 1].len - skip_ws(&d->row[y + 1])) {
      size_t e = y + 2;
      while (e < n && is_table_row(&d->row[e])) e++;
      draw_table(d, y, e);
      y = e;
      continue;
    }
    if (r->s[i] == '>') {	/* > quote: a bar, the text dim; its lines go together */
      int lv = 0;
      size_t k;
      for (; y < n && !is_blank(&d->row[y]); y++) {
        const Row *q = &d->row[y];
        size_t a = skip_ws(q);
        int l = 0;
        while (a < q->len && (q->s[a] == '>' || q->s[a] == ' ')) {
          if (q->s[a] == '>') l++;
          a++;
        }
        if (l > lv) lv = l;
        if (para.n) cells_add(&para, ' ', M.quote, M.bg, 0);
        inline_cells(&para, q->s + a, q->len - a, M.quote, M.bg, 0);
      }
      for (k = 0; k < (size_t)(lv ? lv : 1); k++) {
        cells_add(&pre1, 0x258E, M.link, M.bg, 0);
        cells_add(&pre1, ' ', M.fg, M.bg, 0);
      }
      flow(para.v, para.n, &pre1, &pre1, M.bg);
      continue;
    }
    if ((mk = list_mark(r, &num)) > 0) {	/* - item, 1. item, - [x] task */
      int first_item = 1;
      gap = 1;
      while (y < n && (mk = list_mark(&d->row[y], &num)) > 0) {
        const Row *q = &d->row[y];
        size_t ind = indent_of(q), a = skip_ws(q) + mk, lvl = ind / 2, k;
        char mark[32];
        (void)first_item;
        first_item = 0;
        pre1.n = pre2.n = para.n = 0;
        for (k = 0; k < 2 + lvl * 2 && k < 40; k++) {
          cells_add(&pre1, ' ', M.fg, M.bg, 0);
          cells_add(&pre2, ' ', M.fg, M.bg, 0);
        }
        if (num >= 0) snprintf(mark, sizeof(mark), "%ld. ", num);
        else snprintf(mark, sizeof(mark), "%s ", lvl == 0 ? "\xE2\x80\xA2" : lvl == 1 ? "\xE2\x97\xA6" : "\xE2\x96\xAA");
        if (a + 3 < q->len && q->s[a] == '[' && (q->s[a + 1] == ' ' || q->s[a + 1] == 'x' || q->s[a + 1] == 'X') &&
            q->s[a + 2] == ']') {	/* a task */
          snprintf(mark, sizeof(mark), "%s ", q->s[a + 1] == ' ' ? "\xE2\x98\x90" : "\xE2\x98\x91");
          a += 4;
        }
        cells_str(&pre1, mark, num >= 0 ? M.fg : M.link, M.bg, 0);
        for (k = 0; k < str_cols(mark); k++) cells_add(&pre2, ' ', M.fg, M.bg, 0);
        inline_cells(&para, q->s + a, q->len - a, M.fg, M.bg, 0);
        for (y++; y < n && !is_blank(&d->row[y]) && !list_mark(&d->row[y], &num) &&
                  indent_of(&d->row[y]) > ind; y++) {	/* its lines that go on */
          const Row *c = &d->row[y];
          cells_add(&para, ' ', M.fg, M.bg, 0);
          inline_cells(&para, c->s + skip_ws(c), c->len - skip_ws(c), M.fg, M.bg, 0);
        }
        flow(para.v, para.n, &pre1, &pre2, M.bg);
        while (y < n && is_blank(&d->row[y]) && y + 1 < n && list_mark(&d->row[y + 1], &num)) y++;	/* a loose list */
      }
      continue;
    }
    if (indent_of(r) >= 4) {	/* indented code */
      for (; y < n && (indent_of(&d->row[y]) >= 4 || is_blank(&d->row[y])); y++) {
        Row cut = d->row[y];
        size_t k = 0, col = 0;
        while (k < cut.len && col < 4) col += cut.s[k++] == '\t' ? 4 : 1;
        cut.s += k;
        cut.len -= k;
        code_line(&cut, NULL, 0);
      }
      continue;
    }
    {	/* a paragraph: its lines joined, two spaces or \ at an end break the line */
      for (; y < n && !is_blank(&d->row[y]); y++) {
        const Row *q = &d->row[y];
        size_t a = skip_ws(q), e = q->len;
        char c2;
        long nn;
        if (para.n && (is_fence(q, &c2) || q->s[a] == '#' || q->s[a] == '>' || list_mark(q, &nn) || is_rule(q))) break;
        if (para.n) {
          const Row *p = &d->row[y - 1];
          int hard = (p->len >= 2 && p->s[p->len - 1] == ' ' && p->s[p->len - 2] == ' ') ||
                     (p->len && p->s[p->len - 1] == '\\');
          cells_add(&para, hard ? '\n' : ' ', M.fg, M.bg, 0);
        }
        if (e && q->s[e - 1] == '\\') e--;
        while (e > a && q->s[e - 1] == ' ') e--;
        inline_cells(&para, q->s + a, e - a, M.fg, M.bg, 0);
      }
      flow(para.v, para.n, NULL, NULL, M.bg);
    }
  }
  free(para.v);
  free(pre1.v);
  free(pre2.v);
}

/* }================================================================== */


/* the preview of d at x, y (w by h), from row *top (kept inside the page) */
void md_draw (const Doc *d, int x, int y, int w, int h, size_t *top) {
  uint32_t bg;
  int r;
  M.x = x + 2;	/* the page's margin */
  M.y = y;
  M.w = w - 3;
  M.h = h;
  M.fg = ui_color(C_EDITOR_FG);
  M.bg = ui_color(C_EDITOR_BG);
  M.dim = ui_color(C_DIM);
  M.border = ui_color(C_BORDER);
  M.link = ui_color(C_ICON_BLUE);
  M.quote = ui_color(C_DIM);
  M.code_bg = ui_color(C_INPUT_BG);
  theme_tok(TOK(T_HEADING, B_EDITOR), &M.head, &bg);
  theme_tok(TOK(T_STRING, B_EDITOR), &M.code_fg, &bg);
  if (M.w < 10) return;
  for (r = 0; r < h; r++) {	/* the ground, the margin too */
    int k;
    for (k = 0; k < w; k++) scr_put_rgb(x + k, y + r, ' ', M.fg, M.bg, 0);
  }
  M.row = 0;
  M.top = (size_t)-1;	/* first counted: how many rows there are */
  M.h = 0;
  blocks(d);
  if (*top + (size_t)h > M.row) *top = M.row > (size_t)h ? M.row - (size_t)h : 0;
  M.top = *top;
  M.h = h;
  M.row = 0;
  blocks(d);
}
