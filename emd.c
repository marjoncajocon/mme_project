/*
** emd.c - the Markdown preview (Ctrl+Shift+V, Ctrl+K V), like VS Code's
**
** The text is drawn as a page for the terminal: headings bold in color
** (the first two with a rule under them), paragraphs wrapped, lists with
** bullets and task boxes, quotes with a bar (one for each > deep), code
** blocks on a darker ground with their language's colors, inline `code`,
** **bold**, *italic*, ~~struck~~, links underlined (Ctrl+click follows
** them), footnotes, HTML shown as its text and tables in boxes, their
** columns aligned as the :---: row asks.
**
** Each row of the page remembers the line of the text it came from, so
** the preview and the editor can be scrolled together, and the rows are
** only counted again when the text or the width changed - a page of a
** few thousand lines is drawn from what is already known.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Cell {
  uint32_t cp, fg, bg;
  int at;	/* RGB_* */
  int link;	/* 1 + the place in M.link, 0: not a link */
} Cell;

typedef struct Cells {
  Cell *v;
  size_t n, cap;
} Cells;

typedef struct Anchor {
  char *slug;	/* "my-heading" of "## My heading" */
  size_t row;
} Anchor;

static struct {
  int x, y, w, h;	/* the page on the screen */
  size_t top;	/* the first row shown */
  size_t row;	/* rows made so far */
  size_t line;	/* the line of the text the rows now made come from */
  uint32_t fg, bg, dim, head, code_fg, code_bg, link, border, quote, done;
  /* what the last counting pass learned, kept while the text does not change */
  const Doc *c_doc;
  unsigned long c_edits;
  size_t c_len;
  int c_w;
  size_t *src;	/* for each row of the page, the line it came from */
  size_t nsrc, csrc;
  Anchor *an;	/* where each heading is, for [jumps](#like-this) */
  size_t nan, can;
  int counting;	/* the pass that only counts rows */
  char **links;	/* what the links on the page point at */
  size_t nlink, clink;
  int cur_link;	/* the link the cells now made belong to */
  int no_links;	/* while a column is being measured */
  int *map;	/* a link for each cell shown, for the mouse */
  int map_w, map_h;
  char *dir;	/* the document's folder: ![](pictures/x.png) is next to it */
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
  c->v[c->n].link = M.cur_link;
  c->n++;
}


/* a cell as it is (its link too): the wrapping moves cells about */
static void cells_push (Cells *c, const Cell *s) {
  int save = M.cur_link;
  M.cur_link = s->link;
  cells_add(c, s->cp, s->fg, s->bg, s->at);
  M.cur_link = save;
}


static void cells_str (Cells *c, const char *s, uint32_t fg, uint32_t bg, int at) {
  size_t n = strlen(s), i = 0, len;
  while (i < n) {
    cells_add(c, utf8_decode(s + i, n - i, &len), fg, bg, at);
    i += len ? len : 1;
  }
}


/* a link's place in the table, 1-based (0 is "no link") */
static int link_add (const char *s, size_t n) {
  if (M.counting || M.no_links) return 0;	/* only the drawn page needs them */
  while (n && (*s == ' ' || *s == '\t')) {
    s++;
    n--;
  }
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  if (n == 0) return 0;
  if (M.nlink == M.clink) {
    M.clink = M.clink ? M.clink * 2 : 32;
    M.links = (char **)xrealloc(M.links, M.clink * sizeof(char *));
  }
  M.links[M.nlink++] = xstrndup(s, n);
  return (int)M.nlink;
}


static void links_clear (void) {
  size_t i;
  for (i = 0; i < M.nlink; i++) free(M.links[i]);
  M.nlink = 0;
}


/*
** {==================================================================
** Rows
** ===================================================================
*/

/* the line a row came from, so the two halves can be scrolled together */
static void src_add (size_t line) {
  if (M.nsrc == M.csrc) {
    M.csrc = M.csrc ? M.csrc * 2 : 256;
    M.src = (size_t *)xrealloc(M.src, M.csrc * sizeof(size_t));
  }
  M.src[M.nsrc++] = line;
}


/* one row of the page: drawn when it is in the part shown; fill: the rest of it */
static void row_out (const Cell *c, size_t n, uint32_t fill) {
  if (M.counting) src_add(M.line);
  else if (M.row >= M.top && M.row < M.top + (size_t)M.h) {
    int y = M.y + (int)(M.row - M.top), x = M.x, my = (int)(M.row - M.top);
    size_t i;
    for (i = 0; i < n && x < M.x + M.w; i++) {
      int w = scr_put_rgb(x, y, c[i].cp, c[i].fg, c[i].bg, c[i].at);
      if (M.map && c[i].link) {	/* where the mouse may follow it */
        int k;
        for (k = 0; k < w && x - M.x + k < M.map_w; k++)
          M.map[my * M.map_w + (x - M.x) + k] = c[i].link;
      }
      x += w;
    }
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
    for (k = 0; pre && k < pre->n; k++) cells_push(&row, &pre->v[k]);
    for (j = i; j < n && c[j].cp != '\n'; j++) {
      if (cols + 1 > width) break;
      if (c[j].cp == ' ') lastsp = j;
      cols++;
    }
    fit = j;
    if (j < n && c[j].cp != '\n' && lastsp > i) fit = lastsp;	/* break at the last space */
    /* a prefix as wide as the page fits nothing: take one cell anyway, so the loop ends */
    if (fit == i && c[i].cp != ' ' && c[i].cp != '\n') fit = i + 1;
    for (k = i; k < fit; k++) cells_push(&row, &c[k]);
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
** Inline: **bold** *italic* ~~struck~~ `code` [link](url) ![](picture)
** ===================================================================
*/

/* a file a link names, next to the document; NULL: it points elsewhere */
static char *link_path (const char *url, size_t n) {
  char *rel, *full;
  if (n == 0 || url[0] == '#' || url[0] == '<') return NULL;
  if (n > 5 && (m_strnicmp(url, "http:", 5) == 0 || m_strnicmp(url, "https", 5) == 0 ||
                m_strnicmp(url, "mailto", 6) == 0)) return NULL;
  rel = xstrndup(url, n);
  {	/* "x.png#top" and "x.png?v=2" name the file x.png */
    char *q = strpbrk(rel, "#?");
    if (q) *q = '\0';
  }
  full = (rel[0] && M.dir) ? path_join(M.dir, rel) : xstrdup(rel);
  free(rel);
  return full;
}


/* "my heading" -> "my-heading", the name an anchor link uses */
static char *slug_of (const char *s, size_t n) {
  char *out = (char *)xmalloc(n + 1);
  size_t i, k = 0;
  for (i = 0; i < n; i++) {
    char c = s[i];
    if (c >= 'A' && c <= 'Z') out[k++] = (char)(c + 32);
    else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[k++] = c;
    else if (c == '-' || c == ' ' || c == '_') {
      if (k && out[k - 1] != '-') out[k++] = '-';
    }
  }
  while (k && out[k - 1] == '-') k--;
  out[k] = '\0';
  return out;
}


static void anchor_add (const char *s, size_t n) {
  if (!M.counting) return;	/* the counting pass knows every row's number */
  if (M.nan == M.can) {
    M.can = M.can ? M.can * 2 : 32;
    M.an = (Anchor *)xrealloc(M.an, M.can * sizeof(Anchor));
  }
  M.an[M.nan].slug = slug_of(s, n);
  M.an[M.nan].row = M.row;
  M.nan++;
}


static void anchors_clear (void) {
  size_t i;
  for (i = 0; i < M.nan; i++) free(M.an[i].slug);
  M.nan = 0;
}


static void inline_cells (Cells *out, const char *s, size_t n, uint32_t fg, uint32_t bg, int at0) {
  size_t i = 0, len;
  int bold = 0, ital = 0, strike = 0;
  while (i < n) {
    char c = s[i];
    int at = at0 | (bold ? RGB_BOLD : 0) | (ital ? RGB_ITALIC : 0) | (strike ? RGB_STRIKE : 0);
    if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|<>~", s[i + 1])) {	/* an escaped sign */
      cells_add(out, (unsigned char)s[i + 1], fg, bg, at);
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
    if (c == '~' && i + 1 < n && s[i + 1] == '~') {	/* ~~struck~~ */
      strike = !strike;
      i += 2;
      continue;
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
    if (c == '[' && i + 2 < n && s[i + 1] == '^') {	/* [^1]: the note's number, raised */
      size_t e = i + 2;
      while (e < n && s[e] != ']') e++;
      if (e < n && (e + 1 >= n || s[e + 1] != ':')) {
        char href[80];
        char *sl = slug_of(s + i + 2, e - i - 2);
        snprintf(href, sizeof(href), "#fn-%s", sl);
        free(sl);
        M.cur_link = link_add(href, strlen(href));
        cells_add(out, '[', M.link, bg, RGB_UNDER);
        {
          size_t k = i + 2;
          while (k < e) cells_add(out, (unsigned char)s[k++], M.link, bg, RGB_UNDER);
        }
        cells_add(out, ']', M.link, bg, RGB_UNDER);
        M.cur_link = 0;
        i = e + 1;
        continue;
      }
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
          const char *url = s + b + 2;
          size_t un = e - b - 2;
          if (img) {	/* the picture's place, with what it is when it is there */
            char info[80], *file = link_path(url, un);
            cells_str(out, "\xF0\x9F\x96\xBC ", M.dim, bg, 0);
            M.cur_link = link_add(url, un);
            inline_cells(out, s + a, b - a, M.dim, bg, RGB_ITALIC);
            if (b == a) cells_str(out, "image", M.dim, bg, RGB_ITALIC);
            if (file && img_info(file, info, sizeof(info))) {
              cells_add(out, ' ', M.dim, bg, 0);
              cells_add(out, '(', M.dim, bg, 0);
              cells_str(out, info, M.dim, bg, 0);
              cells_add(out, ')', M.dim, bg, 0);
            }
            M.cur_link = 0;
            free(file);
          }
          else {
            M.cur_link = link_add(url, un);
            inline_cells(out, s + a, b - a, M.link, bg, RGB_UNDER);
            M.cur_link = 0;
          }
          i = e + 1;
          continue;
        }
      }
    }
    if (c == '<' && i + 5 <= n && strncmp(s + i + 1, "http", 4) == 0) {	/* <https://...> */
      size_t e = i + 1;
      while (e < n && s[e] != '>') e++;
      if (e < n) {
        M.cur_link = link_add(s + i + 1, e - i - 1);
        inline_cells(out, s + i + 1, e - i - 1, M.link, bg, RGB_UNDER);
        M.cur_link = 0;
        i = e + 1;
        continue;
      }
    }
    if (c == 'h' && i + 8 < n && (strncmp(s + i, "http://", 7) == 0 ||
                                  strncmp(s + i, "https://", 8) == 0)) {	/* a bare address */
      size_t e = i;
      while (e < n && s[e] != ' ' && s[e] != '\t' && s[e] != '<' && s[e] != ')') e++;
      while (e > i && strchr(".,;:!?", s[e - 1])) e--;	/* the sentence's full stop is not its */
      M.cur_link = link_add(s + i, e - i);
      while (i < e) cells_add(out, (unsigned char)s[i++], M.link, bg, RGB_UNDER);
      M.cur_link = 0;
      continue;
    }
    cells_add(out, utf8_decode(s + i, n - i, &len), fg, bg, at);
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


/* "===" or "---" under a line makes it a heading: 1 or 2, else 0 */
static int setext_level (const Row *r) {
  size_t i = skip_ws(r), n = 0;
  char c = i < r->len ? r->s[i] : 0;
  if (c != '=' && c != '-') return 0;
  for (; i < r->len; i++) {
    if (r->s[i] == c) n++;
    else if (r->s[i] != ' ') return 0;
  }
  return n ? (c == '=' ? 1 : 2) : 0;
}


static int is_fence (const Row *r, char *ch) {
  size_t i = skip_ws(r);
  if (i + 3 <= r->len && (memcmp(r->s + i, "```", 3) == 0 || memcmp(r->s + i, "~~~", 3) == 0)) {
    *ch = r->s[i];
    return 1;
  }
  return 0;
}


/* the > signs a line starts with */
static int quote_level (const Row *r) {
  size_t i = skip_ws(r);
  int lv = 0;
  while (i < r->len && (r->s[i] == '>' || r->s[i] == ' ')) {
    if (r->s[i] == '>') lv++;
    else if (lv == 0) break;
    i++;
  }
  return lv;
}


/* past the > signs of a quoted line */
static size_t quote_text (const Row *r) {
  size_t i = skip_ws(r);
  while (i < r->len && (r->s[i] == '>' || r->s[i] == ' ')) i++;
  return i;
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


/* "[^note]: " at the start of a line: how long it is, 0 when it is not one */
static size_t footnote_mark (const Row *r, const char **id, size_t *idn) {
  size_t i = skip_ws(r), e;
  if (i + 3 >= r->len || r->s[i] != '[' || r->s[i + 1] != '^') return 0;
  for (e = i + 2; e < r->len && r->s[e] != ']'; e++) ;
  if (e + 1 >= r->len || r->s[e + 1] != ':') return 0;
  *id = r->s + i + 2;
  *idn = e - i - 2;
  e += 2;
  while (e < r->len && r->s[e] == ' ') e++;
  return e;
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
  int save = M.cur_link;
  M.cur_link = 0;
  M.no_links = 1;
  inline_cells(&c, s, n, 0, 0, 0);
  M.no_links = 0;
  k = c.n;
  free(c.v);
  M.cur_link = save;
  return k;
}


/* "|---|:--:|---:|" is the row that says how each column sits: 0 left, 1 right, 2 middle */
static int is_table_delim (const Row *r) {
  size_t i = skip_ws(r), dash = 0;
  if (i >= r->len || r->s[i] != '|') return 0;
  for (; i < r->len; i++) {
    if (r->s[i] == '-') dash++;
    else if (r->s[i] != '|' && r->s[i] != ':' && r->s[i] != ' ' && r->s[i] != '\t') return 0;
  }
  return dash >= 1;
}


/* rows a..b-1 are a table: boxes, the first row bold, columns as wide as they must be */
static void draw_table (const Doc *d, size_t a, size_t b) {
  enum { MAXC = 16 };
  size_t wcol[MAXC], y, i;
  int ncol = 0, c, align[MAXC];
  Cells row = {NULL, 0, 0};
  memset(wcol, 0, sizeof(wcol));
  memset(align, 0, sizeof(align));
  {	/* the :---: row: which side each column's text goes to */
    const char *cs[MAXC];
    size_t cl[MAXC];
    int n = table_split(&d->row[a + 1], cs, cl, MAXC);
    for (c = 0; c < n; c++) {
      int left = cl[c] && cs[c][0] == ':', right = cl[c] && cs[c][cl[c] - 1] == ':';
      align[c] = (left && right) ? 2 : right ? 1 : 0;
    }
  }
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
    M.line = y;
    if (y == a + 1) {
      BORDER_ROW(0x251C, 0x253C, 0x2524);
      continue;
    }
    n = table_split(&d->row[y], cs, cl, MAXC);
    row.n = 0;
    cells_add(&row, 0x2502, M.border, M.bg, 0);
    for (c = 0; c < ncol; c++) {
      Cells cell = {NULL, 0, 0};
      size_t k, pad = 0;
      if (c < n) inline_cells(&cell, cs[c], cl[c], y == a ? M.head : M.fg, M.bg, y == a ? RGB_BOLD : 0);
      if (cell.n < wcol[c])	/* where the text sits in its column */
        pad = align[c] == 1 ? wcol[c] - cell.n : align[c] == 2 ? (wcol[c] - cell.n) / 2 : 0;
      cells_add(&row, ' ', M.fg, M.bg, 0);
      for (k = 0; k < wcol[c]; k++)
        if (k >= pad && k - pad < cell.n) cells_push(&row, &cell.v[k - pad]);
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


/*
** {==================================================================
** HTML in the text: its words, not its tags
** ===================================================================
*/

/* a line that starts a block of HTML */
static int is_html_open (const Row *r) {
  size_t i = skip_ws(r);
  char c;
  if (i + 1 >= r->len || r->s[i] != '<') return 0;
  c = r->s[i + 1];
  return c == '!' || c == '/' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}


/* &amp; and its like; how many bytes it took, 0: not one */
static size_t entity (const char *s, size_t n, Buf *out) {
  static const struct { const char *name; const char *text; } ent[] = {
    {"amp;", "&"}, {"lt;", "<"}, {"gt;", ">"}, {"quot;", "\""}, {"apos;", "'"},
    {"nbsp;", " "}, {"#39;", "'"}, {"mdash;", "\xE2\x80\x94"}, {"ndash;", "\xE2\x80\x93"},
    {"hellip;", "\xE2\x80\xA6"}, {"copy;", "\xC2\xA9"}, {"times;", "\xC3\x97"}
  };
  size_t k;
  if (n == 0 || s[0] != '&') return 0;
  for (k = 0; k < sizeof(ent) / sizeof(ent[0]); k++) {
    size_t l = strlen(ent[k].name);
    if (n > l && memcmp(s + 1, ent[k].name, l) == 0) {
      buf_puts(out, ent[k].text);
      return l + 1;
    }
  }
  return 0;
}


/* does the row say tag at i? a Row's bytes are not NUL ended: only they are read */
static int tag_at (const Row *r, size_t i, const char *tag) {
  size_t n = strlen(tag);
  return r->len - i >= n && m_strnicmp(r->s + i, tag, n) == 0;
}


/*
** The HTML from *y on, drawn as what it says: the tags are dropped,
** <br> breaks the line, a comment is not shown at all, and <img src=x>
** leaves the same place-holder a Markdown picture does. It ends at a
** blank line, as a Markdown HTML block does.
*/
static void html_block (const Doc *d, size_t *y) {
  Buf text;
  Cells para = {NULL, 0, 0};
  size_t n = d->n, start = *y;
  int in_tag = 0, hidden = 0;	/* hidden: inside <script>, <style> or a comment */
  buf_init(&text);
  for (; *y < n && !is_blank(&d->row[*y]); (*y)++) {
    const Row *r = &d->row[*y];
    size_t i = 0;
    M.line = *y;
    if (*y > start) buf_putc(&text, ' ');
    while (i < r->len) {
      if (!in_tag && r->s[i] == '<') {
        size_t e = i + 1;
        while (e < r->len && r->s[e] != '>') e++;
        if (i + 4 <= r->len && memcmp(r->s + i, "<!--", 4) == 0) {	/* a comment says nothing */
          const char *end = NULL;
          size_t k;
          for (k = i; k + 3 <= r->len; k++)
            if (memcmp(r->s + k, "-->", 3) == 0) end = r->s + k;
          if (end) {
            i = (size_t)(end - r->s) + 3;
            continue;
          }
          hidden = 1;
          break;
        }
        if (hidden) {	/* the end of what was hidden */
          if (i + 2 < r->len && r->s[i + 1] == '/') hidden = 0;
          i = e < r->len ? e + 1 : r->len;
          continue;
        }
        if (tag_at(r, i, "<br")) buf_putc(&text, '\n');
        else if (tag_at(r, i, "<script") || tag_at(r, i, "<style"))
          hidden = 1;
        else if (tag_at(r, i, "<img")) {	/* the picture's place */
          const char *src = NULL;
          size_t k, sn = 0;
          for (k = i; k + 5 < (e < r->len ? e : r->len); k++)
            if (m_strnicmp(r->s + k, "src=", 4) == 0) {
              char q = r->s[k + 4];
              size_t a = k + 4 + (q == '"' || q == '\'' ? 1 : 0), b = a;
              while (b < e && r->s[b] != (q == '"' || q == '\'' ? q : ' ') && r->s[b] != '>') b++;
              src = r->s + a;
              sn = b - a;
              break;
            }
          buf_puts(&text, "\xF0\x9F\x96\xBC ");
          if (src) buf_putn(&text, src, sn);
          else buf_puts(&text, "image");
        }
        else if (tag_at(r, i, "<li")) buf_puts(&text, "\n\xE2\x80\xA2 ");
        else if (tag_at(r, i, "<p") || tag_at(r, i, "<tr") ||
                 tag_at(r, i, "<div")) buf_putc(&text, '\n');
        if (e >= r->len) {	/* the tag goes on into the next line */
          in_tag = 1;
          break;
        }
        i = e + 1;
        continue;
      }
      if (in_tag) {
        while (i < r->len && r->s[i] != '>') i++;
        if (i < r->len) {
          in_tag = 0;
          i++;
        }
        continue;
      }
      if (hidden) {
        i++;
        continue;
      }
      {
        size_t got = entity(r->s + i, r->len - i, &text);
        if (got) {
          i += got;
          continue;
        }
      }
      buf_putc(&text, r->s[i++]);
    }
  }
  {	/* what is left, dim, a paragraph at a time */
    char *s = buf_take(&text);
    size_t i = 0, len = strlen(s);
    while (i < len && (s[i] == ' ' || s[i] == '\n')) i++;
    while (len > i && (s[len - 1] == ' ' || s[len - 1] == '\n')) len--;
    while (i < len) {
      size_t e = i;
      while (e < len && s[e] != '\n') e++;
      para.n = 0;
      inline_cells(&para, s + i, e - i, M.dim, M.bg, 0);
      if (para.n) flow(para.v, para.n, NULL, NULL, M.bg);
      i = e + 1;
      while (i < len && (s[i] == ' ' || s[i] == '\n')) i++;
    }
    free(s);
  }
  free(para.v);
}

/* }================================================================== */


/* the bars of a quote that many deep */
static void quote_bars (Cells *pre, int lv) {
  int k;
  pre->n = 0;
  for (k = 0; k < lv && k < 8; k++) {
    cells_add(pre, 0x258E, k == 0 ? M.link : M.border, M.bg, 0);
    cells_add(pre, ' ', M.fg, M.bg, 0);
  }
}


/* the page */
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
    const char *fid;
    size_t fidn;
    M.line = y;
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
        M.line = y;
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
        anchor_add(r->s + a, e - a);
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
    if (y + 1 < n && setext_level(&d->row[y + 1]) && !is_table_row(r) && r->s[i] != '>' &&
        !list_mark(r, &num)) {	/* a heading with === or --- under it */
      int lv = setext_level(&d->row[y + 1]);
      size_t e = r->len;
      while (e > i && r->s[e - 1] == ' ') e--;
      anchor_add(r->s + i, e - i);
      inline_cells(&para, r->s + i, e - i, M.head, M.bg, RGB_BOLD);
      flow(para.v, para.n, NULL, NULL, M.bg);
      {
        Cells rule = {NULL, 0, 0};
        int k;
        for (k = 0; k < M.w - 2; k++) cells_add(&rule, lv == 1 ? 0x2501 : 0x2500, M.border, M.bg, 0);
        row_out(rule.v, rule.n, M.bg);
        free(rule.v);
      }
      y += 2;
      continue;
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
    if (is_table_row(r) && y + 1 < n && is_table_delim(&d->row[y + 1])) {
      size_t e = y + 2;
      while (e < n && is_table_row(&d->row[e])) e++;
      draw_table(d, y, e);
      y = e;
      continue;
    }
    if (r->s[i] == '>') {	/* > quote: a bar for each > it is deep */
      while (y < n && !is_blank(&d->row[y]) && quote_level(&d->row[y]) > 0) {
        int lv = quote_level(&d->row[y]);
        para.n = 0;
        M.line = y;
        for (; y < n && !is_blank(&d->row[y]) && quote_level(&d->row[y]) == lv; y++) {
          const Row *q = &d->row[y];
          size_t a = quote_text(q);
          if (para.n) cells_add(&para, ' ', M.quote, M.bg, 0);
          inline_cells(&para, q->s + a, q->len - a, M.quote, M.bg, 0);
        }
        quote_bars(&pre1, lv);
        flow(para.v, para.n, &pre1, &pre1, M.bg);
      }
      continue;
    }
    if ((mk = list_mark(r, &num)) > 0) {	/* - item, 1. item, - [x] task */
      size_t stack[8];
      int depth = 1;
      stack[0] = indent_of(r);
      while (y < n && (mk = list_mark(&d->row[y], &num)) > 0) {
        const Row *q = &d->row[y];
        size_t ind = indent_of(q), a = skip_ws(q) + mk, k;
        int lvl;
        char mark[32];
        int done = 0, task = 0;
        M.line = y;
        while (depth > 1 && ind < stack[depth - 1]) depth--;	/* back out to its level */
        if (ind > stack[depth - 1] && depth < 8) stack[depth++] = ind;
        lvl = depth - 1;
        pre1.n = pre2.n = para.n = 0;
        for (k = 0; k < 2 + (size_t)lvl * 2 && k < 40; k++) {
          cells_add(&pre1, ' ', M.fg, M.bg, 0);
          cells_add(&pre2, ' ', M.fg, M.bg, 0);
        }
        if (num >= 0) snprintf(mark, sizeof(mark), "%ld. ", num);
        else snprintf(mark, sizeof(mark), "%s ", lvl == 0 ? "\xE2\x80\xA2" : lvl == 1 ? "\xE2\x97\xA6" : "\xE2\x96\xAA");
        if (a + 3 < q->len && q->s[a] == '[' && (q->s[a + 1] == ' ' || q->s[a + 1] == 'x' || q->s[a + 1] == 'X') &&
            q->s[a + 2] == ']') {	/* a task */
          done = q->s[a + 1] != ' ';
          task = 1;
          snprintf(mark, sizeof(mark), "%s ", done ? "\xE2\x98\x91" : "\xE2\x98\x90");
          a += 4;
          while (a < q->len && q->s[a] == ' ') a++;
        }
        cells_str(&pre1, mark, task ? (done ? M.done : M.link) : num >= 0 ? M.fg : M.link, M.bg, 0);
        for (k = 0; k < str_cols(mark); k++) cells_add(&pre2, ' ', M.fg, M.bg, 0);
        inline_cells(&para, q->s + a, q->len - a, done ? M.dim : M.fg, M.bg, done ? RGB_STRIKE : 0);
        for (y++; y < n && !is_blank(&d->row[y]) && !list_mark(&d->row[y], &num) &&
                  indent_of(&d->row[y]) > ind; y++) {	/* its lines that go on */
          const Row *c = &d->row[y];
          cells_add(&para, ' ', M.fg, M.bg, 0);
          inline_cells(&para, c->s + skip_ws(c), c->len - skip_ws(c), done ? M.dim : M.fg, M.bg,
                       done ? RGB_STRIKE : 0);
        }
        flow(para.v, para.n, &pre1, &pre2, M.bg);
        while (y < n && is_blank(&d->row[y]) && y + 1 < n && list_mark(&d->row[y + 1], &num)) y++;	/* a loose list */
      }
      continue;
    }
    if ((mk = footnote_mark(r, &fid, &fidn)) > 0) {	/* [^1]: the note itself */
      char href[80];
      char *sl = slug_of(fid, fidn);
      snprintf(href, sizeof(href), "fn-%s", sl);
      free(sl);
      anchor_add(href, strlen(href));
      cells_add(&pre1, '[', M.link, M.bg, 0);
      {
        size_t k;
        for (k = 0; k < fidn; k++) cells_add(&pre1, (unsigned char)fid[k], M.link, M.bg, 0);
      }
      cells_add(&pre1, ']', M.link, M.bg, 0);
      cells_add(&pre1, ' ', M.fg, M.bg, 0);
      {	/* the note's other rows line up under its text */
        size_t k;
        for (k = 0; k < pre1.n; k++) cells_add(&pre2, ' ', M.fg, M.bg, 0);
      }
      inline_cells(&para, r->s + mk, r->len - mk, M.dim, M.bg, 0);
      for (y++; y < n && !is_blank(&d->row[y]) && indent_of(&d->row[y]) >= 2; y++) {
        const Row *c = &d->row[y];
        cells_add(&para, ' ', M.dim, M.bg, 0);
        inline_cells(&para, c->s + skip_ws(c), c->len - skip_ws(c), M.dim, M.bg, 0);
      }
      flow(para.v, para.n, &pre1, &pre2, M.bg);
      continue;
    }
    if (is_html_open(r)) {	/* a block of HTML: what it says, without its tags */
      html_block(d, &y);
      continue;
    }
    if (indent_of(r) >= 4) {	/* indented code */
      for (; y < n && (indent_of(&d->row[y]) >= 4 || is_blank(&d->row[y])); y++) {
        Row cut = d->row[y];
        size_t k = 0, col = 0;
        M.line = y;
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
        if (para.n && (is_fence(q, &c2) || q->s[a] == '#' || q->s[a] == '>' || list_mark(q, &nn) ||
                       is_rule(q) || setext_level(q))) break;
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


/*
** {==================================================================
** The page, and the two halves scrolled together
** ===================================================================
*/

static void set_colors (void) {
  uint32_t bg;
  M.fg = ui_color(C_EDITOR_FG);
  M.bg = ui_color(C_EDITOR_BG);
  M.dim = ui_color(C_DIM);
  M.border = ui_color(C_BORDER);
  M.link = ui_color(C_ICON_BLUE);
  M.done = ui_color(C_ICON_GREEN);
  M.quote = ui_color(C_DIM);
  M.code_bg = ui_color(C_INPUT_BG);
  theme_tok(TOK(T_HEADING, B_EDITOR), &M.head, &bg);
  theme_tok(TOK(T_STRING, B_EDITOR), &M.code_fg, &bg);
}


/*
** The rows of the page counted (and the line each came from, and where
** the headings are). The text and the width are remembered: a page that
** did not change is not walked again, which is what keeps a file of a
** few thousand lines smooth.
*/
static void count_rows (const Doc *d, int w) {
  if (M.c_doc == d && M.c_edits == d->edits && M.c_len == d->n && M.c_w == w) return;
  free(M.dir);
  M.dir = (d->path && *d->path) ? path_dirname(d->path) : NULL;
  set_colors();
  M.x = 2;
  M.w = w - 3;
  M.h = 0;
  M.top = (size_t)-1;
  M.row = 0;
  M.nsrc = 0;
  M.counting = 1;
  anchors_clear();
  links_clear();
  if (M.w >= 10) blocks(d);
  M.counting = 0;
  M.c_doc = d;
  M.c_edits = d->edits;
  M.c_len = d->n;
  M.c_w = w;
}


/* the width the preview was last drawn at; 0: it has not been */
int md_width (void) {
  return M.c_w;
}


/* how many rows the preview of d has at this width */
size_t md_rows (const Doc *d, int w) {
  count_rows(d, w);
  return M.nsrc;
}


/* the row of the preview that line of the text is drawn on */
size_t md_row_of_line (const Doc *d, int w, size_t line) {
  size_t i;
  count_rows(d, w);
  for (i = 0; i < M.nsrc; i++)
    if (M.src[i] >= line) return i;
  return M.nsrc ? M.nsrc - 1 : 0;
}


/* and the line the preview's row came from */
size_t md_line_of_row (const Doc *d, int w, size_t row) {
  count_rows(d, w);
  if (M.nsrc == 0) return 0;
  return M.src[row < M.nsrc ? row : M.nsrc - 1];
}


/* the row a [jump](#anchor) lands on; 0: there is no such heading */
int md_anchor_row (const Doc *d, int w, const char *name, size_t *row) {
  char *want;
  size_t i;
  int got = 0;
  count_rows(d, w);
  while (*name == '#') name++;
  want = slug_of(name, strlen(name));
  for (i = 0; i < M.nan && !got; i++)
    if (strcmp(M.an[i].slug, want) == 0) {
      *row = M.an[i].row;
      got = 1;
    }
  free(want);
  return got;
}


/*
** The link under x, y of the last preview drawn; *link is the target as
** the document wrote it ("#anchor", "notes/other.md", "https://..."),
** to free. 0: nothing is there.
*/
int md_link_at (int x, int y, char **link) {
  int id;
  if (M.map == NULL || y < M.y || y >= M.y + M.map_h || x < M.x || x >= M.x + M.map_w) return 0;
  id = M.map[(y - M.y) * M.map_w + (x - M.x)];
  if (id <= 0 || (size_t)id > M.nlink) return 0;
  *link = xstrdup(M.links[id - 1]);
  return 1;
}


/* the preview of d at x, y (w by h), from row *top (kept inside the page) */
void md_draw (const Doc *d, int x, int y, int w, int h, size_t *top) {
  int r;
  count_rows(d, w);
  set_colors();
  M.x = x + 2;	/* the page's margin */
  M.y = y;
  M.w = w - 3;
  M.h = h;
  if (M.w < 10) return;
  for (r = 0; r < h; r++) {	/* the ground, the margin too */
    int k;
    for (k = 0; k < w; k++) scr_put_rgb(x + k, y + r, ' ', M.fg, M.bg, 0);
  }
  if (*top + (size_t)h > M.nsrc) *top = M.nsrc > (size_t)h ? M.nsrc - (size_t)h : 0;
  if (M.map_w != M.w || M.map_h != h) {	/* where the links are, for the mouse */
    free(M.map);
    M.map = (int *)xmalloc((size_t)M.w * (size_t)h * sizeof(int));
    M.map_w = M.w;
    M.map_h = h;
  }
  memset(M.map, 0, (size_t)M.map_w * (size_t)M.map_h * sizeof(int));
  links_clear();
  M.top = *top;
  M.row = 0;
  blocks(d);
}

/* }================================================================== */
