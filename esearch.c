/*
** esearch.c - the Search view: find and replace in files, like VS Code's
**
** What is typed is looked for in every file of the folder a moment after
** the typing stops. The results are grouped by file; a file's row folds
** its matches. Binary files, very big files, folders like .git and
** node_modules (VS Code's search.exclude) and what .gitignore names are
** skipped. The chevron on the left opens the replace box; "..." opens the
** "files to include" and "files to exclude" boxes (globs, comma separated:
** src, *.c, ./lib, test*).
** A file open in the editor is searched and replaced in its text there.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct SFile {
  char *path;	/* native */
  char *rel;	/* from the folder, for the dim text */
  size_t first, n;	/* its hits */
  int open;
} SFile;

typedef struct SHit {
  size_t file;
  size_t line;	/* from 1 */
  size_t col, len;	/* bytes in the line */
  char *text;	/* the line, cut to a sane length */
  size_t tlen;
} SHit;

enum { FD_FIND, FD_REPL, FD_INC, FD_EXC, FD_N };	/* the input boxes */

static char g_field[FD_N][256];	/* what each box has */
static int g_in;	/* the box the keys go to */
static int g_repl;	/* the replace box shows */
static int g_more;	/* "files to include / exclude" show */
static int g_case, g_word, g_regex;	/* Aa, ab, .* */
static Regex *g_re;	/* g_ran as a regular expression */
static int g_pending;	/* typed since the last search */
static long long g_changed;	/* when */
static char g_ran[256];	/* what the results are for */
static char *g_ignore;	/* .gitignore's patterns, comma separated */

static SFile *g_file;
static size_t g_nfile, g_capfile;
static SHit *g_hit;
static size_t g_nhit, g_caphit;
static int g_cut;	/* stopped at MAX_HITS */

static size_t *g_row;	/* a row: file i as 2i, hit j as 2j+1 */
static size_t g_nrow, g_caprow;
static size_t g_sel, g_top;
static int g_h = 1;

/* where things were drawn, for the mouse (rows from the view's top, columns from its left) */
static struct {
  int w;
  int find, repl, dots, inc, exc, sum, head;	/* rows; -1 not shown */
  int refresh, clear, collapse;	/* the title's icons */
  int chev, box_x, box_w;
} V;

#define DELAY		300000	/* us after the last key */
#define MAX_HITS	10000
#define MAX_FILE	(8 << 20)
#define MAX_TEXT	400

#define Q	(g_field[FD_FIND])
#define R	(g_field[FD_REPL])


/*
** {==================================================================
** Globs
** ===================================================================
*/

static int lower (int c) {
  return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}


/* p against s ('/' between folders): * not over '/', ** over anything, ? one, {a,b} */
static int glob (const char *p, const char *s) {
  while (*p) {
    if (p[0] == '*' && p[1] == '*') {
      p += 2;
      if (*p == '/') p++;	/* "**" and "**" "/": no folder too */
      if (*p == '\0') return 1;
      for (;; s++) {
        if (glob(p, s)) return 1;
        if (*s == '\0') return 0;
      }
    }
    if (*p == '*') {
      p++;
      for (;; s++) {
        if (glob(p, s)) return 1;
        if (*s == '\0' || *s == '/') return 0;
      }
    }
    if (*p == '{') {	/* {a,b}rest: a then rest, b then rest */
      const char *e = strchr(p, '}'), *a = p + 1;
      if (e == NULL) return 0;
      while (a <= e) {
        const char *c = a;
        char buf[512];
        size_t n;
        while (c < e && *c != ',') c++;
        n = (size_t)(c - a);
        if (n + strlen(e + 1) < sizeof(buf)) {
          memcpy(buf, a, n);
          strcpy(buf + n, e + 1);
          if (glob(buf, s)) return 1;
        }
        a = c + 1;
      }
      return 0;
    }
    if (*s == '\0') return 0;
    if (*p == '?') {
      if (*s == '/') return 0;
    }
    else if (lower((unsigned char)*p) != lower((unsigned char)*s) &&
             !((*p == '/' || *p == '\\') && (*s == '/' || *s == '\\')))
      return 0;
    p++;
    s++;
  }
  return *s == '\0';
}


/*
** Does rel (a path from the folder, '/' between its parts) match one of
** the comma separated patterns? A pattern without '/' is looked for in
** every folder, as if "**" were around it; "./x" is from the root.
*/
static int globs_match (const char *list, const char *rel) {
  const char *p = list;
  while (p && *p) {
    char pat[256], buf[300];
    size_t n = 0;
    int anchored = 0;
    while (*p == ',' || *p == ' ' || *p == '\t') p++;
    while (*p && *p != ',' && n + 1 < sizeof(pat)) pat[n++] = *p++;
    while (n > 0 && (pat[n - 1] == ' ' || pat[n - 1] == '\t' || pat[n - 1] == '/')) n--;
    pat[n] = '\0';
    if (n == 0) continue;
    if (pat[0] == '.' && pat[1] == '/') {
      memmove(pat, pat + 2, n - 1);
      anchored = 1;
    }
    else if (pat[0] == '/') {
      memmove(pat, pat + 1, n);
      anchored = 1;
    }
    if (anchored || strchr(pat, '/')) {
      if (glob(pat, rel)) return 1;
      snprintf(buf, sizeof(buf), "%s/**", pat);
      if (glob(buf, rel)) return 1;
    }
    else {
      snprintf(buf, sizeof(buf), "**/%s", pat);
      if (glob(buf, rel)) return 1;
      snprintf(buf, sizeof(buf), "**/%s/**", pat);
      if (glob(buf, rel)) return 1;
    }
  }
  return 0;
}


/* the folder's .gitignore as a pattern list (not the "!" ones) */
static void load_ignore (void) {
  char *f = path_join(side_root(), ".gitignore"), *s, *line;
  Buf b;
  free(g_ignore);
  g_ignore = NULL;
  s = read_file(f, NULL);
  free(f);
  if (s == NULL) return;
  buf_init(&b);
  for (line = strtok(s, "\r\n"); line; line = strtok(NULL, "\r\n")) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#' || *line == '!') continue;
    if (strchr(line, ',')) continue;
    if (b.len) buf_putc(&b, ',');
    buf_puts(&b, line);
  }
  buf_putc(&b, '\0');
  g_ignore = buf_take(&b);
  free(s);
}


/* }================================================================== */


/*
** {==================================================================
** Searching
** ===================================================================
*/

static void clear (void) {
  size_t i;
  for (i = 0; i < g_nfile; i++) {
    free(g_file[i].path);
    free(g_file[i].rel);
  }
  for (i = 0; i < g_nhit; i++) free(g_hit[i].text);
  g_nfile = g_nhit = g_nrow = 0;
  g_sel = g_top = 0;
  g_cut = 0;
}


static int is_word (int c) {
  return c == '_' || c >= 0x80 || (c >= '0' && c <= '9') || (lower(c) >= 'a' && lower(c) <= 'z');
}


/*
** What a search looks for. A worker has one of its own (its own Regex:
** one cannot be used by two threads at once), so nothing of the editor's
** is read while it matches.
*/
typedef struct Match {
  const char *q;
  size_t m;
  int icase, word;
  Regex *re;	/* NULL: the plain text above */
} Match;


/* where the match is in s[from..n), or n; its length in *ml (a regex's varies) */
static size_t find_in (const Match *M, const char *s, size_t n, size_t from, size_t *ml) {
  size_t i, j;
  *ml = M->m;
  if (M->re) {
    size_t a, b;
    while (from < n && re_find(M->re, s, n, from, &a, &b)) {
      if (!M->word || !((a > 0 && is_word((unsigned char)s[a - 1])) || (b < n && is_word((unsigned char)s[b])))) {
        *ml = b - a;
        return a;
      }
      from = a + 1;
    }
    return n;
  }
  for (i = from; i + M->m <= n; i++) {
    for (j = 0; j < M->m; j++) {
      int a = (unsigned char)s[i + j], b = (unsigned char)M->q[j];
      if (!M->icase ? a != b : lower(a) != lower(b)) break;
    }
    if (j < M->m) continue;
    if (M->word && ((i > 0 && is_word((unsigned char)s[i - 1])) ||
                    (i + M->m < n && is_word((unsigned char)s[i + M->m]))))
      continue;
    return i;
  }
  return n;
}


/* the Match the editor's own thread uses (Replace, and the files it has open) */
static void ui_match (Match *M) {
  M->q = g_ran;
  M->m = strlen(g_ran);
  M->icase = !g_case;
  M->word = g_word;
  M->re = g_re;
}


/* the text of path: the editor's when it is open there, else the file's */
static char *text_of (const char *path, size_t *len) {
  OsStat st;
  char *s = open_doc_text(path, len);
  if (s) return s;
  if (os_stat(path, &st) != 0 || st.size > MAX_FILE) return NULL;
  return read_file(path, len);
}


/* a file that is surely not text, by its name: not read at all */
static int binary_name (const char *name) {
  static const char *const ext[] = {".exe", ".dll", ".so", ".dylib", ".o", ".obj", ".a", ".lib", ".pdb",
                                    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".ico", ".webp", ".pdf",
                                    ".zip", ".gz", ".tgz", ".xz", ".7z", ".rar", ".jar", ".class", ".wasm",
                                    ".mp3", ".mp4", ".mov", ".avi", ".wav", ".ogg", ".ttf", ".otf", ".woff",
                                    ".woff2", ".bin", ".dat", ".db", ".sqlite", ".pyc", ".vsix"};
  const char *dot = strrchr(name, '.');
  size_t i;
  if (dot == NULL) return 0;
  for (i = 0; i < sizeof(ext) / sizeof(ext[0]); i++)
    if (m_fncmp(dot, ext[i]) == 0) return 1;
  return 0;
}


static int skip_dir (const char *name) {
  static const char *const skip[] = {".git", ".svn", ".hg", "node_modules", "bower_components", "mme-data"};
  size_t i;
  for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
    if (m_fncmp(name, skip[i]) == 0) return 1;
  return 0;
}


/*
** {==================================================================
** The walk: the workers
** ===================================================================
*/

/*
** One file's matches, made by a worker and taken by the editor's thread
** (esearch never lets a worker touch g_file / g_hit): the strings are
** made in the worker and belong to the editor once it has taken them.
*/
typedef struct SPack {
  char *path, *rel;
  SHit *hit;
  size_t n;
  struct SPack *next;
} SPack;

#define MAX_WORK	8

/*
** The folder is walked by a few workers: each takes a folder or a file
** off one stack, and a folder read puts what is in it back on, so the
** walking and the reading go together. Everything they share is under
** WK.mx; what they found waits on WK.out for search_idle to take. What
** is looked for (q, the globs, the flags) is set before they start and
** not touched until they have ended.
*/
static struct {
  Mutex *mx;
  Vec path, rel;	/* still to look at, the last first */
  SPack *out, *tail;	/* found, for the editor's thread */
  size_t files, hits;	/* looked at, found */
  int cancel, active, alive;
  int busy;	/* the editor's own: a search is on */
  char q[256], inc[256];
  char *exc;	/* "files to exclude", .gitignore and files.exclude, as globs */
  int icase, word, regex;
  Vec skip;	/* the files the editor has open: it searched them itself */
  Thread *th[MAX_WORK];
  int nth;
} WK;

static size_t g_seen;	/* WK.files, copied for the summary */


static void wk_push (char *path, char *rel) {	/* WK.mx is held */
  vec_push(&WK.path, path);
  vec_push(&WK.rel, rel);
}


/* one file read and matched; NULL when it has nothing (a worker, or the editor's thread) */
static SPack *scan_text (const Match *M, const char *path, const char *rel, char *s, size_t len) {
  size_t i, from = 0, line = 1, n = 0, cap = 0, ml;
  SHit *hit = NULL;
  SPack *pk;
  if (s == NULL) return NULL;
  if (memchr(s, '\0', len < 8000 ? len : 8000)) {	/* binary */
    free(s);
    return NULL;
  }
  for (i = 0; i <= len && n < MAX_HITS; i++) {
    size_t end, at;
    if (i < len && s[i] != '\n') continue;
    end = (i > from && s[i - 1] == '\r') ? i - 1 : i;
    for (at = find_in(M, s + from, end - from, 0, &ml); at < end - from;
         at = find_in(M, s + from, end - from, at + (ml ? ml : 1), &ml)) {
      SHit *h;
      if (n == cap) {
        cap = cap ? cap * 2 : 16;
        hit = (SHit *)xrealloc(hit, cap * sizeof(SHit));
      }
      h = &hit[n++];
      h->file = 0;	/* the editor's thread fills it in */
      h->line = line;
      h->col = at;
      h->len = ml;
      h->tlen = (end - from) < MAX_TEXT ? (end - from) : MAX_TEXT;
      h->text = xstrndup(s + from, h->tlen);
      if (n >= MAX_HITS) break;
    }
    line++;
    from = i + 1;
  }
  free(s);
  if (n == 0) {
    free(hit);
    return NULL;
  }
  pk = (SPack *)xmalloc(sizeof(SPack));
  pk->path = xstrdup(path);
  pk->rel = xstrdup(rel);
  pk->hit = hit;
  pk->n = n;
  pk->next = NULL;
  return pk;
}


/* is path one the editor has open (searched on its own thread)? */
static int is_open_file (const char *path) {
  size_t i;
  for (i = 0; i < WK.skip.n; i++)
    if (m_fncmp(WK.skip.v[i], path) == 0) return 1;
  return 0;
}


/* a worker: folders and files off the stack until there are none, or it is told to stop */
static void worker (void *ud) {
  Match M;
  (void)ud;
  M.q = WK.q;
  M.m = strlen(WK.q);
  M.icase = !WK.icase;
  M.word = WK.word;
  M.re = NULL;
  if (WK.regex && WK.q[0]) {
    const char *err = NULL;
    M.re = re_compile(WK.q, !WK.icase, &err);	/* its own: a Regex is not shared */
  }
  for (;;) {
    char *path, *rel;
    const char *name;
    OsStat st;
    SPack *pk = NULL;
    int took;
    mx_lock(WK.mx);
    if (WK.cancel || WK.hits >= MAX_HITS) {
      mx_unlock(WK.mx);
      break;
    }
    if (WK.path.n == 0) {
      int quiet = WK.active == 0;
      mx_unlock(WK.mx);
      if (quiet) break;	/* nothing left and no one is making more */
      th_nap(1);
      continue;
    }
    path = WK.path.v[--WK.path.n];
    rel = WK.rel.v[--WK.rel.n];
    WK.active++;
    mx_unlock(WK.mx);
    name = path_basename(path);
    took = 0;
    if (rel[0] && search_globs(WK.exc, rel)) ;	/* left out */
    else if (os_stat(path, &st) != 0) ;
    else if (st.is_dir) {
      Vec v;
      vec_init(&v);
      if ((!rel[0] || !skip_dir(name)) && os_listdir(path, &v) == 0) {
        char **cp, **cr;
        size_t i;
        vec_sort(&v);
        cp = (char **)xmalloc((v.n ? v.n : 1) * sizeof(char *));
        cr = (char **)xmalloc((v.n ? v.n : 1) * sizeof(char *));
        for (i = 0; i < v.n; i++) {	/* the names are made up here: malloc is a lock of its own */
          cp[i] = path_join(path, v.v[i]);
          cr[i] = rel[0] ? xstrcat3(rel, "/", v.v[i]) : xstrdup(v.v[i]);
        }
        mx_lock(WK.mx);
        for (i = v.n; i-- > 0;)	/* the first one last: it comes off the stack first */
          wk_push(cp[i], cr[i]);
        mx_unlock(WK.mx);
        free(cp);
        free(cr);
      }
      vec_free(&v);
    }
    else if (st.size <= MAX_FILE && !binary_name(name) && !is_open_file(path)) {
      took = 1;
      if (WK.inc[0] == '\0' || search_globs(WK.inc, rel)) {
        size_t len;
        pk = scan_text(&M, path, rel, read_file(path, &len), len);
      }
    }
    free(path);
    free(rel);
    mx_lock(WK.mx);
    WK.active--;
    if (took) WK.files++;
    if (pk) {
      if (WK.tail) WK.tail->next = pk;
      else WK.out = pk;
      WK.tail = pk;
      WK.hits += pk->n;
    }
    mx_unlock(WK.mx);
  }
  re_free(M.re);
  mx_lock(WK.mx);
  WK.alive--;
  mx_unlock(WK.mx);
}


static void pack_free (SPack *p) {
  size_t i;
  for (i = 0; i < p->n; i++) free(p->hit[i].text);
  free(p->hit);
  free(p->path);
  free(p->rel);
  free(p);
}


/* what the workers found so far, into the editor's own lists */
static void wk_take (void) {
  SPack *p, *next;
  if (WK.mx == NULL) return;
  mx_lock(WK.mx);
  p = WK.out;
  WK.out = WK.tail = NULL;
  g_seen = WK.files;
  mx_unlock(WK.mx);
  for (; p; p = next) {
    SFile *f;
    size_t i;
    next = p->next;
    if (g_nhit + p->n > g_caphit) {
      while (g_caphit < g_nhit + p->n) g_caphit = g_caphit ? g_caphit * 2 : 256;
      g_hit = (SHit *)xrealloc(g_hit, g_caphit * sizeof(SHit));
    }
    if (g_nfile == g_capfile) {
      g_capfile = g_capfile ? g_capfile * 2 : 64;
      g_file = (SFile *)xrealloc(g_file, g_capfile * sizeof(SFile));
    }
    for (i = 0; i < p->n; i++) {
      g_hit[g_nhit] = p->hit[i];	/* the text is the editor's now */
      g_hit[g_nhit].file = g_nfile;
      g_nhit++;
    }
    f = &g_file[g_nfile++];
    f->path = p->path;
    f->rel = p->rel;
    f->first = g_nhit - p->n;
    f->n = p->n;
    f->open = 1;
    free(p->hit);
    free(p);
  }
  if (g_nhit >= MAX_HITS) g_cut = 1;
}


/* the workers end and what is left is thrown away */
static void wk_stop (void) {
  int i;
  if (WK.mx == NULL) return;
  mx_lock(WK.mx);
  WK.cancel = 1;
  mx_unlock(WK.mx);
  for (i = 0; i < WK.nth; i++) th_join(WK.th[i]);

  WK.nth = 0;
  while (WK.out) {
    SPack *p = WK.out;
    WK.out = p->next;
    pack_free(p);
  }
  WK.tail = NULL;
  vec_free(&WK.path);	/* vec_free frees what is in them too */
  vec_free(&WK.rel);
  vec_free(&WK.skip);
  free(WK.exc);
  WK.exc = NULL;
  WK.busy = 0;
  WK.active = 0;
  WK.alive = 0;
}


void search_stop (void) {
  wk_stop();
}


/* rel of path when it is in one of the folders ('/' between), else NULL */
static char *rel_in_root (const char *path) {
  int i;
  for (i = 0; i < ws_count(); i++) {
    const char *root = ws_folder(i);
    size_t n = strlen(root);
    if (m_fnncmp(path, root, n) == 0 && path_is_sep(path[n])) {
      char *r = ws_count() > 1 ? xstrcat3(ws_folder_name(i), "/", path + n + 1) : xstrdup(path + n + 1);
      char *c;
      for (c = r; *c; c++)
        if (*c == '\\') *c = '/';
      return r;
    }
  }
  return NULL;
}


/* the files the editor has open, searched in their text here, before the workers start */
static void search_open_docs (void) {
  Match M;
  size_t i;
  ui_match(&M);
  for (i = 0; i < WK.skip.n; i++) {
    char *rel = rel_in_root(WK.skip.v[i]);
    size_t len;
    SPack *pk;
    if (rel == NULL) continue;
    if (search_globs(WK.exc, rel) || (WK.inc[0] && !search_globs(WK.inc, rel)) ||
        binary_name(path_basename(WK.skip.v[i]))) {
      free(rel);
      continue;
    }
    WK.files++;
    pk = scan_text(&M, WK.skip.v[i], rel, open_doc_text(WK.skip.v[i], &len), len);
    if (pk) {	/* straight into the editor's lists: this is its own thread */
      pk->next = NULL;
      WK.out = WK.tail = pk;
      wk_take();
    }
    free(rel);
  }
}


/* the globs a worker leaves out: "files to exclude", .gitignore, files.exclude */
static char *exclude_list (void) {
  Buf b;
  char *ig = search_ignore_list(), *fx = files_exclude_list();
  buf_init(&b);
  buf_puts(&b, g_field[FD_EXC]);
  if (ig && ig[0]) {
    if (b.len) buf_putc(&b, ',');
    buf_puts(&b, ig);
  }
  if (fx && fx[0]) {
    if (b.len) buf_putc(&b, ',');
    buf_puts(&b, fx);
  }
  buf_putc(&b, '\0');
  free(ig);
  free(fx);
  return buf_take(&b);
}


static void wk_sort (void);

/* the workers start on the folders of the workspace */
static void wk_start (void) {
  int i, n;
  wk_stop();
  if (WK.mx == NULL) WK.mx = mx_new();
  WK.cancel = 0;
  WK.files = WK.hits = 0;
  g_seen = 0;
  snprintf(WK.q, sizeof(WK.q), "%s", g_ran);
  snprintf(WK.inc, sizeof(WK.inc), "%s", g_field[FD_INC]);
  WK.exc = exclude_list();
  WK.icase = g_case;
  WK.word = g_word;
  WK.regex = g_regex;
  open_docs_list(&WK.skip);
  search_open_docs();	/* their text here, the disk on the workers */
  for (i = ws_count() - 1; i >= 0; i--)
    wk_push(xstrdup(ws_folder(i)), xstrdup(ws_count() > 1 ? ws_folder_name(i) : ""));
  n = th_cpus();
  for (i = 0; i < n && i < MAX_WORK; i++) {
    Thread *t = th_start(worker, NULL);
    if (t == NULL) break;
    WK.th[WK.nth++] = t;
    WK.alive++;
  }
  WK.busy = 1;
  if (WK.nth == 0) {	/* no thread could start: walk here, as it used to */
    WK.alive = 1;
    worker(NULL);
    wk_take();
    wk_sort();
    WK.busy = 0;
  }
}


static int by_rel (const void *a, const void *b) {
  const SFile *x = (const SFile *)a, *y = (const SFile *)b;
  return m_fncmp(x->rel, y->rel);
}


/*
** The workers hand their files over as they finish them, which is not
** the order of the folder. Once the walk is over the list is put back in
** that order, so the same search always reads the same way.
*/
static void wk_sort (void) {
  SFile *nf;
  SHit *nh;
  size_t i, j, k = 0;
  if (g_nfile < 2) return;
  nf = (SFile *)xmalloc(g_nfile * sizeof(SFile));
  memcpy(nf, g_file, g_nfile * sizeof(SFile));
  qsort(nf, g_nfile, sizeof(SFile), by_rel);
  nh = (SHit *)xmalloc((g_nhit ? g_nhit : 1) * sizeof(SHit));
  for (i = 0; i < g_nfile; i++) {
    size_t first = nf[i].first;
    nf[i].first = k;
    for (j = 0; j < nf[i].n; j++) {	/* the hits follow their file */
      nh[k] = g_hit[first + j];
      nh[k].file = i;
      k++;
    }
  }
  free(g_file);
  free(g_hit);
  g_file = nf;
  g_hit = nh;
  g_capfile = g_nfile;
  g_caphit = g_nhit ? g_nhit : 1;
}


/* the workers have ended and nothing is left to take */
static void wk_poll (void) {
  int done;
  if (!WK.busy) return;
  wk_take();
  mx_lock(WK.mx);
  done = WK.alive == 0 && WK.out == NULL;
  mx_unlock(WK.mx);
  if (!done) return;
  wk_sort();	/* it is all in: the folder's order again */
  wk_stop();	/* joins them and frees what a stop at MAX_HITS left */
}


/* a search is going on: the main loop comes back soon */
int search_busy (void) {
  return WK.busy;
}

/* }================================================================== */


/*
** For Go to File: is rel (from the folder, '/' between) left out by the
** folder's .gitignore or the folders search always skips?
*/
int search_ignored (const char *rel) {
  static char *root;
  const char *b = strrchr(rel, '/');
  if (root == NULL || strcmp(root, side_root()) != 0) {	/* another folder: its .gitignore */
    free(root);
    root = xstrdup(side_root());
    load_ignore();
  }
  if (skip_dir(b ? b + 1 : rel)) return 1;
  return g_ignore && globs_match(g_ignore, rel);
}


/* the folder's .gitignore as globs, for a worker to use without the editor */
char *search_ignore_list (void) {
  load_ignore();
  return xstrdup(g_ignore ? g_ignore : "");
}


/* rel against globs: nothing of esearch's is read, so a worker may call it */
int search_globs (const char *list, const char *rel) {
  return list && list[0] && globs_match(list, rel);
}


static void rows (void) {
  size_t i, j;
  g_nrow = 0;
  for (i = 0; i < g_nfile; i++) {
    size_t need = g_nrow + 1 + (g_file[i].open ? g_file[i].n : 0);
    if (g_file[i].n == 0) continue;	/* all dismissed */
    if (need > g_caprow) {
      g_caprow = need * 2;
      g_row = (size_t *)xrealloc(g_row, g_caprow * sizeof(size_t));
    }
    g_row[g_nrow++] = 2 * i;
    if (g_file[i].open)
      for (j = 0; j < g_file[i].n; j++) g_row[g_nrow++] = 2 * (g_file[i].first + j) + 1;
  }
  if (g_sel >= g_nrow) g_sel = g_nrow ? g_nrow - 1 : 0;
}


static void run (void) {
  size_t sel = g_sel;

  snprintf(g_ran, sizeof(g_ran), "%s", Q);
  g_pending = 0;
  wk_stop();
  clear();
  load_ignore();
  re_free(g_re);
  g_re = NULL;
  if (g_regex && g_ran[0]) {
    const char *err = NULL;
    g_re = re_compile(g_ran, !g_case, &err);
  }
  if (g_ran[0] && !(g_regex && g_re == NULL)) wk_start();
  rows();
  g_sel = sel < g_nrow ? sel : (g_nrow ? g_nrow - 1 : 0);
}


int search_idle (void) {
  if (WK.busy && !g_pending) {	/* the workers go on; what they found shows */
    size_t sel = g_sel;
    wk_poll();
    rows();
    g_sel = sel < g_nrow ? sel : (g_nrow ? g_nrow - 1 : 0);
    return 1;
  }
  if (!g_pending || os_now_us() - g_changed < DELAY) return 0;
  run();
  return 1;
}


void search_set (const char *text) {
  snprintf(Q, sizeof(Q), "%s", text);
  g_in = FD_FIND;
  run();
}


/* Find in Folder...: "files to include" is that folder ("./src"; "" the whole folder) */
void search_scope (const char *rel) {
  char r[256];
  size_t i;
  snprintf(r, sizeof(r), "%s", rel);
  for (i = 0; r[i]; i++)
    if (r[i] == '\\') r[i] = '/';
  if (r[0]) snprintf(g_field[FD_INC], sizeof(g_field[FD_INC]), "./%s", r);
  else g_field[FD_INC][0] = '\0';
  g_more = 1;
  g_in = FD_FIND;
  if (Q[0]) run();
}


/* Ctrl+Shift+H: the replace box open, the keys in it */
void search_replace_mode (const char *text) {
  if (text && *text) snprintf(Q, sizeof(Q), "%s", text);
  g_repl = 1;
  g_in = Q[0] ? FD_REPL : FD_FIND;
  run();
}

/* }================================================================== */


/*
** {==================================================================
** Replacing
** ===================================================================
*/

/*
** s[0..n) with the matches replaced: every one (line 0), or the one at
** line / col. The edits are also given as TextEdits (for a file the
** editor has open). *count: how many.
*/
static char *replaced (const char *path, const char *s, size_t n, size_t line1, size_t col1,
                       size_t *count, size_t *outlen, TextEdit **ev, size_t *nev) {
  Buf o;
  size_t i, from = 0, line = 1, m = strlen(g_ran), rl = strlen(R), cap = 0;
  Match M;
  ui_match(&M);
  (void)m;
  *count = 0;
  *ev = NULL;
  *nev = 0;
  buf_init(&o);
  for (i = 0; i <= n; i++) {
    size_t end, at, done = from;
    if (i < n && s[i] != '\n') continue;
    end = (i > from && s[i - 1] == '\r') ? i - 1 : i;
    size_t ml;
    for (at = find_in(&M, s + from, end - from, 0, &ml); at < end - from;
         at = find_in(&M, s + from, end - from, at + (ml ? ml : 1), &ml)) {
      char *rep = R;
      size_t repl = rl;
      if (line1 && (line != line1 || at != col1)) continue;
      if (g_re) {	/* $1, $& ... from this match */
        size_t cap[20], e;
        if (re_at(g_re, s + from, end - from, at, &e, cap)) rep = re_expand(R, s + from, cap, &repl);
        else rep = xstrdup(R);
      }
      buf_putn(&o, s + done, from + at - done);
      buf_putn(&o, rep, repl);
      done = from + at + ml;
      if (*nev == cap) {
        cap = cap ? cap * 2 : 16;
        *ev = (TextEdit *)xrealloc(*ev, cap * sizeof(TextEdit));
      }
      (*ev)[*nev].path = (char *)path;
      (*ev)[*nev].l0 = (*ev)[*nev].l1 = line - 1;
      (*ev)[*nev].c0 = at;
      (*ev)[*nev].c1 = at + ml;
      (*ev)[*nev].text = rep;	/* a regex's: its own, freed by replace_file */
      (*ev)[*nev].utf16 = 0;
      (*nev)++;
      (*count)++;
    }
    buf_putn(&o, s + done, (i < n ? i + 1 : n) - done);
    line++;
    from = i + 1;
  }
  buf_putc(&o, '\0');
  *outlen = o.len - 1;
  return buf_take(&o);
}


/* file f's matches (or the one at line / col) replaced: in the editor, or on disk */
static size_t replace_file (const SFile *f, size_t line, size_t col) {
  size_t len, count = 0, outlen, nev;
  TextEdit *ev;
  char *s = text_of(f->path, &len), *out;
  if (s == NULL) return 0;
  out = replaced(f->path, s, len, line, col, &count, &outlen, &ev, &nev);
  if (count > 0 && !open_doc_edit(f->path, ev, nev)) {
    int fd = os_open(f->path, OS_WRITE);
    if (fd >= 0) {
      os_write(fd, out, outlen);
      os_close(fd);
    }
    else {
      toast(1, "Unable to write '%s'", path_basename(f->path));
      count = 0;
    }
  }
  if (g_re)	/* a regex's replacements were made for each match */
    while (nev > 0) free(ev[--nev].text);
  free(ev);
  free(out);
  free(s);
  return count;
}


/* Replace All (Ctrl+Alt+Enter, the icon next to the replace box): asks first, like VS Code */
static void replace_all (void) {
  static const char *const bt[] = {"Replace", "Cancel"};
  char msg[512];
  size_t i, n = 0, files = 0, done = 0;
  if (g_pending) run();
  while (WK.busy) {	/* every match first: the workers are given their time */
    wk_poll();
    if (WK.busy) th_nap(2);
  }
  rows();
  for (i = 0; i < g_nfile; i++)
    if (g_file[i].n) {
      n += g_file[i].n;
      files++;
    }
  if (n == 0) return;
  snprintf(msg, sizeof(msg), "Replace %lu occurrence%s across %lu file%s with '%s'?", (unsigned long)n,
           n == 1 ? "" : "s", (unsigned long)files, files == 1 ? "" : "s", R);
  if (dialog(msg, NULL, bt, 2) != 0) return;
  for (i = 0; i < g_nfile; i++)
    if (g_file[i].n) done += replace_file(&g_file[i], 0, 0);
  toast(0, "Replaced %lu occurrence%s across %lu file%s with '%s'.", (unsigned long)done,
        done == 1 ? "" : "s", (unsigned long)files, files == 1 ? "" : "s", R);
  side_refresh();
  run();
}


/* the row's matches go: a file's, or one */
static void replace_row (size_t k) {
  size_t r;
  if (k >= g_nrow) return;
  r = g_row[k];
  if (r % 2 == 0) replace_file(&g_file[r / 2], 0, 0);
  else {
    const SHit *h = &g_hit[r / 2];
    replace_file(&g_file[h->file], h->line, h->col);
  }
  run();
}


/* Delete on a row: out of the results (not of the file), like VS Code's dismiss */
static void dismiss_row (size_t k) {
  size_t r;
  if (k >= g_nrow) return;
  r = g_row[k];
  if (r % 2 == 0) g_file[r / 2].n = 0;
  else {
    size_t j = r / 2;
    SFile *f = &g_file[g_hit[j].file];
    free(g_hit[j].text);
    memmove(g_hit + j, g_hit + j + 1, (g_nhit - j - 1) * sizeof(SHit));
    g_nhit--;
    f->n--;
    for (r = 0; r < g_nfile; r++)
      if (g_file[r].first > j) g_file[r].first--;
  }
  rows();
}

/* }================================================================== */


/*
** {==================================================================
** Drawing
** ===================================================================
*/

static void draw_hit (int x, int y, int w, const SHit *h, int st) {
  size_t from = 0, lim;
  int cx = x, x1 = x + w;
  while (from < h->tlen && (h->text[from] == ' ' || h->text[from] == '\t')) from++;
  if (h->col + h->len <= h->tlen && h->col > from + 20) {	/* a long line: a little before the match */
    from = h->col - 12;
    while (from > 0 && ((unsigned char)h->text[from] & 0xC0) == 0x80) from--;
    cx += scr_put(cx, y, 0x2026, st);
  }
  lim = h->tlen;
  if (from > lim) from = lim;
  if (g_repl && h->col >= from && h->col + h->len <= lim) {	/* old text struck, the new after it */
    cx += (int)scr_text(cx, y, x1 - cx, h->text + from, h->col - from, 0, st, 0, 0, st);
    cx += (int)scr_text(cx, y, x1 - cx, h->text + h->col, h->len, 0, S_DIFF_DEL, 0, 0, S_DIFF_DEL);
    if (cx < x1) cx += scr_putsw(cx, y, x1 - cx, R, S_DIFF_ADD);
    if (cx < x1) scr_text(cx, y, x1 - cx, h->text + h->col + h->len, lim - h->col - h->len, 0, st, 0, 0, st);
    return;
  }
  scr_text(cx, y, x1 - cx, h->text + from, lim - from, 0, st,
           h->col - from, h->col - from + h->len, st == S_SIDE_SEL ? S_SIDE_SEL : S_SIDE_HIT);
}


/* an input box on row y: its text, or the hint; the cursor when it has the keys */
static void draw_box (int x, int y, int w, int which, const char *hint, int focus, int right) {
  const char *s = g_field[which];
  scr_fill(x, y, w, S_INPUT);
  if (s[0]) scr_putsw(x + 1, y, w - 2 - right, s, S_INPUT_ON);
  else scr_putsw(x + 1, y, w - 2 - right, hint, S_INPUT_HINT);
  if (focus && g_in == which) {
    int cx = x + 1 + (int)str_cols(s);
    scr_cursor(cx < x + w - 1 - right ? cx : x + w - 1 - right, y);
  }
}


void search_draw (int x, int y, int w, int h, int focus) {
  char sum[160];
  int row, ry;
  scr_box(x, y, w, h, S_SIDE);
  V.w = w;
  if (h < 4) return;
  scr_puts(x + 2, y, "SEARCH", S_SIDE_HEAD);
  V.collapse = w - 3;	/* the title's icons: refresh, clear, collapse all */
  V.clear = w - 5;
  V.refresh = w - 7;
  scr_put(x + V.refresh, y, 0xEB37, S_SIDE_HEAD);
  scr_put(x + V.clear, y, 0xEABF, S_SIDE_HEAD);
  scr_put(x + V.collapse, y, 0xEAC5, S_SIDE_HEAD);
  /* the chevron, the search box with its toggles, the replace box */
  V.chev = 1;
  V.box_x = 3;
  V.box_w = w - 4;
  ry = 1;
  V.find = ry;
  scr_put(x + V.chev, y + ry, g_repl ? 0xEAB4 : 0xEAB6, S_SIDE);
  draw_box(x + V.box_x, y + ry, V.box_w, FD_FIND, "Search", focus, 7);
  scr_put(x + V.box_x + V.box_w - 7, y + ry, 0xEAB1, g_case ? S_TOGGLE_ON : S_INPUT);	/* case-sensitive */
  scr_put(x + V.box_x + V.box_w - 5, y + ry, 0xEB7E, g_word ? S_TOGGLE_ON : S_INPUT);	/* whole-word */
  scr_put(x + V.box_x + V.box_w - 3, y + ry, 0xEB38, g_regex ? S_TOGGLE_ON : S_INPUT);	/* regex */
  ry++;
  V.repl = -1;
  if (g_repl) {
    V.repl = ry;
    draw_box(x + V.box_x, y + ry, V.box_w, FD_REPL, "Replace", focus, 3);
    scr_put(x + V.box_x + V.box_w - 2, y + ry, 0xEB3C, S_INPUT);	/* replace-all */
    ry++;
  }
  V.dots = ry;	/* "...": the file boxes */
  scr_put(x + w - 3, y + ry, 0xEA7C, g_more ? S_TOGGLE_ON : S_SIDE_DIM);
  ry++;
  V.inc = V.exc = -1;
  if (g_more && ry + 4 < h) {
    scr_puts(x + 2, y + ry++, "files to include", S_SIDE_DIM);
    V.inc = ry;
    draw_box(x + 2, y + ry++, w - 3, FD_INC, "e.g. *.c, src/**", focus, 0);
    scr_puts(x + 2, y + ry++, "files to exclude", S_SIDE_DIM);
    V.exc = ry;
    draw_box(x + 2, y + ry++, w - 3, FD_EXC, "e.g. build, *.min.js", focus, 0);
  }
  V.sum = ry;
  if (g_ran[0] && !g_pending) {
    if (g_regex && g_re == NULL) snprintf(sum, sizeof(sum), "Invalid regular expression.");
    else if (WK.busy) {	/* the walk goes on: VS Code's progress, in words */
      static const char *const spin[] = {"|", "/", "-", "\\"};
      snprintf(sum, sizeof(sum), "%s Searching %lu files: %lu result%s in %lu file%s", spin[(os_now_us() / 150000) % 4],
               (unsigned long)g_seen, (unsigned long)g_nhit, g_nhit == 1 ? "" : "s", (unsigned long)g_nfile,
               g_nfile == 1 ? "" : "s");
    }
    else if (g_nhit == 0) snprintf(sum, sizeof(sum), "No results found. Review your settings for configured exclusions.");
    else snprintf(sum, sizeof(sum), "%lu result%s in %lu file%s%s",
                  (unsigned long)g_nhit, g_nhit == 1 ? "" : "s",
                  (unsigned long)g_nfile, g_nfile == 1 ? "" : "s", g_cut ? " (more not shown)" : "");
    scr_putsw(x + 2, y + ry, w - 3, sum, S_SIDE_DIM);
  }
  ry++;
  V.head = ry;
  g_h = h - V.head;
  if (g_h < 1) return;
  if (g_sel < g_top) g_top = g_sel;
  if (g_sel >= g_top + (size_t)g_h) g_top = g_sel - (size_t)g_h + 1;
  for (row = 0; row < g_h; row++) {
    size_t k = g_top + (size_t)row, r;
    int sy = y + V.head + row, st;
    if (k >= g_nrow) break;
    r = g_row[k];
    st = (k == g_sel && focus && g_in < 0) ? S_SIDE_SEL : (k == g_sel ? S_SIDE_CUR : S_SIDE);
    scr_fill(x, sy, w, st);
    if (r % 2 == 0) {	/* a file */
      const SFile *f = &g_file[r / 2];
      const char *base = path_basename(f->path);
      char num[32];
      int cx = x + 1, nw, ist, right;
      snprintf(num, sizeof(num), " %lu ", (unsigned long)f->n);
      right = (int)strlen(num) + (g_repl && k == g_sel ? 4 : 0);
      scr_put(cx, sy, f->open ? 0xEAB4 : 0xEAB6, st);
      cx += 2;
      cx += scr_put(cx, sy, file_icon(base, &ist), st == S_SIDE ? ist : st) + 1;
      nw = x + w - cx - right - 1;
      cx += scr_putsw(cx, sy, nw, base, st);
      if (strlen(f->rel) > strlen(base) + 1 && cx + 2 < x + w - right - 1) {
        char *dir = xstrndup(f->rel, strlen(f->rel) - strlen(base) - 1);
        scr_putsw(cx + 1, sy, x + w - right - 2 - cx, dir, st == S_SIDE ? S_SIDE_DIM : st);
        free(dir);
      }
      if (g_repl && k == g_sel) {	/* replace all in the file, dismiss */
        scr_put(x + w - 4, sy, 0xEB3C, st);
        scr_put(x + w - 2, sy, 0xEA76, st);
      }
      else scr_puts(x + w - (int)strlen(num) - 1, sy, num, st == S_SIDE ? S_TOGGLE_ON : st);
    }
    else {
      draw_hit(x + 4, sy, w - 5 - (k == g_sel ? 4 : 0), &g_hit[r / 2], st);
      if (k == g_sel) {	/* replace (with the replace box), dismiss */
        if (g_repl) scr_put(x + w - 4, sy, 0xEB3D, st);
        scr_put(x + w - 2, sy, 0xEA76, st);
      }
    }
  }
  side_bar(x, y + V.head, w, g_h, g_nrow, g_top, (size_t)g_h);
}

/* }================================================================== */


/*
** {==================================================================
** Keys and mouse
** ===================================================================
*/

static void typed (void) {
  g_pending = 1;
  g_changed = os_now_us();
}


static void open_row (size_t k, int go, SideAct *act) {
  size_t r = g_row[k];
  if (r % 2 == 0) {
    g_file[r / 2].open = !g_file[r / 2].open;
    rows();
    return;
  }
  act->what = go ? SA_GO : SA_OPEN;
  act->path = g_file[g_hit[r / 2].file].path;
  act->line = g_hit[r / 2].line;
  act->col = g_hit[r / 2].col;
  act->len = g_hit[r / 2].len;
}


static void collapse_all (void) {
  size_t i;
  for (i = 0; i < g_nfile; i++) g_file[i].open = 0;
  g_sel = g_top = 0;
  rows();
}


/* Tab / Shift+Tab: the boxes that show, then the results */
static void next_box (int back) {
  int order[FD_N + 1], n = 0, i, at = 0;
  order[n++] = FD_FIND;
  if (g_repl) order[n++] = FD_REPL;
  if (g_more) {
    order[n++] = FD_INC;
    order[n++] = FD_EXC;
  }
  order[n++] = -1;	/* the results */
  for (i = 0; i < n; i++)
    if (order[i] == g_in) at = i;
  g_in = order[(at + (back ? n - 1 : 1)) % n];
}


/* a key in the box that has the keys */
static int box_key (int k) {
  char *s = g_field[g_in];
  size_t len = strlen(s);
  int code = KEY_CODE(k);
  if (code == K_BS) {
    while (len > 0 && ((unsigned char)s[len - 1] & 0xC0) == 0x80) len--;
    if (len > 0) s[len - 1] = '\0';
  }
  else if (code == K_PASTE) {
    Buf b;
    size_t i;
    buf_init(&b);
    term_paste(&b);
    for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < sizeof(g_field[0]); i++) s[len++] = b.s[i];
    s[len] = '\0';
    buf_free(&b);
  }
  else if (IS_TEXT(k) && len + 4 < sizeof(g_field[0])) {
    len += (size_t)utf8_encode((uint32_t)k, s + len);
    s[len] = '\0';
  }
  else return 0;
  if (g_in != FD_REPL) typed();	/* the replace text only changes the preview */
  return 1;
}


int search_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  if (k == ('c' | KM_ALT)) {	/* Alt+C, Alt+W: VS Code's toggles */
    g_case = !g_case;
    typed();
    return 1;
  }
  if (k == ('w' | KM_ALT)) {
    g_word = !g_word;
    typed();
    return 1;
  }
  if (k == ('r' | KM_ALT)) {	/* Alt+R: .* */
    g_regex = !g_regex;
    typed();
    return 1;
  }
  if (code == K_ENTER && (k & KM_CTRL) && (k & KM_ALT)) {	/* Ctrl+Alt+Enter: Replace All */
    if (g_repl) replace_all();
    return 1;
  }
  if (k == ('1' | KM_CTRL | KM_SHIFT) || k == ('!' | KM_CTRL | KM_SHIFT)) {	/* Replace, on the row */
    if (g_repl && g_nrow) replace_row(g_sel);
    return 1;
  }
  if (k == ('j' | KM_CTRL | KM_SHIFT)) {	/* Ctrl+Shift+J: Toggle Search Details */
    g_more = !g_more;
    if (g_more) g_in = FD_INC;
    else if (g_in == FD_INC || g_in == FD_EXC) g_in = FD_FIND;
    return 1;
  }
  if (code == K_TAB) {
    if (g_in < 0 && !(k & KM_SHIFT) && g_nrow == 0) return 0;	/* nothing to go to: back to the editor */
    next_box((k & KM_SHIFT) != 0);
    return 1;
  }
  if (g_in >= 0 && box_key(k)) return 1;
  switch (code) {
    case K_UP:
      if (g_in >= 0) return 1;
      if (g_sel > 0) g_sel--;
      else g_in = FD_FIND;
      return 1;
    case K_DOWN:
      if (g_in >= 0) {
        if (g_nrow) g_in = -1;
        return 1;
      }
      if (g_sel + 1 < g_nrow) g_sel++;
      return 1;
    case K_PGUP: g_sel = g_sel > (size_t)g_h ? g_sel - (size_t)g_h : 0; return 1;
    case K_PGDN:
      g_sel += (size_t)g_h;
      if (g_sel >= g_nrow) g_sel = g_nrow ? g_nrow - 1 : 0;
      return 1;
    case K_LEFT: case K_RIGHT:
      if (g_in < 0 && g_nrow && g_row[g_sel] % 2 == 0 && g_file[g_row[g_sel] / 2].open != (code == K_RIGHT))
        open_row(g_sel, 0, act);
      return 1;
    case K_DEL:
      if (g_in < 0 && g_nrow) dismiss_row(g_sel);
      return 1;
    case K_ENTER:
      if (g_in == FD_REPL) replace_all();
      else if (g_pending || g_in >= 0) {
        if (g_pending) run();
        if (g_in >= 0 && g_nrow) g_in = -1;
      }
      else if (g_nrow) open_row(g_sel, 1, act);
      return 1;
    case K_F4:	/* the next / previous match, opened */
      if (g_nrow == 0) return 1;
      do {
        if (k & KM_SHIFT) g_sel = g_sel > 0 ? g_sel - 1 : g_nrow - 1;
        else g_sel = (g_sel + 1) % g_nrow;
      } while (g_row[g_sel] % 2 == 0 && g_nrow > g_nfile);
      open_row(g_sel, 0, act);
      return 1;
  }
  if (g_in < 0 && (IS_TEXT(k) || code == K_BS || code == K_PASTE)) {	/* typing in the list: to the box */
    g_in = FD_FIND;
    return box_key(k);
  }
  return 0;
}


void search_click (int row, int col, SideAct *act) {
  size_t k;
  if (row == 0) {
    if (col == V.refresh) run();
    else if (col == V.clear) {
      Q[0] = R[0] = '\0';
      g_ran[0] = '\0';
      clear();
      g_in = FD_FIND;
    }
    else if (col == V.collapse) collapse_all();
    return;
  }
  if (row == V.find) {
    int bx = V.box_x + V.box_w;
    if (col <= V.chev) {
      g_repl = !g_repl;
      if (!g_repl && g_in == FD_REPL) g_in = FD_FIND;
    }
    else if (col == bx - 7) g_case = !g_case, typed();
    else if (col == bx - 5) g_word = !g_word, typed();
    else if (col == bx - 3) g_regex = !g_regex, typed();
    else g_in = FD_FIND;
    return;
  }
  if (row == V.repl) {
    if (col == V.box_x + V.box_w - 2) replace_all();
    else g_in = FD_REPL;
    return;
  }
  if (row == V.dots) {
    if (col >= V.w - 4) g_more = !g_more;
    return;
  }
  if (row == V.inc) {
    g_in = FD_INC;
    return;
  }
  if (row == V.exc) {
    g_in = FD_EXC;
    return;
  }
  if (row < V.head) return;
  k = g_top + (size_t)(row - V.head);
  if (k >= g_nrow) return;
  if (k == g_sel && col >= V.w - 2) {	/* the row's icons */
    dismiss_row(k);
    return;
  }
  if (k == g_sel && g_repl && col >= V.w - 4 && col < V.w - 2) {
    replace_row(k);
    return;
  }
  g_sel = k;
  g_in = -1;
  open_row(k, 0, act);
}


void search_wheel (int d) {
  size_t h = (size_t)g_h, st = (size_t)wheel_step(0);
  if (d < 0) g_top = g_top > st ? g_top - st : 0;
  else if (g_nrow > h) {
    g_top += st;
    if (g_top > g_nrow - h) g_top = g_nrow - h;
  }
  if (g_sel < g_top) g_sel = g_top;
  if (g_sel >= g_top + h) g_sel = g_top + h - 1;
}

/* }================================================================== */
