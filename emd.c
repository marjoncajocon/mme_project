/*
** emd.c - the Markdown preview (Ctrl+Shift+V, Ctrl+K V), like VS Code's
**
** The text is drawn as a page for the terminal: headings bold in color
** (the first two with a rule under them), paragraphs wrapped, lists with
** bullets and task boxes, quotes with a bar (one for each > deep), code
** blocks on a darker ground with their language's colors, inline `code`,
** **bold**, *italic*, ~~struck~~, links underlined (Ctrl+click follows
** them), footnotes, HTML shown as its text and tables in boxes, their
** columns aligned as the :---: row asks. Pictures on a line of their own
** are drawn (eimage.c's), $math$ is made text as KaTeX would set it, and
** a mermaid flowchart is drawn in boxes and arrows.
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
  int c_px;	/* the cells' pixels then */
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
** Math: $x^2$ and $$\frac{a}{b}$$ as VS Code's KaTeX shows them, in
** text - Greek letters, operators and arrows as their characters, x^2
** and a_i raised and lowered when Unicode has the letters (x², aᵢ),
** \frac{a}{b} as a/b, \sqrt{x} as √x, \text{...} as it is
** ===================================================================
*/

#define UPRIGHT	'\x01'	/* in the text made: the letters after it are not a variable's (italic) until the next one */

static const struct {
  const char *name, *text;
} tex_sym[] = {
  {"alpha", "\xCE\xB1"}, {"beta", "\xCE\xB2"}, {"gamma", "\xCE\xB3"}, {"delta", "\xCE\xB4"},
  {"epsilon", "\xCF\xB5"}, {"varepsilon", "\xCE\xB5"}, {"zeta", "\xCE\xB6"}, {"eta", "\xCE\xB7"},
  {"theta", "\xCE\xB8"}, {"vartheta", "\xCF\x91"}, {"iota", "\xCE\xB9"}, {"kappa", "\xCE\xBA"},
  {"lambda", "\xCE\xBB"}, {"mu", "\xCE\xBC"}, {"nu", "\xCE\xBD"}, {"xi", "\xCE\xBE"}, {"pi", "\xCF\x80"},
  {"varpi", "\xCF\x96"}, {"rho", "\xCF\x81"}, {"varrho", "\xCF\xB1"}, {"sigma", "\xCF\x83"},
  {"varsigma", "\xCF\x82"}, {"tau", "\xCF\x84"}, {"upsilon", "\xCF\x85"}, {"phi", "\xCF\x95"},
  {"varphi", "\xCF\x86"}, {"chi", "\xCF\x87"}, {"psi", "\xCF\x88"}, {"omega", "\xCF\x89"},
  {"Gamma", "\xCE\x93"}, {"Delta", "\xCE\x94"}, {"Theta", "\xCE\x98"}, {"Lambda", "\xCE\x9B"},
  {"Xi", "\xCE\x9E"}, {"Pi", "\xCE\xA0"}, {"Sigma", "\xCE\xA3"}, {"Upsilon", "\xCE\xA5"},
  {"Phi", "\xCE\xA6"}, {"Psi", "\xCE\xA8"}, {"Omega", "\xCE\xA9"},
  {"times", "\xC3\x97"}, {"cdot", "\xE2\x8B\x85"}, {"div", "\xC3\xB7"}, {"pm", "\xC2\xB1"},
  {"mp", "\xE2\x88\x93"}, {"ast", "\xE2\x88\x97"}, {"star", "\xE2\x8B\x86"}, {"circ", "\xE2\x88\x98"},
  {"bullet", "\xE2\x88\x99"}, {"oplus", "\xE2\x8A\x95"}, {"otimes", "\xE2\x8A\x97"},
  {"leq", "\xE2\x89\xA4"}, {"le", "\xE2\x89\xA4"}, {"geq", "\xE2\x89\xA5"}, {"ge", "\xE2\x89\xA5"},
  {"neq", "\xE2\x89\xA0"}, {"ne", "\xE2\x89\xA0"}, {"approx", "\xE2\x89\x88"}, {"equiv", "\xE2\x89\xA1"},
  {"sim", "\xE2\x88\xBC"}, {"simeq", "\xE2\x89\x83"}, {"cong", "\xE2\x89\x85"}, {"propto", "\xE2\x88\x9D"},
  {"ll", "\xE2\x89\xAA"}, {"gg", "\xE2\x89\xAB"}, {"infty", "\xE2\x88\x9E"}, {"partial", "\xE2\x88\x82"},
  {"nabla", "\xE2\x88\x87"}, {"sum", "\xE2\x88\x91"}, {"prod", "\xE2\x88\x8F"}, {"coprod", "\xE2\x88\x90"},
  {"int", "\xE2\x88\xAB"}, {"iint", "\xE2\x88\xAC"}, {"iiint", "\xE2\x88\xAD"}, {"oint", "\xE2\x88\xAE"},
  {"in", "\xE2\x88\x88"}, {"notin", "\xE2\x88\x89"}, {"ni", "\xE2\x88\x8B"}, {"subset", "\xE2\x8A\x82"},
  {"subseteq", "\xE2\x8A\x86"}, {"supset", "\xE2\x8A\x83"}, {"supseteq", "\xE2\x8A\x87"},
  {"cup", "\xE2\x88\xAA"}, {"cap", "\xE2\x88\xA9"}, {"setminus", "\xE2\x88\x96"}, {"emptyset", "\xE2\x88\x85"},
  {"varnothing", "\xE2\x88\x85"}, {"forall", "\xE2\x88\x80"}, {"exists", "\xE2\x88\x83"},
  {"nexists", "\xE2\x88\x84"}, {"neg", "\xC2\xAC"}, {"lnot", "\xC2\xAC"}, {"land", "\xE2\x88\xA7"},
  {"wedge", "\xE2\x88\xA7"}, {"lor", "\xE2\x88\xA8"}, {"vee", "\xE2\x88\xA8"}, {"to", "\xE2\x86\x92"},
  {"rightarrow", "\xE2\x86\x92"}, {"leftarrow", "\xE2\x86\x90"}, {"gets", "\xE2\x86\x90"},
  {"leftrightarrow", "\xE2\x86\x94"}, {"Rightarrow", "\xE2\x87\x92"}, {"Leftarrow", "\xE2\x87\x90"},
  {"Leftrightarrow", "\xE2\x87\x94"}, {"implies", "\xE2\x9F\xB9"}, {"iff", "\xE2\x9F\xBA"},
  {"mapsto", "\xE2\x86\xA6"}, {"uparrow", "\xE2\x86\x91"}, {"downarrow", "\xE2\x86\x93"},
  {"longrightarrow", "\xE2\x9F\xB6"}, {"longleftarrow", "\xE2\x9F\xB5"},
  {"ldots", "\xE2\x80\xA6"}, {"dots", "\xE2\x80\xA6"}, {"cdots", "\xE2\x8B\xAF"}, {"vdots", "\xE2\x8B\xAE"},
  {"ddots", "\xE2\x8B\xB1"}, {"angle", "\xE2\x88\xA0"}, {"perp", "\xE2\x8A\xA5"}, {"parallel", "\xE2\x88\xA5"},
  {"mid", "\xE2\x88\xA3"}, {"langle", "\xE2\x9F\xA8"}, {"rangle", "\xE2\x9F\xA9"}, {"lfloor", "\xE2\x8C\x8A"},
  {"rfloor", "\xE2\x8C\x8B"}, {"lceil", "\xE2\x8C\x88"}, {"rceil", "\xE2\x8C\x89"}, {"hbar", "\xE2\x84\x8F"},
  {"ell", "\xE2\x84\x93"}, {"Re", "\xE2\x84\x9C"}, {"Im", "\xE2\x84\x91"}, {"aleph", "\xE2\x84\xB5"},
  {"prime", "\xE2\x80\xB2"}, {"degree", "\xC2\xB0"}, {"triangle", "\xE2\x96\xB3"}, {"square", "\xE2\x96\xA1"},
  {"therefore", "\xE2\x88\xB4"}, {"because", "\xE2\x88\xB5"}, {"vert", "|"}, {"lvert", "|"}, {"rvert", "|"},
  {"Vert", "\xE2\x80\x96"}, {"lbrace", "{"}, {"rbrace", "}"}, {"quad", "  "}, {"qquad", "    "},
  {"colon", ":"}, {"backslash", "\\"}, {"dagger", "\xE2\x80\xA0"}, {"top", "\xE2\x8A\xA4"}, {"bot", "\xE2\x8A\xA5"}
};

/* the names written upright (sin, lim ...): their word as it is */
static const char *const tex_func[] = {
  "sin", "cos", "tan", "cot", "sec", "csc", "arcsin", "arccos", "arctan", "sinh", "cosh", "tanh",
  "log", "ln", "lg", "exp", "lim", "limsup", "liminf", "max", "min", "sup", "inf", "det", "dim",
  "ker", "deg", "gcd", "lcm", "arg", "Pr", "mod", "bmod", "hom"
};

/* the letters Unicode raises and lowers: "0123456789+-=()" then letters */
static const char *const sup_from = "0123456789+-=()abcdefghijklmnoprstuvwxyzABDEGHIJKLMNOPRTUVW";
static const char *const sup_to[] = {
  "\xE2\x81\xB0", "\xC2\xB9", "\xC2\xB2", "\xC2\xB3", "\xE2\x81\xB4", "\xE2\x81\xB5", "\xE2\x81\xB6",
  "\xE2\x81\xB7", "\xE2\x81\xB8", "\xE2\x81\xB9", "\xE2\x81\xBA", "\xE2\x81\xBB", "\xE2\x81\xBC",
  "\xE2\x81\xBD", "\xE2\x81\xBE",
  "\xE1\xB5\x83", "\xE1\xB5\x87", "\xE1\xB6\x9C", "\xE1\xB5\x88", "\xE1\xB5\x89", "\xE1\xB6\xA0",
  "\xE1\xB5\x8D", "\xCA\xB0", "\xE2\x81\xB1", "\xCA\xB2", "\xE1\xB5\x8F", "\xCB\xA1", "\xE1\xB5\x90",
  "\xE2\x81\xBF", "\xE1\xB5\x92", "\xE1\xB5\x96", "\xCA\xB3", "\xCB\xA2", "\xE1\xB5\x97", "\xE1\xB5\x98",
  "\xE1\xB5\x9B", "\xCA\xB7", "\xCB\xA3", "\xCA\xB8", "\xE1\xB6\xBB",
  "\xE1\xB4\xAC", "\xE1\xB4\xAE", "\xE1\xB4\xB0", "\xE1\xB4\xB1", "\xE1\xB4\xB3", "\xE1\xB4\xB4",
  "\xE1\xB4\xB5", "\xE1\xB4\xB6", "\xE1\xB4\xB7", "\xE1\xB4\xB8", "\xE1\xB4\xB9", "\xE1\xB4\xBA",
  "\xE1\xB4\xBC", "\xE1\xB4\xBE", "\xE1\xB4\xBF", "\xE1\xB5\x80", "\xE1\xB5\x81", "\xE2\xB1\xBD",
  "\xE1\xB5\x82"
};
static const char *const sub_from = "0123456789+-=()aehijklmnoprstuvx";
static const char *const sub_to[] = {
  "\xE2\x82\x80", "\xE2\x82\x81", "\xE2\x82\x82", "\xE2\x82\x83", "\xE2\x82\x84", "\xE2\x82\x85",
  "\xE2\x82\x86", "\xE2\x82\x87", "\xE2\x82\x88", "\xE2\x82\x89", "\xE2\x82\x8A", "\xE2\x82\x8B",
  "\xE2\x82\x8C", "\xE2\x82\x8D", "\xE2\x82\x8E",
  "\xE2\x82\x90", "\xE2\x82\x91", "\xE2\x82\x95", "\xE1\xB5\xA2", "\xE2\xB1\xBC", "\xE2\x82\x96",
  "\xE2\x82\x97", "\xE2\x82\x98", "\xE2\x82\x99", "\xE2\x82\x92", "\xE2\x82\x9A", "\xE1\xB5\xA3",
  "\xE2\x82\x9B", "\xE2\x82\x9C", "\xE1\xB5\xA4", "\xE1\xB5\xA5", "\xE2\x82\x93"
};


/* the letters with an accent of their own: \hat{a} is â */
static const char *const acc_hat[] = {
  "\xC3\xA2", "\xC3\xAA", "\xC3\xAE", "\xC3\xB4", "\xC3\xBB", "\xC5\xB7", "\xC4\x89", "\xC4\x9D", "\xC5\x9D",
  "\xC4\xA5", "\xC4\xB5", "\xC5\xB5", "\xE1\xBA\x91"
};
static const char *const acc_bar[] = {"\xC4\x81", "\xC4\x93", "\xC4\xAB", "\xC5\x8D", "\xC5\xAB", "\xC8\xB3"};
static const char *const acc_dot[] = {
  "\xC8\xA7", "\xE1\xB8\x83", "\xC4\x8B", "\xE1\xB8\x8B", "\xC4\x97", "\xE1\xB8\x9F", "\xC4\xA1", "\xE1\xB8\xA3",
  "\xE1\xB9\x81", "\xE1\xB9\x85", "\xC8\xAF", "\xE1\xB9\x97", "\xE1\xB9\x99", "\xE1\xB9\xA1", "\xE1\xB9\xAB",
  "\xE1\xBA\x87", "\xE1\xBA\x8B", "\xE1\xBA\x8F", "\xC5\xBC"
};
static const char *const acc_ddot[] = {"\xC3\xA4", "\xC3\xAB", "\xC3\xAF", "\xC3\xB6", "\xC3\xBC", "\xC3\xBF", "\xE1\xBA\x8D"};
static const char *const acc_tilde[] = {
  "\xC3\xA3", "\xE1\xBA\xBD", "\xC4\xA9", "\xC3\xB1", "\xC3\xB5", "\xC5\xA9", "\xE1\xBB\xB9"
};


static void tex (Buf *o, const char *s, size_t n, int display);


/* the argument of a command or of ^ _ at *i: a {group}'s inside, a \command, or one character */
static void tex_arg (const char *s, size_t n, size_t *i, const char **a, size_t *an) {
  size_t k = *i;
  while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
  *a = s + k;
  if (k >= n) {
    *an = 0;
    *i = k;
    return;
  }
  if (s[k] == '{') {
    size_t e = k + 1;
    int depth = 1;
    while (e < n && depth) {
      if (s[e] == '\\' && e + 1 < n) e++;
      else if (s[e] == '{') depth++;
      else if (s[e] == '}') depth--;
      if (depth) e++;
    }
    *a = s + k + 1;
    *an = e - k - 1;
    *i = e < n ? e + 1 : n;
    return;
  }
  if (s[k] == '\\') {
    size_t e = k + 1;
    while (e < n && ((s[e] >= 'a' && s[e] <= 'z') || (s[e] >= 'A' && s[e] <= 'Z'))) e++;
    if (e == k + 1 && e < n) e++;
    *an = e - k;
    *i = e;
    return;
  }
  {
    size_t len;
    utf8_decode(s + k, n - k, &len);
    *an = len ? len : 1;
    *i = k + *an;
  }
}


/* an argument made into text */
static char *tex_str (const char *a, size_t an) {
  Buf b;
  buf_init(&b);
  tex(&b, a, an, 0);
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* t raised (up) or lowered in Unicode's small letters; when one has none: ^(t), or ^t for one character */
static void tex_script (Buf *o, const char *t, int up) {
  const char *from = up ? sup_from : sub_from;
  const char *const *to = up ? sup_to : sub_to;
  size_t i, n = strlen(t), vis = 0;
  int all = 1;
  for (i = 0; i < n; i++) {
    const char *p;
    if (t[i] == UPRIGHT) continue;
    vis++;
    if (up && (unsigned char)t[i] == 0xE2 && i + 2 < n && (unsigned char)t[i + 1] == 0x80 &&
        (unsigned char)t[i + 2] == 0xB2) {	/* x^\prime: the prime itself */
      i += 2;
      continue;
    }
    if (t[i] == ' ' || (p = strchr(from, t[i])) == NULL || t[i] == '\0') all = 0;
  }
  if (all && vis) {
    for (i = 0; i < n; i++) {
      if (t[i] == UPRIGHT) continue;
      if ((unsigned char)t[i] == 0xE2) {
        buf_putn(o, t + i, 3);
        i += 2;
        continue;
      }
      buf_puts(o, to[strchr(from, t[i]) - from]);
    }
    return;
  }
  buf_putc(o, up ? '^' : '_');
  if (str_cols(t) <= 1) buf_puts(o, t);
  else {
    buf_putc(o, '(');
    buf_puts(o, t);
    buf_putc(o, ')');
  }
}


/* a / b's halves: in parentheses when there is more to them than one term */
static void tex_part (Buf *o, const char *t) {
  int wrap = strpbrk(t, "+-/ ,=<>") != NULL;
  if (wrap) buf_putc(o, '(');
  buf_puts(o, t);
  if (wrap) buf_putc(o, ')');
}


/* TeX's math at s (n bytes) as text into o; display: \\ breaks the line */
static void tex (Buf *o, const char *s, size_t n, int display) {
  size_t i = 0, k;
  while (i < n) {
    char c = s[i];
    if (c == '\\' && i + 1 < n && !((s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z'))) {
      char d = s[i + 1];
      if (d == '\\') buf_putc(o, display ? '\n' : ' ');
      else if (d == ' ' || d == ';' || d == ':' || d == '>') buf_putc(o, ' ');
      else if (d == '|') buf_puts(o, "\xE2\x80\x96");
      else if (d != ',' && d != '!') buf_putc(o, d);	/* \{ \} \$ \% \_ \# */
      i += 2;
      continue;
    }
    if (c == '\\') {
      size_t e = i + 1, nl;
      const char *a, *b;
      size_t an, bn;
      char name[24];
      while (e < n && ((s[e] >= 'a' && s[e] <= 'z') || (s[e] >= 'A' && s[e] <= 'Z'))) e++;
      nl = e - i - 1;
      snprintf(name, sizeof(name), "%.*s", (int)(nl < 23 ? nl : 23), s + i + 1);
      i = e;
      for (k = 0; k < sizeof(tex_sym) / sizeof(tex_sym[0]); k++)
        if (strcmp(name, tex_sym[k].name) == 0) break;
      if (k < sizeof(tex_sym) / sizeof(tex_sym[0])) {
        buf_puts(o, tex_sym[k].text);
        continue;
      }
      for (k = 0; k < sizeof(tex_func) / sizeof(tex_func[0]); k++)
        if (strcmp(name, tex_func[k]) == 0) break;
      if (k < sizeof(tex_func) / sizeof(tex_func[0])) {
        buf_putc(o, UPRIGHT);
        buf_puts(o, name[0] == 'b' && strcmp(name, "bmod") == 0 ? "mod" : name);
        buf_putc(o, UPRIGHT);
        if (i < n && s[i] != '_' && s[i] != '^' && s[i] != '(') buf_putc(o, ' ');
        continue;
      }
      if (strcmp(name, "frac") == 0 || strcmp(name, "dfrac") == 0 || strcmp(name, "tfrac") == 0 ||
          strcmp(name, "cfrac") == 0 || strcmp(name, "binom") == 0) {
        char *p, *q;
        tex_arg(s, n, &i, &a, &an);
        tex_arg(s, n, &i, &b, &bn);
        p = tex_str(a, an);
        q = tex_str(b, bn);
        if (name[0] == 'b') buf_printf(o, "C(%s, %s)", p, q);
        else {
          tex_part(o, p);
          buf_putc(o, '/');
          tex_part(o, q);
        }
        free(p);
        free(q);
        continue;
      }
      if (strcmp(name, "sqrt") == 0) {	/* \sqrt{x}, \sqrt[3]{x} */
        char *p, idx[16] = "";
        while (i < n && s[i] == ' ') i++;
        if (i < n && s[i] == '[') {
          size_t e2 = i + 1;
          while (e2 < n && s[e2] != ']') e2++;
          snprintf(idx, sizeof(idx), "%.*s", (int)(e2 - i - 1 < 15 ? e2 - i - 1 : 15), s + i + 1);
          i = e2 < n ? e2 + 1 : n;
        }
        tex_arg(s, n, &i, &a, &an);
        p = tex_str(a, an);
        if (strcmp(idx, "3") == 0) buf_puts(o, "\xE2\x88\x9B");
        else if (strcmp(idx, "4") == 0) buf_puts(o, "\xE2\x88\x9C");
        else {
          if (idx[0]) tex_script(o, idx, 1);
          buf_puts(o, "\xE2\x88\x9A");
        }
        if (str_cols(p) <= 1) buf_puts(o, p);
        else buf_printf(o, "(%s)", p);
        free(p);
        continue;
      }
      if (strncmp(name, "text", 4) == 0 || strcmp(name, "mbox") == 0 || strcmp(name, "operatorname") == 0 ||
          strcmp(name, "mathrm") == 0) {	/* words, upright */
        char *p;
        tex_arg(s, n, &i, &a, &an);
        buf_putc(o, UPRIGHT);
        if (name[0] == 't' || strcmp(name, "mbox") == 0) buf_putn(o, a, an);	/* \text{...}: as it is written */
        else {
          p = tex_str(a, an);
          buf_puts(o, p);
          free(p);
        }
        buf_putc(o, UPRIGHT);
        continue;
      }
      if (strcmp(name, "mathbb") == 0) {	/* the number sets */
        static const char *const bb[][2] = {
          {"R", "\xE2\x84\x9D"}, {"N", "\xE2\x84\x95"}, {"Z", "\xE2\x84\xA4"}, {"Q", "\xE2\x84\x9A"},
          {"C", "\xE2\x84\x82"}, {"P", "\xE2\x84\x99"}, {"H", "\xE2\x84\x8D"}
        };
        tex_arg(s, n, &i, &a, &an);
        for (k = 0; k < sizeof(bb) / sizeof(bb[0]); k++)
          if (an == 1 && a[0] == bb[k][0][0]) break;
        if (k < sizeof(bb) / sizeof(bb[0])) buf_puts(o, bb[k][1]);
        else buf_putn(o, a, an);
        continue;
      }
      {	/* accents: a letter that has one of its own (â, ā, ẋ, ñ, ä), else the mark after it (a cell has no room over it) */
        static const struct {
          const char *name, *from, *mark;
          const char *const *to;
        } acc[] = {
          {"hat", "aeiouycgshjwz", "\xCB\x86", acc_hat}, {"widehat", "aeiouycgshjwz", "\xCB\x86", acc_hat},
          {"bar", "aeiouy", "\xC2\xAF", acc_bar}, {"overline", "aeiouy", "\xC2\xAF", acc_bar},
          {"dot", "abcdefghmnoprstwxyz", "\xCB\x99", acc_dot}, {"ddot", "aeiouyx", "\xC2\xA8", acc_ddot},
          {"tilde", "aeinouy", "\xCB\x9C", acc_tilde}, {"widetilde", "aeinouy", "\xCB\x9C", acc_tilde},
          {"vec", "", "\xE2\x83\x97", NULL}, {"underline", "", "", NULL}
        };
        for (k = 0; k < sizeof(acc) / sizeof(acc[0]); k++)
          if (strcmp(name, acc[k].name) == 0) break;
        if (k < sizeof(acc) / sizeof(acc[0])) {
          char *p;
          const char *f;
          tex_arg(s, n, &i, &a, &an);
          p = tex_str(a, an);
          if (p[0] && p[1] == '\0' && acc[k].to && (f = strchr(acc[k].from, p[0])) != NULL)
            buf_puts(o, acc[k].to[f - acc[k].from]);
          else if (strcmp(name, "vec") == 0) buf_printf(o, "%s\xE2\x83\x97", p);	/* the arrow over it (math_cells: after it) */
          else {
            buf_puts(o, p);
            buf_puts(o, acc[k].mark);
          }
          free(p);
          continue;
        }
      }
      if (strncmp(name, "math", 4) == 0 || strcmp(name, "boldsymbol") == 0 || strcmp(name, "bm") == 0) {
        char *p;	/* \mathbf{x}, \mathcal{L} ...: the letters */
        tex_arg(s, n, &i, &a, &an);
        p = tex_str(a, an);
        buf_puts(o, p);
        free(p);
        continue;
      }
      if (strcmp(name, "begin") == 0 || strcmp(name, "end") == 0) {	/* \begin{aligned}: the lines, not the name */
        tex_arg(s, n, &i, &a, &an);
        continue;
      }
      if (strcmp(name, "left") == 0 || strcmp(name, "right") == 0 || strncmp(name, "big", 3) == 0 ||
          strncmp(name, "Big", 3) == 0 || strcmp(name, "displaystyle") == 0 || strcmp(name, "limits") == 0 ||
          strcmp(name, "nolimits") == 0 || strcmp(name, "textstyle") == 0) {
        if (i < n && s[i] == '.') i++;	/* \left. says nothing */
        continue;
      }
      buf_putc(o, '\\');	/* not known: as written */
      buf_puts(o, name);
      continue;
    }
    if (c == '^' || c == '_') {
      const char *a;
      size_t an;
      char *p;
      i++;
      tex_arg(s, n, &i, &a, &an);
      p = tex_str(a, an);
      tex_script(o, p, c == '^');
      free(p);
      continue;
    }
    if (c == '{' || c == '}') {	/* a group: only what is in it */
      i++;
      continue;
    }
    if (c == '&') {	/* an aligned line's column */
      i++;
      continue;
    }
    if (c == '~') {
      buf_putc(o, ' ');
      i++;
      continue;
    }
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (c == ' ' && (o->len == 0 || o->s[o->len - 1] == ' ' || o->s[o->len - 1] == '\n')) {
      i++;
      continue;
    }
    buf_putc(o, c);
    i++;
  }
}


/* math's text as cells: a variable's letters italic, the rest upright */
static void math_cells (Cells *out, const char *s, size_t n, uint32_t fg, uint32_t bg) {
  size_t i = 0, len;
  int upright = 0;
  while (i < n) {
    uint32_t cp;
    if (s[i] == UPRIGHT) {
      upright = !upright;
      i++;
      continue;
    }
    cp = utf8_decode(s + i, n - i, &len);
    if (len == 0) len = 1;
    if (cp == 0x20D7) cp = 0x2192;	/* \vec's arrow: a cell has no room over its letter, so after it */
    else if (cp >= 0x300 && cp < 0x370) {	/* a mark that joins the one before: a cell cannot */
      i += len;
      continue;
    }
    cells_add(out, cp, fg, bg, !upright && ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') ||
                                            (cp >= 0x3B1 && cp <= 0x3C9)) ? RGB_ITALIC : 0);
    i += len;
  }
}


/* $ ... $ at s[i]: where its closing $ is (as markdown-it-katex: no space inside either $, no digit after the last) */
static size_t math_end (const char *s, size_t n, size_t i) {
  size_t e;
  if (i + 1 >= n || s[i + 1] == ' ' || s[i + 1] == '\t' || s[i + 1] == '$') return 0;
  for (e = i + 1; e < n; e++) {
    if (s[e] == '\\') {
      e++;
      continue;
    }
    if (s[e] == '$') {
      if (s[e - 1] == ' ' || s[e - 1] == '\t') return 0;
      if (e + 1 < n && s[e + 1] >= '0' && s[e + 1] <= '9') return 0;
      return e;
    }
  }
  return 0;
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
    if (c == '\\' && i + 1 < n && strchr("\\`*_{}[]()#+-.!|<>~$", s[i + 1])) {	/* an escaped sign */
      cells_add(out, (unsigned char)s[i + 1], fg, bg, at);
      i += 2;
      continue;
    }
    if (c == '$') {	/* $math$ (or $$math$$ in a line of text) */
      size_t a = i + 1, e = 0;
      if (i + 1 < n && s[i + 1] == '$') {
        for (e = i + 2; e + 1 < n && !(s[e] == '$' && s[e + 1] == '$'); e++) ;
        a = i + 2;
        if (e + 1 >= n || e == a) e = 0;
      }
      else e = math_end(s, n, i);
      if (e) {
        Buf b;
        buf_init(&b);
        tex(&b, s + a, e - a, 0);
        math_cells(out, b.s ? b.s : "", b.len, fg, bg);
        buf_free(&b);
        i = e + (a == i + 2 ? 2 : 1);
        continue;
      }
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


/* ```mermaid */
static int is_mermaid (const Row *r) {
  size_t i = skip_ws(r) + 3;
  while (i < r->len && (r->s[i] == '`' || r->s[i] == '~' || r->s[i] == ' ')) i++;
  return r->len - i >= 7 && m_strnicmp(r->s + i, "mermaid", 7) == 0 &&
         (r->len - i == 7 || r->s[i + 7] == ' ' || r->s[i + 7] == '{');
}


/*
** $$ math $$ from line *y (on one line, or on the lines up to the one
** with the closing $$): its lines centred on the page
*/
static void math_block (const Doc *d, size_t *y) {
  Buf src, out;
  size_t n = d->n, i = 0, e;
  const Row *r = &d->row[*y];
  buf_init(&src);
  buf_init(&out);
  {	/* the text between the $$ */
    size_t a = skip_ws(r) + 2, b = r->len;
    const char *close = NULL;
    for (e = a; e + 1 < b; e++)
      if (r->s[e] == '$' && r->s[e + 1] == '$') close = r->s + e;
    if (close) {	/* $$ x $$ on its line */
      buf_putn(&src, r->s + a, (size_t)(close - (r->s + a)));
      (*y)++;
    }
    else {
      buf_putn(&src, r->s + a, b - a);
      for ((*y)++; *y < n; (*y)++) {
        const Row *q = &d->row[*y];
        const char *end = NULL;
        size_t k;
        for (k = 0; k + 1 < q->len; k++)
          if (q->s[k] == '$' && q->s[k + 1] == '$') {
            end = q->s + k;
            break;
          }
        buf_putc(&src, '\n');
        buf_putn(&src, q->s, end ? (size_t)(end - q->s) : q->len);
        if (end) {
          (*y)++;
          break;
        }
      }
    }
  }
  tex(&out, src.s ? src.s : "", src.len, 1);
  while (i < out.len) {	/* each line in the middle of the page */
    Cells row = {NULL, 0, 0}, m = {NULL, 0, 0};
    size_t k, a = i;
    int pad;
    while (i < out.len && out.s[i] != '\n') i++;
    while (a < i && out.s[a] == ' ') a++;
    math_cells(&m, out.s + a, i - a, M.fg, M.bg);
    pad = m.n < (size_t)(M.w - 2) ? (int)((size_t)(M.w - 2) - m.n) / 2 : 0;
    for (k = 0; k < (size_t)pad; k++) cells_add(&row, ' ', M.fg, M.bg, 0);
    for (k = 0; k < m.n; k++) cells_push(&row, &m.v[k]);
    if (m.n) {
      if (row.n > (size_t)M.w) row.n = (size_t)M.w;
      row_out(row.v, row.n, M.bg);
    }
    free(row.v);
    free(m.v);
    i++;
  }
  buf_free(&src);
  buf_free(&out);
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


/*
** {==================================================================
** Pictures: ![alt](pictures/x.png) or a data: URI, drawn in the page as
** the notebook draws its plots (eimage.c: sixel, painted by mme-sdl;
** Braille in a terminal without pictures), as wide as the page at most
** ===================================================================
*/

typedef struct Picture {
  char *key;	/* the file's path, or the data: URI */
  void *img;	/* eimage.c's, NULL: not a picture mme reads */
} Picture;

static Picture *g_pic;
static size_t g_npic;
static int g_real;	/* pictures shown themselves on this page (edraw.c keeps eight) */


static int b64_val (int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}


static unsigned char *b64_bytes (const char *s, size_t n, size_t *out) {
  unsigned char *b = (unsigned char *)xmalloc(n / 4 * 3 + 4);
  unsigned acc = 0;
  int bits = 0;
  size_t i, k = 0;
  for (i = 0; i < n; i++) {
    int v = b64_val((unsigned char)s[i]);
    if (v < 0) continue;
    acc = (acc << 6) | (unsigned)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      b[k++] = (unsigned char)((acc >> bits) & 255);
    }
  }
  *out = k;
  return b;
}


static void pictures_clear (void);


/* the picture url names (read once, then kept); NULL: none mme can show */
static void *picture (const char *url, size_t n) {
  char *key;
  void *img = NULL;
  size_t i;
  while (n && (*url == ' ' || *url == '<')) url++, n--;
  for (i = 0; i < n && url[i] != ' ' && url[i] != '>'; i++) ;	/* ![](x.png "its title") */
  n = i;
  if (n > 5 && m_strnicmp(url, "data:", 5) == 0) key = xstrndup(url, n);
  else if ((key = link_path(url, n)) == NULL) return NULL;	/* on the web: not fetched */
  for (i = 0; i < g_npic; i++)
    if (strcmp(g_pic[i].key, key) == 0) {
      free(key);
      return g_pic[i].img;
    }
  if (strncmp(key, "data:", 5) == 0) {
    const char *b = strstr(key, ";base64,");
    if (b) {
      size_t len;
      unsigned char *bytes = b64_bytes(b + 8, strlen(b + 8), &len);
      img = img_open_mem(bytes, len);
      free(bytes);
    }
  }
  else img = img_open(key);	/* NULL: not there */
  if (img && img_rows(img, 40) == 0) {	/* a kind it cannot read (JPEG, WEBP): the place-holder */
    img_close(img);
    img = NULL;
  }
  if (g_npic >= 32) pictures_clear();	/* enough kept: they are read again when shown again */
  g_pic = (Picture *)xrealloc(g_pic, (g_npic + 1) * sizeof(Picture));
  g_pic[g_npic].key = key;
  g_pic[g_npic].img = img;
  g_npic++;
  return img;
}


static void pictures_clear (void) {
  size_t i;
  for (i = 0; i < g_npic; i++) {
    free(g_pic[i].key);
    if (g_pic[i].img) img_close(g_pic[i].img);
  }
  g_npic = 0;
}


/* a line that is only a picture, "![alt](url)" or "<img src=...>": its url; 0 when it is not one */
static int picture_line (const Row *r, const char **url, size_t *un) {
  size_t i = skip_ws(r), e = r->len;
  while (e > i && (r->s[e - 1] == ' ' || r->s[e - 1] == '\t')) e--;
  if (e - i > 5 && r->s[i] == '!' && r->s[i + 1] == '[' && r->s[e - 1] == ')') {
    size_t b = i + 2;
    int depth = 1;
    while (b < e && depth) {
      if (r->s[b] == '[') depth++;
      else if (r->s[b] == ']') depth--;
      if (depth) b++;
    }
    if (b + 1 < e && r->s[b + 1] == '(' && memchr(r->s + b + 2, ')', e - b - 3) == NULL) {
      *url = r->s + b + 2;
      *un = e - 1 - (b + 2);
      return 1;
    }
  }
  if (e - i > 8 && m_strnicmp(r->s + i, "<img", 4) == 0 && r->s[e - 1] == '>') {
    size_t k;
    for (k = i; k + 5 < e; k++)
      if (m_strnicmp(r->s + k, "src=", 4) == 0) {
        char q = r->s[k + 4];
        size_t a = k + 4 + (q == '"' || q == '\'' ? 1 : 0), b = a;
        while (b < e && r->s[b] != (q == '"' || q == '\'' ? q : ' ') && r->s[b] != '>') b++;
        *url = r->s + a;
        *un = b - a;
        return 1;
      }
  }
  return 0;
}


/* the picture's rows of the page: blank cells it is drawn over */
static void picture_rows (void *img) {
  size_t first = M.row;
  int rows = img_rows(img, M.w - 2), k;
  for (k = 0; k < rows; k++) row_out(NULL, 0, M.bg);
  if (!M.counting && first + (size_t)rows > M.top && first < M.top + (size_t)M.h) {
    int skip = first < M.top ? (int)(M.top - first) : 0;
    int y = M.y + (int)(first + (size_t)skip - M.top), h = rows - skip;
    if (y + h > M.y + M.h) h = M.y + M.h - y;
    img_draw_at(img, M.x, y, M.w - 2, skip, h, g_real < 8);
    g_real++;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Mermaid: a ```mermaid block's flowchart (graph TD, graph LR ...) drawn
** as boxes and arrows in text, laid out in layers as mermaid does; the
** other kinds of diagram show their code, marked "mermaid"
** ===================================================================
*/

#define MM_NODES	64
#define MM_EDGES	128

typedef struct MNode {
  char id[32];
  char label[64];
  int shape;	/* '[' box, '(' round, '{' decision */
  int layer, order, x, y, w, h;
  int dummy;	/* a place an edge passes through a layer */
} MNode;

typedef struct MEdge {
  int from, to;
  int arrow;
  char label[32];
} MEdge;

typedef struct Graph {
  MNode node[MM_NODES + MM_EDGES * 4];
  int nnode, nreal;
  MEdge edge[MM_EDGES];
  int nedge;
  int lr;	/* laid out left to right */
} Graph;

/* the drawing: each cell a character, or lines' ends (up 1, down 2, left 4, right 8) */
typedef struct Canvas {
  int w, h;
  uint32_t *cp;
  unsigned char *line, *kind;	/* kind: 0 nothing, 1 a box, 2 a line, 3 a label */
} Canvas;


static int mm_node (Graph *g, const char *id, size_t n) {
  int i;
  if (n == 0 || n >= sizeof(g->node[0].id)) return -1;
  for (i = 0; i < g->nnode; i++)
    if (strlen(g->node[i].id) == n && memcmp(g->node[i].id, id, n) == 0) return i;
  if (g->nnode == MM_NODES) return -1;
  memset(&g->node[g->nnode], 0, sizeof(MNode));
  memcpy(g->node[g->nnode].id, id, n);
  memcpy(g->node[g->nnode].label, id, n);
  g->node[g->nnode].shape = '[';
  return g->nnode++;
}


static int mm_idch (char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ? 1 : 0;
}


/* a node at s[*i]: "A", "A[Label]", "B(Round)", "C{Decision}" ...; its index, -1: none */
static int mm_parse_node (Graph *g, const char *s, size_t n, size_t *i) {
  size_t k = *i, a;
  int id;
  while (k < n && s[k] == ' ') k++;
  a = k;
  while (k < n && mm_idch(s[k]) && !(s[k] == '-' && k + 1 < n && (s[k + 1] == '-' || s[k + 1] == '.' || s[k + 1] == '>')))
    k++;
  if (k == a || (id = mm_node(g, s + a, k - a)) < 0) return -1;
  if (k < n && strchr("[({>", s[k])) {	/* its shape and label: to the brackets that close it */
    size_t b = k, e, l0, l1;
    char shape = s[k] == '>' ? '[' : s[k];
    while (b < n && strchr("[({>/\\", s[b])) b++;
    for (e = b; e < n && !strchr(")]}", s[e]); e++)
      if (s[e] == '"') {	/* "a quoted label (with brackets)" */
        e++;
        while (e < n && s[e] != '"') e++;
      }
    l0 = b;
    l1 = e;
    while (e < n && strchr(")]}/\\", s[e])) e++;
    while (l0 < l1 && (s[l0] == ' ' || s[l0] == '"')) l0++;
    while (l1 > l0 && (s[l1 - 1] == ' ' || s[l1 - 1] == '"')) l1--;
    if (b > k + 1 && s[k] == '(' && s[k + 1] == '(') shape = '(';
    if (b > k + 1 && s[k] == '{' && s[k + 1] == '{') shape = '{';
    g->node[id].shape = shape;
    {
      size_t m = 0, j;
      char *lab = g->node[id].label;
      for (j = l0; j < l1 && m + 1 < sizeof(g->node[id].label); j++) {
        if (s[j] == '<' && j + 3 < l1 && m_strnicmp(s + j, "<br", 3) == 0) {	/* <br/>: a space in one line */
          while (j < l1 && s[j] != '>') j++;
          lab[m++] = ' ';
          continue;
        }
        lab[m++] = s[j];
      }
      lab[m] = '\0';
    }
    k = e;
  }
  *i = k;
  return id;
}


/* a link at s[*i]: "-->", "---", "==>", "-.->", "-->|label|", "-- label -->"; 0 none, 1 a line, 2 an arrow */
static int mm_parse_link (const char *s, size_t n, size_t *i, char *label, size_t lsize) {
  size_t k = *i, a;
  int arrow = 0;
  label[0] = '\0';
  while (k < n && s[k] == ' ') k++;
  a = k;
  if (k < n && s[k] == '<') k++;
  while (k < n && (s[k] == '-' || s[k] == '=' || s[k] == '.')) k++;
  if (k - a < 2) return 0;
  if (k < n && (s[k] == '>' || ((s[k] == 'o' || s[k] == 'x') && (k + 1 >= n || s[k + 1] == ' ' || s[k + 1] == '|')))) {
    arrow = s[k] == '>';
    k++;
  }
  if (!arrow && k - a == 2 && k < n && s[k] == ' ') {	/* "-- label -->": the label, then the rest of the link */
    size_t e = k;
    while (e + 2 < n && !((s[e] == '-' || s[e] == '=' || s[e] == '.') && (s[e + 1] == '-' || s[e + 1] == '=') &&
                          (s[e + 2] == '>' || s[e + 2] == '-' || s[e + 2] == '='))) e++;
    if (e + 2 < n) {
      size_t l0 = k, l1 = e;
      while (l0 < l1 && s[l0] == ' ') l0++;
      while (l1 > l0 && s[l1 - 1] == ' ') l1--;
      snprintf(label, lsize, "%.*s", (int)(l1 - l0), s + l0);
      k = e;
      while (k < n && (s[k] == '-' || s[k] == '=' || s[k] == '.')) k++;
      if (k < n && s[k] == '>') {
        arrow = 1;
        k++;
      }
    }
  }
  while (k < n && s[k] == ' ') k++;
  if (k < n && s[k] == '|') {	/* -->|label| */
    size_t e = k + 1;
    while (e < n && s[e] != '|') e++;
    snprintf(label, lsize, "%.*s", (int)(e - k - 1), s + k + 1);
    k = e < n ? e + 1 : n;
  }
  *i = k;
  return arrow ? 2 : 1;
}


/* a statement: "A --> B --> C", "A & B --> C", "A[Label]"; the rest (style, class, click ...) is not drawn */
static void mm_statement (Graph *g, const char *s, size_t n) {
  int left[8], nl = 0, right[8], nr, k, j;
  size_t i = 0;
  while (i < n && s[i] == ' ') i++;
  if (n - i >= 5 && (strncmp(s + i, "style", 5) == 0 || strncmp(s + i, "class", 5) == 0 ||
                     strncmp(s + i, "click", 5) == 0 || strncmp(s + i, "linkStyle", 9) == 0)) return;
  if (n - i >= 8 && strncmp(s + i, "subgraph", 8) == 0) return;
  if (n - i == 3 && strncmp(s + i, "end", 3) == 0) return;
  if (n - i >= 9 && strncmp(s + i, "direction", 9) == 0) return;
  for (;;) {	/* the nodes on the left: A & B */
    int id = mm_parse_node(g, s, n, &i);
    if (id < 0) return;
    if (nl < 8) left[nl++] = id;
    while (i < n && s[i] == ' ') i++;
    if (i < n && s[i] == '&') i++;
    else break;
  }
  for (;;) {
    char label[32];
    int arrow = mm_parse_link(s, n, &i, label, sizeof(label));
    if (arrow == 0) return;
    nr = 0;
    for (;;) {
      int id = mm_parse_node(g, s, n, &i);
      if (id < 0) return;
      if (nr < 8) right[nr++] = id;
      while (i < n && s[i] == ' ') i++;
      if (i < n && s[i] == '&') i++;
      else break;
    }
    for (k = 0; k < nl; k++)
      for (j = 0; j < nr && g->nedge < MM_EDGES; j++) {
        MEdge *e = &g->edge[g->nedge++];
        e->from = left[k];
        e->to = right[j];
        e->arrow = arrow == 2;
        snprintf(e->label, sizeof(e->label), "%s", label);
      }
    memcpy(left, right, sizeof(right));
    nl = nr;
  }
}


/* the text of a mermaid block: a flowchart made into g; 0 another kind of diagram */
static int mm_parse (Graph *g, const Doc *d, size_t a, size_t b) {
  size_t y;
  int head = 0;
  memset(g, 0, sizeof(*g));
  for (y = a; y < b; y++) {
    const Row *r = &d->row[y];
    size_t i = skip_ws(r), e;
    if (i >= r->len || (r->len - i >= 2 && r->s[i] == '%' && r->s[i + 1] == '%')) continue;	/* %% a comment */
    if (!head) {	/* graph TD, flowchart LR ... */
      if (!((r->len - i >= 5 && strncmp(r->s + i, "graph", 5) == 0) ||
            (r->len - i >= 9 && strncmp(r->s + i, "flowchart", 9) == 0))) return 0;
      i += r->s[i] == 'g' ? 5 : 9;
      while (i < r->len && r->s[i] == ' ') i++;
      g->lr = r->len - i >= 2 && (strncmp(r->s + i, "LR", 2) == 0 || strncmp(r->s + i, "RL", 2) == 0);
      head = 1;
      while (i < r->len && r->s[i] != ';') i++;
      if (i < r->len) i++;
    }
    while (i < r->len) {	/* statements, ; between them */
      e = i;
      while (e < r->len && r->s[e] != ';') e++;
      mm_statement(g, r->s + i, e - i);
      i = e + 1;
    }
  }
  g->nreal = g->nnode;
  return head && g->nnode > 0;
}


/* the edges that close a cycle (to a node on the way to this one), as a walk from the first nodes finds them */
static void mm_walk (const Graph *g, int u, unsigned char *state, unsigned char *back) {
  int k;
  state[u] = 1;
  for (k = 0; k < g->nedge; k++)
    if (g->edge[k].from == u) {
      int v = g->edge[k].to;
      if (state[v] == 1) back[k] = 1;
      else if (state[v] == 0) mm_walk(g, v, state, back);
    }
  state[u] = 2;
}


/* the layers: a node one past the deepest node that points at it (the edges back up not followed) */
static void mm_layers (Graph *g) {
  unsigned char state[MM_NODES], back[MM_EDGES];
  int i, k, changed = 1, pass = 0;
  memset(state, 0, sizeof(state));
  memset(back, 0, sizeof(back));
  for (i = 0; i < g->nnode; i++) {
    g->node[i].layer = 0;
    if (state[i] == 0) mm_walk(g, i, state, back);
  }
  while (changed && pass++ <= g->nnode) {
    changed = 0;
    for (k = 0; k < g->nedge; k++) {
      MEdge *e = &g->edge[k];
      if (back[k] || e->to == e->from) continue;
      if (g->node[e->to].layer < g->node[e->from].layer + 1) {
        g->node[e->to].layer = g->node[e->from].layer + 1;
        changed = 1;
      }
    }
  }
}


static void cv_put (Canvas *c, int x, int y, uint32_t cp, int kind) {
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
  c->cp[y * c->w + x] = cp;
  c->kind[y * c->w + x] = (unsigned char)kind;
}


static void cv_text (Canvas *c, int x, int y, const char *s, int kind) {
  size_t n = strlen(s), i = 0, len;
  while (i < n) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    cv_put(c, x, y, cp, kind);
    x += uc_width(cp) > 0 ? uc_width(cp) : 1;
    i += len ? len : 1;
  }
}


/* a line's end in a cell (lines drawn into a box's border join it: ┬ ┴ ├ ┤) */
static void cv_line (Canvas *c, int x, int y, int bits) {
  size_t k;
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
  k = (size_t)y * (size_t)c->w + (size_t)x;
  if (c->kind[k] == 1) {	/* a box's border */
    uint32_t cp = c->cp[k];
    if (cp == 0x2500) c->cp[k] = (bits & 2) ? 0x252C : 0x2534;
    else if (cp == 0x2502) c->cp[k] = (bits & 8) ? 0x251C : 0x2524;
    return;
  }
  if (c->kind[k] == 3) return;	/* a label stays readable */
  c->line[k] |= (unsigned char)bits;
  c->kind[k] = 2;
}


static void cv_hline (Canvas *c, int x0, int x1, int y) {
  int x, a = x0 < x1 ? x0 : x1, b = x0 < x1 ? x1 : x0;
  for (x = a; x <= b; x++) cv_line(c, x, y, (x > a ? 4 : 0) | (x < b ? 8 : 0));
}


static void cv_vline (Canvas *c, int x, int y0, int y1) {
  int y, a = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
  for (y = a; y <= b; y++) cv_line(c, x, y, (y > a ? 1 : 0) | (y < b ? 2 : 0));
}


static void cv_box (Canvas *c, const MNode *nd) {
  static const uint32_t corner[3][4] = {
    {0x250C, 0x2510, 0x2514, 0x2518}, {0x256D, 0x256E, 0x2570, 0x256F}, {0x2571, 0x2572, 0x2572, 0x2571}
  };
  int s = nd->shape == '(' ? 1 : nd->shape == '{' ? 2 : 0, i, j;
  int x = nd->x, y = nd->y, w = nd->w, h = nd->h;
  for (i = 1; i < w - 1; i++) {
    cv_put(c, x + i, y, 0x2500, 1);
    cv_put(c, x + i, y + h - 1, 0x2500, 1);
  }
  for (j = 1; j < h - 1; j++) {
    cv_put(c, x, y + j, s == 2 ? '<' : 0x2502, 1);
    cv_put(c, x + w - 1, y + j, s == 2 ? '>' : 0x2502, 1);
    for (i = 1; i < w - 1; i++) cv_put(c, x + i, y + j, ' ', 3);
  }
  cv_put(c, x, y, corner[s][0], 1);
  cv_put(c, x + w - 1, y, corner[s][1], 1);
  cv_put(c, x, y + h - 1, corner[s][2], 1);
  cv_put(c, x + w - 1, y + h - 1, corner[s][3], 1);
  cv_text(c, x + 2, y + h / 2, nd->label, 3);
}


/*
** The layout: each layer a row of boxes (TD) or a column of them (LR),
** an edge that jumps layers goes through a place in each layer between
** (a "dummy" node), the nodes of a layer ordered by where the nodes that
** point at them are; between two layers a band the lines turn in. An
** edge back up is not drawn in the picture but said under it.
*/
static int mm_layout (Graph *g, Canvas *cv, int maxw, Buf *back) {
  int nl = 0, i, k, l, band, cross = 0, total = 0;
  int lpos[MM_NODES + MM_EDGES * 4];
  int esrc[MM_EDGES * 4], edst[MM_EDGES * 4], earrow[MM_EDGES * 4], elab[MM_EDGES * 4], ne = 0;
  int size[MM_NODES + MM_EDGES * 4];	/* a layer's length along it */
  int thick[MM_NODES + MM_EDGES * 4];	/* and across it */
  mm_layers(g);
  for (k = 0; k < g->nedge; k++) {	/* the edges, cut in steps of one layer */
    const MEdge *e = &g->edge[k];
    int from = e->from, to = e->to, lf = g->node[from].layer, lt = g->node[to].layer, prev = from;
    if (lt <= lf) {	/* back up (or to itself): in words */
      buf_printf(back, "%s \xE2\x86\xBA %s%s%s%s", back->len ? "," : "", g->node[from].label,
                 e->arrow ? " \xE2\x86\x92 " : " \xE2\x80\x94 ", g->node[to].label, "");
      if (e->label[0]) buf_printf(back, " (%s)", e->label);
      continue;
    }
    for (l = lf + 1; l <= lt; l++) {
      int cur = to;
      if (l < lt) {	/* a place in layer l */
        if (g->nnode == (int)(sizeof(g->node) / sizeof(g->node[0]))) return 0;
        cur = g->nnode++;
        memset(&g->node[cur], 0, sizeof(MNode));
        g->node[cur].dummy = 1;
        g->node[cur].layer = l;
      }
      if (ne == (int)(sizeof(esrc) / sizeof(esrc[0]))) return 0;
      esrc[ne] = prev;
      edst[ne] = cur;
      earrow[ne] = l == lt && e->arrow;
      elab[ne] = l == lt && e->label[0] ? k : -1;
      ne++;
      prev = cur;
    }
  }
  for (i = 0; i < g->nnode; i++)
    if (g->node[i].layer + 1 > nl) nl = g->node[i].layer + 1;
  for (i = 0; i < g->nnode; i++) {	/* the sizes of the boxes: 3 rows, the label and two spaces */
    MNode *nd = &g->node[i];
    if (nd->dummy) {
      nd->w = nd->h = 1;
      continue;
    }
    nd->w = (int)str_cols(nd->label) + 4;
    nd->h = 3;
  }
  for (l = 0; l < nl; l++) {	/* the order in each layer: by the places of the nodes before it */
    int n = 0;
    for (i = 0; i < g->nnode; i++)
      if (g->node[i].layer == l) {
        double sum = 0;
        int cnt = 0;
        for (k = 0; k < ne; k++)
          if (edst[k] == i) {
            sum += g->node[esrc[k]].order;
            cnt++;
          }
        g->node[i].order = n;
        lpos[i] = cnt ? (int)(sum * 100 / cnt) : n * 100;
        n++;
      }
    for (i = 0; i < g->nnode; i++)	/* ranked by that, the first ones first when even */
      if (g->node[i].layer == l) {
        int rank = 0;
        for (k = 0; k < g->nnode; k++)
          if (g->node[k].layer == l && (lpos[k] < lpos[i] || (lpos[k] == lpos[i] && k < i))) rank++;
        g->node[i].order = rank;
      }
  }
  band = 3;
  for (k = 0; k < ne; k++)
    if (elab[k] >= 0) band = g->lr ? band : 4;
  if (g->lr) {	/* the band between columns is as wide as the longest label */
    int lw = 0;
    for (k = 0; k < ne; k++)
      if (elab[k] >= 0 && (int)str_cols(g->edge[elab[k]].label) > lw) lw = (int)str_cols(g->edge[elab[k]].label);
    band = 5 + (lw ? lw + 1 : 0);
  }
  for (l = 0; l < nl; l++) {	/* each layer's length and thickness */
    size[l] = 0;
    thick[l] = 1;
    for (k = 0; k < g->nnode + 1; k++) {
      int o;
      for (i = 0; i < g->nnode; i++)
        if (g->node[i].layer == l && g->node[i].order == k) break;
      if (i == g->nnode) break;
      o = g->lr ? g->node[i].h : g->node[i].w;
      size[l] += (k ? (g->lr ? 1 : 3) : 0) + o;
      if ((g->lr ? g->node[i].w : g->node[i].h) > thick[l]) thick[l] = g->lr ? g->node[i].w : g->node[i].h;
    }
    if (size[l] > cross) cross = size[l];
    total += thick[l] + (l ? band : 0);
  }
  cv->w = g->lr ? total : cross;
  cv->h = g->lr ? cross : total;
  if (cv->w > maxw || cv->w < 1 || cv->h < 1 || cv->h > 400) return 0;
  {	/* the places: each layer centred across */
    int at = 0;
    for (l = 0; l < nl; l++) {
      int p = (cross - size[l]) / 2;
      for (k = 0; k < g->nnode + 1; k++) {
        MNode *nd;
        for (i = 0; i < g->nnode; i++)
          if (g->node[i].layer == l && g->node[i].order == k) break;
        if (i == g->nnode) break;
        nd = &g->node[i];
        if (g->lr) {
          nd->x = at + (nd->dummy ? thick[l] / 2 : (thick[l] - nd->w) / 2);
          nd->y = p;
          p += nd->h + 1;
        }
        else {
          nd->x = p;
          nd->y = at + (nd->dummy ? thick[l] / 2 : 0);
          p += nd->w + 3;
        }
      }
      at += thick[l] + band;
    }
  }
  cv->cp = (uint32_t *)xmalloc((size_t)cv->w * (size_t)cv->h * sizeof(uint32_t));
  cv->line = (unsigned char *)xmalloc((size_t)cv->w * (size_t)cv->h);
  cv->kind = (unsigned char *)xmalloc((size_t)cv->w * (size_t)cv->h);
  for (i = 0; i < cv->w * cv->h; i++) cv->cp[i] = ' ';
  memset(cv->line, 0, (size_t)cv->w * (size_t)cv->h);
  memset(cv->kind, 0, (size_t)cv->w * (size_t)cv->h);
  for (i = 0; i < g->nnode; i++)
    if (!g->node[i].dummy) cv_box(cv, &g->node[i]);
  for (i = 0; i < g->nnode; i++)	/* a place a line goes through */
    if (g->node[i].dummy) {
      const MNode *nd = &g->node[i];
      int t = thick[nd->layer], start = g->lr ? nd->x - t / 2 : nd->y - t / 2;
      if (g->lr) cv_hline(cv, start, start + t - 1, nd->y);
      else cv_vline(cv, nd->x, start, start + t - 1);
    }
  for (k = 0; k < ne; k++) {	/* the lines through the bands */
    const MNode *a = &g->node[esrc[k]], *b = &g->node[edst[k]];
    int la = a->layer, ta = thick[la];
    if (g->lr) {
      int ax = (a->dummy ? a->x - ta / 2 : a->x - (ta - a->w) / 2) + ta, ay = a->y + a->h / 2;
      int bx = b->dummy ? b->x - thick[b->layer] / 2 - 1 : b->x - 1, by = b->y + b->h / 2, mid = ax + 1;
      if (a->dummy) cv_hline(cv, a->x, ax, ay);
      else cv_hline(cv, a->x + a->w - 1, ax, ay);
      cv_hline(cv, ax, mid, ay);
      cv_vline(cv, mid, ay, by);
      cv_hline(cv, mid, earrow[k] ? bx : bx + 1, by);	/* into the place or the box's side (├ ┤), or to the arrow */
      if (elab[k] >= 0) cv_text(cv, mid + 2, by > 0 ? by - 1 : by + 1, g->edge[elab[k]].label, 3);
      if (earrow[k]) cv_put(cv, bx, by, 0x25B6, 2);
    }
    else {
      int ay = a->dummy ? a->y - ta / 2 + ta - 1 : a->y + a->h - 1, ax = a->x + (a->dummy ? 0 : a->w / 2);
      int by = b->dummy ? b->y - thick[b->layer] / 2 : b->y, bx = b->x + (b->dummy ? 0 : b->w / 2);
      int mid = a->dummy ? a->y - ta / 2 + ta : a->y + ta;
      cv_vline(cv, ax, ay, mid);
      cv_hline(cv, ax, bx, mid);
      cv_vline(cv, bx, mid, earrow[k] ? by - 1 : by);
      if (elab[k] >= 0) cv_text(cv, bx + 2, mid + 1, g->edge[elab[k]].label, 3);
      if (earrow[k]) cv_put(cv, bx, by - 1, 0x25BC, 2);
    }
  }
  return 1;
}


/* the glyph of a cell's lines */
static uint32_t cv_glyph (int bits) {
  static const uint32_t g[16] = {
    ' ', 0x2575, 0x2577, 0x2502, 0x2574, 0x2518, 0x2510, 0x2524,
    0x2576, 0x2514, 0x250C, 0x251C, 0x2500, 0x2534, 0x252C, 0x253C
  };
  return g[bits & 15];
}


/* a mermaid block (its lines a .. b-1): the flowchart drawn, 1; 0 when it is not one that can be */
static int mermaid (const Doc *d, size_t a, size_t b) {
  Graph *g = (Graph *)xmalloc(sizeof(Graph));
  Canvas cv;
  Buf back;
  int ok = 0, x, y;
  memset(&cv, 0, sizeof(cv));
  buf_init(&back);
  if (mm_parse(g, d, a, b) && mm_layout(g, &cv, M.w - 2, &back)) {
    Cells row = {NULL, 0, 0};
    int left = (M.w - 2 - cv.w) / 2;
    for (y = 0; y < cv.h; y++) {
      row.n = 0;
      M.line = a + (size_t)y * (b - a) / (size_t)cv.h;	/* the rows go with the block's lines, for the scrolling */
      for (x = 0; x < left; x++) cells_add(&row, ' ', M.fg, M.bg, 0);
      for (x = 0; x < cv.w; x++) {
        size_t k = (size_t)y * (size_t)cv.w + (size_t)x;
        int kind = cv.kind[k];
        uint32_t cp = kind == 2 && cv.cp[k] == ' ' ? cv_glyph(cv.line[k]) : cv.cp[k];
        if (kind == 2 && cv.cp[k] != ' ' && cv.line[k] == 0) cp = cv.cp[k];	/* an arrow's head */
        cells_add(&row, cp, kind == 1 ? M.link : kind == 3 ? M.fg : M.dim, M.bg, 0);
      }
      row_out(row.v, row.n, M.bg);
    }
    if (back.len) {	/* the edges back up */
      Cells para = {NULL, 0, 0};
      buf_putc(&back, '\0');
      inline_cells(&para, back.s, strlen(back.s), M.dim, M.bg, RGB_ITALIC);
      flow(para.v, para.n, NULL, NULL, M.bg);
      free(para.v);
    }
    free(row.v);
    ok = 1;
  }
  free(cv.cp);
  free(cv.line);
  free(cv.kind);
  buf_free(&back);
  free(g);
  return ok;
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
      int state = 0, mm = is_mermaid(r);
      if (mm) {	/* ```mermaid: the flowchart drawn; another diagram, its code marked "mermaid" */
        size_t e = y + 1;
        char c2;
        while (e < n && !(is_fence(&d->row[e], &c2) && c2 == fch)) e++;
        if (mermaid(d, y + 1, e)) {
          y = e < n ? e + 1 : n;
          continue;
        }
        M.line = y;
        {
          Cells lab = {NULL, 0, 0};
          cells_str(&lab, "  mermaid", M.dim, M.code_bg, RGB_ITALIC);
          row_out(lab.v, lab.n, M.code_bg);
          free(lab.v);
        }
      }
      else row_out(NULL, 0, M.code_bg);
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
    if (i + 1 < r->len && r->s[i] == '$' && r->s[i + 1] == '$') {	/* $$ math $$ */
      math_block(d, &y);
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
      int pics = 0;
      for (; y < n && !is_blank(&d->row[y]); y++) {
        const Row *q = &d->row[y];
        size_t a = skip_ws(q), e = q->len, un;
        char c2;
        long nn;
        const char *url;
        void *img;
        if (para.n && (is_fence(q, &c2) || q->s[a] == '#' || q->s[a] == '>' || list_mark(q, &nn) ||
                       is_rule(q) || setext_level(q))) break;
        if (picture_line(q, &url, &un) && (img = picture(url, un)) != NULL) {	/* a picture on a line of its own */
          if (para.n) flow(para.v, para.n, NULL, NULL, M.bg);
          para.n = 0;
          M.line = y;
          picture_rows(img);
          pics = 1;
          continue;
        }
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
      if (para.n || !pics) flow(para.v, para.n, NULL, NULL, M.bg);
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
  int cpx = 0, cpy = 0;
  term_cell_px(&cpx, &cpy);	/* a picture's rows: by the cells' pixels */
  if (M.c_doc == d && M.c_edits == d->edits && M.c_len == d->n && M.c_w == w && M.c_px == cpx * 10000 + cpy) return;
  M.c_px = cpx * 10000 + cpy;
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
  g_real = 0;
  blocks(d);
}

/* }================================================================== */
