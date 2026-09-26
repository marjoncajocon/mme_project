/*
** enb.c - Jupyter notebooks (.ipynb) in an editor tab: VS Code's notebook
** editor (PAGE_NOTEBOOK). Code and Markdown cells, their outputs, a kernel
** that runs them, saved as nbformat 4.
**
** The kernel is mme-kernel.py (written into mme-data): a Jupyter kernel
** through jupyter_client when that is installed (any language it has a
** kernel for), else the Python it runs in. mme talks to it in JSON lines
** over its stdin and stdout, as it talks to a language server.
**
** Command mode (a cell selected): Enter edits it, A / B add a cell above /
** below, D D deletes, M makes it Markdown, Y code, Alt+Up / Alt+Down move
** it. Edit mode: typing, Esc back. Both: Ctrl+Enter runs the cell,
** Shift+Enter runs it and goes to the next, Alt+Enter runs it and adds one.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { CELL_CODE, CELL_MD, CELL_RAW };
enum { OT_STREAM, OT_DATA, OT_ERROR, OT_OTHER };
enum { KS_OFF, KS_START, KS_IDLE, KS_BUSY };

#define GW	7	/* the gutter: the selection's bar, [12] */

typedef struct Out {
  int type;
  char name[8];	/* OT_STREAM: stdout, stderr */
  Buf text;	/* OT_STREAM: what came; the others: what shows */
  const Json *j;	/* OT_DATA, OT_ERROR, OT_OTHER: the output as it is saved */
  Json *own;	/* the tree j is in when it is not the file's */
  void *img;	/* image/png, decoded */
} Out;

typedef struct Snap {
  char *s;
  size_t cur;
} Snap;

typedef struct Cell {
  int type;
  Buf src;	/* its text, lines ended by LF, no LF at the end */
  const Json *meta;	/* its metadata, written back as it is (NULL: {}) */
  char id[40];	/* nbformat 4.5's cell id; "": none */
  Out *out;
  size_t nout;
  int count;	/* execution_count; 0: never run */
  int state;	/* 0; 1 queued; 2 running */
  unsigned serial;	/* how the kernel calls it */
  size_t cur, anchor;	/* the caret, where the selection starts (bytes) */
  int sel;	/* a selection */
  size_t want;	/* the column Up / Down keep */
  size_t left;	/* the source's columns scrolled away */
  Snap *undo;
  size_t nundo, iundo;
  int typing;	/* the last edit was typing: the next joins its undo step */
  Doc *md;	/* the Markdown as drawn */
  unsigned long gen, md_gen;
} Cell;

typedef struct Nb {
  char *path;
  Json *root;	/* the file as read: its metadata */
  Cell **cell;
  size_t n, cap;
  size_t sel;
  int edit;
  long changes, saved;
  long top;	/* the first row shown */
  const Syntax *sx;
  char kname[64];	/* the kernelspec's name */
  char klang[32];
  int dd;	/* D once: the next D deletes */
  int minor;	/* nbformat_minor */
  int ks;
  OsProc proc;
  long pid;
  int to, from, err;
  Buf in;
  char kinfo[80];
  unsigned *queue;
  size_t nq, capq;
  unsigned running;
  char *ask;	/* input()'s prompt, waiting to be asked */
  long long int_at;	/* when Interrupt was asked: the kernel starts again if it does not stop */
  /* completions: the kernel's, under the caret */
  int comp_open, comp_want, comp_all;	/* comp_all: Ctrl+Space, after a dot: shown with nothing typed */
  unsigned comp_req, comp_cell;
  char **comp;
  size_t ncomp, comp_sel, comp_top, comp_start, comp_end;
  int cur_sx, cur_sy;	/* where the caret was drawn */
  int x, y, w, h;
  int *rowcell, *rowpart, *rowline;	/* what each row on the screen is (the mouse) */
  int rows_cap;
  int follow;	/* the caret (the cell) must be shown */
  char title[300];
} Nb;

enum { RP_NONE, RP_TOOL, RP_SRC, RP_OUT, RP_ADD, RP_GAP };

#define MAX_NB	32
static Nb *g_nb[MAX_NB];
static unsigned g_serial;


static const char *S_ (const Cell *c) {	/* a cell's text */
  return c->src.s ? c->src.s : "";
}


/*
** {==================================================================
** Reading and writing .ipynb
** ===================================================================
*/

static char *join_str (const Json *j) {	/* a string, or a list of strings: one text */
  Buf b;
  size_t i;
  buf_init(&b);
  if (j && j->type == J_STR) buf_putn(&b, j->str, j->len);
  else if (j && j->type == J_ARR)
    for (i = 0; i < j->n; i++)
      if (j->kid[i]->type == J_STR) buf_putn(&b, j->kid[i]->str, j->kid[i]->len);
  buf_putc(&b, '\0');
  return buf_take(&b);
}


static void strip_ansi (Buf *b, const char *s, size_t n) {	/* Jupyter's tracebacks are colored: the colors go */
  size_t i;
  for (i = 0; i < n; i++) {
    if (s[i] == 27 && i + 1 < n && s[i + 1] == '[') {
      i += 2;
      while (i < n && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) i++;
      continue;
    }
    if (s[i] != '\r') buf_putc(b, s[i]);
  }
}


static int b64v (int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}


static unsigned char *b64_decode (const char *s, size_t n, size_t *out) {
  unsigned char *r = (unsigned char *)xmalloc(n / 4 * 3 + 4);
  unsigned v = 0;
  int bits = 0;
  size_t i, k = 0;
  for (i = 0; i < n; i++) {
    int d = b64v((unsigned char)s[i]);
    if (d < 0) continue;
    v = (v << 6) | (unsigned)d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      r[k++] = (unsigned char)((v >> bits) & 0xFF);
    }
  }
  *out = k;
  return r;
}


/* HTML's entities, the few that matter */
static void put_entity (Buf *b, const char *s, size_t n, size_t *i) {
  static const struct {
    const char *name, *text;
  } e[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&nbsp;", " "}, {"&#x27;", "'"}};
  size_t k;
  for (k = 0; k < sizeof(e) / sizeof(e[0]); k++) {
    size_t l = strlen(e[k].name);
    if (*i + l <= n && strncmp(s + *i, e[k].name, l) == 0) {
      buf_puts(b, e[k].text);
      *i += l - 1;
      return;
    }
  }
  buf_putc(b, '&');
}


static int tag_is (const char *s, size_t n, size_t at, const char *name) {	/* <name or </name at s[at] */
  size_t l = strlen(name), k = at + 1;
  if (k < n && s[k] == '/') k++;
  if (k + l > n) return 0;
  if (m_strnicmp(s + k, name, l) != 0) return 0;
  return k + l >= n || s[k + l] == '>' || s[k + l] == ' ' || s[k + l] == '/' || s[k + l] == '\n';
}


/* the text between tags, entities read, blanks folded */
static char *html_plain (const char *s, size_t n) {
  Buf b;
  size_t i;
  int space = 0;
  buf_init(&b);
  for (i = 0; i < n; i++) {
    if (s[i] == '<') {
      const char *e = memchr(s + i, '>', n - i);
      if (tag_is(s, n, i, "br") || tag_is(s, n, i, "p") || tag_is(s, n, i, "div") || tag_is(s, n, i, "li") ||
          tag_is(s, n, i, "h1") || tag_is(s, n, i, "h2") || tag_is(s, n, i, "h3")) {
        if (b.len && b.s[b.len - 1] != '\n') buf_putc(&b, '\n');
        space = 0;
      }
      if (tag_is(s, n, i, "style") || tag_is(s, n, i, "script")) {	/* their text is not shown */
        const char *end = NULL;
        size_t k;
        for (k = i + 1; k + 1 < n; k++)
          if (s[k] == '<' && s[k + 1] == '/') {
            end = s + k;
            break;
          }
        e = end ? memchr(end, '>', n - (size_t)(end - s)) : NULL;
      }
      if (e == NULL) break;
      i = (size_t)(e - s);
      continue;
    }
    if (s[i] == '\n' || s[i] == '\r' || s[i] == '\t' || s[i] == ' ') {
      space = 1;
      continue;
    }
    if (space && b.len && b.s[b.len - 1] != '\n') buf_putc(&b, ' ');
    space = 0;
    if (s[i] == '&') put_entity(&b, s, n, &i);
    else buf_putc(&b, s[i]);
  }
  while (b.len && (b.s[b.len - 1] == '\n' || b.s[b.len - 1] == ' ')) b.len--;
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/*
** text/html as text: its tables (pandas' DataFrames) in columns, the
** header over a line; what is around them without the tags
*/
static void html_text (Buf *out, const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    const char *t = NULL;
    size_t k;
    for (k = i; k < n; k++)
      if (s[k] == '<' && tag_is(s, n, k, "table") && s[k + 1] != '/') {
        t = s + k;
        break;
      }
    {
      char *plain = html_plain(s + i, (t ? (size_t)(t - s) : n) - i);
      if (*plain) {
        if (out->len && out->s[out->len - 1] != '\n') buf_putc(out, '\n');
        buf_puts(out, plain);
      }
      free(plain);
    }
    if (t == NULL) break;
    {	/* the table: its rows, their cells; then the widths */
      char **cell = NULL;
      int *rowlen = NULL, rows = 0, ncell = 0, maxc = 0, head = 0, r, c;
      size_t end = n, p = (size_t)(t - s), *w;
      for (k = p + 1; k + 1 < n; k++)
        if (s[k] == '<' && tag_is(s, n, k, "table") && s[k + 1] == '/') {
          end = k;
          break;
        }
      while (p < end) {
        if (s[p] == '<' && tag_is(s, n, p, "tr") && s[p + 1] != '/') {
          rowlen = (int *)xrealloc(rowlen, (size_t)(rows + 1) * sizeof(int));
          rowlen[rows++] = 0;
        }
        else if (s[p] == '<' && (tag_is(s, n, p, "td") || tag_is(s, n, p, "th")) && s[p + 1] != '/' && rows > 0) {
          const char *gt = memchr(s + p, '>', end - p);
          size_t q, from;
          if (gt == NULL) break;
          if (tag_is(s, n, p, "th") && rows == 1) head = 1;
          from = (size_t)(gt - s) + 1;
          for (q = from; q + 2 < end; q++)
            if (s[q] == '<' && s[q + 1] == '/' && (tag_is(s, n, q, "td") || tag_is(s, n, q, "th"))) break;
          cell = (char **)xrealloc(cell, (size_t)(ncell + 1) * sizeof(char *));
          cell[ncell++] = html_plain(s + from, q - from);
          rowlen[rows - 1]++;
          if (rowlen[rows - 1] > maxc) maxc = rowlen[rows - 1];
          p = q;
          continue;
        }
        p++;
      }
      w = (size_t *)xmalloc((size_t)(maxc > 0 ? maxc : 1) * sizeof(size_t));
      memset(w, 0, (size_t)(maxc > 0 ? maxc : 1) * sizeof(size_t));
      for (r = 0, k = 0; r < rows; r++)
        for (c = 0; c < rowlen[r]; c++, k++)
          if (str_cols(cell[k]) > w[c]) w[c] = str_cols(cell[k]);
      if (out->len && out->s[out->len - 1] != '\n') buf_putc(out, '\n');
      for (r = 0, k = 0; r < rows; r++) {
        for (c = 0; c < rowlen[r]; c++, k++) {
          size_t pad = w[c] - str_cols(cell[k]), q;
          if (c) buf_puts(out, "  ");
          buf_puts(out, cell[k]);
          if (c + 1 < rowlen[r])
            for (q = 0; q < pad; q++) buf_putc(out, ' ');
        }
        buf_putc(out, '\n');
        if (r == 0 && head) {	/* the header's line */
          size_t tot = 0, q;
          for (c = 0; c < maxc; c++) tot += w[c] + (c ? 2 : 0);
          for (q = 0; q < tot; q++) buf_puts(out, "\xE2\x94\x80");
          buf_putc(out, '\n');
        }
      }
      for (k = 0; k < (size_t)ncell; k++) free(cell[k]);
      free(cell);
      free(rowlen);
      free(w);
      i = end;
      {
        const char *gt = memchr(s + end, '>', n - end);
        i = gt ? (size_t)(gt - s) + 1 : n;
      }
    }
  }
  while (out->len && out->s[out->len - 1] == '\n') out->len--;
}


/* what an output shows: its text (the traceback, text/plain), its picture */
static void out_show (Out *o) {
  const Json *j = o->j;
  if (o->type == OT_STREAM) return;
  buf_free(&o->text);
  buf_init(&o->text);
  if (o->type == OT_ERROR) {
    const Json *tb = json_get(j, "traceback");
    size_t i;
    if (tb && tb->type == J_ARR && tb->n > 0)
      for (i = 0; i < tb->n; i++) {
        if (tb->kid[i]->type != J_STR) continue;
        if (o->text.len) buf_putc(&o->text, '\n');
        strip_ansi(&o->text, tb->kid[i]->str, tb->kid[i]->len);
      }
    else buf_printf(&o->text, "%s: %s", json_str(json_get(j, "ename"), "Error"), json_str(json_get(j, "evalue"), ""));
    return;
  }
  if (o->type == OT_DATA) {
    const Json *d = json_get(j, "data"), *png = json_get(d, "image/png"), *plain = json_get(d, "text/plain");
    if (png && o->img == NULL) {
      char *s = join_str(png);
      size_t n;
      unsigned char *bytes = b64_decode(s, strlen(s), &n);
      o->img = img_open_mem(bytes, n);
      free(bytes);
      free(s);
    }
    if (o->img) return;
    {
      const Json *html = json_get(d, "text/html");
      if (html) {	/* a table (a DataFrame): drawn as one; else plain text is better than the tags' */
        char *h = join_str(html);
        if (strstr(h, "<table") || strstr(h, "<TABLE") || plain == NULL) {
          html_text(&o->text, h, strlen(h));
          free(h);
          return;
        }
        free(h);
      }
    }
    if (plain) {
      char *s = join_str(plain);
      strip_ansi(&o->text, s, strlen(s));
      free(s);
    }
    else if (json_get(d, "text/markdown")) {
      char *s = join_str(json_get(d, "text/markdown"));
      buf_puts(&o->text, s);
      free(s);
    }
    else if (d && d->type == J_OBJ && d->n > 0) buf_printf(&o->text, "[%s: not shown in mme]", d->kid[0]->key);
    return;
  }
  buf_printf(&o->text, "[%s]", json_str(json_get(j, "output_type"), "output"));
}


static void out_free (Out *o) {
  buf_free(&o->text);
  json_free(o->own);
  if (o->img) img_close(o->img);
}


static void cell_clear (Cell *c) {
  size_t i;
  for (i = 0; i < c->nout; i++) out_free(&c->out[i]);
  free(c->out);
  c->out = NULL;
  c->nout = 0;
}


static Cell *cell_new (int type, const char *src, size_t n) {
  Cell *c = (Cell *)xmalloc(sizeof(Cell));
  memset(c, 0, sizeof(*c));
  c->type = type;
  buf_init(&c->src);
  buf_putn(&c->src, src, n);
  buf_putc(&c->src, '\0');
  c->src.len--;
  c->serial = ++g_serial;
  c->gen = 1;
  return c;
}


static void cell_free (Cell *c) {
  size_t i;
  cell_clear(c);
  buf_free(&c->src);
  for (i = 0; i < c->nundo; i++) free(c->undo[i].s);
  free(c->undo);
  if (c->md) {
    doc_free(c->md);
    free(c->md);
  }
  free(c);
}


static Out *out_add (Cell *c) {
  Out *o;
  c->out = (Out *)xrealloc(c->out, (c->nout + 1) * sizeof(Out));
  o = &c->out[c->nout++];
  memset(o, 0, sizeof(*o));
  buf_init(&o->text);
  return o;
}


static void cell_insert (Nb *nb, size_t at, Cell *c) {
  if (nb->n == nb->cap) {
    nb->cap = nb->cap ? nb->cap * 2 : 16;
    nb->cell = (Cell **)xrealloc(nb->cell, nb->cap * sizeof(Cell *));
  }
  memmove(nb->cell + at + 1, nb->cell + at, (nb->n - at) * sizeof(Cell *));
  nb->cell[at] = c;
  nb->n++;
}


static void load_cells (Nb *nb) {
  const Json *cells = json_get(nb->root, "cells");
  size_t i, k;
  if (cells == NULL || cells->type != J_ARR) return;
  for (i = 0; i < cells->n; i++) {
    const Json *cj = cells->kid[i], *outs = json_get(cj, "outputs");
    const char *t = json_str(json_get(cj, "cell_type"), "code");
    char *src = join_str(json_get(cj, "source"));
    Cell *c = cell_new(strcmp(t, "markdown") == 0 ? CELL_MD : strcmp(t, "raw") == 0 ? CELL_RAW : CELL_CODE, src, strlen(src));
    free(src);
    c->meta = json_get(cj, "metadata");
    snprintf(c->id, sizeof(c->id), "%s", json_str(json_get(cj, "id"), ""));
    c->count = (int)json_num(json_get(cj, "execution_count"), 0);
    for (k = 0; outs && outs->type == J_ARR && k < outs->n; k++) {
      const Json *oj = outs->kid[k];
      const char *ot = json_str(json_get(oj, "output_type"), "");
      Out *o = out_add(c);
      o->j = oj;
      if (strcmp(ot, "stream") == 0) {
        char *s = join_str(json_get(oj, "text"));
        o->type = OT_STREAM;
        snprintf(o->name, sizeof(o->name), "%s", json_str(json_get(oj, "name"), "stdout"));
        strip_ansi(&o->text, s, strlen(s));
        free(s);
      }
      else o->type = strcmp(ot, "error") == 0 ? OT_ERROR :
                     (strcmp(ot, "execute_result") == 0 || strcmp(ot, "display_data") == 0) ? OT_DATA : OT_OTHER;
      out_show(o);
    }
    cell_insert(nb, nb->n, c);
  }
}


/* JSON as Jupyter writes it: one space of indent a level, keys in the order they came */
static void jw (Buf *b, const Json *j, int ind) {
  size_t i;
  int k;
  if (j == NULL) {
    buf_puts(b, "null");
    return;
  }
  switch (j->type) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_BOOL: buf_puts(b, j->b ? "true" : "false"); break;
    case J_NUM:
      if (j->num == (double)(long long)j->num) buf_printf(b, "%lld", (long long)j->num);
      else buf_printf(b, "%.17g", j->num);
      break;
    case J_STR: json_put_str(b, j->str, j->len); break;
    case J_ARR: case J_OBJ:
      if (j->n == 0) {
        buf_puts(b, j->type == J_ARR ? "[]" : "{}");
        break;
      }
      buf_putc(b, j->type == J_ARR ? '[' : '{');
      for (i = 0; i < j->n; i++) {
        buf_putc(b, '\n');
        for (k = 0; k <= ind; k++) buf_putc(b, ' ');
        if (j->type == J_OBJ) {
          json_put_str(b, j->kid[i]->key, strlen(j->kid[i]->key));
          buf_puts(b, ": ");
        }
        jw(b, j->kid[i], ind + 1);
        if (i + 1 < j->n) buf_putc(b, ',');
      }
      buf_putc(b, '\n');
      for (k = 0; k < ind; k++) buf_putc(b, ' ');
      buf_putc(b, j->type == J_ARR ? ']' : '}');
      break;
  }
}


static void pad (Buf *b, int ind) {
  int k;
  buf_putc(b, '\n');
  for (k = 0; k < ind; k++) buf_putc(b, ' ');
}


/* a text as nbformat keeps it: its lines, each with its LF but the last */
static void put_lines (Buf *b, const char *s, size_t n, int ind) {
  size_t i = 0;
  if (n == 0) {
    buf_puts(b, "[]");
    return;
  }
  buf_putc(b, '[');
  while (i < n) {
    const char *nl = memchr(s + i, '\n', n - i);
    size_t e = nl ? (size_t)(nl - s) + 1 : n;
    pad(b, ind + 1);
    json_put_str(b, s + i, e - i);
    if (e < n) buf_putc(b, ',');
    i = e;
  }
  pad(b, ind);
  buf_putc(b, ']');
}


static void new_id (Cell *c) {
  static unsigned long long x = 0x9E3779B97F4A7C15ULL;
  x ^= (unsigned long long)os_now_us() + c->serial;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  snprintf(c->id, sizeof(c->id), "%08llx", x & 0xFFFFFFFFULL);
}


static char *nb_text (Nb *nb, size_t *len) {
  Buf b;
  size_t i, k;
  const Json *meta = json_get(nb->root, "metadata");
  buf_init(&b);
  buf_puts(&b, "{\n \"cells\": [");
  for (i = 0; i < nb->n; i++) {
    Cell *c = nb->cell[i];
    pad(&b, 2);
    buf_puts(&b, "{");
    pad(&b, 3);
    buf_printf(&b, "\"cell_type\": \"%s\",", c->type == CELL_MD ? "markdown" : c->type == CELL_RAW ? "raw" : "code");
    if (c->type == CELL_CODE) {
      pad(&b, 3);
      if (c->count > 0) buf_printf(&b, "\"execution_count\": %d,", c->count);
      else buf_puts(&b, "\"execution_count\": null,");
    }
    if (c->id[0] == '\0' && nb->minor >= 5) new_id(c);
    if (c->id[0]) {
      pad(&b, 3);
      buf_puts(&b, "\"id\": ");
      json_put_str(&b, c->id, strlen(c->id));
      buf_putc(&b, ',');
    }
    pad(&b, 3);
    buf_puts(&b, "\"metadata\": ");
    if (c->meta) jw(&b, c->meta, 3);
    else buf_puts(&b, "{}");
    buf_putc(&b, ',');
    if (c->type == CELL_CODE) {
      pad(&b, 3);
      buf_puts(&b, "\"outputs\": [");
      for (k = 0; k < c->nout; k++) {
        Out *o = &c->out[k];
        pad(&b, 4);
        if (o->type == OT_STREAM) {
          buf_puts(&b, "{");
          pad(&b, 5);
          buf_puts(&b, "\"name\": ");
          json_put_str(&b, o->name, strlen(o->name));
          buf_puts(&b, ",");
          pad(&b, 5);
          buf_puts(&b, "\"output_type\": \"stream\",");
          pad(&b, 5);
          buf_puts(&b, "\"text\": ");
          put_lines(&b, o->text.s ? o->text.s : "", o->text.len, 5);
          pad(&b, 4);
          buf_puts(&b, "}");
        }
        else jw(&b, o->j, 4);
        if (k + 1 < c->nout) buf_putc(&b, ',');
      }
      if (c->nout) pad(&b, 3);
      buf_puts(&b, "],");
    }
    pad(&b, 3);
    buf_puts(&b, "\"source\": ");
    put_lines(&b, c->src.s ? c->src.s : "", c->src.len, 3);
    pad(&b, 2);
    buf_puts(&b, "}");
    if (i + 1 < nb->n) buf_putc(&b, ',');
  }
  if (nb->n) pad(&b, 1);
  buf_puts(&b, "],\n \"metadata\": ");
  if (meta) jw(&b, meta, 1);
  else {
    buf_puts(&b, "{\n  \"kernelspec\": {\n   \"display_name\": \"Python 3\",\n   \"language\": \"python\",\n"
                 "   \"name\": \"python3\"\n  },\n  \"language_info\": {\n   \"name\": \"python\"\n  }\n }");
  }
  buf_printf(&b, ",\n \"nbformat\": 4,\n \"nbformat_minor\": %d\n}\n", nb->minor);
  *len = b.len;
  buf_putc(&b, '\0');
  return buf_take(&b);
}

/* }================================================================== */


/*
** {==================================================================
** The kernel
** ===================================================================
*/

static const char *const kernel_lines[] = {	/* mme-kernel.py, a line each (C's strings are short) */
#include "enb_kernel.h"
  NULL
};


static char *kernel_script (void) {	/* mme-kernel.py in mme-data, as this mme has it */
  char *p = data_path("mme-kernel.py");
  size_t n, have = 0, i;
  char *old = NULL, *kernel_py;
  Buf kb;
  buf_init(&kb);
  for (i = 0; kernel_lines[i]; i++) buf_puts(&kb, kernel_lines[i]);
  n = kb.len;
  buf_putc(&kb, '\0');
  kernel_py = buf_take(&kb);
  int fd = os_open(p, OS_READ);
  if (fd >= 0) {
    Buf b;
    char chunk[4096];
    long k;
    buf_init(&b);
    while ((k = os_read(fd, chunk, sizeof(chunk))) > 0) buf_putn(&b, chunk, (size_t)k);
    os_close(fd);
    have = b.len;
    buf_putc(&b, '\0');
    old = buf_take(&b);
  }
  if (old == NULL || have != n || memcmp(old, kernel_py, n) != 0) {
    fd = os_open(p, OS_WRITE);
    if (fd >= 0) {
      os_write(fd, kernel_py, n);
      os_close(fd);
    }
  }
  free(old);
  free(kernel_py);
  return p;
}


/* python.defaultInterpreterPath (VS Code's), else python in PATH */
static char *python_path (void) {
  const char *s = json_str(settings_get("python\\.defaultInterpreterPath"), "");
  static const char *const names[] = {"python", "python3", "py"};
  size_t i;
  if (*s && strcmp(s, "python") != 0) return xstrdup(s);
  for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
    char *p = find_program(names[i]);
    if (p) return p;
  }
  return NULL;
}


static void k_send (Nb *nb, const char *s, size_t n) {
  if (nb->ks == KS_OFF || nb->to < 0) return;
  os_write(nb->to, s, n);
}


static void k_close (Nb *nb) {
  if (nb->to >= 0) os_close(nb->to);
  if (nb->from >= 0) os_close(nb->from);
  if (nb->err >= 0) os_close(nb->err);
  nb->to = nb->from = nb->err = -1;
}


static void k_stop (Nb *nb) {
  if (nb->ks == KS_OFF) return;
  os_kill(nb->pid, 9);
  os_wait(nb->proc);
  k_close(nb);
  nb->ks = KS_OFF;
}


static int k_start (Nb *nb) {
  char *py = python_path(), *script, *argv[5], *cwd, *dir;
  int to[2], from[2], err[2], io[3], r;
  if (py == NULL) {
    toast(1, "Notebook: no Python was found (set python.defaultInterpreterPath).");
    return -1;
  }
  script = kernel_script();
  if (os_pipe(to) != 0) {
    free(py);
    free(script);
    return -1;
  }
  if (os_pipe(from) != 0) {
    os_close(to[0]);
    os_close(to[1]);
    free(py);
    free(script);
    return -1;
  }
  if (os_pipe(err) != 0) err[0] = err[1] = -1;
  io[0] = to[0];
  io[1] = from[1];
  io[2] = err[1];
  argv[0] = py;
  argv[1] = (char *)"-u";
  argv[2] = script;
  argv[3] = nb->kname;
  argv[4] = NULL;
  cwd = os_getcwd();
  dir = nb->path ? path_dirname(nb->path) : xstrdup(side_root());
  os_chdir(dir);	/* the notebook's folder: its files are found as in Jupyter */
  r = os_spawn(py, argv, NULL, io, 3, &nb->proc, &nb->pid);
  if (cwd) os_chdir(cwd);
  free(cwd);
  free(dir);
  os_close(to[0]);
  os_close(from[1]);
  if (err[1] >= 0) os_close(err[1]);
  nb->to = to[1];
  nb->from = from[0];
  nb->err = err[0];
  free(script);
  if (r != 0) {
    k_close(nb);
    toast(1, "Notebook: %s could not start.", py);
    free(py);
    return -1;
  }
  out_log("Jupyter", "[info] Starting the kernel: %s mme-kernel.py %s", py, nb->kname);
  free(py);
  nb->ks = KS_START;
  snprintf(nb->kinfo, sizeof(nb->kinfo), "Starting...");
  buf_free(&nb->in);
  buf_init(&nb->in);
  return 0;
}


static Cell *by_serial (Nb *nb, unsigned s, size_t *at) {
  size_t i;
  for (i = 0; i < nb->n; i++)
    if (nb->cell[i]->serial == s) {
      if (at) *at = i;
      return nb->cell[i];
    }
  return NULL;
}


static void dirty (Nb *nb) {
  nb->changes++;
}


/* the queue's next cell, when the kernel is free */
static void k_next (Nb *nb) {
  while (nb->ks == KS_IDLE && nb->nq > 0) {
    unsigned s = nb->queue[0];
    Cell *c = by_serial(nb, s, NULL);
    memmove(nb->queue, nb->queue + 1, (nb->nq - 1) * sizeof(unsigned));
    nb->nq--;
    if (c == NULL || c->state != 1) continue;
    {
      Buf b;
      buf_init(&b);
      buf_printf(&b, "{\"op\":\"exec\",\"id\":%u,\"code\":", s);
      json_put_str(&b, c->src.s ? c->src.s : "", c->src.len);
      buf_puts(&b, "}\n");
      k_send(nb, b.s, b.len);
      buf_free(&b);
    }
    c->state = 2;
    nb->running = s;
    nb->ks = KS_BUSY;
  }
}


/*
** Completions, the kernel's (Jupyter's complete_request; jedi or
** rlcompleter in mme's Python): asked as a code cell is typed in, shown
** under the caret; Up / Down, Tab or Enter takes one, Esc closes them
*/
static size_t chars_of (const char *s, size_t bytes) {	/* the code points in bytes: the kernel counts those */
  size_t i, n = 0;
  for (i = 0; i < bytes; i++)
    if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
  return n;
}


static size_t bytes_of (const char *s, size_t len, size_t chars) {
  size_t i = 0, n = 0;
  while (i < len && n < chars) {
    i++;
    while (i < len && ((unsigned char)s[i] & 0xC0) == 0x80) i++;
    n++;
  }
  return i;
}


static void comp_close (Nb *nb) {
  size_t i;
  for (i = 0; i < nb->ncomp; i++) free(nb->comp[i]);
  free(nb->comp);
  nb->comp = NULL;
  nb->ncomp = 0;
  nb->comp_open = 0;
}


static int k_start (Nb *nb);


static void comp_ask (Nb *nb) {
  Cell *c;
  Buf b;
  if (!nb->edit || nb->sel >= nb->n) return;
  c = nb->cell[nb->sel];
  if (c->type != CELL_CODE) return;
  if (nb->ks == KS_OFF || nb->ks == KS_START) {	/* the kernel knows the names: it starts, then is asked */
    if (nb->ks == KS_OFF && k_start(nb) != 0) return;
    nb->comp_want = 1;
    nb->comp_cell = c->serial;
    return;
  }
  nb->comp_req++;
  nb->comp_cell = c->serial;
  buf_init(&b);
  buf_printf(&b, "{\"op\":\"complete\",\"req\":%u,\"code\":", nb->comp_req);
  json_put_str(&b, S_(c), c->src.len);
  buf_printf(&b, ",\"pos\":%lu}\n", (unsigned long)chars_of(S_(c), c->cur));
  k_send(nb, b.s, b.len);
  buf_free(&b);
}


static void comp_answer (Nb *nb, const Json *m) {
  const Json *ms = json_get(m, "matches");
  Cell *c;
  size_t i, s, e;
  if ((unsigned)json_num(json_get(m, "req"), 0) != nb->comp_req || !nb->edit || nb->sel >= nb->n) return;
  c = nb->cell[nb->sel];
  if (c->serial != nb->comp_cell) return;
  comp_close(nb);
  if (ms == NULL || ms->type != J_ARR || ms->n == 0) return;
  s = bytes_of(S_(c), c->src.len, (size_t)json_num(json_get(m, "start"), 0));
  e = bytes_of(S_(c), c->src.len, (size_t)json_num(json_get(m, "end"), 0));
  if (s > c->cur) s = c->cur;
  if (e < c->cur) e = c->cur;
  if (ms->n == 1 && ms->kid[0]->type == J_STR && e - s == ms->kid[0]->len && memcmp(S_(c) + s, ms->kid[0]->str, e - s) == 0)
    return;	/* the word is whole already */
  if (s == e && !nb->comp_all && !(c->cur > 0 && S_(c)[c->cur - 1] == '.')) return;	/* nothing typed of a name */
  nb->comp = (char **)xmalloc(ms->n * sizeof(char *));
  for (i = 0; i < ms->n; i++)
    if (ms->kid[i]->type == J_STR) nb->comp[nb->ncomp++] = xstrdup(ms->kid[i]->str);
  nb->comp_start = s;
  nb->comp_end = e;
  nb->comp_sel = nb->comp_top = 0;
  nb->comp_open = nb->ncomp > 0;
}


static void insert (Nb *nb, Cell *c, const char *s, size_t n, int typing);


static void comp_take (Nb *nb) {	/* the one selected replaces what was typed of it */
  Cell *c = nb->cell[nb->sel];
  const char *w = nb->comp[nb->comp_sel];
  size_t s = nb->comp_start, e = nb->comp_end;
  if (e > c->src.len) e = c->src.len;
  if (s > e) s = e;
  c->sel = 1;
  c->anchor = s;
  c->cur = e;
  if (s == e) c->sel = 0;
  insert(nb, c, w, strlen(w), 0);
  comp_close(nb);
}


/* a line from the kernel */
static void k_message (Nb *nb, const Json *m) {
  const char *t = json_str(json_get(m, "type"), "");
  Cell *c = by_serial(nb, (unsigned)json_num(json_get(m, "id"), 0), NULL);
  if (c == NULL && strcmp(t, "done") == 0) {	/* its cell was deleted meanwhile: the kernel is free all the same */
    nb->running = 0;
    nb->ks = KS_IDLE;
    k_next(nb);
    return;
  }
  if (strcmp(t, "ready") == 0) {
    snprintf(nb->kinfo, sizeof(nb->kinfo), "%s", json_str(json_get(m, "info"), "Python"));
    out_log("Jupyter", "[info] Kernel ready (%s): %s", json_str(json_get(m, "mode"), ""), nb->kinfo);
    nb->ks = KS_IDLE;
    if (nb->comp_want) {	/* completions asked for while it started: if that cell is still being typed in */
      nb->comp_want = 0;
      if (nb->edit && nb->sel < nb->n && nb->cell[nb->sel]->serial == nb->comp_cell) comp_ask(nb);
    }
    k_next(nb);
    return;
  }
  if (strcmp(t, "log") == 0) {
    out_log("Jupyter", "%s", json_str(json_get(m, "text"), ""));
    return;
  }
  if (strcmp(t, "complete") == 0) {
    comp_answer(nb, m);
    return;
  }
  if (strcmp(t, "input") == 0) {	/* input(): asked once this line is read (not in the middle of reading) */
    free(nb->ask);
    nb->ask = xstrdup(json_str(json_get(m, "prompt"), ""));
    return;
  }
  if (c == NULL) return;
  if (strcmp(t, "stream") == 0) {
    const char *name = json_str(json_get(m, "name"), "stdout"), *s = json_str(json_get(m, "text"), "");
    Out *o = c->nout ? &c->out[c->nout - 1] : NULL;
    if (o == NULL || o->type != OT_STREAM || strcmp(o->name, name) != 0) {	/* the same stream: one output, as Jupyter's */
      o = out_add(c);
      o->type = OT_STREAM;
      snprintf(o->name, sizeof(o->name), "%s", name);
    }
    strip_ansi(&o->text, s, strlen(s));
  }
  else if (strcmp(t, "result") == 0 || strcmp(t, "display") == 0 || strcmp(t, "error") == 0) {
    Buf b;
    Out *o;
    Json *j;
    buf_init(&b);
    if (strcmp(t, "error") == 0) {
      buf_puts(&b, "{\"ename\": ");
      jw(&b, json_get(m, "ename"), 0);
      buf_puts(&b, ", \"evalue\": ");
      jw(&b, json_get(m, "evalue"), 0);
      buf_puts(&b, ", \"output_type\": \"error\", \"traceback\": ");
      jw(&b, json_get(m, "traceback"), 0);
      buf_puts(&b, "}");
    }
    else {
      buf_puts(&b, "{\"data\": ");
      jw(&b, json_get(m, "data"), 0);
      if (strcmp(t, "result") == 0) buf_printf(&b, ", \"execution_count\": %d", (int)json_num(json_get(m, "count"), 0));
      buf_printf(&b, ", \"metadata\": {}, \"output_type\": \"%s\"}", strcmp(t, "result") == 0 ? "execute_result" : "display_data");
    }
    j = json_parse(b.s, b.len);
    buf_free(&b);
    if (j == NULL) return;
    o = out_add(c);
    o->type = strcmp(t, "error") == 0 ? OT_ERROR : OT_DATA;
    o->own = j;
    o->j = j;
    out_show(o);
  }
  else if (strcmp(t, "done") == 0) {
    c->count = (int)json_num(json_get(m, "count"), c->count);
    c->state = 0;
    nb->running = 0;
    nb->ks = KS_IDLE;
    nb->int_at = 0;
    k_next(nb);
  }
  dirty(nb);
}


static int g_asking;	/* input()'s box is open: no kernel is read meanwhile */

static void restart (Nb *nb);


static int k_poll (Nb *nb) {
  char chunk[65536];
  int got = 0, status;
  if (nb->ks == KS_OFF) return 0;
  while (nb->from >= 0 && os_wait_readable(nb->from, 0) == 1) {
    long n = os_read(nb->from, chunk, sizeof(chunk));
    if (n <= 0) break;
    buf_putn(&nb->in, chunk, (size_t)n);
  }
  while (nb->err >= 0 && os_wait_readable(nb->err, 0) == 1) {	/* what it says on stderr: the Jupyter output channel */
    long n = os_read(nb->err, chunk, sizeof(chunk));
    if (n <= 0) {
      os_close(nb->err);
      nb->err = -1;
      break;
    }
    out_append("Jupyter", chunk, (size_t)n);
  }
  for (;;) {
    char *nl = nb->in.len ? memchr(nb->in.s, '\n', nb->in.len) : NULL;
    size_t len;
    Json *j;
    if (nl == NULL) break;
    len = (size_t)(nl - nb->in.s);
    j = json_parse(nb->in.s, len);
    if (j) {
      k_message(nb, j);
      json_free(j);
    }
    memmove(nb->in.s, nl + 1, nb->in.len - len - 1);
    nb->in.len -= len + 1;
    got = 1;
  }
  if (nb->ask && !g_asking) {	/* input(): VS Code's box at the top; Esc stops the cell */
    char *prompt = nb->ask, *v;
    nb->ask = NULL;
    g_asking = 1;
    v = ask_text(*prompt ? prompt : "Input", NULL);
    g_asking = 0;
    if (v == NULL) k_send(nb, "{\"op\":\"interrupt\"}\n", 19);
    else {
      Buf b;
      buf_init(&b);
      buf_puts(&b, "{\"op\":\"input\",\"value\":");
      json_put_str(&b, v, strlen(v));
      buf_puts(&b, "}\n");
      k_send(nb, b.s, b.len);
      buf_free(&b);
    }
    free(v);
    free(prompt);
    got = 1;
  }
  if (nb->ks == KS_BUSY && nb->int_at && !g_asking && os_now_us() - nb->int_at > 10000000) {
    /* it did not stop in 10 s (a call that blocks, Windows' sleep in a Jupyter kernel): VS Code's question */
    static const char *const bt[] = {"Restart", "Cancel"};
    int r;
    nb->int_at = 0;
    g_asking = 1;
    r = dialog("Interrupting the kernel timed out. Do you want to restart the kernel instead?",
               "All variables will be lost.", bt, 2);
    g_asking = 0;
    if (r == 0) {
      restart(nb);
      return 1;
    }
  }
  if (os_poll_proc(nb->proc, &status) == 1) {	/* it ended */
    size_t i;
    k_close(nb);
    nb->ks = KS_OFF;
    for (i = 0; i < nb->n; i++) nb->cell[i]->state = 0;
    nb->nq = 0;
    nb->running = 0;
    snprintf(nb->kinfo, sizeof(nb->kinfo), "Kernel stopped");
    out_log("Jupyter", "[error] The kernel stopped (exit %d)", status);
    toast(1, "Notebook: the kernel stopped (see Output: Jupyter).");
    got = 1;
  }
  return got;
}


static void run_cell (Nb *nb, size_t i) {
  Cell *c = nb->cell[i];
  if (c->type != CELL_CODE) return;
  if (c->state) return;
  cell_clear(c);
  c->state = 1;
  if (nb->nq == nb->capq) {
    nb->capq = nb->capq ? nb->capq * 2 : 16;
    nb->queue = (unsigned *)xrealloc(nb->queue, nb->capq * sizeof(unsigned));
  }
  nb->queue[nb->nq++] = c->serial;
  if (nb->ks == KS_OFF && k_start(nb) != 0) {
    c->state = 0;
    nb->nq = 0;
    return;
  }
  k_next(nb);
}


static void restart (Nb *nb) {
  size_t i;
  k_stop(nb);
  for (i = 0; i < nb->n; i++) nb->cell[i]->state = 0;
  nb->nq = 0;
  nb->running = 0;
  k_start(nb);
}


static void interrupt (Nb *nb) {	/* KeyboardInterrupt in the cell, as Jupyter's */
  size_t i;
  if (nb->ks != KS_BUSY) return;
  for (i = 0; i < nb->n; i++)	/* the ones waiting their turn do not run */
    if (nb->cell[i]->state == 1) nb->cell[i]->state = 0;
  nb->nq = 0;
  k_send(nb, "{\"op\":\"interrupt\"}\n", 19);
  nb->int_at = os_now_us();
}

/* }================================================================== */


/*
** {==================================================================
** The page
** ===================================================================
*/

int nb_is_file (const char *path) {
  const char *dot = path ? strrchr(path_basename(path), '.') : NULL;
  return dot && m_fncmp(dot, ".ipynb") == 0;
}


static void lang_from_meta (Nb *nb) {
  const Json *m = json_get(nb->root, "metadata");
  const char *lang = json_str(json_get(m, "language_info.name"), json_str(json_get(m, "kernelspec.language"), "python"));
  snprintf(nb->kname, sizeof(nb->kname), "%s", json_str(json_get(m, "kernelspec.name"), "python3"));
  snprintf(nb->klang, sizeof(nb->klang), "%s", lang);
  nb->sx = syntax_by_id(lang);
  if (nb->sx == NULL) nb->sx = syntax_by_id("python");
}


void *nb_open (const char *path) {
  Nb *nb;
  char *s = NULL;
  size_t len = 0;
  int fd = path ? os_open(path, OS_READ) : -1, i;
  static int untitled;
  if (fd >= 0) {
    Buf b;
    char chunk[65536];
    long k;
    buf_init(&b);
    while ((k = os_read(fd, chunk, sizeof(chunk))) > 0) buf_putn(&b, chunk, (size_t)k);
    os_close(fd);
    len = b.len;
    buf_putc(&b, '\0');
    s = buf_take(&b);
  }
  nb = (Nb *)xmalloc(sizeof(Nb));
  memset(nb, 0, sizeof(*nb));
  nb->path = path ? xstrdup(path) : NULL;
  nb->to = nb->from = nb->err = -1;
  buf_init(&nb->in);
  nb->minor = 5;
  if (s && len > 0) {
    size_t skip = (len >= 3 && memcmp(s, "\xEF\xBB\xBF", 3) == 0) ? 3 : 0;
    nb->root = json_parse(s + skip, len - skip);
    if (nb->root == NULL || nb->root->type != J_OBJ) {
      json_free(nb->root);
      free(s);
      free(nb->path);
      free(nb);
      return NULL;
    }
    nb->minor = (int)json_num(json_get(nb->root, "nbformat_minor"), 5);
    load_cells(nb);
  }
  free(s);
  lang_from_meta(nb);
  if (nb->n == 0) {	/* a new notebook: one empty cell, the caret in it */
    cell_insert(nb, 0, cell_new(CELL_CODE, "", 0));
    nb->edit = 1;
  }
  if (path) snprintf(nb->title, sizeof(nb->title), "%s", path_basename(path));
  else snprintf(nb->title, sizeof(nb->title), "Untitled-%d.ipynb", ++untitled);
  for (i = 0; i < MAX_NB; i++)
    if (g_nb[i] == NULL) {
      g_nb[i] = nb;
      break;
    }
  return nb;
}


void nb_close (void *page) {
  Nb *nb = (Nb *)page;
  size_t i;
  int k;
  if (nb == NULL) return;
  k_stop(nb);
  comp_close(nb);
  free(nb->ask);
  for (k = 0; k < MAX_NB; k++)
    if (g_nb[k] == nb) g_nb[k] = NULL;
  for (i = 0; i < nb->n; i++) cell_free(nb->cell[i]);
  free(nb->cell);
  free(nb->queue);
  buf_free(&nb->in);
  json_free(nb->root);
  free(nb->rowcell);
  free(nb->rowpart);
  free(nb->rowline);
  free(nb->path);
  free(nb);
}


const char *nb_title (void *page) {
  return page ? ((Nb *)page)->title : "Notebook";
}


const char *nb_path (void *page) {
  return page ? ((Nb *)page)->path : NULL;
}


void nb_changes (void *page, long *changes, long *saved) {
  Nb *nb = (Nb *)page;
  *changes = nb ? nb->changes : 0;
  *saved = nb ? nb->saved : 0;
}


int nb_save (void *page, int as) {
  Nb *nb = (Nb *)page;
  size_t len;
  char *s;
  int fd;
  if (nb == NULL) return -1;
  if (as || nb->path == NULL) {	/* Save As: its path (a new one: Untitled-1.ipynb in the folder) */
    char *def = nb->path ? xstrdup(nb->path) : path_join(side_root(), nb->title);
    char *name = ask_text("Save As: the path of the notebook", def);
    free(def);
    if (name == NULL || *name == '\0') {
      free(name);
      return -1;
    }
    if (!path_is_sep(name[0]) && !(name[0] && name[1] == ':')) {
      char *full = path_join(side_root(), name);
      free(name);
      name = full;
    }
    if (!nb_is_file(name)) {
      char *with = (char *)xmalloc(strlen(name) + 7);
      sprintf(with, "%s.ipynb", name);
      free(name);
      name = with;
    }
    free(nb->path);
    nb->path = name;
    snprintf(nb->title, sizeof(nb->title), "%s", path_basename(name));
  }
  s = nb_text(nb, &len);
  fd = os_open(nb->path, OS_WRITE);
  if (fd < 0) {
    toast(1, "Failed to save '%s'", path_basename(nb->path));
    free(s);
    return -1;
  }
  os_write(fd, s, len);
  os_close(fd);
  free(s);
  nb->saved = nb->changes;
  return 0;
}


int nb_busy (void) {	/* a kernel works: mme looks for its answers often */
  int i;
  for (i = 0; i < MAX_NB; i++)
    if (g_nb[i] && (g_nb[i]->ks == KS_BUSY || g_nb[i]->ks == KS_START)) return 1;
  return 0;
}


int nb_poll (void) {
  int i, got = 0;
  if (g_asking) return 0;
  for (i = 0; i < MAX_NB; i++)
    if (g_nb[i]) got |= k_poll(g_nb[i]);
  return got;
}


void nb_shutdown (void) {	/* mme quits: the kernels go */
  int i;
  for (i = 0; i < MAX_NB; i++)
    if (g_nb[i]) k_stop(g_nb[i]);
}

/* }================================================================== */


/*
** {==================================================================
** Editing a cell
** ===================================================================
*/

static size_t line_start (const Cell *c, size_t at) {
  const char *s = S_(c);
  while (at > 0 && s[at - 1] != '\n') at--;
  return at;
}


static size_t line_end (const Cell *c, size_t at) {
  const char *s = S_(c);
  while (at < c->src.len && s[at] != '\n') at++;
  return at;
}


static size_t cols (const char *s, size_t n) {	/* the columns n bytes take */
  char tmp[1024];
  if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
  memcpy(tmp, s, n);
  tmp[n] = '\0';
  return str_cols(tmp);
}


static size_t at_col (const Cell *c, size_t ls, size_t col) {	/* the byte of a line at a column */
  const char *s = S_(c);
  size_t le = line_end(c, ls), at = ls;
  while (at < le) {
    size_t len;
    utf8_decode(s + at, le - at, &len);
    if (len == 0) len = 1;
    if (cols(s + ls, at + len - ls) > col) break;
    at += len;
  }
  return at;
}


static void snap (Cell *c, int typing) {	/* before an edit: an undo step (typing joins the last one) */
  size_t i;
  if (typing && c->typing && c->iundo > 0) return;
  c->typing = typing;
  for (i = c->iundo; i < c->nundo; i++) free(c->undo[i].s);
  c->nundo = c->iundo;
  c->undo = (Snap *)xrealloc(c->undo, (c->nundo + 1) * sizeof(Snap));
  c->undo[c->nundo].s = xstrdup(S_(c));
  c->undo[c->nundo].cur = c->cur;
  c->nundo++;
  c->iundo = c->nundo;
}


static void set_src (Cell *c, const char *s) {
  buf_free(&c->src);
  buf_init(&c->src);
  buf_puts(&c->src, s);
  buf_putc(&c->src, '\0');
  c->src.len--;
  c->gen++;
}


static void undo (Nb *nb, Cell *c, int redo) {
  char *now;
  size_t cur;
  if (!redo && c->iundo == 0) return;
  if (redo && c->iundo >= c->nundo) return;
  now = xstrdup(S_(c));
  cur = c->cur;
  if (!redo) {
    if (c->iundo == c->nundo) {	/* the text as it is: kept for Redo */
      c->undo = (Snap *)xrealloc(c->undo, (c->nundo + 1) * sizeof(Snap));
      c->undo[c->nundo].s = xstrdup(now);
      c->undo[c->nundo].cur = cur;
      c->nundo++;
    }
    c->iundo--;
  }
  else c->iundo++;
  set_src(c, c->undo[c->iundo].s);
  c->cur = c->undo[c->iundo].cur;
  if (c->cur > c->src.len) c->cur = c->src.len;
  if (redo && c->iundo == c->nundo - 1) {	/* back at the newest: it is the text again */
    free(c->undo[c->iundo].s);
    c->nundo--;
  }
  c->sel = 0;
  c->typing = 0;
  free(now);
  dirty(nb);
}


static void sel_range (const Cell *c, size_t *a, size_t *b) {
  *a = c->anchor < c->cur ? c->anchor : c->cur;
  *b = c->anchor < c->cur ? c->cur : c->anchor;
}


static void del_range (Cell *c, size_t a, size_t b) {
  memmove(c->src.s + a, c->src.s + b, c->src.len - b + 1);
  c->src.len -= b - a;
  c->cur = a;
  c->gen++;
}


static void del_sel (Cell *c) {
  size_t a, b;
  if (!c->sel) return;
  sel_range(c, &a, &b);
  if (b > a) del_range(c, a, b);
  c->sel = 0;
}


static void insert (Nb *nb, Cell *c, const char *s, size_t n, int typing) {
  snap(c, typing);
  del_sel(c);
  if (c->src.s == NULL) {
    buf_putc(&c->src, '\0');
    c->src.len = 0;
  }
  {
    size_t tail = c->src.len - c->cur;
    buf_putn(&c->src, s, n);	/* room at the end */
    buf_putc(&c->src, '\0');
    c->src.len--;
    memmove(c->src.s + c->cur + n, c->src.s + c->cur, tail);
    memcpy(c->src.s + c->cur, s, n);
    c->src.s[c->src.len] = '\0';
  }
  c->cur += n;
  c->gen++;
  dirty(nb);
}


static void move_to (Cell *c, size_t at, int shift) {
  if (shift && !c->sel) {
    c->anchor = c->cur;
    c->sel = 1;
  }
  else if (!shift) c->sel = 0;
  c->cur = at;
  if (c->sel && c->anchor == c->cur) c->sel = 0;
}


static size_t prev_char (const Cell *c, size_t at) {
  const char *s = S_(c);
  if (at == 0) return 0;
  at--;
  while (at > 0 && ((unsigned char)s[at] & 0xC0) == 0x80) at--;
  return at;
}


static size_t next_char (const Cell *c, size_t at) {
  const char *s = S_(c);
  size_t len;
  if (at >= c->src.len) return c->src.len;
  utf8_decode(s + at, c->src.len - at, &len);
  return at + (len ? len : 1);
}


static int is_word (int ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch >= 0x80;
}


static size_t word_left (const Cell *c, size_t at) {
  const char *s = S_(c);
  while (at > 0 && !is_word((unsigned char)s[at - 1])) at--;
  while (at > 0 && is_word((unsigned char)s[at - 1])) at--;
  return at;
}


static size_t word_right (const Cell *c, size_t at) {
  const char *s = S_(c);
  while (at < c->src.len && !is_word((unsigned char)s[at])) at++;
  while (at < c->src.len && is_word((unsigned char)s[at])) at++;
  return at;
}


static void copy_sel (Cell *c, int cut, Nb *nb) {	/* no selection: the line, as VS Code */
  size_t a, b;
  if (c->sel) sel_range(c, &a, &b);
  else {
    a = line_start(c, c->cur);
    b = line_end(c, c->cur);
    if (b < c->src.len) b++;
  }
  if (b <= a) return;
  if (!c->sel) {
    char *t = (char *)xmalloc(b - a + 2);
    memcpy(t, S_(c) + a, b - a);
    if (t[b - a - 1] != '\n') t[b - a++] = '\n';
    clip_set(t, b - a);
    free(t);
  }
  else clip_set(S_(c) + a, b - a);
  if (cut) {
    snap(c, 0);
    del_range(c, a, b);
    c->sel = 0;
    dirty(nb);
  }
}


static void indent_lines (Nb *nb, Cell *c, int out) {	/* Tab / Shift+Tab on the lines of the selection */
  size_t a, b, at;
  sel_range(c, &a, &b);
  if (!c->sel) a = b = c->cur;
  snap(c, 0);
  at = line_start(c, a);
  for (;;) {
    size_t le;
    if (out) {
      int k = 0;
      while (k < 4 && at < c->src.len && c->src.s[at] == ' ') {
        del_range(c, at, at + 1);
        if (c->anchor > at) c->anchor--;
        if (b > at) b--;
        k++;
      }
    }
    else {
      size_t save = c->anchor, tail = c->src.len - at;
      buf_puts(&c->src, "    ");
      buf_putc(&c->src, '\0');
      c->src.len--;
      memmove(c->src.s + at + 4, c->src.s + at, tail);
      memcpy(c->src.s + at, "    ", 4);
      c->src.s[c->src.len] = '\0';
      c->anchor = save >= at ? save + 4 : save;
      b += 4;
    }
    le = line_end(c, at);
    if (le >= b || le >= c->src.len) break;
    at = le + 1;
  }
  c->cur = c->sel ? b : line_end(c, line_start(c, c->cur));
  if (!c->sel) c->cur = at_col(c, line_start(c, c->cur), c->want);
  c->gen++;
  dirty(nb);
}


static void toggle_comment (Nb *nb, Cell *c) {
  const char *lc = "#", *o = NULL, *cl = NULL;
  size_t a, b, at, n;
  int all = 1;
  if (c->type == CELL_CODE && nb->sx) syntax_comment(nb->sx, &lc, &o, &cl);
  if (lc == NULL || *lc == '\0') return;
  n = strlen(lc);
  sel_range(c, &a, &b);
  if (!c->sel) a = b = c->cur;
  for (at = line_start(c, a);;) {	/* all commented: uncomment */
    size_t p = at, le = line_end(c, at);
    while (p < le && c->src.s[p] == ' ') p++;
    if (p < le && strncmp(c->src.s + p, lc, n) != 0) all = 0;
    if (le >= b || le >= c->src.len) break;
    at = le + 1;
  }
  snap(c, 0);
  for (at = line_start(c, a);;) {
    size_t p = at, le = line_end(c, at);
    while (p < le && c->src.s[p] == ' ') p++;
    if (p < le) {
      if (all) {
        size_t k = n + (p + n < le && c->src.s[p + n] == ' ' ? 1 : 0);
        del_range(c, p, p + k);
        if (b >= p + k) b -= k;
      }
      else {
        size_t tail = c->src.len - p;
        char ins[16];
        snprintf(ins, sizeof(ins), "%s ", lc);
        buf_puts(&c->src, ins);
        buf_putc(&c->src, '\0');
        c->src.len--;
        memmove(c->src.s + p + strlen(ins), c->src.s + p, tail);
        memcpy(c->src.s + p, ins, strlen(ins));
        c->src.s[c->src.len] = '\0';
        b += strlen(ins);
      }
    }
    le = line_end(c, at);
    if (le >= b || le >= c->src.len) break;
    at = le + 1;
  }
  c->sel = 0;
  if (c->cur > c->src.len) c->cur = c->src.len;
  c->gen++;
  dirty(nb);
}


static void paste (Nb *nb, Cell *c, const char *s, size_t n) {
  Buf b;
  size_t i;
  buf_init(&b);
  for (i = 0; i < n; i++)
    if (s[i] != '\r') buf_putc(&b, s[i]);
  insert(nb, c, b.s ? b.s : "", b.len, 0);
  buf_free(&b);
}


static void enter (Nb *nb, Cell *c) {	/* a new line, the indent of this one (more after ':') */
  size_t ls = line_start(c, c->cur), p = ls;
  char buf[256];
  size_t k = 0;
  buf[k++] = '\n';
  while (p < c->cur && c->src.s[p] == ' ' && k + 1 < sizeof(buf)) buf[k++] = c->src.s[p++];
  if (c->type == CELL_CODE && c->cur > ls) {	/* after ':' (a block), an open bracket: one more */
    size_t q = c->cur;
    int j;
    while (q > ls && c->src.s[q - 1] == ' ') q--;
    if (q > ls && (c->src.s[q - 1] == ':' || c->src.s[q - 1] == '{' || c->src.s[q - 1] == '(' || c->src.s[q - 1] == '['))
      for (j = 0; j < 4 && k + 1 < sizeof(buf); j++) buf[k++] = ' ';
  }
  insert(nb, c, buf, k, 0);
}

/* }================================================================== */


/*
** {==================================================================
** Drawing
** ===================================================================
*/

static int text_w (const Nb *nb) {
  int w = nb->w - GW - 2;
  return w < 8 ? 8 : w;
}


static size_t count_lines (const char *s, size_t n) {
  size_t k = 1, i;
  for (i = 0; i < n; i++)
    if (s[i] == '\n') k++;
  return k;
}


static void md_doc (Cell *c) {	/* the Markdown drawn from its text */
  if (c->md == NULL) {
    c->md = (Doc *)xmalloc(sizeof(Doc));
    doc_init(c->md);
    c->md_gen = 0;
  }
  if (c->md_gen != c->gen) {
    doc_set_text(c->md, S_(c), c->src.len);
    c->md_gen = c->gen;
  }
}


static int shows_md (const Nb *nb, size_t i) {	/* a Markdown cell not being edited: drawn */
  const Cell *c = nb->cell[i];
  return c->type == CELL_MD && !(nb->edit && nb->sel == i) && c->src.len > 0;
}


static int out_rows (const Out *o, int w) {
  const char *s = o->text.s ? o->text.s : "";
  size_t n = o->text.len, i = 0;
  int rows = 0;
  if (o->img) return img_rows(o->img, w);
  while (n > 0 && s[n - 1] == '\n') n--;	/* its last LF: no row */
  if (n == 0) return o->type == OT_STREAM ? 0 : 1;
  while (i <= n) {
    const char *nl = i < n ? memchr(s + i, '\n', n - i) : NULL;
    size_t e = nl ? (size_t)(nl - s) : n, c = cols(s + i, e - i);
    rows += c == 0 ? 1 : (int)((c + (size_t)w - 1) / (size_t)w);
    if (nl == NULL) break;
    i = e + 1;
  }
  return rows;
}


static int src_rows (Nb *nb, size_t i) {
  Cell *c = nb->cell[i];
  if (shows_md(nb, i)) {
    md_doc(c);
    return (int)md_rows(c->md, text_w(nb));
  }
  return (int)count_lines(S_(c), c->src.len);
}


static int cell_rows (Nb *nb, size_t i) {	/* the gap over it, its source, its outputs */
  Cell *c = nb->cell[i];
  size_t k;
  int r = 1 + src_rows(nb, i);
  for (k = 0; k < c->nout; k++) r += out_rows(&c->out[k], text_w(nb));
  return r;
}


static void put_row (Nb *nb, int sy, int cell, int part, int line) {
  int k = sy - nb->y;
  if (k < 0 || k >= nb->rows_cap) return;
  nb->rowcell[k] = cell;
  nb->rowpart[k] = part;
  nb->rowline[k] = line;
}


static void fill_row (int x, int y, int w, uint32_t bg) {
  int k;
  for (k = 0; k < w; k++) scr_put_rgb(x + k, y, ' ', ui_color(C_EDITOR_FG), bg, 0);
}


/* the text of an output from row skip, wrapped at w, rows at most h, from sy */
static void draw_out_text (const Out *o, int x, int sy, int w, int skip, int h, uint32_t fg, uint32_t bg) {
  const char *s = o->text.s ? o->text.s : "";
  size_t n = o->text.len, i = 0;
  int row = 0;
  while (n > 0 && s[n - 1] == '\n') n--;
  while (i <= n && row < skip + h) {
    const char *nl = i < n ? memchr(s + i, '\n', n - i) : NULL;
    size_t e = nl ? (size_t)(nl - s) : n, p = i;
    do {	/* the line, a row at a time */
      size_t q = p, len;
      int cw = 0;
      while (q < e) {
        int ww;
        utf8_decode(s + q, e - q, &len);
        if (len == 0) len = 1;
        ww = (int)cols(s + q, len);
        if (cw + ww > w && cw > 0) break;
        cw += ww;
        q += len;
      }
      if (row >= skip && row < skip + h) {
        size_t k = p;
        int cx = x;
        while (k < q) {
          uint32_t cp = utf8_decode(s + k, q - k, &len);
          if (cp == '\t') cp = ' ';
          cx += scr_put_rgb(cx, sy + row - skip, cp < 32 ? ' ' : cp, fg, bg, 0);
          k += len ? len : 1;
        }
      }
      row++;
      p = q;
    } while (p < e && row < skip + h);
    if (nl == NULL) break;
    i = e + 1;
  }
}


/*
** A code cell's source, lines from skip, rows at most h, from sy: the
** language's colors, the selection, the caret when it is being edited
*/
static void draw_src (Nb *nb, size_t i, int x, int sy, int w, int skip, int h, int focus) {
  Cell *c = nb->cell[i];
  const char *s = S_(c);
  const Syntax *sx = c->type == CELL_CODE ? nb->sx : c->type == CELL_MD ? syntax_by_id("markdown") : NULL;
  size_t ls = 0, a = 0, b = 0;
  int line = 0, state = 0, editing = nb->edit && nb->sel == i;
  unsigned char *tok = NULL;
  size_t tokcap = 0;
  if (c->sel && editing) sel_range(c, &a, &b);
  if (editing) {	/* the caret's column in sight */
    size_t cc = cols(s + line_start(c, c->cur), c->cur - line_start(c, c->cur));
    if (cc < c->left) c->left = cc;
    if (cc >= c->left + (size_t)w) c->left = cc - (size_t)w + 1;
  }
  else c->left = 0;
  for (;;) {
    size_t le = line_end(c, ls), n = le - ls;
    if (sx) {
      if (n + 1 > tokcap) {
        tokcap = n + 64;
        tok = (unsigned char *)xrealloc(tok, tokcap);
      }
      state = syntax_scan(sx, s + ls, n, state, tok);
    }
    if (line >= skip && line < skip + h) {
      int y = sy + line - skip;
      size_t h0 = 0, h1 = 0;
      if (b > a && b > ls && a <= le) {
        h0 = a > ls ? a - ls : 0;
        h1 = b < le ? b - ls : n + (b > le ? 1 : 0);
        if (h1 > n) h1 = n;
      }
      fill_row(x, y, w, ui_color(C_LINE_BG));
      scr_code(x, y, w, s + ls, n, c->left, sx ? tok : NULL, S_LINE, h0, h1, S_SEL);
      put_row(nb, y, (int)i, RP_SRC, line);
      if (editing && focus && c->cur >= ls && c->cur <= le) {
        nb->cur_sx = x + (int)(cols(s + ls, c->cur - ls) - c->left);
        nb->cur_sy = y;
        scr_cursor(nb->cur_sx, y);
      }
    }
    line++;
    if (le >= c->src.len) break;
    ls = le + 1;
  }
  free(tok);
}


static uint32_t blend (uint32_t a, uint32_t b, int pct) {	/* pct of a over b */
  uint32_t r = (((a >> 16) & 255) * (uint32_t)pct + ((b >> 16) & 255) * (uint32_t)(100 - pct)) / 100;
  uint32_t g = (((a >> 8) & 255) * (uint32_t)pct + ((b >> 8) & 255) * (uint32_t)(100 - pct)) / 100;
  uint32_t bl = ((a & 255) * (uint32_t)pct + (b & 255) * (uint32_t)(100 - pct)) / 100;
  return (r << 16) | (g << 8) | bl;
}


void nb_draw (void *page, int x, int y, int w, int h, int focus) {
  Nb *nb = (Nb *)page;
  uint32_t bg = ui_color(C_EDITOR_BG), fg = ui_color(C_EDITOR_FG), dim = ui_color(C_DIM), acc = ui_color(C_ACCENT);
  int row, k, tw, total = 0, real_img = 0;
  size_t i;
  char tool[256];
  if (nb == NULL) return;
  nb->x = x;
  nb->y = y;
  nb->w = w;
  nb->h = h;
  if (nb->rows_cap < h) {
    nb->rows_cap = h;
    nb->rowcell = (int *)xrealloc(nb->rowcell, (size_t)h * sizeof(int));
    nb->rowpart = (int *)xrealloc(nb->rowpart, (size_t)h * sizeof(int));
    nb->rowline = (int *)xrealloc(nb->rowline, (size_t)h * sizeof(int));
  }
  for (k = 0; k < h; k++) {
    nb->rowcell[k] = -1;
    nb->rowpart[k] = RP_NONE;
    nb->rowline[k] = 0;
    fill_row(x, y + k, w, bg);
  }
  if (h < 3 || w < 20) return;
  tw = text_w(nb);
  /* the toolbar, VS Code's: its buttons, the kernel on the right */
  snprintf(tool, sizeof(tool), " + Code   + Markdown   \xE2\x96\xB7 Run All   \xE2\x86\xBB Restart   \xE2\x96\xA1 Interrupt   Clear All Outputs");
  scr_putsw(x, y, w, tool, S_TEXT);
  {
    char kn[120];
    const char *st = nb->ks == KS_BUSY ? " \xE2\x97\x8F" : nb->ks == KS_START ? " \xE2\x80\xA6" : "";
    snprintf(kn, sizeof(kn), "%s%s ", nb->ks == KS_OFF && nb->kinfo[0] == '\0' ? (nb->klang[0] ? nb->klang : "python") : nb->kinfo, st);
    if ((int)str_cols(kn) + (int)str_cols(tool) + 2 < w) scr_putsw(x + w - (int)str_cols(kn), y, (int)str_cols(kn), kn, S_TEXT);
  }
  put_row(nb, y, -1, RP_TOOL, 0);
  y++;
  h--;
  /* where each cell starts, the selected one (the caret) in sight */
  {
    long start = 0, want0 = -1, want1 = -1;
    for (i = 0; i < nb->n; i++) {
      int r = cell_rows(nb, i);
      if (i == nb->sel) {
        want0 = start;
        want1 = start + r - 1;
        if (nb->edit && !shows_md(nb, i)) {
          Cell *c = nb->cell[i];
          long ln = (long)count_lines(S_(c), c->cur) - 1;
          want0 = start + 1 + ln;
          want1 = want0;
        }
      }
      start += r;
    }
    total = (int)start + 2;
    if (nb->follow && want0 >= 0) {
      if (want0 < nb->top) nb->top = want0;
      if (want1 >= nb->top + h) nb->top = want1 - h + 1;
      if (want0 < nb->top) nb->top = want0;
      nb->follow = 0;
    }
    if (nb->top > total - h) nb->top = total - h;
    if (nb->top < 0) nb->top = 0;
  }
  row = 0;
  for (i = 0; i < nb->n; i++) {
    Cell *c = nb->cell[i];
    int r = cell_rows(nb, i), sr = src_rows(nb, i), selected = i == nb->sel;
    int r0 = row;
    size_t o;
    row++;	/* the gap */
    if (r0 + r <= nb->top) {
      row = r0 + r;
      continue;
    }
    if (r0 >= nb->top + h) break;
    if (r0 >= nb->top) put_row(nb, y + r0 - (int)nb->top, (int)i, RP_GAP, 0);
    /* the source */
    {
      int from = row, skip = nb->top > from ? (int)(nb->top - from) : 0, first = from + skip - (int)nb->top;
      int rows = sr - skip;
      if (first + rows > h) rows = h - first;
      if (rows > 0) {
        if (shows_md(nb, i)) {
          size_t top = (size_t)skip;
          md_doc(c);
          md_draw(c->md, x + GW, y + first, tw, rows, &top);
          for (k = 0; k < rows; k++) put_row(nb, y + first + k, (int)i, RP_SRC, skip + k);
        }
        else draw_src(nb, i, x + GW, y + first, tw, skip, rows, focus);
        for (k = 0; k < rows; k++) {	/* the gutter: the selected cell's bar, [n] */
          int gy = y + first + k;
          uint32_t bar = selected ? (nb->edit ? acc : blend(acc, bg, 50)) : bg;
          scr_put_rgb(x, gy, 0x258E, bar, bg, 0);
          if (skip + k == 0 && c->type == CELL_CODE) {
            char cnt[16];
            if (c->state == 2) snprintf(cnt, sizeof(cnt), "[*]");
            else if (c->state == 1) snprintf(cnt, sizeof(cnt), "[-]");
            else if (c->count > 0) snprintf(cnt, sizeof(cnt), "[%d]", c->count);
            else snprintf(cnt, sizeof(cnt), "[ ]");
            {
              int cx = x + GW - 1 - (int)strlen(cnt), j2;
              for (j2 = 0; cnt[j2]; j2++) scr_put_rgb(cx + j2, gy, (unsigned char)cnt[j2], c->state ? acc : dim, bg, 0);
            }
            if (selected || c->state) scr_put_rgb(x + 1, gy, c->state ? 0x25A1 : 0x25B7, selected ? acc : dim, bg, 0);	/* run (stop) */
          }
          else if (skip + k == 0 && c->type == CELL_MD && !shows_md(nb, i))
            scr_puts(x + 1, gy, "md", S_TEXT);
        }
        if (c->src.len == 0 && skip == 0 && !(nb->edit && selected)) {
          const char *hint = c->type == CELL_MD ? "Empty Markdown cell, Enter to edit" : "";
          int j2;
          for (j2 = 0; hint[j2] && j2 < tw; j2++) scr_put_rgb(x + GW + j2, y + first, (unsigned char)hint[j2], dim, ui_color(C_LINE_BG), 0);
        }
      }
      row += sr;
    }
    /* the outputs */
    for (o = 0; o < c->nout; o++) {
      Out *out = &c->out[o];
      int orows = out_rows(out, tw), skip = nb->top > row ? (int)(nb->top - row) : 0, first = row + skip - (int)nb->top;
      int rows = orows - skip;
      if (first + rows > h) rows = h - first;
      if (rows > 0) {
        if (out->img) {
          img_draw_at(out->img, x + GW, y + first, tw, skip, rows, real_img < 8);
          real_img++;	/* eight pictures themselves at a time (edraw.c keeps that many); more: Braille */
        }
        else draw_out_text(out, x + GW, y + first, tw, skip, rows,
                           out->type == OT_ERROR || (out->type == OT_STREAM && strcmp(out->name, "stderr") == 0) ?
                           ui_color(C_ERROR) : fg, bg);
        for (k = 0; k < rows; k++) {
          put_row(nb, y + first + k, (int)i, RP_OUT, (int)o);
          scr_put_rgb(x, y + first + k, 0x258E, selected ? (nb->edit ? acc : blend(acc, bg, 50)) : bg, bg, 0);
        }
      }
      row += orows;
    }
  }
  if (row + 1 >= nb->top && row + 1 < nb->top + h) {	/* at the end: add a cell */
    int ay = y + row + 1 - (int)nb->top;
    scr_putsw(x + GW, ay, tw, "+ Code    + Markdown", S_TEXT);
    for (k = 0; k < 20 && k < tw; k++) scr_set_fg(x + GW + k, ay, dim);
    put_row(nb, ay, -1, RP_ADD, 0);
  }
  if (nb->comp_open && nb->edit && focus && nb->cur_sy > 0) {	/* the completions, under the caret (over it near the foot): last, over all */
    int n = nb->ncomp < 8 ? (int)nb->ncomp : 8, bw = 12, px, py, r2;
    size_t q;
    Cell *c = nb->cell[nb->sel];
    for (q = 0; q < nb->ncomp; q++)
      if ((int)str_cols(nb->comp[q]) + 2 > bw) bw = (int)str_cols(nb->comp[q]) + 2;
    if (bw > 48) bw = 48;
    if (nb->comp_sel < nb->comp_top) nb->comp_top = nb->comp_sel;
    if (nb->comp_sel >= nb->comp_top + (size_t)n) nb->comp_top = nb->comp_sel - (size_t)n + 1;
    px = nb->cur_sx - (int)cols(S_(c) + nb->comp_start, c->cur > nb->comp_start ? c->cur - nb->comp_start : 0);
    if (px + bw > x + w) px = x + w - bw;
    if (px < x) px = x;
    py = nb->cur_sy + 1;
    if (py + n > y + h) py = nb->cur_sy - n;
    for (r2 = 0; r2 < n; r2++) {
      size_t it = nb->comp_top + (size_t)r2;
      int st = it == nb->comp_sel ? S_MENU_SEL : S_MENU;
      if (py + r2 < y || py + r2 >= y + h) continue;
      scr_fill(px, py + r2, bw, st);
      scr_putsw(px + 1, py + r2, bw - 2, nb->comp[it], st);
    }
  }
}

/* }================================================================== */


/*
** {==================================================================
** Keys and the mouse
** ===================================================================
*/

static void select_cell (Nb *nb, size_t i, int edit) {
  if (nb->n == 0) return;
  comp_close(nb);
  if (i >= nb->n) i = nb->n - 1;
  nb->sel = i;
  nb->edit = edit;
  nb->dd = 0;
  nb->follow = 1;
}


static void add_cell (Nb *nb, size_t at, int type) {
  Cell *c = cell_new(type, "", 0);
  if (at > nb->n) at = nb->n;
  cell_insert(nb, at, c);
  select_cell(nb, at, 1);
  dirty(nb);
}


static Cell *g_deleted;	/* Z brings the last cell deleted back */
static size_t g_deleted_at;


static void delete_cell (Nb *nb, size_t i) {
  if (i >= nb->n) return;
  if (g_deleted) cell_free(g_deleted);
  g_deleted = nb->cell[i];
  g_deleted_at = i;
  if (g_deleted->state) g_deleted->state = 0;
  memmove(nb->cell + i, nb->cell + i + 1, (nb->n - i - 1) * sizeof(Cell *));
  nb->n--;
  if (nb->n == 0) cell_insert(nb, 0, cell_new(CELL_CODE, "", 0));
  select_cell(nb, i < nb->n ? i : nb->n - 1, 0);
  dirty(nb);
}


/* run the cell; then: 0 stay, 1 the next (a new one at the end), 2 a new one under it */
static void run_and (Nb *nb, int then) {
  size_t i = nb->sel;
  if (nb->cell[i]->type == CELL_CODE) run_cell(nb, i);
  nb->edit = 0;
  if (then == 1) {
    if (i + 1 < nb->n) select_cell(nb, i + 1, 0);
    else add_cell(nb, nb->n, CELL_CODE);
  }
  else if (then == 2) add_cell(nb, i + 1, CELL_CODE);
  nb->follow = 1;
}


static void run_all (Nb *nb) {
  size_t i;
  for (i = 0; i < nb->n; i++)
    if (nb->cell[i]->type == CELL_CODE) run_cell(nb, i);
}


static void clear_all (Nb *nb) {
  size_t i;
  for (i = 0; i < nb->n; i++)
    if (nb->cell[i]->nout || nb->cell[i]->count) {
      cell_clear(nb->cell[i]);
      nb->cell[i]->count = 0;
      dirty(nb);
    }
}


void nb_command (void *page, int what) {
  Nb *nb = (Nb *)page;
  if (nb == NULL) return;
  switch (what) {
    case NB_RUN_ALL: run_all(nb); break;
    case NB_RESTART: restart(nb); break;
    case NB_INTERRUPT: interrupt(nb); break;
    case NB_CLEAR: clear_all(nb); break;
    case NB_RUN: run_and(nb, 0); break;
  }
}


/* the keys it takes before mme's own (its typing, Enter, Ctrl+Enter ...) */
int nb_takes (void *page, int k) {
  Nb *nb = (Nb *)page;
  int code = KEY_CODE(k), mods = k & (KM_CTRL | KM_ALT | KM_SHIFT);
  if (nb == NULL) return 0;
  if (code == K_ENTER) return 1;	/* Ctrl+Enter, Shift+Enter, Alt+Enter */
  if (nb->edit) {
    if (IS_TEXT(k) || IS_PASTE(k)) return 1;
    if (k == CTRL('a') || k == CTRL('c') || k == CTRL('x') || k == CTRL('z') || k == CTRL('y') ||
        k == ('/' | KM_CTRL) || k == 0x1F || k == CTRL('d') || k == (' ' | KM_CTRL) || k == 0) return 1;
    if (code == K_TAB || code == K_BS || code == K_DEL || code == K_ESC || code == K_UP || code == K_DOWN ||
        code == K_LEFT || code == K_RIGHT || code == K_HOME || code == K_END || code == K_PGUP || code == K_PGDN) return !(mods & KM_ALT);
    return 0;
  }
  if ((code == K_UP || code == K_DOWN) && mods == KM_ALT) return 1;	/* move the cell */
  if (mods == 0 && (code == K_UP || code == K_DOWN || code == K_HOME || code == K_END || code == K_PGUP || code == K_PGDN ||
                    code == K_ESC || code == K_DEL)) return 1;
  if (k == 'a' || k == 'b' || k == 'd' || k == 'm' || k == 'y' || k == 'j' || k == 'k' || k == 'z' || k == 'r' ||
      k == 'A' || k == 'B') return 1;
  if (k == CTRL('c') || k == CTRL('x') || IS_PASTE(k)) return 1;
  return 0;
}


static Cell *g_cell_clip;	/* a cell copied (command mode's Ctrl+C) */


void nb_key (void *page, int k) {
  Nb *nb = (Nb *)page;
  int code = KEY_CODE(k), shift = (k & KM_SHIFT) != 0, ctrl = (k & KM_CTRL) != 0;
  Cell *c;
  if (nb == NULL || nb->n == 0) return;
  if (nb->sel >= nb->n) nb->sel = nb->n - 1;
  c = nb->cell[nb->sel];
  nb->follow = 1;
  if (code == K_ENTER && (k & (KM_CTRL | KM_SHIFT | KM_ALT))) {	/* run */
    run_and(nb, (k & KM_ALT) ? 2 : (k & KM_SHIFT) ? 1 : 0);
    return;
  }
  if (!nb->edit) {	/* command mode */
    if (k != 'd') nb->dd = 0;
    if (code == K_ENTER) {
      select_cell(nb, nb->sel, 1);
      c->cur = c->src.len;
      c->sel = 0;
    }
    else if (code == K_UP && (k & KM_ALT)) {
      if (nb->sel > 0) {
        Cell *t = nb->cell[nb->sel - 1];
        nb->cell[nb->sel - 1] = c;
        nb->cell[nb->sel] = t;
        nb->sel--;
        dirty(nb);
      }
    }
    else if (code == K_DOWN && (k & KM_ALT)) {
      if (nb->sel + 1 < nb->n) {
        Cell *t = nb->cell[nb->sel + 1];
        nb->cell[nb->sel + 1] = c;
        nb->cell[nb->sel] = t;
        nb->sel++;
        dirty(nb);
      }
    }
    else if (code == K_UP || k == 'k') select_cell(nb, nb->sel > 0 ? nb->sel - 1 : 0, 0);
    else if (code == K_DOWN || k == 'j') select_cell(nb, nb->sel + 1, 0);
    else if (code == K_HOME) select_cell(nb, 0, 0);
    else if (code == K_END) select_cell(nb, nb->n - 1, 0);
    else if (code == K_PGUP || code == K_PGDN) {
      nb->top += (code == K_PGUP ? -1 : 1) * (nb->h > 4 ? nb->h - 2 : 1);
      nb->follow = 0;
    }
    else if (k == 'a' || k == 'A') add_cell(nb, nb->sel, CELL_CODE);
    else if (k == 'b' || k == 'B') add_cell(nb, nb->sel + 1, CELL_CODE);
    else if (k == 'd') {	/* D D */
      if (nb->dd) delete_cell(nb, nb->sel);
      else nb->dd = 1;
    }
    else if (code == K_DEL) delete_cell(nb, nb->sel);
    else if (k == 'z' && g_deleted) {	/* the cell deleted, back */
      size_t at = g_deleted_at <= nb->n ? g_deleted_at : nb->n;
      cell_insert(nb, at, g_deleted);
      g_deleted = NULL;
      select_cell(nb, at, 0);
      dirty(nb);
    }
    else if (k == 'm' && c->type != CELL_MD) {
      c->type = CELL_MD;
      cell_clear(c);
      c->count = 0;
      c->gen++;
      dirty(nb);
    }
    else if (k == 'y' && c->type != CELL_CODE) {
      c->type = CELL_CODE;
      c->gen++;
      dirty(nb);
    }
    else if (k == 'r' && c->type != CELL_RAW) {
      c->type = CELL_RAW;
      cell_clear(c);
      c->gen++;
      dirty(nb);
    }
    else if (k == CTRL('c') || k == CTRL('x')) {	/* the cell, to paste as a cell */
      if (g_cell_clip) cell_free(g_cell_clip);
      g_cell_clip = cell_new(c->type, S_(c), c->src.len);
      clip_set(S_(c), c->src.len);
      if (k == CTRL('x')) delete_cell(nb, nb->sel);
    }
    else if (IS_PASTE(k) && g_cell_clip) {
      Cell *n2 = cell_new(g_cell_clip->type, S_(g_cell_clip), g_cell_clip->src.len);
      cell_insert(nb, nb->sel + 1, n2);
      select_cell(nb, nb->sel + 1, 0);
      dirty(nb);
      if (KEY_CODE(k) == K_PASTE) {	/* what the terminal pasted is not needed */
        Buf b;
        buf_init(&b);
        term_paste(&b);
        buf_free(&b);
      }
    }
    return;
  }
  /* edit mode */
  if (nb->comp_open) {	/* the completions' keys */
    if (code == K_ESC) {
      comp_close(nb);
      return;
    }
    if (code == K_UP || code == K_DOWN) {
      if (code == K_UP) nb->comp_sel = nb->comp_sel > 0 ? nb->comp_sel - 1 : nb->ncomp - 1;
      else nb->comp_sel = nb->comp_sel + 1 < nb->ncomp ? nb->comp_sel + 1 : 0;
      return;
    }
    if ((code == K_TAB || code == K_ENTER) && !(k & (KM_CTRL | KM_ALT | KM_SHIFT))) {
      comp_take(nb);
      return;
    }
    if (!IS_TEXT(k) && code != K_BS) comp_close(nb);
  }
  if (k == (' ' | KM_CTRL) || k == 0) {	/* Ctrl+Space: the completions now, every name when nothing is typed */
    nb->comp_all = 1;
    comp_ask(nb);
    return;
  }
  nb->comp_all = 0;
  if (code == K_ESC) {
    nb->edit = 0;
    c->sel = 0;
    return;
  }
  if (IS_PASTE(k)) {
    Buf b;
    buf_init(&b);
    paste_take(k, &b);
    paste(nb, c, b.s ? b.s : "", b.len);
    buf_free(&b);
  }
  else if (code == K_ENTER) enter(nb, c);
  else if (IS_TEXT(k)) {
    char u[4];
    int n = utf8_encode((uint32_t)k, u);
    insert(nb, c, u, (size_t)n, 1);
  }
  else if (code == K_TAB) {
    if (shift || c->sel) indent_lines(nb, c, shift);
    else {
      size_t col = cols(S_(c) + line_start(c, c->cur), c->cur - line_start(c, c->cur));
      insert(nb, c, "    ", 4 - col % 4, 0);
    }
  }
  else if (code == K_BS) {
    if (c->sel) {
      snap(c, 0);
      del_sel(c);
      dirty(nb);
    }
    else if (c->cur > 0) {
      size_t a = ctrl ? word_left(c, c->cur) : prev_char(c, c->cur);
      snap(c, 0);
      del_range(c, a, c->cur);
      dirty(nb);
    }
  }
  else if (code == K_DEL) {
    if (c->sel) {
      snap(c, 0);
      del_sel(c);
      dirty(nb);
    }
    else if (c->cur < c->src.len) {
      size_t b = ctrl ? word_right(c, c->cur) : next_char(c, c->cur);
      snap(c, 0);
      del_range(c, c->cur, b);
      dirty(nb);
    }
  }
  else if (code == K_LEFT) {
    if (c->sel && !shift) {
      size_t a, b;
      sel_range(c, &a, &b);
      move_to(c, a, 0);
    }
    else move_to(c, ctrl ? word_left(c, c->cur) : prev_char(c, c->cur), shift);
    c->want = cols(S_(c) + line_start(c, c->cur), c->cur - line_start(c, c->cur));
  }
  else if (code == K_RIGHT) {
    if (c->sel && !shift) {
      size_t a, b;
      sel_range(c, &a, &b);
      move_to(c, b, 0);
    }
    else move_to(c, ctrl ? word_right(c, c->cur) : next_char(c, c->cur), shift);
    c->want = cols(S_(c) + line_start(c, c->cur), c->cur - line_start(c, c->cur));
  }
  else if (code == K_UP || code == K_DOWN || code == K_PGUP || code == K_PGDN) {
    int n = (code == K_PGUP || code == K_PGDN) ? (nb->h > 4 ? nb->h - 2 : 1) : 1, up = code == K_UP || code == K_PGUP;
    size_t at = c->cur;
    int moved = 0;
    while (n-- > 0) {
      size_t ls = line_start(c, at);
      if (up) {
        if (ls == 0) break;
        at = at_col(c, line_start(c, ls - 1), c->want);
      }
      else {
        size_t le = line_end(c, at);
        if (le >= c->src.len) break;
        at = at_col(c, le + 1, c->want);
      }
      moved = 1;
    }
    if (!moved && !shift && (code == K_UP || code == K_DOWN)) {	/* past the first (last) line: the cell before (after) */
      if (up && nb->sel > 0) {
        select_cell(nb, nb->sel - 1, 1);
        c = nb->cell[nb->sel];
        c->cur = at_col(c, line_start(c, c->src.len), c->want);
        c->sel = 0;
      }
      else if (!up && nb->sel + 1 < nb->n) {
        select_cell(nb, nb->sel + 1, 1);
        c = nb->cell[nb->sel];
        c->cur = at_col(c, 0, c->want);
        c->sel = 0;
      }
      return;
    }
    move_to(c, at, shift);
  }
  else if (code == K_HOME) {
    size_t ls = ctrl ? 0 : line_start(c, c->cur), p = ls;
    if (!ctrl) {	/* the first non-blank, then the line's start */
      while (p < c->src.len && c->src.s[p] == ' ') p++;
      if (p == c->cur) p = ls;
    }
    move_to(c, p, shift);
    c->want = cols(S_(c) + line_start(c, c->cur), c->cur - line_start(c, c->cur));
  }
  else if (code == K_END) {
    move_to(c, ctrl ? c->src.len : line_end(c, c->cur), shift);
    c->want = cols(S_(c) + line_start(c, c->cur), c->cur - line_start(c, c->cur));
  }
  else if (k == CTRL('a')) {
    c->anchor = 0;
    c->cur = c->src.len;
    c->sel = c->src.len > 0;
  }
  else if (k == CTRL('c') || k == CTRL('x')) copy_sel(c, k == CTRL('x'), nb);
  else if (k == CTRL('z')) undo(nb, c, 0);
  else if (k == CTRL('y')) undo(nb, c, 1);
  else if (k == ('/' | KM_CTRL) || k == 0x1F) toggle_comment(nb, c);
  if (c->type == CELL_CODE) {	/* a name typed (or after a dot): the completions; else they go */
    int prev = c->cur > 0 ? (unsigned char)c->src.s[c->cur - 1] : 0;
    int name = prev == '_' || prev == '.' || (prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
               (prev >= '0' && prev <= '9' && nb->comp_open);
    if ((IS_TEXT(k) && name) || (code == K_BS && nb->comp_open && name)) comp_ask(nb);
    else if (IS_TEXT(k) || code == K_BS) comp_close(nb);
  }
  if (code == K_ENTER || IS_TEXT(k) || code == K_TAB || code == K_BS || code == K_DEL || IS_PASTE(k))
    c->want = cols(S_(c) + line_start(c, c->cur), c->cur - line_start(c, c->cur));
}


void nb_mouse (void *page, const Mouse *m) {
  Nb *nb = (Nb *)page;
  int k, cell, part, line;
  if (nb == NULL) return;
  if (m->wheel) {
    nb->top += m->wheel * 3;
    nb->follow = 0;
    return;
  }
  if (!m->press || m->drag || m->button != 0) return;
  k = m->y - nb->y;
  if (k < 0 || k >= nb->h || k >= nb->rows_cap) return;
  cell = nb->rowcell[k];
  part = nb->rowpart[k];
  line = nb->rowline[k];
  if (part == RP_TOOL) {	/* the toolbar's buttons, by their place */
    int x = m->x - nb->x;
    if (x < 8) add_cell(nb, nb->sel + 1, CELL_CODE);
    else if (x < 21) add_cell(nb, nb->sel + 1, CELL_MD);
    else if (x < 33) run_all(nb);
    else if (x < 45) restart(nb);
    else if (x < 59) interrupt(nb);
    else if (x < 78) clear_all(nb);
    return;
  }
  if (part == RP_ADD) {
    int x = m->x - nb->x - GW;
    add_cell(nb, nb->n, x >= 10 ? CELL_MD : CELL_CODE);
    return;
  }
  if (cell < 0 || (size_t)cell >= nb->n) return;
  if (part == RP_SRC && m->x - nb->x < GW && m->x - nb->x <= 2 && line == 0 && nb->cell[cell]->type == CELL_CODE) {
    select_cell(nb, (size_t)cell, 0);	/* the run button */
    if (nb->cell[cell]->state) interrupt(nb);
    else run_cell(nb, (size_t)cell);
    return;
  }
  if (part == RP_SRC && m->x - nb->x >= GW) {	/* in its text: edit it there */
    Cell *c = nb->cell[cell];
    int was = nb->edit && nb->sel == (size_t)cell;
    if (c->type == CELL_MD && !was) {	/* the Markdown: a click selects it, Enter (a second click) edits */
      if (nb->sel == (size_t)cell) {
        select_cell(nb, (size_t)cell, 1);
        c->cur = c->src.len;
      }
      else select_cell(nb, (size_t)cell, 0);
      return;
    }
    select_cell(nb, (size_t)cell, 1);
    {
      size_t ls = 0;
      int ln = 0;
      while (ln < line && ls < c->src.len) {
        ls = line_end(c, ls) + 1;
        ln++;
      }
      if (ls > c->src.len) ls = c->src.len;
      c->cur = at_col(c, ls, (size_t)(m->x - nb->x - GW) + c->left);
      c->sel = 0;
      c->want = cols(S_(c) + ls, c->cur - ls);
    }
    return;
  }
  select_cell(nb, (size_t)cell, 0);
}

/* }================================================================== */
