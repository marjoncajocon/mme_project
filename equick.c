/*
** equick.c - VS Code's quick diff and merge conflicts
**
** Quick diff: a file of the repository is compared, as it is typed, with
** its copy in git's index; the changes (hunks of lines) give the bars in
** the gutter (added, modified, deleted), the dirty diff peek and Stage /
** Revert Change. Staging a change gives the index the whole file with
** only that change made, as VS Code does. The staged changes (HEAD's copy
** against the index's) are shown too, on the lines they are on now.
**
** Merge conflicts: the <<<<<<< ======= >>>>>>> blocks git leaves in a file,
** each with its current and incoming side (and the base between |||||||
** and =======, for diff3).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Quick diff
** ===================================================================
*/

typedef struct QDoc {
  Doc *d;
  char *path;	/* the file (native) it is for */
  int gen;	/* g_gen when the index's copy was read */
  char *orig;	/* the index's copy; NULL: git has none */
  size_t olen;
  size_t *oline, noline;	/* where each of its lines starts */
  int final_nl, crlf;	/* its last line ends with a newline; its lines end with \r\n */
  char *head;	/* HEAD's copy ("" when HEAD has none: all of the index's is staged) */
  size_t hlen;
  size_t *hline, nhline;
  int hfinal_nl;
  QHunk *sh;	/* HEAD -> the index: the staged changes */
  size_t nsh;
  QStaged *sv;	/* those on d's lines now */
  unsigned long edits;	/* d->edits when the hunks were made; ~0: not yet */
  QHunk *h;
  size_t nh;
  unsigned short *mark;	/* QM_* of each line of d */
  size_t nmark;
  int want;	/* its copy is to be read (quick_idle does it) */
} QDoc;

#define NQ	16

static QDoc g_q[NQ];
static int g_gen = 1, g_next;
static unsigned long g_ver;	/* quick_version's */


unsigned long quick_version (void) {
  return g_ver;
}


static void qdoc_free (QDoc *q) {
  free(q->path);
  free(q->orig);
  free(q->oline);
  free(q->head);
  free(q->hline);
  free(q->sh);
  free(q->sv);
  free(q->h);
  free(q->mark);
  memset(q, 0, sizeof(*q));
}


/* git refreshed (a save, a stage, a checkout ...): the index's copies are read again */
void quick_clear (void) {
  g_gen++;
}


/* a closed file's entry goes */
void quick_forget (Doc *d) {
  int i;
  for (i = 0; i < NQ; i++)
    if (g_q[i].d == d) qdoc_free(&g_q[i]);
}


/* where the lines of s[0..len) start, as text_hunks counts them */
static size_t *line_starts (const char *s, size_t len, size_t *n) {
  size_t i, cap = 64, *v = (size_t *)xmalloc(cap * sizeof(size_t));
  *n = 0;
  for (i = 0; i <= len; i++) {
    if (i != 0 && s[i - 1] != '\n') continue;
    if (i == len && i > 0) break;	/* the newline at the end ends the last line */
    if (*n == cap) v = (size_t *)xrealloc(v, (cap *= 2) * sizeof(size_t));
    v[(*n)++] = i;
  }
  return v;
}


static void read_orig (QDoc *q) {
  char *rel, *spec;
  OsStat st;
  free(q->orig);
  free(q->oline);
  free(q->head);
  free(q->hline);
  free(q->sh);
  q->orig = q->head = NULL;
  q->oline = q->hline = NULL;
  q->sh = NULL;
  q->noline = q->olen = q->nhline = q->hlen = q->nsh = 0;
  q->gen = g_gen;
  q->want = 0;
  q->edits = ~0UL;
  if (os_stat(q->path, &st) == 0 && st.size > (8 << 20)) return;	/* a big file: no bars (reading git's copy takes seconds) */
  rel = git_rel_of(q->path);
  if (rel == NULL) return;
  spec = xstrcat3(":", rel, "");
  q->orig = git_blob(spec, &q->olen);
  free(spec);
  if (q->orig == NULL) {
    free(rel);
    return;
  }
  q->oline = line_starts(q->orig, q->olen, &q->noline);
  q->final_nl = q->olen > 0 && q->orig[q->olen - 1] == '\n';
  q->crlf = memchr(q->orig, '\r', q->olen) != NULL;
  spec = xstrcat3("HEAD:", rel, "");	/* and what of it is staged */
  q->head = git_blob(spec, &q->hlen);
  free(spec);
  free(rel);
  if (q->head == NULL) {	/* added: no commit has it yet */
    q->head = xstrdup("");
    q->hlen = 0;
  }
  q->hline = line_starts(q->head, q->hlen, &q->nhline);
  if (q->hlen == 0) q->nhline = 0;
  q->hfinal_nl = q->hlen > 0 && q->head[q->hlen - 1] == '\n';
  q->sh = text_hunks(q->head, q->hlen, q->orig, q->olen, &q->nsh);
}


/*
** The entry of d. git cat-file takes tens of milliseconds, so drawing asks
** for the copy (sync 0) and quick_idle reads it once the editor is quiet;
** a command that stages or reverts (sync 1) cannot wait and reads it now.
*/
static QDoc *entry (Doc *d, const char *path, int sync) {
  int i;
  QDoc *q = NULL;
  if (path == NULL || git_root() == NULL) return NULL;
  for (i = 0; i < NQ && q == NULL; i++)
    if (g_q[i].d == d && g_q[i].path && m_fncmp(g_q[i].path, path) == 0) q = &g_q[i];
  if (q == NULL) {
    for (i = 0; i < NQ && q == NULL; i++)
      if (g_q[i].d == d) q = &g_q[i];	/* saved under another name */
    if (q == NULL) {
      q = &g_q[g_next];
      g_next = (g_next + 1) % NQ;
    }
    qdoc_free(q);
    q->d = d;
    q->path = xstrdup(path);
    q->gen = 0;
  }
  if (q->gen != g_gen) {
    if (!sync) {
      q->want = 1;
      return q;
    }
    read_orig(q);
  }
  return q;
}


/* the editor is quiet: the copy asked for is read; 1 when one was */
int quick_idle (void) {
  int i;
  for (i = 0; i < NQ; i++)
    if (g_q[i].want && g_q[i].path) {
      g_q[i].want = 0;
      if (g_q[i].gen != g_gen) read_orig(&g_q[i]);
      return 1;
    }
  return 0;
}


/*
** The index's lines on d's lines: q->h (the index -> d) walked once, the
** lines asked for in order. A line d no longer has: *gone, and the line
** where it was.
*/
typedef struct QMap {
  const QHunk *h;
  size_t n, k;
  long off;	/* d's lines minus the index's, before hunk k */
} QMap;

static size_t map_line (QMap *m, size_t i, int *gone) {
  const QHunk *u;
  *gone = 0;
  while (m->k < m->n && i >= m->h[m->k].o0 + m->h[m->k].on) {
    m->off += (long)m->h[m->k].nn - (long)m->h[m->k].on;
    m->k++;
  }
  if (m->k < m->n && i >= (u = &m->h[m->k])->o0) {	/* changed since it was staged */
    if (u->nn == 0) {
      *gone = 1;
      return u->n0;
    }
    return u->n0 + (i - u->o0 < u->nn ? i - u->o0 : u->nn - 1);
  }
  return (size_t)((long)i + m->off);
}


static void make_marks (QDoc *q) {
  size_t i, y;
  QMap m;
  g_ver++;
  q->nmark = q->d->n;
  q->mark = (unsigned short *)xrealloc(q->mark, (q->nmark + 1) * sizeof(unsigned short));
  memset(q->mark, 0, (q->nmark + 1) * sizeof(unsigned short));
  for (i = 0; i < q->nh; i++) {
    const QHunk *h = &q->h[i];
    if (h->nn == 0) {	/* lines went: the mark between two lines */
      if (h->n0 > 0 && h->n0 - 1 < q->nmark) q->mark[h->n0 - 1] |= QM_DEL | QM_HEAD;
      else if (q->nmark) q->mark[0] |= QM_DEL_TOP | QM_HEAD;
      continue;
    }
    for (y = h->n0; y < h->n0 + h->nn && y < q->nmark; y++) q->mark[y] |= h->on == 0 ? QM_ADD : QM_MOD;
    if (h->n0 < q->nmark) q->mark[h->n0] |= QM_HEAD;
  }
  q->sv = (QStaged *)xrealloc(q->sv, (q->nsh + 1) * sizeof(QStaged));
  memset(&m, 0, sizeof(m));
  m.h = q->h;
  m.n = q->nh;
  for (i = 0; i < q->nsh; i++) {	/* the staged ones, where they are now */
    const QHunk *h = &q->sh[i];
    QStaged *s = &q->sv[i];
    int gone, any = 0;
    s->h = *h;
    if (h->nn == 0) {	/* lines went (from HEAD's): before the index's line n0 */
      y = map_line(&m, h->n0, &gone);
      s->y = s->last = y ? y - 1 : 0;
      if (s->y < q->nmark) q->mark[s->y] |= (y ? QM_S_DEL : QM_S_DEL_TOP) | QM_S_HEAD;
      continue;
    }
    s->y = s->last = 0;
    for (y = 0; y < h->nn; y++) {
      size_t at = map_line(&m, h->n0 + y, &gone);
      if (!any) s->y = at;
      s->last = at;
      any = 1;
      if (!gone && at < q->nmark) q->mark[at] |= h->on == 0 ? QM_S_ADD : QM_S_MOD;
    }
    if (s->last < s->y) s->last = s->y;
    if (s->y >= q->nmark && q->nmark) s->y = s->last = q->nmark - 1;
    if (s->last >= q->nmark && q->nmark) s->last = q->nmark - 1;	/* its last lines went at the end: up to the last line */
    if (s->y < q->nmark) q->mark[s->y] |= QM_S_HEAD;
  }
}


static QDoc *fresh (Doc *d, const char *path) {
  QDoc *q = entry(d, path, 0);
  Buf b;
  size_t y;
  if (q == NULL || q->orig == NULL) return NULL;
  if (q->edits == d->edits && q->mark && q->nmark == d->n) return q;
  buf_init(&b);
  for (y = 0; y < d->n; y++) {
    buf_putn(&b, d->row[y].s, d->row[y].len);
    if (y + 1 < d->n) buf_putc(&b, '\n');
  }
  free(q->h);
  q->h = text_hunks(q->orig, q->olen, b.s ? b.s : "", b.len, &q->nh);
  buf_free(&b);
  q->edits = d->edits;
  make_marks(q);
  return q;
}


/* the changes of d against the index; NULL: it has no quick diff (not in git) */
const QHunk *quick_hunks (Doc *d, const char *path, size_t *n) {
  QDoc *q = fresh(d, path);
  *n = q ? q->nh : 0;
  return q ? q->h : NULL;
}


/* the staged changes of d (HEAD -> the index), on its lines now; NULL: no quick diff */
const QStaged *quick_staged (Doc *d, const char *path, size_t *n) {
  QDoc *q = fresh(d, path);
  *n = q && q->sv ? q->nsh : 0;
  return q ? q->sv : NULL;
}


/* the gutter's decoration of line y: QM_* */
int quick_mark (Doc *d, const char *path, size_t y) {
  QDoc *q = fresh(d, path);
  if (q == NULL || y >= q->nmark) return 0;
  return q->mark[y];
}


/* line i of a copy s (no newline, no \r); NULL: none */
static const char *copy_line (const char *s, size_t slen, const size_t *at, size_t n, int final_nl, size_t i,
                              size_t *len) {
  size_t e;
  if (s == NULL || i >= n) return NULL;
  e = i + 1 < n ? at[i + 1] - 1 : slen;
  if (i + 1 >= n && final_nl && e > at[i]) e--;
  if (e > at[i] && s[e - 1] == '\r') e--;
  *len = e - at[i];
  return s + at[i];
}


/* line i of the index's copy (no newline, no \r); NULL: none */
const char *quick_old (Doc *d, const char *path, size_t i, size_t *len) {
  QDoc *q = entry(d, path, 0);
  return q ? copy_line(q->orig, q->olen, q->oline, q->noline, q->final_nl, i, len) : NULL;
}


/* line i of HEAD's copy; NULL: none */
const char *quick_head (Doc *d, const char *path, size_t i, size_t *len) {
  QDoc *q = entry(d, path, 0);
  return q ? copy_line(q->head, q->hlen, q->hline, q->nhline, q->hfinal_nl, i, len) : NULL;
}


/*
** Git: Stage Change / Stage Selected Ranges: the index gets its copy with
** these changes (h[0..nh), in order) made as d has them; 0 done.
*/
int quick_stage (Doc *d, const char *path, const QHunk *h, size_t nh) {
  QDoc *q = entry(d, path, 1);	/* a command: its copy now */
  Buf b;
  size_t i = 0, k, y, len;
  const char *eol;
  char *rel;
  int r;
  if (q == NULL || q->orig == NULL || nh == 0) return -1;
  eol = q->crlf ? "\r\n" : "\n";
  buf_init(&b);
  for (k = 0; k < nh; k++) {
    for (; i < h[k].o0 && i < q->noline; i++) {
      const char *s = quick_old(d, path, i, &len);
      buf_putn(&b, s, len);
      buf_puts(&b, eol);
    }
    for (y = h[k].n0; y < h[k].n0 + h[k].nn && y < d->n; y++) {
      buf_putn(&b, d->row[y].s, d->row[y].len);
      buf_puts(&b, eol);
    }
    i = h[k].o0 + h[k].on;
  }
  for (; i < q->noline; i++) {
    const char *s = quick_old(d, path, i, &len);
    buf_putn(&b, s, len);
    buf_puts(&b, eol);
  }
  if (!q->final_nl && b.len >= strlen(eol)) b.len -= strlen(eol);	/* no newline at the end, as before */
  rel = git_rel_of(path);
  r = rel ? git_index_put(rel, b.s ? b.s : "", b.len) : -1;
  free(rel);
  buf_free(&b);
  return r;
}


/* the lines of s[0..n): their starts and lengths (the '\n' not in them) */
static size_t split_lines (const char *s, size_t n, size_t **at, size_t **len) {
  size_t i, from = 0, k = 0, cap = 64;
  *at = (size_t *)xmalloc(cap * sizeof(size_t));
  *len = (size_t *)xmalloc(cap * sizeof(size_t));
  for (i = 0; i <= n; i++) {
    if (i < n && s[i] != '\n') continue;
    if (i == n && i == from && k > 0) break;
    if (k == cap) {
      cap *= 2;
      *at = (size_t *)xrealloc(*at, cap * sizeof(size_t));
      *len = (size_t *)xrealloc(*len, cap * sizeof(size_t));
    }
    (*at)[k] = from;
    (*len)[k++] = i - from;
    from = i + 1;
  }
  return k;
}


/*
** Git: Unstage Selected Ranges from the editor: the changes HEAD -> index
** that touch the index's lines lo..hi are undone in the index. 0 done, 1
** nothing there, -1 failed.
*/
int quick_unstage (const char *path, size_t lo, size_t hi) {
  char *rel = git_rel_of(path), *spec, *head, *idx;
  size_t hl = 0, il = 0, nh, k, i = 0, j, nhl, nil, *ha, *hn, *ia, *in;
  QHunk *h;
  int r = 1;
  Buf b;
  if (rel == NULL) return -1;
  spec = xstrcat3("HEAD:", rel, "");
  head = git_blob(spec, &hl);
  free(spec);
  spec = xstrcat3(":", rel, "");
  idx = git_blob(spec, &il);
  free(spec);
  if (idx == NULL) {
    free(rel);
    free(head);
    return -1;
  }
  if (head == NULL) head = xstrdup("");
  h = text_hunks(head, hl, idx, il, &nh);
  nhl = split_lines(head, hl, &ha, &hn);
  nil = split_lines(idx, il, &ia, &in);
  buf_init(&b);
  for (k = 0; k < nh; k++) {
    int undo = h[k].n0 <= hi && h[k].n0 + (h[k].nn ? h[k].nn : 1) > lo;
    for (; i < h[k].n0 && i < nil; i++) {	/* the same lines */
      buf_putn(&b, idx + ia[i], in[i]);
      buf_putc(&b, '\n');
    }
    if (undo) {	/* HEAD's lines instead */
      for (j = h[k].o0; j < h[k].o0 + h[k].on && j < nhl; j++) {
        buf_putn(&b, head + ha[j], hn[j]);
        buf_putc(&b, '\n');
      }
      r = 0;
    }
    else
      for (j = h[k].n0; j < h[k].n0 + h[k].nn && j < nil; j++) {
        buf_putn(&b, idx + ia[j], in[j]);
        buf_putc(&b, '\n');
      }
    i = h[k].n0 + h[k].nn;
  }
  for (; i < nil; i++) {
    buf_putn(&b, idx + ia[i], in[i]);
    buf_putc(&b, '\n');
  }
  if (b.len > 0 && !(il > 0 && idx[il - 1] == '\n')) b.len--;	/* no newline at its end, as before */
  if (r == 0) r = git_index_put(rel, b.s ? b.s : "", b.len);
  buf_free(&b);
  free(ha);
  free(hn);
  free(ia);
  free(in);
  free(h);
  free(head);
  free(idx);
  free(rel);
  return r;
}

/* }================================================================== */


/*
** {==================================================================
** Merge conflicts
** ===================================================================
*/

static struct {
  Doc *d;
  unsigned long edits;
  Conflict *v;
  size_t n, cap;
} CF;


static int marker (const Row *r, char c) {
  size_t i;
  if (r->len < 7) return 0;
  for (i = 0; i < 7; i++)
    if (r->s[i] != c) return 0;
  return r->len == 7 || r->s[7] == ' ' || r->s[7] == '\t' || (c == '=' && r->s[7] == '\r');
}


/* the conflicts of d, each a <<<<<<< ... >>>>>>> block, in order */
const Conflict *conflicts (Doc *d, size_t *n) {
  size_t y;
  if (CF.d != d || CF.edits != d->edits) {
    Conflict c;
    int in = 0;
    CF.d = d;
    CF.edits = d->edits;
    CF.n = 0;
    for (y = 0; y < d->n; y++) {
      const Row *r = &d->row[y];
      if (marker(r, '<')) {
        c.start = y;
        c.base = c.mid = (size_t)-1;
        in = 1;
      }
      else if (in && marker(r, '|') && c.mid == (size_t)-1) c.base = y;
      else if (in && marker(r, '=') && c.mid == (size_t)-1) c.mid = y;
      else if (in && marker(r, '>') && c.mid != (size_t)-1) {
        c.end = y;
        if (CF.n == CF.cap) CF.v = (Conflict *)xrealloc(CF.v, (CF.cap = CF.cap ? CF.cap * 2 : 8) * sizeof(Conflict));
        CF.v[CF.n++] = c;
        in = 0;
      }
    }
  }
  *n = CF.n;
  return CF.v;
}


/* the conflict line y is in (from its <<<<<<< to its >>>>>>>), or -1 */
long conflict_at (Doc *d, size_t y) {
  size_t n, i;
  const Conflict *v = conflicts(d, &n);
  for (i = 0; i < n; i++)
    if (y >= v[i].start && y <= v[i].end) return (long)i;
  return -1;
}

/* }================================================================== */
