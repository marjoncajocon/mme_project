/*
** egitlog.c - git: the Source Control Graph, branches, the git commands, blame
**
** The GRAPH is VS Code's Source Control Graph: the commits of every branch
** (git log --all), newest first, each on a row with its lanes drawn in the
** graph's colors, its branches and tags as badges, its subject, its author
** and when. A commit opens to its files; a file opens the diff editor with
** the commit's change of it. The branch in the status bar opens "Checkout
** to...", and the git commands of VS Code's Source Control "..." menu are
** here too. Blame is the author and the age of the cursor's line, after it
** in dim text and in the status bar (git.blame.* settings).
*/

#include "mme.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/*
** {==================================================================
** Running git
** ===================================================================
*/

/* git with the arguments after out (NULL ends them); err: stderr into out too */
static int gitv (Buf *out, int err, ...) {
  const char *a[48];
  int n = 0;
  va_list ap;
  va_start(ap, err);
  while (n < 47 && (a[n] = va_arg(ap, const char *)) != NULL) n++;
  va_end(ap);
  a[n] = NULL;
  return git_exec(out, err, a);
}


/* git's first line that says something ("fatal: x" -> "x"), as VS Code shows it */
void git_error (const Buf *b, const char *fallback) {
  const char *p = b->s ? b->s : "";
  char line[300];
  size_t n;
  while (*p) {
    const char *e = strchr(p, '\n');
    n = e ? (size_t)(e - p) : strlen(p);
    while (n > 0 && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
    if (n > 0 && strncmp(p, "hint:", 5) != 0) break;
    p = e ? e + 1 : p + strlen(p);	/* the last line: past it, not past its trimmed length (that would not move) */
  }
  if (*p == '\0') {
    toast(1, "%s", fallback);
    return;
  }
  if (strncmp(p, "fatal: ", 7) == 0) p += 7, n -= 7;
  else if (strncmp(p, "error: ", 7) == 0) p += 7, n -= 7;
  if (n >= sizeof(line)) n = sizeof(line) - 1;
  memcpy(line, p, n);
  line[n] = '\0';
  toast(1, "Git: %s", line);
}


/* "3 days ago", like VS Code's fromNow */
void git_ago (long long t, char *out, size_t n) {
  long long d = (long long)time(NULL) - t;
  static const struct {
    long long secs;
    const char *one, *many;
  } unit[] = {
    {31536000, "year", "years"}, {2592000, "month", "months"}, {604800, "week", "weeks"},
    {86400, "day", "days"}, {3600, "hour", "hours"}, {60, "minute", "minutes"}
  };
  size_t i;
  if (d < 60) {
    snprintf(out, n, "now");
    return;
  }
  for (i = 0; i < sizeof(unit) / sizeof(unit[0]); i++)
    if (d >= unit[i].secs) {
      long long k = d / unit[i].secs;
      snprintf(out, n, "%lld %s ago", k, k == 1 ? unit[i].one : unit[i].many);
      return;
    }
}


/* the change is on disk: the open files see it, the views too */
static void changed (void) {
  git_refresh();
  on_disk_changed();
}

/* }================================================================== */


/*
** {==================================================================
** The graph
** ===================================================================
*/

typedef struct GFile {
  char st;	/* 'M' 'A' 'D' 'R' ... */
  char *rel, *old;	/* from the top, '/'; old: a rename's old name */
  char *path;	/* native */
} GFile;

typedef struct Commit {
  char hash[41];
  char **par;	/* the parents' hashes */
  int npar;
  char *refs;	/* git's %D with --decorate=full: "HEAD -> refs/heads/main, tag: refs/tags/v1" */
  char *author, *subject;
  long long time;
  uint32_t *cell;	/* the graph on its row: a character a column */
  unsigned char *ccol;	/* and its lane's color */
  int ncell;
  unsigned char *pass;	/* the lanes that go on under it (their colors, 0xFF none) */
  int npass;
  int open;	/* its files show */
  GFile *file;
  size_t nfile;
  int loaded;
} Commit;

static Commit *g_cm;
static size_t g_ncm, g_capcm;
static int g_limit = 300, g_more;
static char g_sync[64];	/* "1↓ 2↑" against the upstream, "" none */
static int g_upstream;

/* VS Code's scmGraph.foreground1..5 */
static const uint32_t lane_rgb[] = {0xFFB000, 0xDC267F, 0x994F00, 0x40B0A6, 0xB66DFF};
#define NCOLOR	5


static void commit_free (Commit *c) {
  int i;
  size_t f;
  for (i = 0; i < c->npar; i++) free(c->par[i]);
  free(c->par);
  free(c->refs);
  free(c->author);
  free(c->subject);
  free(c->cell);
  free(c->ccol);
  free(c->pass);
  for (f = 0; f < c->nfile; f++) {
    free(c->file[f].rel);
    free(c->file[f].old);
    free(c->file[f].path);
  }
  free(c->file);
}


/* the lanes: each waits for a commit (its hash), with a color */
typedef struct Lanes {
  char **want;
  unsigned char *col;
  int n, cap;
  int next_col;
} Lanes;


static int lane_find (const Lanes *l, const char *h, int not) {
  int i;
  for (i = 0; i < l->n; i++)
    if (i != not && l->want[i] && strcmp(l->want[i], h) == 0) return i;
  return -1;
}


static int lane_new (Lanes *l, int not) {	/* a free lane, or one more */
  int i;
  for (i = 0; i < l->n; i++)
    if (i != not && l->want[i] == NULL) break;
  if (i == l->n) {
    if (l->n == l->cap) {
      l->cap = l->cap ? l->cap * 2 : 16;
      l->want = (char **)xrealloc(l->want, (size_t)l->cap * sizeof(char *));
      l->col = (unsigned char *)xrealloc(l->col, (size_t)l->cap);
    }
    l->n++;
  }
  l->want[i] = NULL;
  l->col[i] = (unsigned char)(l->next_col++ % NCOLOR);
  return i;
}


/* a horizontal line on the row from lane a to lane b, over the lanes between */
static void hline (uint32_t *cell, unsigned char *ccol, const Lanes *l, int a, int b, int col) {
  int lo = a < b ? a : b, hi = a < b ? b : a, k;
  if (hi > 126) hi = 126;	/* the row holds 127 lanes */
  for (k = 2 * lo + 1; k < 2 * hi; k++) {
    if (k % 2 == 0 && (cell[k] == 0x256F || cell[k] == 0x2570)) cell[k] = 0x2534;	/* ┴ two join */
    else if (k % 2 == 0 && l->want[k / 2]) cell[k] = 0x253C;	/* ┼ over a lane */
    else cell[k] = 0x2500;	/* ─ */
    if (k % 2 == 1 || cell[k] == 0x2500) ccol[k] = (unsigned char)col;
  }
}


/*
** Every commit's row of the graph. A lane waits for a commit; the commit
** takes the lane that waits for it (the others that wait for it end there,
** bending into it), and the lane then waits for its first parent; its other
** parents get lanes of their own that bend away from it.
*/
static void graph_lanes (void) {
  Lanes l;
  size_t c;
  memset(&l, 0, sizeof(l));
  for (c = 0; c < g_ncm; c++) {
    Commit *cm = &g_cm[c];
    int i = lane_find(&l, cm->hash, -1), j, p, w;
    uint32_t cell[256];
    unsigned char ccol[256];
    if (i < 0) i = lane_new(&l, -1);	/* a branch's tip */
    w = l.n + cm->npar + 1;
    if (w > 127) w = 127;
    for (j = 0; j < 2 * w; j++) {
      cell[j] = ' ';
      ccol[j] = 0;
    }
    for (j = 0; j < l.n && j < w; j++)	/* the lanes going through */
      if (l.want[j] && j != i) {
        cell[2 * j] = 0x2502;	/* │ */
        ccol[2 * j] = l.col[j];
      }
    for (j = 0; j < l.n && j < w; j++)	/* the others waiting for it end here */
      if (j != i && l.want[j] && strcmp(l.want[j], cm->hash) == 0) {
        free(l.want[j]);
        l.want[j] = NULL;
        if (i >= w) continue;
        hline(cell, ccol, &l, i, j, l.col[j]);
        cell[2 * j] = j > i ? 0x256F : 0x2570;	/* ╯ ╰ */
        ccol[2 * j] = l.col[j];
      }
    free(l.want[i]);
    l.want[i] = cm->npar > 0 ? xstrdup(cm->par[0]) : NULL;
    for (p = 1; p < cm->npar; p++) {	/* a merge: its other parents */
      int m = lane_find(&l, cm->par[p], i);
      if (i >= w) break;
      if (m >= 0 && m < w) {	/* one that is waited for already: into it */
        hline(cell, ccol, &l, i, m, l.col[m]);
        cell[2 * m] = m > i ? 0x2524 : 0x251C;	/* ┤ ├ */
      }
      else if (m < 0) {
        m = lane_new(&l, i);
        if (m >= w) continue;
        l.want[m] = xstrdup(cm->par[p]);
        hline(cell, ccol, &l, i, m, l.col[m]);
        cell[2 * m] = m > i ? 0x256E : 0x256D;	/* ╮ ╭ */
        ccol[2 * m] = l.col[m];
      }
    }
    if (i < w) {
      cell[2 * i] = strstr(cm->refs, "HEAD") == cm->refs ? 0x25C9 : 0x25CF;	/* ◉ HEAD, ● */
      ccol[2 * i] = l.col[i];
    }
    while (l.n > 0 && l.want[l.n - 1] == NULL) l.n--;	/* the empty ones at the end go */
    for (w = 2 * w; w > 0 && cell[w - 1] == ' '; w--) ;
    cm->ncell = w;
    cm->cell = (uint32_t *)xmalloc((size_t)(w + 1) * sizeof(uint32_t));
    cm->ccol = (unsigned char *)xmalloc((size_t)w + 1);
    memcpy(cm->cell, cell, (size_t)w * sizeof(uint32_t));
    memcpy(cm->ccol, ccol, (size_t)w);
    cm->npass = l.n;
    cm->pass = (unsigned char *)xmalloc((size_t)l.n + 1);
    for (j = 0; j < l.n; j++) cm->pass[j] = l.want[j] ? l.col[j] : 0xFF;
  }
  for (c = 0; c < (size_t)l.n; c++) free(l.want[c]);
  free(l.want);
  free(l.col);
}


/* the ahead / behind of the branch against its upstream, for the status bar */
static void load_sync (void) {
  Buf b;
  long behind = 0, ahead = 0;
  g_sync[0] = '\0';
  g_upstream = 0;
  buf_init(&b);
  if (gitv(&b, 0, "rev-list", "--left-right", "--count", "@{upstream}...HEAD", NULL) == 0 && b.s) {
    char *e;
    behind = strtol(b.s, &e, 10);
    ahead = strtol(e, NULL, 10);
    g_upstream = 1;
    if (behind || ahead)
      snprintf(g_sync, sizeof(g_sync), "%ld\xE2\x86\x93 %ld\xE2\x86\x91", behind, ahead);	/* ↓ ↑ */
  }
  buf_free(&b);
}


/* git log --all: the commits, newest first; the ones that were open stay open */
void graph_load (void) {
  Buf b;
  char num[32], **was = NULL;
  size_t i, nwas = 0;
  const char *p;
  for (i = 0; i < g_ncm; i++)
    if (g_cm[i].open) {
      was = (char **)xrealloc(was, (nwas + 1) * sizeof(char *));
      was[nwas++] = xstrdup(g_cm[i].hash);
    }
  for (i = 0; i < g_ncm; i++) commit_free(&g_cm[i]);
  g_ncm = 0;
  g_more = 0;
  g_sync[0] = '\0';
  g_upstream = 0;
  if (git_root() == NULL) {
    for (i = 0; i < nwas; i++) free(was[i]);
    free(was);
    return;
  }
  snprintf(num, sizeof(num), "-n%d", g_limit + 1);
  buf_init(&b);
  if (gitv(&b, 0, "log", "--all", "--date-order", "--decorate=full", num,
           "--format=%H%x1f%P%x1f%D%x1f%an%x1f%at%x1f%s%x1e", NULL) == 0) {
    for (p = b.s ? b.s : ""; *p;) {
      const char *f[6], *e = strchr(p, 0x1e);
      size_t len[6];
      int k;
      Commit *c;
      if (e == NULL) break;
      while (*p == '\n' || *p == '\r') p++;
      for (k = 0; k < 6; k++) {	/* the fields */
        const char *q = memchr(p, k < 5 ? 0x1f : 0x1e, (size_t)(e - p + 1));
        if (q == NULL || q > e) q = e;
        f[k] = p;
        len[k] = (size_t)(q - p);
        p = q < e ? q + 1 : e;
      }
      p = e + 1;
      if (len[0] != 40) continue;
      if (g_ncm == (size_t)g_limit) {
        g_more = 1;
        break;
      }
      if (g_ncm == g_capcm) {
        g_capcm = g_capcm ? g_capcm * 2 : 256;
        g_cm = (Commit *)xrealloc(g_cm, g_capcm * sizeof(Commit));
      }
      c = &g_cm[g_ncm++];
      memset(c, 0, sizeof(*c));
      memcpy(c->hash, f[0], 40);
      c->hash[40] = '\0';
      for (k = 0; (size_t)k + 40 <= len[1];) {	/* "p1 p2" */
        c->par = (char **)xrealloc(c->par, (size_t)(c->npar + 1) * sizeof(char *));
        c->par[c->npar++] = xstrndup(f[1] + k, 40);
        k += 41;
      }
      c->refs = xstrndup(f[2], len[2]);
      c->author = xstrndup(f[3], len[3]);
      c->time = strtoll(f[4], NULL, 10);
      c->subject = xstrndup(f[5], len[5]);
      for (i = 0; i < nwas; i++)
        if (strcmp(was[i], c->hash) == 0) c->open = 1;
    }
  }
  buf_free(&b);
  for (i = 0; i < nwas; i++) free(was[i]);
  free(was);
  graph_lanes();
  load_sync();
}


size_t graph_count (void) {
  return g_ncm;
}


int graph_more (void) {
  return g_more;
}


void graph_load_more (void) {
  g_limit += 300;
  graph_load();
}


const char *graph_hash (size_t i) {
  return i < g_ncm ? g_cm[i].hash : "";
}


int graph_open (size_t i) {
  return i < g_ncm && g_cm[i].open;
}


/* the files a commit changed, against its first parent (git's empty tree for the first) */
static void load_files (Commit *c) {
  Buf b;
  const char *p, *end;
  static const char empty_tree[] = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
  if (c->loaded) return;
  c->loaded = 1;
  buf_init(&b);
  if (gitv(&b, 0, "diff", "--name-status", "-M", "-z", c->npar ? c->par[0] : empty_tree, c->hash, NULL) != 0) {
    buf_free(&b);
    return;
  }
  p = b.s ? b.s : "";
  end = p + b.len;
  while (p < end && *p) {	/* "M\0path\0", "R100\0old\0new\0" */
    GFile *g;
    const char *st = p, *a, *nw;
    a = st + strlen(st) + 1;
    if (a >= end) break;
    nw = a;
    if (st[0] == 'R' || st[0] == 'C') {
      nw = a + strlen(a) + 1;
      if (nw >= end) break;
    }
    c->file = (GFile *)xrealloc(c->file, (c->nfile + 1) * sizeof(GFile));
    g = &c->file[c->nfile++];
    g->st = st[0];
    g->rel = xstrdup(nw);
    g->old = xstrdup(a);
    g->path = path_join(git_root(), nw);
#ifdef _WIN32
    {
      char *q;
      for (q = g->path; *q; q++)
        if (*q == '/') *q = '\\';
    }
#endif
    p = nw + strlen(nw) + 1;
  }
  buf_free(&b);
}


void graph_toggle (size_t i) {
  if (i >= g_ncm) return;
  g_cm[i].open = !g_cm[i].open;
  if (g_cm[i].open) load_files(&g_cm[i]);
}


size_t graph_nfiles (size_t i) {
  if (i >= g_ncm) return 0;
  load_files(&g_cm[i]);
  return g_cm[i].nfile;
}


const char *graph_file_path (size_t i, size_t f) {
  return (i < g_ncm && f < g_cm[i].nfile) ? g_cm[i].file[f].path : "";
}


/* the diff editor: the commit's change of its file f */
int graph_diff (size_t i, size_t f) {
  Commit *c;
  GFile *g;
  char title[512], a[64];
  static const char empty_tree[] = "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
  if (i >= g_ncm || f >= g_cm[i].nfile) return -1;
  c = &g_cm[i];
  g = &c->file[f];
  snprintf(title, sizeof(title), "%s (%.7s^ \xE2\x86\x94 %.7s)", path_basename(g->path), c->hash, c->hash);	/* ↔ */
  snprintf(a, sizeof(a), "%s", c->npar ? c->par[0] : empty_tree);
  return diff_open_rev(g->path, g->rel, g->old, a, c->hash, title);
}


static uint32_t row_bg (int st) {
  if (st == S_SIDE_SEL) return ui_color(C_LIST_SEL_BG);
  if (st == S_SIDE_CUR) return ui_color(C_LIST_CUR_BG);
  return ui_color(C_SIDE_BG);
}


/* a badge: " name " in its color, with an icon */
static int badge (int x, int y, int end, uint32_t icon, const char *name, size_t n, uint32_t bg) {
  uint32_t fg = 0x1E1E1E;
  int x0 = x;
  size_t i = 0;
  if (x + 4 >= end) return 0;
  x += scr_put_rgb(x, y, ' ', fg, bg, 0);
  x += scr_put_rgb(x, y, icon, fg, bg, 0);
  x += scr_put_rgb(x, y, ' ', fg, bg, 0);
  while (i < n && x < end - 1) {
    size_t len;
    uint32_t cp = utf8_decode(name + i, n - i, &len);
    x += scr_put_rgb(x, y, cp, fg, bg, 0);
    i += len;
  }
  x += scr_put_rgb(x, y, ' ', fg, bg, 0);
  return x - x0 + 1;
}


/* the branches and tags of a commit, VS Code's colors */
static int draw_refs (const char *refs, int x, int y, int end) {
  const char *p = refs;
  int x0 = x;
  while (*p && x < end - 4) {
    const char *e = strstr(p, ", ");
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (strncmp(p, "HEAD -> ", 8) == 0) {
      p += 8;
      n -= 8;
    }
    if (n > 11 && strncmp(p, "refs/heads/", 11) == 0)
      x += badge(x, y, end, 0xEA68, p + 11, n - 11, 0x59A4F9);	/* git-branch */
    else if (n > 13 && strncmp(p, "refs/remotes/", 13) == 0) {
      if (!(n >= 5 && strncmp(p + n - 5, "/HEAD", 5) == 0))
        x += badge(x, y, end, 0xEBAA, p + 13, n - 13, 0xB180D7);	/* cloud */
    }
    else if (n > 15 && strncmp(p, "tag: refs/tags/", 15) == 0)
      x += badge(x, y, end, 0xEA66, p + 15, n - 15, 0xE8AB53);	/* tag */
    else if (n == 4 && strncmp(p, "HEAD", 4) == 0)
      x += badge(x, y, end, 0xEAFC, "HEAD", 4, 0x59A4F9);	/* detached: git-commit */
    p += n + (e ? 2 : 0);
  }
  return x - x0;
}


/* a commit's row: the graph, its refs, the subject, then (dim, as VS Code's description) the author and when */
void graph_draw (size_t i, int x, int y, int w, int st) {
  const Commit *c;
  uint32_t bg = row_bg(st);
  int k, cx = x, end = x + w;
  char right[160], ago[48];
  if (i >= g_ncm) return;
  c = &g_cm[i];
  for (k = 0; k < c->ncell && cx < end - 2; k++)
    cx += scr_put_rgb(cx, y, c->cell[k], lane_rgb[c->ccol[k] % NCOLOR], bg, 0);
  cx += 1;
  cx += draw_refs(c->refs, cx, y, cx + (end - cx) / 2);
  cx += scr_putsw(cx, y, end - cx - 1, c->subject, st);
  git_ago(c->time, ago, sizeof(ago));
  snprintf(right, sizeof(right), "%s, %s", c->author, ago);
  if (cx + 6 < end) scr_putsw(cx + 2, y, end - cx - 3, right, st == S_SIDE ? S_SIDE_DIM : st);
}


/* a file of an open commit, under it: the lanes go on beside it */
void graph_draw_file (size_t i, size_t f, int x, int y, int w, int st) {
  const Commit *c;
  const GFile *g;
  uint32_t bg = row_bg(st), icon;
  int k, cx = x, end = x + w, ist, l;
  const char *base;
  if (i >= g_ncm || f >= g_cm[i].nfile) return;
  c = &g_cm[i];
  g = &c->file[f];
  for (k = 0; k < c->npass && cx < end - 4; k++) {
    if (c->pass[k] != 0xFF) scr_put_rgb(cx, y, 0x2502, lane_rgb[c->pass[k] % NCOLOR], bg, 0);
    cx += 2;
  }
  if (cx == x) cx += 2;
  cx += 1;
  base = strrchr(g->rel, '/');
  base = base ? base + 1 : g->rel;
  icon = file_icon(base, &ist);
  cx += scr_put(cx, y, icon, st == S_SIDE ? ist : st) + 1;
  l = g->st == 'R' ? 'R' : g->st;
  cx += scr_putsw(cx, y, end - cx - 3, base, st == S_SIDE ? git_letter_style(l) : st);
  if (base != g->rel && cx + 2 < end - 3) {
    char *dir = xstrndup(g->rel, (size_t)(base - g->rel - 1));
    scr_putsw(cx + 1, y, end - cx - 4, dir, st == S_SIDE ? S_SIDE_DIM : st);
    free(dir);
  }
  scr_put(end - 2, y, (uint32_t)l, st == S_SIDE ? git_letter_style(l) : st);
}


const char *git_sync_text (void) {
  return g_sync;
}


int git_has_upstream (void) {
  return g_upstream;
}

/* }================================================================== */


/*
** {==================================================================
** Branches: Checkout to... and the others
** ===================================================================
*/

typedef struct Ref {
  char *name;	/* "main", "origin/main", "v1.0" */
  int kind;	/* 0 local, 1 remote, 2 tag */
  char *detail;	/* "abc1234 • 3 days ago • subject" */
} Ref;


static void refs_free (Ref *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    free(v[i].name);
    free(v[i].detail);
  }
  free(v);
}


/* the branches and tags, the latest first; kinds: 1 local, 2 remote, 4 tags */
static Ref *refs_load (int kinds, size_t *n) {
  Buf b;
  Ref *v = NULL;
  const char *p;
  *n = 0;
  buf_init(&b);
  if (gitv(&b, 0, "for-each-ref", "--sort=-committerdate",
           "--format=%(refname)%1f%(objectname:short)%1f%(committerdate:unix)%1f%(subject)",
           "refs/heads", "refs/remotes", "refs/tags", NULL) != 0) {
    buf_free(&b);
    return NULL;
  }
  for (p = b.s ? b.s : ""; *p;) {
    const char *e = strchr(p, '\n'), *f[4];
    size_t len = e ? (size_t)(e - p) : strlen(p), fl[4];
    int k, kind;
    const char *q = p;
    char ago[48], det[400];
    for (k = 0; k < 4; k++) {
      const char *s = memchr(q, 0x1f, (size_t)(p + len - q));
      if (s == NULL || k == 3) s = p + len;
      f[k] = q;
      fl[k] = (size_t)(s - q);
      q = s < p + len ? s + 1 : s;
    }
    p += len + (e ? 1 : 0);
    if (fl[0] > 11 && strncmp(f[0], "refs/heads/", 11) == 0) kind = 0;
    else if (fl[0] > 13 && strncmp(f[0], "refs/remotes/", 13) == 0) kind = 1;
    else if (fl[0] > 10 && strncmp(f[0], "refs/tags/", 10) == 0) kind = 2;
    else continue;
    if (!(kinds & (1 << kind))) continue;
    if (kind == 1 && fl[0] >= 5 && strncmp(f[0] + fl[0] - 5, "/HEAD", 5) == 0) continue;
    v = (Ref *)xrealloc(v, (*n + 1) * sizeof(Ref));
    v[*n].kind = kind;
    k = kind == 0 ? 11 : kind == 1 ? 13 : 10;
    v[*n].name = xstrndup(f[0] + k, fl[0] - (size_t)k);
    {
      char *t = xstrndup(f[2], fl[2]);
      git_ago(strtoll(t, NULL, 10), ago, sizeof(ago));
      free(t);
    }
    snprintf(det, sizeof(det), "%.*s \xE2\x80\xA2 %s \xE2\x80\xA2 %.*s", (int)fl[1], f[1], ago, (int)fl[3], f[3]);
    v[*n].detail = xstrdup(det);
    (*n)++;
  }
  buf_free(&b);
  return v;
}


static uint32_t ref_icon (int kind) {
  return kind == 0 ? 0xEA68 : kind == 1 ? 0xEBAA : 0xEA66;	/* git-branch, cloud, tag */
}


/* a ref picked from the list (NULL: Esc); kinds as refs_load; skip: a name not listed */
static char *pick_ref (const char *title, int kinds, const char *skip) {
  size_t n, i, *map, m = 0;
  Ref *v = refs_load(kinds, &n);
  Pick p;
  int r;
  char *out = NULL;
  map = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  pick_init(&p, title);
  for (i = 0; i < n; i++) {
    if (skip && v[i].kind == 0 && strcmp(v[i].name, skip) == 0) continue;
    map[m++] = i;
    pick_add(&p, v[i].name, v[i].detail, (int)ref_icon(v[i].kind));
  }
  if (m == 0) {
    toast(0, "There are no branches to choose from");
    pick_free(&p);
    free(map);
    refs_free(v, n);
    return NULL;
  }
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0 && (size_t)r < m) out = xstrdup(v[map[r]].name);
  free(map);
  refs_free(v, n);
  return out;
}


/* a branch name: VS Code's git.branchWhitespaceChar puts '-' for spaces */
static char *ask_branch (const char *title, const char *init) {
  char *s = ask_text(title, init), *q;
  if (s == NULL) return NULL;
  for (q = s; *q; q++)
    if (*q == ' ' || *q == '\t') *q = '-';
  if (*s == '\0') {
    free(s);
    return NULL;
  }
  return s;
}


/* the result of a git command: its error, or "done" and the views and files anew */
static int report (int r, Buf *b, const char *fallback, const char *done) {
  if (r != 0) git_error(b, fallback);
  else if (done) toast(0, "%s", done);
  buf_free(b);
  changed();
  return r;
}


static void create_branch (int from) {
  char *name, *base = NULL, msg[300];
  Buf b;
  int r;
  if (from && (base = pick_ref("Select a ref to create the branch from", 7, NULL)) == NULL) return;
  name = ask_branch("Please provide a new branch name (Press 'Enter' to confirm or 'Escape' to cancel)", "");
  if (name == NULL) {
    free(base);
    return;
  }
  buf_init(&b);
  if (base) r = gitv(&b, 1, "checkout", "-q", "-b", name, base, NULL);
  else r = gitv(&b, 1, "checkout", "-q", "-b", name, NULL);
  snprintf(msg, sizeof(msg), "Switched to a new branch '%s'", name);
  report(r, &b, "Could not create the branch", msg);
  free(name);
  free(base);
}


/* Git: Checkout to...: VS Code's list, the branches and tags under "Create new branch..." */
static void checkout (void) {
  size_t n, i;
  Ref *v = refs_load(7, &n);
  Pick p;
  int r;
  Buf b;
  char msg[300];
  pick_init(&p, "Select a branch or tag to checkout");
  pick_add(&p, "Create new branch...", NULL, 0xEA60);	/* codicon add */
  pick_add(&p, "Create new branch from...", NULL, 0xEA60);
  pick_add(&p, "Checkout detached...", NULL, 0xEA68);
  for (i = 0; i < n; i++) pick_add(&p, v[i].name, v[i].detail, (int)ref_icon(v[i].kind));
  p.keep_order = 0;
  r = pick_run(&p);
  pick_free(&p);
  if (r == 0 || r == 1) {
    refs_free(v, n);
    create_branch(r == 1);
    return;
  }
  if (r == 2) {
    char *ref = pick_ref("Select a ref to checkout in detached mode", 7, NULL);
    refs_free(v, n);
    if (ref == NULL) return;
    buf_init(&b);
    report(gitv(&b, 1, "checkout", "-q", "--detach", ref, NULL), &b, "Checkout failed", NULL);
    free(ref);
    return;
  }
  if (r < 3 || (size_t)(r - 3) >= n) {
    refs_free(v, n);
    return;
  }
  {
    const Ref *f = &v[r - 3];
    buf_init(&b);
    if (f->kind == 1) {	/* a remote one: a local branch that tracks it, as VS Code does */
      const char *slash = strchr(f->name, '/');
      const char *local = slash ? slash + 1 : f->name;
      Buf t;
      buf_init(&t);
      if (gitv(&t, 0, "rev-parse", "--verify", "-q", local, NULL) == 0)
        r = gitv(&b, 1, "checkout", "-q", local, NULL);
      else r = gitv(&b, 1, "checkout", "-q", "--track", f->name, NULL);
      buf_free(&t);
      snprintf(msg, sizeof(msg), "Switched to branch '%s'", local);
    }
    else {
      r = gitv(&b, 1, "checkout", "-q", f->name, NULL);
      snprintf(msg, sizeof(msg), f->kind == 2 ? "HEAD is now at tag '%s'" : "Switched to branch '%s'", f->name);
    }
    report(r, &b, "Checkout failed", msg);
  }
  refs_free(v, n);
}


static void delete_branch (void) {
  char *name = pick_ref("Select a branch to delete", 1, git_branch()), msg[300];
  Buf b;
  int r;
  if (name == NULL) return;
  buf_init(&b);
  r = gitv(&b, 1, "branch", "-d", name, NULL);
  if (r != 0 && b.s && strstr(b.s, "not fully merged")) {	/* VS Code asks */
    static const char *const bt[] = {"Delete Branch", "Cancel"};
    char q[300];
    snprintf(q, sizeof(q), "The branch '%s' is not fully merged. Delete anyway?", name);
    if (dialog(q, NULL, bt, 2) == 0) {
      buf_free(&b);
      buf_init(&b);
      r = gitv(&b, 1, "branch", "-D", name, NULL);
    }
    else {
      buf_free(&b);
      free(name);
      return;
    }
  }
  snprintf(msg, sizeof(msg), "Deleted branch '%s'", name);
  report(r, &b, "Could not delete the branch", msg);
  free(name);
}


static void rename_branch (void) {
  char *name = ask_branch("Please provide a new branch name", git_branch()), msg[300];
  Buf b;
  if (name == NULL) return;
  buf_init(&b);
  snprintf(msg, sizeof(msg), "Renamed the branch to '%s'", name);
  report(gitv(&b, 1, "branch", "-m", name, NULL), &b, "Could not rename the branch", msg);
  free(name);
}


static void merge_branch (void) {
  char *name = pick_ref("Select a branch to merge from", 3, git_branch());
  Buf b;
  int r;
  if (name == NULL) return;
  buf_init(&b);
  r = gitv(&b, 1, "merge", name, NULL);
  if (r != 0 && b.s && strstr(b.s, "CONFLICT")) {
    toast(1, "Git: There are merge conflicts. Resolve them before committing.");
    buf_free(&b);
    changed();
  }
  else report(r, &b, "Merge failed", "Merged");
  free(name);
}


/* a long git command: its notice shows at once, the screen waits for it */
static int slow (Buf *b, const char *what, const char *a0, const char *a1, const char *a2, const char *a3) {
  toast(0, "%s", what);
  toast_draw();
  scr_flush();
  return gitv(b, 1, a0, a1, a2, a3, NULL);
}


/* git pull; merging (VS Code's git.rebaseWhenSync off) when pull.rebase is not set */
static int pull (Buf *b, const char *what) {
  Buf c;
  int set;
  buf_init(&c);
  set = gitv(&c, 0, "config", "--get", "pull.rebase", NULL) == 0;
  buf_free(&c);
  return slow(b, what, "pull", set ? NULL : "--no-rebase", NULL, NULL);
}


static int push (void) {
  Buf b;
  int r;
  buf_init(&b);
  if (git_has_upstream()) r = slow(&b, "Pushing...", "push", NULL, NULL, NULL);
  else {	/* VS Code: publish the branch to the remote */
    Buf rm;
    char remote[128] = "origin";
    buf_init(&rm);
    if (gitv(&rm, 0, "remote", NULL) == 0 && rm.s && rm.s[0]) {
      size_t n = strcspn(rm.s, "\r\n");
      if (n < sizeof(remote)) {
        memcpy(remote, rm.s, n);
        remote[n] = '\0';
      }
    }
    else {
      buf_free(&rm);
      buf_free(&b);
      toast(1, "Git: Your repository has no remotes configured to push to.");
      return -1;
    }
    buf_free(&rm);
    r = slow(&b, "Publishing branch...", "push", "-u", remote, git_branch());
  }
  return report(r, &b, "Push failed", "Pushed");
}


static void stash_apply (int pop) {
  Buf b, l;
  Pick p;
  const char *s;
  int r, n = 0;
  char ref[40];
  buf_init(&l);
  gitv(&l, 0, "stash", "list", "--format=%gd%x1f%s", NULL);
  pick_init(&p, pop ? "Choose a stash to pop" : "Choose a stash to apply");
  for (s = l.s ? l.s : ""; *s;) {
    const char *e = strchr(s, '\n'), *u;
    size_t len = e ? (size_t)(e - s) : strlen(s);
    char *line = xstrndup(s, len);
    if ((u = strchr(line, 0x1f)) != NULL) {
      char *name = xstrndup(line, (size_t)(u - line));
      pick_add(&p, u + 1, name, 0xEA7B);	/* codicon archive... */
      free(name);
      n++;
    }
    free(line);
    s += len + (e ? 1 : 0);
  }
  buf_free(&l);
  if (n == 0) {
    pick_free(&p);
    toast(0, "There are no stashes in the repository.");
    return;
  }
  p.keep_order = 1;
  r = pick_run(&p);
  if (r >= 0) snprintf(ref, sizeof(ref), "%s", p.item[r].detail);
  pick_free(&p);
  if (r < 0) return;
  buf_init(&b);
  report(gitv(&b, 1, "stash", pop ? "pop" : "apply", ref, NULL), &b, "Could not apply the stash",
         pop ? "Popped the stash" : "Applied the stash");
}


/* Git: View File History: the file's commits; one opens its change */
static void file_history (const char *path, SideAct *act) {
  Buf b;
  const char *top = git_root(), *s;
  char *rel;
  size_t tl, n = 0, i;
  Pick p;
  int r;
  char **hash = NULL, **parent = NULL;
  if (top == NULL || path == NULL) {
    toast(0, "Open a file of the repository to see its history");
    return;
  }
  tl = strlen(top);
  if (m_fnncmp(path, top, tl) != 0 || !path_is_sep(path[tl])) {
    toast(0, "The file is not in the repository");
    return;
  }
  rel = xstrdup(path + tl + 1);
  {
    char *q;
    for (q = rel; *q; q++)
      if (*q == '\\') *q = '/';
  }
  buf_init(&b);
  gitv(&b, 0, "log", "--follow", "-n500", "--format=%H%x1f%P%x1f%an%x1f%at%x1f%s", "--", rel, NULL);
  pick_init(&p, "Select a commit to see its changes of the file");
  for (s = b.s ? b.s : ""; *s;) {
    const char *e = strchr(s, '\n');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    char *line = xstrndup(s, len), *f[5], *q = line, det[300], ago[48];
    int k;
    s += len + (e ? 1 : 0);
    for (k = 0; k < 5; k++) {
      f[k] = q;
      q = strchr(q, 0x1f);
      if (q == NULL) break;
      *q++ = '\0';
    }
    if (k < 4 || strlen(f[0]) != 40) {
      free(line);
      continue;
    }
    hash = (char **)xrealloc(hash, (n + 1) * sizeof(char *));
    parent = (char **)xrealloc(parent, (n + 1) * sizeof(char *));
    hash[n] = xstrdup(f[0]);
    parent[n] = strlen(f[1]) >= 40 ? xstrndup(f[1], 40) : xstrdup("4b825dc642cb6eb9a060e54bf8d69288fbee4904");
    git_ago(strtoll(f[3], NULL, 10), ago, sizeof(ago));
    snprintf(det, sizeof(det), "%.7s \xE2\x80\xA2 %s, %s", f[0], f[2], ago);
    pick_add(&p, f[4], det, 0xEAFC);	/* git-commit */
    n++;
    free(line);
  }
  buf_free(&b);
  if (n == 0) toast(0, "No history for %s", path_basename(path));
  else {
    p.keep_order = 1;
    r = pick_run(&p);
    if (r >= 0) {
      char title[512];
      snprintf(title, sizeof(title), "%s (%.7s^ \xE2\x86\x94 %.7s)", path_basename(path), hash[r], hash[r]);
      if (diff_open_rev(path, rel, NULL, parent[r], hash[r], title) == 0) {
        act->what = SA_SHOW_DIFF;
        act->go = 1;
      }
    }
  }
  pick_free(&p);
  for (i = 0; i < n; i++) {
    free(hash[i]);
    free(parent[i]);
  }
  free(hash);
  free(parent);
  free(rel);
}


/*
** {==================================================================
** Worktrees, VS Code's: Create Worktree (a branch checked out in a
** folder of its own, <repo>.worktrees/<branch> by default), Open Worktree
** (the folder is opened here), Delete Worktree
** ===================================================================
*/

typedef struct Wt {
  char *path;	/* native */
  char *branch;	/* "main"; NULL: detached */
  char head[12];
} Wt;


/* git worktree list --porcelain: the first one is the main worktree */
static Wt *wt_load (size_t *n) {
  Buf b;
  Wt *v = NULL;
  const char *p;
  size_t cap = 0;
  *n = 0;
  buf_init(&b);
  if (gitv(&b, 0, "worktree", "list", "--porcelain", NULL) != 0 || b.s == NULL) {
    buf_free(&b);
    return NULL;
  }
  for (p = b.s; *p;) {
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len > 0 && p[len - 1] == '\r') len--;
    if (len > 9 && strncmp(p, "worktree ", 9) == 0) {
      char *c;
      if (*n == cap) v = (Wt *)xrealloc(v, (cap = cap ? cap * 2 : 8) * sizeof(Wt));
      memset(&v[*n], 0, sizeof(Wt));
      v[*n].path = xstrndup(p + 9, len - 9);
#ifdef _WIN32
      for (c = v[*n].path; *c; c++)
        if (*c == '/') *c = '\\';
#else
      (void)c;
#endif
      (*n)++;
    }
    else if (*n && len > 5 && strncmp(p, "HEAD ", 5) == 0) snprintf(v[*n - 1].head, sizeof(v[0].head), "%.7s", p + 5);
    else if (*n && len > 18 && strncmp(p, "branch refs/heads/", 18) == 0) v[*n - 1].branch = xstrndup(p + 18, len - 18);
    p += len;
    while (*p == '\r' || *p == '\n') p++;
  }
  buf_free(&b);
  return v;
}


static void wt_free (Wt *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) {
    free(v[i].path);
    free(v[i].branch);
  }
  free(v);
}


/* a worktree picked (skip: the main one, and the one open, are not offered); NULL: Esc */
static char *wt_pick (const char *title, int skip) {
  size_t n, i, *map, m = 0;
  Wt *v = wt_load(&n);
  const char *top = git_root();
  Pick p;
  int r;
  char *out = NULL;
  if (v == NULL) {
    toast(1, "Git: could not list the worktrees");
    return NULL;
  }
  map = (size_t *)xmalloc((n + 1) * sizeof(size_t));
  pick_init(&p, title);
  for (i = 0; i < n; i++) {
    char d[400];
    int here = top && m_fncmp(v[i].path, top) == 0;
    if (skip && (i == 0 || here)) continue;
    snprintf(d, sizeof(d), "%s%s  %s", v[i].branch ? v[i].branch : v[i].head, here ? " (current)" : i == 0 ? " (main)" : "",
             v[i].path);
    pick_add(&p, path_basename(v[i].path), d, 0xF0E7);	/* list-tree */
    map[m++] = i;
  }
  if (m == 0) toast(0, skip ? "There are no other worktrees." : "There are no worktrees.");
  else if ((r = pick_run(&p)) >= 0) out = xstrdup(v[map[r]].path);
  pick_free(&p);
  free(map);
  wt_free(v, n);
  return out;
}


static char *g_wt_open;	/* the folder SA_OPEN_FOLDER opens */

/* the folder is opened, here: VS Code's "Open Worktree in Current Window" */
static void wt_open (const char *path, SideAct *act) {
  free(g_wt_open);
  g_wt_open = xstrdup(path);
  act->what = SA_OPEN_FOLDER;
  act->path = g_wt_open;
}


/* Git: Create Worktree...: a branch (or a new one) checked out in a new folder */
static void wt_create (SideAct *act) {
  size_t n, i;
  Ref *v = refs_load(3, &n);	/* the branches, local and remote */
  Pick p;
  int r;
  char *branch = NULL, *path, *def, *parent, *wts, *leaf, *c, msg[600];
  const char *top = git_root();
  int fresh = 0, remote = 0;
  Buf b;
  pick_init(&p, "Select a branch to create the new worktree from");
  pick_add(&p, "Create new branch...", "from HEAD", 0xEA60);	/* codicon add */
  for (i = 0; i < n; i++) pick_add(&p, v[i].name, v[i].detail, (int)ref_icon(v[i].kind));
  r = pick_run(&p);
  pick_free(&p);
  if (r == 0) {
    branch = ask_branch("Please provide a new branch name for the worktree (Press 'Enter' to confirm or 'Escape' to cancel)", "");
    fresh = 1;
  }
  else if (r > 0) {
    branch = xstrdup(v[r - 1].name);
    remote = v[r - 1].kind == 1;
  }
  refs_free(v, n);
  if (branch == NULL || !branch[0]) {
    free(branch);
    return;
  }
  parent = path_dirname(top);	/* VS Code's default: next to the repository, <repo>.worktrees/<branch> */
  leaf = xstrcat3(path_basename(top), ".worktrees", "");
  wts = path_join(parent, leaf);
  def = xstrdup(branch);
  for (c = def; *c; c++)
    if (*c == '/' || *c == '\\' || *c == ':') *c = '-';
  {
    char *d = path_join(wts, def);
    free(def);
    def = d;
  }
  path = ask_text("Worktree location (Press 'Enter' to confirm or 'Escape' to cancel)", def);
  free(def);
  free(wts);
  free(leaf);
  free(parent);
  if (path == NULL || !path[0]) {
    free(path);
    free(branch);
    return;
  }
  buf_init(&b);
  if (fresh) r = gitv(&b, 1, "worktree", "add", "-b", branch, path, NULL);
  else {
    const char *local = remote && strchr(branch, '/') ? strchr(branch, '/') + 1 : branch;	/* origin/x: a local x tracking it */
    if (local != branch) r = gitv(&b, 1, "worktree", "add", "--track", "-b", local, path, branch, NULL);
    else r = gitv(&b, 1, "worktree", "add", path, branch, NULL);
  }
  if (r != 0) {
    git_error(&b, "Could not create the worktree");
    buf_free(&b);
  }
  else {
    static const char *const bt[] = {"Open", "Not Now"};
    buf_free(&b);
    changed();
    snprintf(msg, sizeof(msg), "The worktree for '%s' was created in %s.", branch, path);
    if (dialog(msg, "Open it in this window?", bt, 2) == 0) wt_open(path, act);
  }
  free(path);
  free(branch);
}


/* Git: Delete Worktree...: git worktree remove, and --force when it has changes and that is asked */
static void wt_delete (void) {
  static const char *const bt[] = {"Delete", "Cancel"}, *const force[] = {"Force Delete", "Cancel"};
  char *path = wt_pick("Select a worktree to delete", 1), msg[600];
  Buf b;
  if (path == NULL) return;
  snprintf(msg, sizeof(msg), "Are you sure you want to delete the worktree %s?", path);
  if (dialog(msg, "Its folder is deleted; its branch stays.", bt, 2) != 0) {
    free(path);
    return;
  }
  buf_init(&b);
  if (gitv(&b, 1, "worktree", "remove", path, NULL) != 0) {
    if (b.s && (strstr(b.s, "modified or untracked") || strstr(b.s, "--force")) &&
        dialog("The worktree has changes that are not committed.", "Delete it with them?", force, 2) == 0) {
      buf_free(&b);
      buf_init(&b);
      report(gitv(&b, 1, "worktree", "remove", "--force", path, NULL), &b, "Could not delete the worktree", "Deleted the worktree");
    }
    else report(1, &b, "Could not delete the worktree", NULL);
  }
  else report(0, &b, NULL, "Deleted the worktree");
  free(path);
}

/* }================================================================== */


/* the "..." of the Source Control view: VS Code's More Actions */
static void more_actions (SideAct *act) {
  static const int cmds[] = {
    CMD_GIT_PULL, CMD_GIT_PUSH, CMD_GIT_SYNC, CMD_GIT_FETCH, CMD_GIT_COMMIT, CMD_GIT_AMEND,
    CMD_GIT_UNDO_COMMIT, CMD_GIT_CHECKOUT, CMD_GIT_BRANCH, CMD_GIT_BRANCH_FROM, CMD_GIT_RENAME_BRANCH,
    CMD_GIT_DELETE_BRANCH, CMD_GIT_MERGE, CMD_GIT_STASH, CMD_GIT_STASH_POP, CMD_GIT_STASH_APPLY,
    CMD_GIT_WT_CREATE, CMD_GIT_WT_OPEN, CMD_GIT_WT_DELETE,
    CMD_GIT_FILE_HISTORY, CMD_GIT_BLAME, CMD_GIT_REFRESH, CMD_GIT_PUBLISH, CMD_GIT_CLONE,
    CMD_STAGE_RANGES, CMD_UNSTAGE_RANGES, CMD_REVERT_RANGES
  };
  Pick p;
  size_t i;
  int r;
  pick_init(&p, "Source Control: More Actions");
  for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    const char *name = cmd_name(cmds[i]);
    pick_add(&p, strncmp(name, "Git: ", 5) == 0 ? name + 5 : name, cmd_keys(cmds[i]), 0);
  }
  p.keep_order = 1;
  r = pick_run(&p);
  pick_free(&p);
  if (r >= 0) {
    act->what = SA_CMD;
    act->cmd = cmds[r];
  }
}


/* Git: Initialize Repository: git init in the folder open */
void git_init_repo (void) {
  Buf b;
  if (git_root()) {
    toast(0, "The folder is already a git repository.");
    return;
  }
  buf_init(&b);
  if (gitv(&b, 1, "init", NULL) != 0) git_error(&b, "git init failed");
  else toast(0, "Initialized a git repository in %s", path_basename(side_root()));
  buf_free(&b);
  git_refresh();
}


/* Git: Publish Branch: the branch pushed to the remote, its upstream set */
void git_publish (void) {
  if (git_root() == NULL) {
    toast(1, "The folder currently open doesn't have a git repository.");
    return;
  }
  push();
  changed();
}


/*
** Git: Clone: the repository's URL, the folder to put it in; git clone
** there. The folder of the clone (to open, when the user says so), or NULL.
*/
char *git_clone (void) {
  char *url, *parent, *def, *name, *dest, *e;
  const char *base;
  size_t n;
  Buf b;
  int r;
  static const char *const bt[] = {"Open", "Cancel"};
  url = ask_text("Provide repository URL (Press 'Enter' to confirm or 'Escape' to cancel)", "");
  if (url == NULL || url[0] == '\0') {
    free(url);
    return NULL;
  }
  for (e = url + strlen(url); e > url && (e[-1] == ' ' || e[-1] == '/' || e[-1] == '\\'); e--) ;
  *e = '\0';
  def = path_dirname(side_root());
  parent = ask_text("Choose a folder to clone the repository into", def ? def : "");
  free(def);
  if (parent == NULL || parent[0] == '\0') {
    free(url);
    free(parent);
    return NULL;
  }
  base = url + strlen(url);	/* "https://x/y/name.git" -> "name" */
  while (base > url && base[-1] != '/' && base[-1] != '\\' && base[-1] != ':') base--;
  n = strlen(base);
  if (n > 4 && strcmp(base + n - 4, ".git") == 0) n -= 4;
  name = xstrndup(base, n);
  dest = path_join(parent, name[0] ? name : "repository");
  buf_init(&b);
  toast(0, "Cloning git repository '%s'...", url);
  toast_draw();
  scr_flush();
  r = gitv(&b, 1, "clone", "--progress", url, dest, NULL);
  if (r != 0) {
    git_error(&b, "git clone failed");
    free(dest);
    dest = NULL;
  }
  else if (dialog("Would you like to open the cloned repository?", dest, bt, 2) != 0) {
    toast(0, "Cloned into %s", dest);
    free(dest);
    dest = NULL;
  }
  buf_free(&b);
  free(name);
  free(parent);
  free(url);
  return dest;
}


/* a Git: command of the palette; path: the file in front (NULL none); act: a diff to show */
void git_command (int cmd, const char *path, SideAct *act) {
  Buf b;
  act->what = SA_NONE;
  if (cmd == CMD_GIT_BLAME) {
    opt.blame_line = !opt.blame_line;
    settings_put_raw("git.blame.editorDecoration.enabled", opt.blame_line ? "true" : "false");
    return;
  }
  if (cmd == CMD_GIT_DIFF_INLINE) {
    diff_toggle_inline();
    return;
  }
  if (cmd == CMD_GIT_REFRESH) {
    changed();
    return;
  }
  if (git_root() == NULL) {
    toast(1, "The folder currently open doesn't have a git repository.");
    return;
  }
  buf_init(&b);
  switch (cmd) {
    case CMD_GIT_CHECKOUT: checkout(); break;
    case CMD_GIT_BRANCH: create_branch(0); break;
    case CMD_GIT_BRANCH_FROM: create_branch(1); break;
    case CMD_GIT_DELETE_BRANCH: delete_branch(); break;
    case CMD_GIT_RENAME_BRANCH: rename_branch(); break;
    case CMD_GIT_MERGE: merge_branch(); break;
    case CMD_GIT_PULL: report(pull(&b, "Pulling..."), &b, "Pull failed", "Pulled"); return;
    case CMD_GIT_FETCH:
      report(slow(&b, "Fetching...", "fetch", "--all", NULL, NULL), &b, "Fetch failed", "Fetched");
      return;
    case CMD_GIT_PUSH: push(); break;
    case CMD_GIT_SYNC:
      if (report(pull(&b, "Syncing (pull)..."), &b, "Pull failed", NULL) == 0) push();
      return;
    case CMD_GIT_STASH: {
      Buf s;
      char *msg;
      buf_init(&s);
      gitv(&s, 0, "status", "--porcelain", "-uno", NULL);
      if (s.len == 0) {
        buf_free(&s);
        toast(0, "There are no changes to stash.");
        break;
      }
      buf_free(&s);
      msg = ask_text("Stash message (Press 'Enter' to confirm or 'Escape' to cancel)", "");
      if (msg == NULL) break;
      if (msg[0]) report(gitv(&b, 1, "stash", "push", "-m", msg, NULL), &b, "Could not stash", "Stashed the changes");
      else report(gitv(&b, 1, "stash", "push", NULL), &b, "Could not stash", "Stashed the changes");
      free(msg);
      return;
    }
    case CMD_GIT_STASH_POP:
      report(gitv(&b, 1, "stash", "pop", NULL), &b, "Could not pop the stash", "Popped the latest stash");
      return;
    case CMD_GIT_STASH_APPLY: stash_apply(0); break;
    case CMD_GIT_UNDO_COMMIT: {	/* VS Code: git reset --soft HEAD~1, the changes stay staged */
      Buf t;
      int first;
      buf_init(&t);
      first = gitv(&t, 0, "rev-parse", "-q", "--verify", "HEAD~1", NULL) != 0;
      buf_free(&t);
      if (first) report(gitv(&b, 1, "update-ref", "-d", "HEAD", NULL), &b, "Could not undo", "Undid the last commit");
      else report(gitv(&b, 1, "reset", "--soft", "HEAD~1", NULL), &b, "Could not undo", "Undid the last commit");
      return;
    }
    case CMD_GIT_COMMIT: if (!git_commit(0)) { act->what = SA_FOCUS_SCM; } break;
    case CMD_GIT_AMEND: git_commit(1); on_disk_changed(); break;
    case CMD_GIT_FILE_HISTORY: file_history(path, act); break;
    case CMD_GIT_MORE: more_actions(act); break;
    case CMD_GIT_WT_CREATE: wt_create(act); break;
    case CMD_GIT_WT_OPEN: {
      char *w = wt_pick("Select a worktree to open", 0);
      if (w) wt_open(w, act);
      free(w);
      break;
    }
    case CMD_GIT_WT_DELETE: wt_delete(); break;
  }
  buf_free(&b);
}

/* }================================================================== */


/*
** {==================================================================
** Blame
** ===================================================================
*/

typedef struct BCommit {
  char hash[41];
  char *author, *summary;
  long long time;
} BCommit;

typedef struct Blame {
  char *path;	/* the file it is for; NULL: the slot is free */
  BCommit *c;
  size_t nc;
  size_t *line;	/* each line's commit */
  size_t nline;
} Blame;

/*
** git blame takes a tenth of a second and more, so it must not run while a
** tab is being drawn: git_blame says what it knows and remembers what it
** was asked for, and git_blame_idle runs git once the editor is quiet.
** The files looked at lately are kept, so going back to a tab is free.
*/
#define NBLAME	8
/* blame is only asked for files under a megabyte: no line of one is past this */
#define MAX_BLAME_LINES	((size_t)1 << 20)

static Blame g_bl[NBLAME];
static int g_bl_next;
static char *g_bl_want;	/* asked for and not read yet */


static void blame_free (Blame *b) {
  size_t i;
  for (i = 0; i < b->nc; i++) {
    free(b->c[i].author);
    free(b->c[i].summary);
  }
  free(b->c);
  free(b->line);
  free(b->path);
  memset(b, 0, sizeof(*b));
}


void blame_clear (void) {
  int i;
  for (i = 0; i < NBLAME; i++) blame_free(&g_bl[i]);
  free(g_bl_want);
  g_bl_want = NULL;
}


static Blame *blame_of (const char *path) {
  int i;
  for (i = 0; i < NBLAME; i++)
    if (g_bl[i].path && m_fncmp(g_bl[i].path, path) == 0) return &g_bl[i];
  return NULL;
}


/* git blame --porcelain: every line's commit, and the commits' authors */
/* "41a4118... 1 1 89": a blame record starts with a commit's 40 hex digits */
static int is_hash40 (const char *p) {
  int i;
  for (i = 0; i < 40; i++)
    if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f'))) return 0;
  return 1;
}


static void blame_load (const char *path) {
  const char *top = git_root(), *p;
  size_t tl;
  char *rel;
  Buf b;
  OsStat st;
  Blame *BLp = &g_bl[g_bl_next];
  g_bl_next = (g_bl_next + 1) % NBLAME;
  blame_free(BLp);
  BLp->path = xstrdup(path);
  if (top == NULL) return;
  if (os_stat(path, &st) == 0 && st.size > (1 << 20)) return;	/* a big file: git blame would take seconds, each save */
  tl = strlen(top);
  if (m_fnncmp(path, top, tl) != 0 || !path_is_sep(path[tl])) return;
  rel = xstrdup(path + tl + 1);
  buf_init(&b);
  if (gitv(&b, 0, "blame", "--porcelain", "--", rel, NULL) != 0) {
    free(rel);
    buf_free(&b);
    return;
  }
  free(rel);
  for (p = b.s ? b.s : ""; *p;) {	/* "<hash> <orig> <final> [<n>]", its keys, "\t<line>" */
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (len > 41 && p[40] == ' ' && is_hash40(p)) {	/* a line of the file can look like one: check it */
      /* the final line number is the third field of this line, not of a later one */
      const char *sp = (const char *)memchr(p + 41, ' ', len - 41);
      size_t k, fin = (size_t)strtoul(sp ? sp + 1 : p + 41, NULL, 10);
      if (fin > MAX_BLAME_LINES) fin = 0;	/* git said something no file that size could say */
      for (k = 0; k < BLp->nc; k++)
        if (memcmp(BLp->c[k].hash, p, 40) == 0) break;
      if (k == BLp->nc) {
        BLp->c = (BCommit *)xrealloc(BLp->c, (BLp->nc + 1) * sizeof(BCommit));
        memset(&BLp->c[BLp->nc], 0, sizeof(BCommit));
        memcpy(BLp->c[BLp->nc].hash, p, 40);
        BLp->nc++;
      }
      if (fin > 0) {
        if (fin > BLp->nline) {
          BLp->line = (size_t *)xrealloc(BLp->line, fin * sizeof(size_t) * 2);
          while (BLp->nline < fin * 2) BLp->line[BLp->nline++] = (size_t)-1;
        }
        BLp->line[fin - 1] = k;
      }
      p += len + (e ? 1 : 0);
      while (*p && *p != '\t') {	/* its keys, until the line itself */
        const char *e2 = strchr(p, '\n');
        size_t l2 = e2 ? (size_t)(e2 - p) : strlen(p);
        BCommit *c = &BLp->c[k];
        if (l2 > 7 && strncmp(p, "author ", 7) == 0 && c->author == NULL) c->author = xstrndup(p + 7, l2 - 7);
        else if (l2 > 12 && strncmp(p, "author-time ", 12) == 0) c->time = strtoll(p + 12, NULL, 10);
        else if (l2 > 8 && strncmp(p, "summary ", 8) == 0 && c->summary == NULL) c->summary = xstrndup(p + 8, l2 - 8);
        p += l2 + (e2 ? 1 : 0);
      }
      if (*p == '\t') {	/* the line of the file itself: not a record */
        const char *e2 = strchr(p, '\n');
        p += e2 ? (size_t)(e2 - p) + 1 : strlen(p);
      }
      continue;
    }
    p += len + (e ? 1 : 0);
  }
  buf_free(&b);
}


/*
** VS Code's blame of line y (from 0) of path: "Author, 3 days ago • subject"
** (short: "Author, 3 days ago", for the status bar); NULL: none
*/
const char *git_blame (const char *path, size_t y, int short_form) {
  static char out[400];
  const BCommit *c;
  const Blame *b;
  char ago[48];
  if (path == NULL || git_root() == NULL) return NULL;
  if ((b = blame_of(path)) == NULL) {	/* git_blame_idle reads it: the frame is not kept waiting */
    if (g_bl_want == NULL || m_fncmp(g_bl_want, path) != 0) {
      free(g_bl_want);
      g_bl_want = xstrdup(path);
    }
    return NULL;
  }
  if (y >= b->nline || b->line[y] == (size_t)-1) return NULL;
  c = &b->c[b->line[y]];
  if (strncmp(c->hash, "0000000000", 10) == 0) {
    snprintf(out, sizeof(out), short_form ? "You, Uncommitted" : "You \xE2\x80\xA2 Uncommitted changes");
    return out;
  }
  git_ago(c->time, ago, sizeof(ago));
  if (short_form) snprintf(out, sizeof(out), "%s, %s", c->author ? c->author : "?", ago);
  else snprintf(out, sizeof(out), "%s, %s \xE2\x80\xA2 %s", c->author ? c->author : "?", ago,
                c->summary ? c->summary : "");
  return out;
}


/* the editor is quiet: git blame of the file asked for; 1 when it ran */
int git_blame_idle (void) {
  char *path = g_bl_want;
  if (path == NULL) return 0;
  g_bl_want = NULL;
  if (blame_of(path) == NULL) blame_load(path);
  free(path);
  return 1;
}

/* }================================================================== */
