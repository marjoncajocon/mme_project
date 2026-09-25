/*
** esearched.c - the Search Editor: a search in an editor tab, like VS Code's
**
** "Search Editor: New Search Editor", or "Open in editor" in the Search view
** (Alt+Enter there), opens a tab "Search: <query>". On top: the query box
** with its toggles (Aa, ab, .*), the context lines (the icon next to it),
** and "..." for files to include / exclude; typing there searches again a
** moment later (search.searchOnType), Enter at once. Under it the results,
** written as VS Code writes them in a .code-search file:
**
**   3 results - 2 files
**
**   src/a.c:
**     11    the line before
**     12:   the line with the match
**
** Enter, F12 or a double click on a line opens the file there; Ctrl+S saves
** the tab as a .code-search file (the query in its "# Query:" header ...),
** which opens here again. The results can be selected and copied, but not
** typed in: VS Code lets them be edited, mme keeps them read only.
** The folder is walked on a thread of its own, with the globs and the
** matching of the Search view (esearch.c).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


enum { SE_FIND, SE_CTX, SE_INC, SE_EXC, SE_N };	/* the input boxes */

#define DELAY		300000	/* us after the last key, as the Search view waits */
#define MAX_HITS	10000
#define MAX_FILE	(8 << 20)
#define MAX_TEXT	1000	/* bytes of a line kept, like VS Code's preview of it */
#define EXT		".code-search"


/*
** {==================================================================
** The search, on its thread
** ===================================================================
*/

/* one file's results, made by the worker, taken by the editor's thread */
typedef struct SFound {
  char *rel;
  char *text;	/* "rel:\n  12: ...\n": its lines, each ended by '\n' */
  size_t hits;
  struct SFound *next;
} SFound;

/*
** A search. What it looks for is set before its thread starts and is not
** touched until the thread has ended; what it found waits on out, under mx.
*/
typedef struct SJob {
  Mutex *mx;
  Thread *th;
  char q[256], inc[256];
  char *exc;	/* the globs left out */
  int mcase, word, regex, ctx;
  Vec root, name;	/* the folders, and what their files' paths start with */
  Vec open;	/* the files open in the editor: otext is their text there */
  char **otext;
  size_t *olen;
  int cancel, done;	/* under mx */
  size_t hits;
  SFound *out;
} SJob;


static int digits (size_t n) {
  int d = 1;
  while (n >= 10) {
    n /= 10;
    d++;
  }
  return d;
}


/* line k of s ([ls[k], its end)), without its \r, cut to MAX_TEXT */
static void put_text (Buf *b, const char *s, const size_t *ls, size_t nl, size_t len, size_t k) {
  size_t a = ls[k], e = k + 1 < nl ? ls[k + 1] - 1 : len;
  if (e > a && s[e - 1] == '\n') e--;	/* the last line: its end is the file's */
  if (e > a && s[e - 1] == '\r') e--;
  if (e - a > MAX_TEXT) {
    e = a + MAX_TEXT;
    while (e > a && ((unsigned char)s[e] & 0xC0) == 0x80) e--;
  }
  buf_putn(b, s + a, e - a);
  buf_putc(b, '\n');
}


/* a line of context: "  13  text"; pad is how much shorter its number is than the longest */
static void put_ctx (Buf *b, const char *s, const size_t *ls, size_t nl, size_t len, size_t k, int w) {
  buf_printf(b, "  %*lu  ", w, (unsigned long)(k + 1));
  put_text(b, s, ls, nl, len, k);
}


/*
** The matches of one file, in VS Code's words (fileMatchToSearchResultFormat):
** "rel:", each matching line "  12: text", the ctx lines around them
** "  13  text", an empty line where lines were skipped between two groups.
** The numbers are padded to the longest match line's; the context after the
** last match is not (VS Code does the same).
*/
static SFound *scan (SJob *J, const Regex *re, const char *rel, const char *s, size_t len) {
  size_t *ls = NULL, *hit = NULL, nl = 0, cap = 0, nh = 0, hcap = 0, hits = 0, i, start = 0, j, last = 0;
  int w, have = 0;
  Buf b;
  SFound *f;
  if (memchr(s, '\0', len < 8000 ? len : 8000)) return NULL;	/* binary */
  for (i = 0; i <= len; i++) {	/* where each line starts */
    if (i < len && s[i] != '\n') continue;
    if (nl == cap) {
      cap = cap ? cap * 2 : 256;
      ls = (size_t *)xrealloc(ls, cap * sizeof(size_t));
    }
    ls[nl++] = start;
    start = i + 1;
  }
  if (nl > 1 && len > 0 && s[len - 1] == '\n') nl--;	/* the end of the last line is not one more */
  for (i = 0; i < nl && J->hits + hits < MAX_HITS; i++) {
    size_t a = ls[i], e = i + 1 < nl ? ls[i + 1] - 1 : len, at, ml, n = 0;
    if (e > a && s[e - 1] == '\n') e--;
    if (e > a && s[e - 1] == '\r') e--;
    for (at = search_find(J->q, J->mcase, J->word, re, s + a, e - a, 0, &ml); at < e - a;
         at = search_find(J->q, J->mcase, J->word, re, s + a, e - a, at + (ml ? ml : 1), &ml))
      n++;
    if (n == 0) continue;
    hits += n;
    if (nh == hcap) {
      hcap = hcap ? hcap * 2 : 16;
      hit = (size_t *)xrealloc(hit, hcap * sizeof(size_t));
    }
    hit[nh++] = i;
  }
  if (nh == 0) {
    free(ls);
    return NULL;
  }
  buf_init(&b);
  buf_puts(&b, rel);
  buf_puts(&b, ":\n");
  w = digits(hit[nh - 1] + 1);
  for (j = 0; j < nh; j++) {
    size_t m = hit[j], c, lo = m > (size_t)J->ctx ? m - (size_t)J->ctx : 0;
    if (have) {	/* after the last match, then before this one: a gap between them is an empty line */
      size_t to = last + (size_t)J->ctx < m ? last + (size_t)J->ctx : m - 1;
      for (c = last + 1; c <= to && c < m; c++) put_ctx(&b, s, ls, nl, len, c, w);
      if (lo <= to) lo = to + 1;
      if (lo < m && lo > to + 1) buf_putc(&b, '\n');
      if (lo <= last) lo = last + 1;
    }
    for (c = lo; c < m; c++) put_ctx(&b, s, ls, nl, len, c, w);
    buf_printf(&b, "  %*lu: ", w, (unsigned long)(m + 1));
    put_text(&b, s, ls, nl, len, m);
    last = m;
    have = 1;
  }
  for (i = last + 1; i < nl && i <= last + (size_t)J->ctx; i++) put_ctx(&b, s, ls, nl, len, i, 0);
  buf_putc(&b, '\0');
  free(ls);
  free(hit);
  f = (SFound *)xmalloc(sizeof(SFound));
  f->rel = xstrdup(rel);
  f->text = buf_take(&b);
  f->hits = hits;
  f->next = NULL;
  return f;
}


/* the text of path as the editor has it (a copy made before the thread started), or NULL */
static const char *open_text (SJob *J, const char *path, size_t *len) {
  size_t i;
  for (i = 0; i < J->open.n; i++)
    if (J->otext[i] && m_fncmp(J->open.v[i], path) == 0) {
      *len = J->olen[i];
      return J->otext[i];
    }
  return NULL;
}


/* the thread: the folders walked, the files read and matched, until the end or a cancel */
static void worker (void *ud) {
  SJob *J = (SJob *)ud;
  Regex *re = NULL;
  Vec path, rel;
  size_t i;
  if (J->regex) {	/* its own: a Regex is not shared between threads */
    const char *err = NULL;
    re = re_compile(J->q, !J->mcase, &err);
  }
  vec_init(&path);
  vec_init(&rel);
  for (i = J->root.n; i-- > 0;) {
    vec_push(&path, xstrdup(J->root.v[i]));
    vec_push(&rel, xstrdup(J->name.v[i]));
  }
  while (path.n > 0 && (!J->regex || re)) {
    char *p = path.v[--path.n], *r = rel.v[--rel.n];
    const char *name = path_basename(p);
    OsStat st;
    int stop;
    path.v[path.n] = rel.v[rel.n] = NULL;
    mx_lock(J->mx);
    stop = J->cancel || J->hits >= MAX_HITS;
    mx_unlock(J->mx);
    if (stop) {
      free(p);
      free(r);
      break;
    }
    if (r[0] && search_globs(J->exc, r)) ;	/* left out */
    else if (os_stat(p, &st) != 0) ;
    else if (st.is_dir) {
      Vec v;
      vec_init(&v);
      if ((!r[0] || !search_skipped(name, 1)) && os_listdir(p, &v) == 0) {
        vec_sort(&v);
        for (i = v.n; i-- > 0;) {	/* the first one last: it comes off first */
          vec_push(&path, path_join(p, v.v[i]));
          vec_push(&rel, r[0] ? xstrcat3(r, "/", v.v[i]) : xstrdup(v.v[i]));
        }
      }
      vec_free(&v);
    }
    else if (st.size <= MAX_FILE && !search_skipped(name, 0) && (J->inc[0] == '\0' || search_globs(J->inc, r))) {
      size_t len = 0;
      const char *ot = open_text(J, p, &len);
      char *text = ot ? NULL : read_file(p, &len);
      SFound *f = (ot || text) ? scan(J, re, r, ot ? ot : text, len) : NULL;
      free(text);
      if (f) {
        mx_lock(J->mx);
        f->next = J->out;
        J->out = f;
        J->hits += f->hits;
        mx_unlock(J->mx);
      }
    }
    free(p);
    free(r);
  }
  vec_free(&path);
  vec_free(&rel);
  re_free(re);
  mx_lock(J->mx);
  J->done = 1;
  mx_unlock(J->mx);
}


static void found_free (SFound *f) {
  while (f) {
    SFound *next = f->next;
    free(f->rel);
    free(f->text);
    free(f);
    f = next;
  }
}


/* the search ends (it is told to, and waited for) and all of it goes */
static void job_free (SJob *J) {
  size_t i;
  if (J == NULL) return;
  mx_lock(J->mx);
  J->cancel = 1;
  mx_unlock(J->mx);
  if (J->th) th_join(J->th);
  found_free(J->out);
  for (i = 0; i < J->open.n; i++) free(J->otext[i]);
  free(J->otext);
  free(J->olen);
  vec_free(&J->open);
  vec_free(&J->root);
  vec_free(&J->name);
  free(J->exc);
  mx_free(J->mx);
  free(J);
}

/* }================================================================== */


/*
** {==================================================================
** A Search Editor
** ===================================================================
*/

typedef struct SEd {
  char f[SE_N][256];	/* what each box has */
  int in;	/* the box with the keys, -1: the results */
  int mcase, word, regex;	/* Aa, ab, .* */
  int ctx_on;	/* the context lines show (SE_CTX says how many) */
  int more;	/* files to include / exclude show */
  int noexcl;	/* "IgnoreExcludeSettings": files.exclude and .gitignore not used */
  char *path;	/* its .code-search file; NULL: not saved */
  char title[80];
  Vec line;	/* the results, a line each */
  char hq[256];	/* what they are for: their matches are lit */
  int hcase, hword, hregex;
  Regex *hre;
  size_t cy, cx, ay, ax;	/* the cursor and the other end of the selection: line, byte */
  int sel, reveal;
  size_t top, left, goal;	/* goal: the column Up and Down keep to */
  int pending;	/* typed since the last search */
  long long changed;
  SJob *job;
  char *go;	/* the file a result opens (SideAct's path points here) */
  /* where it was drawn, for the mouse */
  int x, y, w, ry, rh, tx, tw, bx, bw, ctx_x, dots_y, inc_y, exc_y;
  int drag;
  long long click_t;
  int click_x, click_y;
  struct SEd *next;
} SEd;

static SEd *g_all;	/* every Search Editor: their searches go on while hidden */

static struct {	/* the last one's settings (search.searchEditor.reusePriorSearchConfiguration) */
  int set, mcase, word, regex, ctx_on, more, noexcl;
  char ctx[8], inc[256], exc[256];
} g_prior;


static const char *line_of (const SEd *e, size_t y) {
  return y < e->line.n ? e->line.v[y] : "";
}


static size_t nlines (const SEd *e) {
  return e->line.n ? e->line.n : 1;
}


/* "Search: query", cut like VS Code's (12 at most); a saved one: its file's name */
static void set_title (SEd *e) {
  char q[40];
  const char *s = e->hq;
  size_t n = strlen(s);
  if (e->path) {
    const char *b = path_basename(e->path);
    size_t bn = strlen(b);
    if (bn > strlen(EXT) && m_fncmp(b + bn - strlen(EXT), EXT) == 0) bn -= strlen(EXT);
    snprintf(e->title, sizeof(e->title), "Search: %.*s", (int)(bn < 60 ? bn : 60), b);
    return;
  }
  while (n > 0 && (*s == ' ' || *s == '\t')) {
    s++;
    n--;
  }
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
  if (n == 0) {
    snprintf(e->title, sizeof(e->title), "Search");
    return;
  }
  if (n >= 12) {
    n = 9;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
    snprintf(q, sizeof(q), "%.*s...", (int)n, s);
  }
  else snprintf(q, sizeof(q), "%.*s", (int)n, s);
  snprintf(e->title, sizeof(e->title), "Search: %s", q);
}


static int ctx_lines (const SEd *e) {
  int n = atoi(e->f[SE_CTX]);
  return e->ctx_on && n > 0 ? (n > 100 ? 100 : n) : 0;
}


/* what the results are for: what the widget says now */
static void set_ran (SEd *e) {
  snprintf(e->hq, sizeof(e->hq), "%s", e->f[SE_FIND]);
  e->hcase = e->mcase;
  e->hword = e->word;
  e->hregex = e->regex;
  re_free(e->hre);
  e->hre = NULL;
  if (e->hregex && e->hq[0]) {
    const char *err = NULL;
    e->hre = re_compile(e->hq, !e->hcase, &err);
  }
  set_title(e);
}


static void results_clear (SEd *e) {
  vec_free(&e->line);
  vec_init(&e->line);
  e->cy = e->cx = e->ay = e->ax = e->top = e->left = e->goal = 0;
  e->sel = 0;
}


static int by_rel (const void *a, const void *b) {
  return m_fncmp((*(SFound *const *)a)->rel, (*(SFound *const *)b)->rel);
}


/* VS Code's text of the results (serializeSearchResultForEditor), a line each */
static void compose (SEd *e, SFound *list, int limit) {
  SFound **v, *f;
  size_t n = 0, i, hits = 0;
  char sum[160];
  for (f = list; f; f = f->next) {
    n++;
    hits += f->hits;
  }
  v = (SFound **)xmalloc((n ? n : 1) * sizeof(SFound *));
  for (n = 0, f = list; f; f = f->next) v[n++] = f;
  qsort(v, n, sizeof(SFound *), by_rel);	/* the folder's order, as the Search view's */
  results_clear(e);
  if (hits) snprintf(sum, sizeof(sum), "%lu result%s - %lu file%s", (unsigned long)hits, hits == 1 ? "" : "s",
                     (unsigned long)n, n == 1 ? "" : "s");
  else snprintf(sum, sizeof(sum), "No Results");
  vec_push(&e->line, xstrdup(sum));
  if (limit)
    vec_push(&e->line, xstrdup("The result set only contains a subset of all matches. Be more specific in your "
                               "search to narrow down the results."));
  vec_push(&e->line, xstrdup(""));
  for (i = 0; i < n; i++) {
    const char *s = v[i]->text, *nl;
    for (; (nl = strchr(s, '\n')) != NULL; s = nl + 1) vec_push(&e->line, xstrndup(s, (size_t)(nl - s)));
    vec_push(&e->line, xstrdup(""));
  }
  free(v);
}


/* the globs the search leaves out: files to exclude, then .gitignore and files.exclude */
static char *exclude_list (const SEd *e) {
  Buf b;
  buf_init(&b);
  buf_puts(&b, e->f[SE_EXC]);
  if (!e->noexcl) {
    char *ig = search_ignore_list(), *fx = files_exclude_list();
    if (ig && ig[0]) {
      if (b.len) buf_putc(&b, ',');
      buf_puts(&b, ig);
    }
    if (fx && fx[0]) {
      if (b.len) buf_putc(&b, ',');
      buf_puts(&b, fx);
    }
    free(ig);
    free(fx);
  }
  buf_putc(&b, '\0');
  return buf_take(&b);
}


static void job_start (SEd *e) {
  SJob *J = (SJob *)xmalloc(sizeof(SJob));
  size_t i;
  int k;
  memset(J, 0, sizeof(*J));
  J->mx = mx_new();
  snprintf(J->q, sizeof(J->q), "%s", e->hq);
  snprintf(J->inc, sizeof(J->inc), "%s", e->f[SE_INC]);
  J->exc = exclude_list(e);
  J->mcase = e->hcase;
  J->word = e->hword;
  J->regex = e->hregex;
  J->ctx = ctx_lines(e);
  vec_init(&J->root);
  vec_init(&J->name);
  for (k = 0; k < ws_count(); k++) {
    vec_push(&J->root, xstrdup(ws_folder(k)));
    vec_push(&J->name, xstrdup(ws_count() > 1 ? ws_folder_name(k) : ""));
  }
  open_docs_list(&J->open);	/* their text as the editor has it, edits and all */
  J->otext = (char **)xmalloc((J->open.n ? J->open.n : 1) * sizeof(char *));
  J->olen = (size_t *)xmalloc((J->open.n ? J->open.n : 1) * sizeof(size_t));
  for (i = 0; i < J->open.n; i++) {
    J->olen[i] = 0;
    J->otext[i] = open_doc_text(J->open.v[i], &J->olen[i]);
  }
  if ((J->th = th_start(worker, J)) == NULL) worker(J);	/* no thread: here, as it used to be */
  e->job = J;
}


/* the search again, with what the widget says now */
static void run (SEd *e) {
  e->pending = 0;
  job_free(e->job);
  e->job = NULL;
  set_ran(e);
  g_prior.set = 1;	/* for search.searchEditor.reusePriorSearchConfiguration */
  g_prior.mcase = e->mcase;
  g_prior.word = e->word;
  g_prior.regex = e->regex;
  g_prior.ctx_on = e->ctx_on;
  g_prior.more = e->more;
  g_prior.noexcl = e->noexcl;
  snprintf(g_prior.ctx, sizeof(g_prior.ctx), "%s", e->f[SE_CTX]);
  snprintf(g_prior.inc, sizeof(g_prior.inc), "%s", e->f[SE_INC]);
  snprintf(g_prior.exc, sizeof(g_prior.exc), "%s", e->f[SE_EXC]);
  if (e->hq[0] == '\0') {
    results_clear(e);
    return;
  }
  if (e->hregex && e->hre == NULL) {
    results_clear(e);
    vec_push(&e->line, xstrdup("Invalid regular expression."));
    return;
  }
  job_start(e);
}


/* the search has ended: its results replace the ones shown; 1 when that happened */
static int job_poll (SEd *e) {
  SJob *J = e->job;
  SFound *out;
  int done;
  if (J == NULL) return 0;
  mx_lock(J->mx);
  done = J->done;
  mx_unlock(J->mx);
  if (!done) return 0;
  out = J->out;
  J->out = NULL;
  compose(e, out, J->hits >= MAX_HITS);
  found_free(out);
  job_free(J);
  e->job = NULL;
  return 1;
}


static SEd *se_new (void) {
  SEd *e = (SEd *)xmalloc(sizeof(SEd));
  memset(e, 0, sizeof(*e));
  vec_init(&e->line);
  snprintf(e->f[SE_CTX], sizeof(e->f[SE_CTX]), "%d", opt.se_context > 0 ? opt.se_context : 1);
  e->ctx_on = opt.se_context > 0;
  if (opt.se_reuse && g_prior.set) {	/* the toggles, files and context of the last one */
    e->mcase = g_prior.mcase;
    e->word = g_prior.word;
    e->regex = g_prior.regex;
    e->ctx_on = g_prior.ctx_on;
    e->more = g_prior.more;
    e->noexcl = g_prior.noexcl;
    snprintf(e->f[SE_CTX], sizeof(e->f[SE_CTX]), "%s", g_prior.ctx);
    snprintf(e->f[SE_INC], sizeof(e->f[SE_INC]), "%s", g_prior.inc);
    snprintf(e->f[SE_EXC], sizeof(e->f[SE_EXC]), "%s", g_prior.exc);
  }
  e->next = g_all;
  g_all = e;
  set_title(e);
  return e;
}


void *searched_new (const char *query) {
  SEd *e = se_new();
  e->in = SE_FIND;
  if (query && *query) {	/* the selection: searched at once, like VS Code */
    snprintf(e->f[SE_FIND], sizeof(e->f[SE_FIND]), "%s", query);
    run(e);
  }
  return e;
}


/* Open Results in Editor: the Search view's search, with the context lines, the results focused */
void *searched_from_view (void) {
  SearchQuery sq;
  SEd *e = se_new();
  search_view_query(&sq);
  snprintf(e->f[SE_FIND], sizeof(e->f[SE_FIND]), "%s", sq.q);
  snprintf(e->f[SE_INC], sizeof(e->f[SE_INC]), "%s", sq.inc);
  snprintf(e->f[SE_EXC], sizeof(e->f[SE_EXC]), "%s", sq.exc);
  e->mcase = sq.match_case;
  e->word = sq.word;
  e->regex = sq.regex;
  e->more = sq.inc[0] || sq.exc[0];
  e->in = -1;
  run(e);
  return e;
}


int searched_is_file (const char *path) {
  size_t n = strlen(path), x = strlen(EXT);
  return n > x && m_fncmp(path + n - x, EXT) == 0;
}


/* the "# Query: " header's value: \n and \\ as VS Code escapes them */
static void unescape (char *s) {
  char *o = s;
  for (; *s; s++) {
    if (s[0] == '\\' && s[1] == 'n') {
      *o++ = '\n';
      s++;
    }
    else if (s[0] == '\\' && s[1] == '\\') {
      *o++ = '\\';
      s++;
    }
    else *o++ = *s;
  }
  *o = '\0';
}


/*
** A .code-search file: the header ("# Query: x", "# Flags: RegExp ...",
** "# Including:", "# Excluding:", "# ContextLines:") down to the first
** empty line, then the results as they were written. They are shown as
** they are, not searched again (Rerun Search does that).
*/
void *searched_load (const char *path) {
  size_t len = 0;
  char *s = read_file(path, &len), *p, *nl;
  int head = 1;
  SEd *e;
  if (s == NULL) return NULL;
  e = se_new();
  e->ctx_on = 0;
  for (p = s; p; p = nl ? nl + 1 : NULL) {
    size_t n;
    nl = strchr(p, '\n');
    n = nl ? (size_t)(nl - p) : strlen(p);
    if (n > 0 && p[n - 1] == '\r') n--;
    if (!head) {
      vec_push(&e->line, xstrndup(p, n));
      continue;
    }
    if (n == 0) {
      head = 0;
      continue;
    }
    if (n > 2 && p[0] == '#' && p[1] == ' ') {
      char key[32], val[256];
      const char *c = p + 2, *colon = memchr(c, ':', n - 2);
      size_t kl, vl;
      if (colon == NULL || (size_t)(colon - p) + 1 >= n || colon[1] != ' ') continue;
      kl = (size_t)(colon - c);
      vl = n - (size_t)(colon + 2 - p);
      snprintf(key, sizeof(key), "%.*s", (int)kl, c);
      snprintf(val, sizeof(val), "%.*s", (int)vl, colon + 2);
      if (strcmp(key, "Query") == 0) {
        unescape(val);
        snprintf(e->f[SE_FIND], sizeof(e->f[SE_FIND]), "%s", val);
      }
      else if (strcmp(key, "Including") == 0) snprintf(e->f[SE_INC], sizeof(e->f[SE_INC]), "%s", val);
      else if (strcmp(key, "Excluding") == 0) snprintf(e->f[SE_EXC], sizeof(e->f[SE_EXC]), "%s", val);
      else if (strcmp(key, "ContextLines") == 0) {
        e->ctx_on = atoi(val) > 0;
        if (e->ctx_on) snprintf(e->f[SE_CTX], sizeof(e->f[SE_CTX]), "%d", atoi(val));
      }
      else if (strcmp(key, "Flags") == 0) {
        e->regex = strstr(val, "RegExp") != NULL;
        e->mcase = strstr(val, "CaseSensitive") != NULL;
        e->word = strstr(val, "WordMatch") != NULL;
        e->noexcl = strstr(val, "IgnoreExcludeSettings") != NULL;
      }
    }
  }
  free(s);
  e->more = e->f[SE_INC][0] || e->f[SE_EXC][0];
  e->path = xstrdup(path);
  e->in = -1;
  set_ran(e);
  return e;
}


void searched_close (void *page) {
  SEd *e = (SEd *)page, **pp;
  if (e == NULL) return;
  for (pp = &g_all; *pp; pp = &(*pp)->next)
    if (*pp == e) {
      *pp = e->next;
      break;
    }
  job_free(e->job);
  vec_free(&e->line);
  re_free(e->hre);
  free(e->path);
  free(e->go);
  free(e);
}


const char *searched_title (void *page) {
  return page ? ((SEd *)page)->title : "Search";
}


const char *searched_path (void *page) {
  return page ? ((SEd *)page)->path : NULL;
}


int searched_busy (void) {
  SEd *e;
  for (e = g_all; e; e = e->next)
    if (e->job || e->pending) return 1;
  return 0;
}


int searched_idle (void) {
  SEd *e;
  int r = 0;
  for (e = g_all; e; e = e->next) {
    if (e->pending && opt.search_on_type && os_now_us() - e->changed >= DELAY) {
      run(e);
      r = 1;
    }
    r |= job_poll(e);
  }
  return r;
}


void searched_stop (void) {
  SEd *e;
  for (e = g_all; e; e = e->next) {
    job_free(e->job);
    e->job = NULL;
  }
}

/* }================================================================== */


/*
** {==================================================================
** Saving: a .code-search file
** ===================================================================
*/

/* VS Code's header (serializeSearchConfiguration), an empty line after it */
static void header (const SEd *e, Buf *b) {
  const char *s;
  int ctx = ctx_lines(e);
  buf_puts(b, "# Query: ");
  for (s = e->f[SE_FIND]; *s; s++) {
    if (*s == '\\') buf_puts(b, "\\\\");
    else if (*s == '\n') buf_puts(b, "\\n");
    else buf_putc(b, *s);
  }
  buf_putc(b, '\n');
  if (e->mcase || e->word || e->regex || e->noexcl) {
    const char *sep = "";
    buf_puts(b, "# Flags: ");
    if (e->mcase) buf_printf(b, "%sCaseSensitive", sep), sep = " ";
    if (e->word) buf_printf(b, "%sWordMatch", sep), sep = " ";
    if (e->regex) buf_printf(b, "%sRegExp", sep), sep = " ";
    if (e->noexcl) buf_printf(b, "%sIgnoreExcludeSettings", sep);
    buf_putc(b, '\n');
  }
  if (e->f[SE_INC][0]) buf_printf(b, "# Including: %s\n", e->f[SE_INC]);
  if (e->f[SE_EXC][0]) buf_printf(b, "# Excluding: %s\n", e->f[SE_EXC]);
  if (ctx) buf_printf(b, "# ContextLines: %d\n", ctx);
  buf_putc(b, '\n');
}


/* a file name from the query, for Save As to offer */
static char *save_name (const SEd *e) {
  char name[80];
  size_t i, n = 0;
  for (i = 0; e->hq[i] && n + 1 < 60; i++) {
    char c = e->hq[i];
    name[n++] = (strchr("\\/:*?\"<>|\t\n", c) || (unsigned char)c < 32) ? '_' : c;
  }
  name[n] = '\0';
  if (n == 0) snprintf(name, sizeof(name), "Untitled");
  strcat(name, EXT);
  return path_join(side_root(), name);
}


int searched_save (void *page, int as) {
  SEd *e = (SEd *)page;
  Buf b;
  size_t i;
  int fd;
  if (e == NULL) return -1;
  if (as || e->path == NULL) {
    char *init = e->path ? xstrdup(e->path) : save_name(e);
    char *name = ask_text("Save As: the path of the file", init);
    free(init);
    if (name == NULL || *name == '\0') {
      free(name);
      return -1;
    }
    if (!path_is_sep(name[0]) && !(name[0] && name[1] == ':')) {
      char *full = path_join(side_root(), name);
      free(name);
      name = full;
    }
    if (!searched_is_file(name)) {	/* VS Code's dialog only offers .code-search */
      char *full = xstrcat3(name, EXT, "");
      free(name);
      name = full;
    }
    free(e->path);
    e->path = name;
  }
  buf_init(&b);
  header(e, &b);
  for (i = 0; i < e->line.n; i++) {
    if (i) buf_putc(&b, '\n');
    buf_puts(&b, e->line.v[i]);
  }
  fd = os_open(e->path, OS_WRITE);
  if (fd < 0) {
    toast(1, "Unable to write '%s'", path_basename(e->path));
    buf_free(&b);
    return -1;
  }
  os_write(fd, b.s ? b.s : "", b.len);
  os_close(fd);
  buf_free(&b);
  set_title(e);
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** The results: what a line is, where it goes
** ===================================================================
*/

/* "path:" of a file's results (not indented, ends in ':') */
static int is_file_line (const char *s) {
  size_t n = strlen(s);
  return n > 1 && s[0] != ' ' && s[0] != '\t' && s[n - 1] == ':';
}


/*
** A result line: "  12: text" (a match) or "  12  text" (context). The
** bytes before its text, 0 when it is not one; its line number, and
** whether it matched.
*/
static size_t result_prefix (const char *s, size_t *line, int *match) {
  size_t i = 0, n = 0;
  while (s[i] == ' ') i++;
  if (i == 0 || s[i] < '0' || s[i] > '9') return 0;
  for (; s[i] >= '0' && s[i] <= '9'; i++) n = n * 10 + (size_t)(s[i] - '0');
  if (s[i] == ':' && s[i + 1] == ' ') *match = 1;
  else if (s[i] == ' ' && s[i + 1] == ' ') *match = 0;
  else return 0;
  *line = n;
  return i + 2;
}


/* the path of rel ('/' between) from the folders */
static char *resolve (const char *rel) {
  char *r, *c, *p = NULL;
  int k;
  if (path_is_sep(rel[0]) || (rel[0] && rel[1] == ':')) return xstrdup(rel);
  for (k = 0; ws_count() > 1 && k < ws_count() && p == NULL; k++) {	/* "folder/rel" in a workspace */
    const char *nm = ws_folder_name(k);
    size_t n = strlen(nm);
    if (strncmp(rel, nm, n) == 0 && rel[n] == '/') p = xstrcat3(ws_folder(k), MMC_SEPS, rel + n + 1);
  }
  r = p ? p : path_join(side_root(), rel);
  for (c = r; *c; c++)
    if (*c == '/') *c = MMC_SEP;
  return r;
}


/*
** Where line y (byte x) of the results goes: its file at its line, the match
** under the cursor selected (else the line's first). 0: not a result.
*/
static int location (SEd *e, size_t y, size_t x, SideAct *act) {
  const char *s = line_of(e, y), *file = NULL;
  size_t pre, ln = 0, col = 0, len = 0, i;
  int match = 0;
  if (is_file_line(s) && y > 0) {
    file = s;
    ln = 1;
  }
  else if ((pre = result_prefix(s, &ln, &match)) > 0) {
    for (i = y; i-- > 0 && file == NULL;)
      if (is_file_line(line_of(e, i))) file = line_of(e, i);
    if (file == NULL) return 0;
    col = x > pre ? x - pre : 0;
    if (match && e->hq[0] && !(e->hregex && e->hre == NULL)) {
      const char *t = s + pre;
      size_t n = strlen(t), at, ml, first = n, flen = 0;
      for (at = search_find(e->hq, e->hcase, e->hword, e->hre, t, n, 0, &ml); at < n;
           at = search_find(e->hq, e->hcase, e->hword, e->hre, t, n, at + (ml ? ml : 1), &ml)) {
        if (first == n || (x >= pre && col >= at && col <= at + ml)) {	/* the first, or the one under the cursor */
          first = at;
          flen = ml;
        }
        if (x >= pre && col >= at && col <= at + ml) break;
      }
      if (first < n && (x < pre || (col >= first && col <= first + flen))) {
        col = first;
        len = flen;
      }
    }
  }
  else return 0;
  free(e->go);
  e->go = xstrndup(file, strlen(file) - 1);
  {
    char *p = resolve(e->go);
    free(e->go);
    e->go = p;
  }
  act->what = SA_GO;
  act->path = e->go;
  act->line = ln;
  act->col = col;
  act->len = len;
  return 1;
}

/* }================================================================== */


/*
** {==================================================================
** Drawing
** ===================================================================
*/

/* the column byte x of s is drawn at (tabs expanded, as scr_text does) */
static size_t col_of (const char *s, size_t x) {
  size_t i = 0, col = 0, len;
  while (s[i] && i < x) {
    uint32_t cp = utf8_decode(s + i, strlen(s + i), &len);
    if (cp == '\t') col += TABW - col % TABW;
    else if (cp < 32 || cp == 127) col += 2;
    else col += (size_t)uc_width(cp);
    i += len;
  }
  return col;
}


/* the byte of s drawn at column c (the end of the line after it) */
static size_t byte_at (const char *s, size_t c) {
  size_t i = 0, col = 0, len;
  while (s[i]) {
    uint32_t cp = utf8_decode(s + i, strlen(s + i), &len);
    size_t cw = cp == '\t' ? TABW - col % TABW : (cp < 32 || cp == 127) ? 2 : (size_t)uc_width(cp);
    if (col + cw > c) return (c - col) * 2 >= cw ? i + len : i;
    col += cw;
    i += len;
  }
  return i;
}


static void sel_range (const SEd *e, size_t *y0, size_t *x0, size_t *y1, size_t *x1) {
  if (e->ay < e->cy || (e->ay == e->cy && e->ax <= e->cx)) {
    *y0 = e->ay;
    *x0 = e->ax;
    *y1 = e->cy;
    *x1 = e->cx;
  }
  else {
    *y0 = e->cy;
    *x0 = e->cx;
    *y1 = e->ay;
    *x1 = e->ax;
  }
}


/* a line of the results, colored like VS Code's: paths as strings, line numbers as numbers, the matches lit */
static void draw_line (const SEd *e, size_t y, int x, int sy, int w, int cur) {
  const char *s = line_of(e, y);
  size_t n = strlen(s), i, col, len, pre, ln, right = e->left + (size_t)w;
  unsigned char *tok = (unsigned char *)xmalloc(n + 1), *bg = (unsigned char *)xmalloc(n + 1);
  int base = cur ? B_LINE : B_EDITOR, match = 0, eol_sel = 0;
  memset(tok, T_TEXT, n + 1);
  memset(bg, base, n + 1);
  if (is_file_line(s) && y > 0) memset(tok, T_STRING, n);
  else if ((pre = result_prefix(s, &ln, &match)) > 0) {
    memset(tok, T_NUMBER, pre);
    if (match && e->hq[0] && !(e->hregex && e->hre == NULL)) {
      size_t at, ml;
      for (at = search_find(e->hq, e->hcase, e->hword, e->hre, s + pre, n - pre, 0, &ml); at < n - pre;
           at = search_find(e->hq, e->hcase, e->hword, e->hre, s + pre, n - pre, at + (ml ? ml : 1), &ml))
        memset(bg + pre + at, B_MATCH, ml);
    }
  }
  if (e->sel) {
    size_t y0, x0, y1, x1;
    sel_range(e, &y0, &x0, &y1, &x1);
    if (y >= y0 && y <= y1) {
      size_t a = y == y0 ? x0 : 0, b = y == y1 ? x1 : n;
      if (a < b && b <= n) memset(bg + a, B_SEL, b - a);
      eol_sel = y < y1;	/* the line's end is selected too: one cell shows it */
    }
  }
  scr_fill(x, sy, w, TOK(T_TEXT, base));
  for (i = 0, col = 0; i < n && col < right;) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    size_t cw = cp == '\t' ? TABW - col % TABW : (cp < 32 || cp == 127) ? 2 : (size_t)uc_width(cp), k;
    int st = TOK(tok[i], bg[i]);
    for (k = 0; k < cw; k++) {
      size_t c = col + k;
      int sx = x + (int)(c - e->left);
      if (c < e->left || c >= right) continue;
      if (cp == '\t') scr_put(sx, sy, ' ', st);
      else if (cp < 32 || cp == 127) scr_put(sx, sy, k == 0 ? '^' : (cp == 127 ? '?' : cp + '@'), st);
      else if (k == 0 && col >= e->left && col + cw <= right) scr_put(sx, sy, cp, st);
      else if (k == 0 || col < e->left) scr_put(sx, sy, ' ', st);
    }
    col += cw;
    i += len;
  }
  if (eol_sel && col >= e->left && col < right) scr_put(x + (int)(col - e->left), sy, ' ', TOK(T_TEXT, B_SEL));
  free(tok);
  free(bg);
}


/* an input box: its text or the hint; the cursor when it has the keys */
static void draw_box (const SEd *e, int x, int y, int w, int which, const char *hint, int focus, int right) {
  const char *s = e->f[which];
  if (w < 3) return;
  scr_fill(x, y, w, S_INPUT);
  if (s[0]) scr_putsw(x + 1, y, w - 2 - right, s, S_INPUT_ON);
  else scr_putsw(x + 1, y, w - 2 - right, hint, S_INPUT_HINT);
  if (focus && e->in == which) {
    int cx = x + 1 + (int)str_cols(s);
    scr_cursor(cx < x + w - 1 - right ? cx : x + w - 1 - right, y);
  }
}


/* the scrollbar at the right of the results */
static void draw_bar (int x, int y, int h, size_t total, size_t top) {
  int len, at;
  size_t shown = (size_t)h;
  if (h < 2 || total <= shown) return;
  len = (int)((double)shown * h / (double)total + 0.5);
  if (len < 1) len = 1;
  if (len > h - 1) len = h - 1;
  at = (int)((double)top * (h - len) / (double)(total - shown) + 0.5);
  if (at < 0) at = 0;
  if (at > h - len) at = h - len;
  for (len += at; at < len; at++) scr_put_rgb(x, y + at, 0x2590, ui_color(C_THUMB), ui_color(C_EDITOR_BG), 0);
}


/* the cursor's line and column in view */
static void reveal (SEd *e) {
  size_t c = col_of(line_of(e, e->cy), e->cx), h = e->rh > 0 ? (size_t)e->rh : 1, w = e->tw > 0 ? (size_t)e->tw : 1;
  if (e->cy < e->top) e->top = e->cy;
  if (e->cy >= e->top + h) e->top = e->cy - h + 1;
  if (c < e->left) e->left = c;
  if (c >= e->left + w) e->left = c - w + 1;
}


void searched_draw (void *page, int x, int y, int w, int h, int focus) {
  SEd *e = (SEd *)page;
  int row, ry;
  scr_box(x, y, w, h, S_TEXT);
  if (e == NULL || h < 3 || w < 16) return;
  e->x = x;
  e->y = y;
  e->w = w;
  /* the query box and its toggles, the context lines next to it */
  e->bx = 2;
  e->bw = w - 12 > 10 ? w - 12 : 10;
  draw_box(e, x + e->bx, y, e->bw, SE_FIND, "Search", focus, 7);
  scr_put(x + e->bx + e->bw - 7, y, 0xEAB1, e->mcase ? S_TOGGLE_ON : S_INPUT);	/* Match Case */
  scr_put(x + e->bx + e->bw - 5, y, 0xEB7E, e->word ? S_TOGGLE_ON : S_INPUT);	/* Match Whole Word */
  scr_put(x + e->bx + e->bw - 3, y, 0xEB38, e->regex ? S_TOGGLE_ON : S_INPUT);	/* Use Regular Expression */
  e->ctx_x = e->bx + e->bw + 1;
  if (e->ctx_x + 7 <= w) {
    scr_put(x + e->ctx_x, y, 0xEB85, e->ctx_on ? S_TOGGLE_ON : S_TEXT);	/* Toggle Context Lines */
    if (e->ctx_on) draw_box(e, x + e->ctx_x + 2, y, 5, SE_CTX, "", focus, 0);
  }
  ry = 1;
  e->dots_y = ry;	/* "...": Toggle Search Details; the progress on its left */
  scr_put(x + e->bx + e->bw - 2, y + ry, 0xEA7C, e->more ? S_TOGGLE_ON : S_GUTTER);
  if (e->job) {
    char msg[80];
    snprintf(msg, sizeof(msg), "%s Searching...", ui_spinner());
    scr_putsw(x + e->bx, y + ry, e->bw - 4, msg, S_GUTTER);
  }
  ry++;
  e->inc_y = e->exc_y = -1;
  if (e->more && ry + 5 < h) {
    scr_puts(x + e->bx, y + ry++, "files to include", S_GUTTER);
    e->inc_y = ry;
    draw_box(e, x + e->bx, y + ry++, e->bw, SE_INC, "e.g. *.c, src/**", focus, 0);
    scr_puts(x + e->bx, y + ry++, "files to exclude", S_GUTTER);
    e->exc_y = ry;
    draw_box(e, x + e->bx, y + ry++, e->bw, SE_EXC, "e.g. build, *.min.js", focus, 0);
  }
  /* the results, VS Code's text of them */
  e->ry = ry;
  e->rh = h - ry;
  e->tx = 2;
  e->tw = w - 3;
  if (e->rh < 1) return;
  if (e->reveal) {
    reveal(e);
    e->reveal = 0;
  }
  if (e->top >= nlines(e)) e->top = nlines(e) - 1;
  for (row = 0; row < e->rh; row++) {
    size_t k = e->top + (size_t)row;
    int cur = k == e->cy && focus && e->in < 0;
    if (k >= nlines(e)) break;
    if (cur) scr_fill(x, y + ry + row, w, TOK(T_TEXT, B_LINE));
    draw_line(e, k, x + e->tx, y + ry + row, e->tw, cur);
  }
  draw_bar(x + w - 1, y + ry, e->rh, nlines(e), e->top);
  if (focus && e->in < 0 && e->cy >= e->top && e->cy < e->top + (size_t)e->rh) {
    size_t c = col_of(line_of(e, e->cy), e->cx);
    if (c >= e->left && c < e->left + (size_t)e->tw) scr_cursor(x + e->tx + (int)(c - e->left), y + ry + (int)(e->cy - e->top));
  }
}

/* }================================================================== */


/*
** {==================================================================
** Keys and mouse
** ===================================================================
*/

static void typed (SEd *e) {
  e->pending = 1;
  e->changed = os_now_us();
}


/* Tab / Shift+Tab: the boxes that show, then the results */
static void next_box (SEd *e, int back) {
  int order[SE_N + 1], n = 0, i, at = 0;
  order[n++] = SE_FIND;
  if (e->ctx_on) order[n++] = SE_CTX;
  if (e->more) {
    order[n++] = SE_INC;
    order[n++] = SE_EXC;
  }
  order[n++] = -1;	/* the results */
  for (i = 0; i < n; i++)
    if (order[i] == e->in) at = i;
  e->in = order[(at + (back ? n - 1 : 1)) % n];
}


/* a key in the box with the keys; 0: not one for it */
static int box_key (SEd *e, int k) {
  char *s = e->f[e->in];
  size_t len = strlen(s), max = e->in == SE_CTX ? 3 : sizeof(e->f[0]) - 4;
  int code = KEY_CODE(k);
  if (code == K_BS) {
    while (len > 0 && ((unsigned char)s[len - 1] & 0xC0) == 0x80) len--;
    if (len > 0) s[len - 1] = '\0';
  }
  else if (IS_PASTE(k)) {
    Buf b;
    size_t i;
    buf_init(&b);
    paste_take(k, &b);
    for (i = 0; i < b.len && b.s[i] != '\n' && len < max; i++)
      if (e->in != SE_CTX || (b.s[i] >= '0' && b.s[i] <= '9')) s[len++] = b.s[i];
    s[len] = '\0';
    buf_free(&b);
  }
  else if (IS_TEXT(k) && len < max && (e->in != SE_CTX || (k >= '0' && k <= '9'))) {
    len += (size_t)utf8_encode((uint32_t)k, s + len);
    s[len] = '\0';
  }
  else return 0;
  typed(e);
  return 1;
}


static void set_context (SEd *e, int n) {
  if (n < 0) n = 0;
  if (n > 100) n = 100;
  if (n > 0) snprintf(e->f[SE_CTX], sizeof(e->f[SE_CTX]), "%d", n);
  e->ctx_on = n > 0;
  if (!e->ctx_on && e->in == SE_CTX) e->in = SE_FIND;
  typed(e);
}


/* Search Editor: Delete File Results - the block of the file the cursor is in */
static void delete_file (SEd *e) {
  size_t a = e->cy, b, i;
  if (e->line.n == 0) return;
  while (a > 0 && !is_file_line(line_of(e, a))) a--;
  if (a == 0) return;
  for (b = a + 1; b < e->line.n && !is_file_line(line_of(e, b)); b++) ;
  for (i = a; i < b; i++) free(e->line.v[i]);
  memmove(e->line.v + a, e->line.v + b, (e->line.n - b + 1) * sizeof(char *));	/* the NULL after them too */
  e->line.n -= b - a;
  e->cy = a < e->line.n ? a : (e->line.n ? e->line.n - 1 : 0);
  e->cx = 0;
  e->sel = 0;
  e->reveal = 1;
}


/* the selection (or the cursor's line, editor.emptySelectionClipboard) to the clipboard */
static void copy (SEd *e) {
  Buf b;
  buf_init(&b);
  if (e->sel && (e->ay != e->cy || e->ax != e->cx)) {
    size_t y0, x0, y1, x1, y;
    sel_range(e, &y0, &x0, &y1, &x1);
    for (y = y0; y <= y1; y++) {
      const char *s = line_of(e, y);
      size_t n = strlen(s), a = y == y0 ? x0 : 0, z = y == y1 ? x1 : n;
      if (a > n) a = n;
      if (z > n) z = n;
      if (a < z) buf_putn(&b, s + a, z - a);
      if (y < y1) buf_putc(&b, '\n');
    }
  }
  else {
    buf_puts(&b, line_of(e, e->cy));
    buf_putc(&b, '\n');
  }
  clip_set(b.s ? b.s : "", b.len);
  buf_free(&b);
}


/* the cursor to line y, byte x; shift: the selection grows */
static void move (SEd *e, size_t y, size_t x, int shift, int keep_goal) {
  size_t n = nlines(e);
  if (y >= n) y = n - 1;
  if (x > strlen(line_of(e, y))) x = strlen(line_of(e, y));
  while (x > 0 && ((unsigned char)line_of(e, y)[x] & 0xC0) == 0x80) x--;
  if (shift && !e->sel) {
    e->ay = e->cy;
    e->ax = e->cx;
    e->sel = 1;
  }
  else if (!shift) e->sel = 0;
  e->cy = y;
  e->cx = x;
  if (!keep_goal) e->goal = col_of(line_of(e, y), x);
  e->reveal = 1;
}


/* the results' keys: moving, selecting, copying, going to a result */
static void results_key (SEd *e, int k, SideAct *act) {
  int code = KEY_CODE(k), shift = (k & KM_SHIFT) != 0, ctrl = (k & KM_CTRL) != 0;
  const char *s = line_of(e, e->cy);
  size_t h = e->rh > 1 ? (size_t)e->rh - 1 : 1, len;
  switch (code) {
    case K_UP:
      if (e->cy > 0) move(e, e->cy - 1, byte_at(line_of(e, e->cy - 1), e->goal), shift, 1);
      return;
    case K_DOWN:
      if (e->cy + 1 < nlines(e)) move(e, e->cy + 1, byte_at(line_of(e, e->cy + 1), e->goal), shift, 1);
      return;
    case K_LEFT:
      if (e->sel && !shift) move(e, e->cy < e->ay || (e->cy == e->ay && e->cx < e->ax) ? e->cy : e->ay,
                                 e->cy < e->ay || (e->cy == e->ay && e->cx < e->ax) ? e->cx : e->ax, 0, 0);
      else if (e->cx > 0) {
        size_t x = e->cx - 1;
        while (x > 0 && ((unsigned char)s[x] & 0xC0) == 0x80) x--;
        move(e, e->cy, x, shift, 0);
      }
      else if (e->cy > 0) move(e, e->cy - 1, strlen(line_of(e, e->cy - 1)), shift, 0);
      return;
    case K_RIGHT:
      if (e->sel && !shift) move(e, e->cy > e->ay || (e->cy == e->ay && e->cx > e->ax) ? e->cy : e->ay,
                                 e->cy > e->ay || (e->cy == e->ay && e->cx > e->ax) ? e->cx : e->ax, 0, 0);
      else if (s[e->cx]) {
        utf8_decode(s + e->cx, strlen(s + e->cx), &len);
        move(e, e->cy, e->cx + len, shift, 0);
      }
      else if (e->cy + 1 < nlines(e)) move(e, e->cy + 1, 0, shift, 0);
      return;
    case K_HOME: move(e, ctrl ? 0 : e->cy, 0, shift, 0); return;
    case K_END:
      if (ctrl) move(e, nlines(e) - 1, strlen(line_of(e, nlines(e) - 1)), shift, 0);
      else move(e, e->cy, strlen(s), shift, 0);
      return;
    case K_PGUP: move(e, e->cy > h ? e->cy - h : 0, byte_at(line_of(e, e->cy > h ? e->cy - h : 0), e->goal), shift, 1); return;
    case K_PGDN: move(e, e->cy + h, byte_at(line_of(e, e->cy + h < nlines(e) ? e->cy + h : nlines(e) - 1), e->goal), shift, 1); return;
    case K_ENTER: case K_F12:	/* to the result, like F12 (Go to Definition) on it in VS Code */
      location(e, e->cy, e->cx, act);
      return;
    case K_ESC:	/* Search Editor: Focus Search Editor Input */
      if (e->sel) e->sel = 0;
      else e->in = SE_FIND;
      return;
  }
  if (k == CTRL('a')) {	/* Select All */
    e->ay = e->ax = 0;
    e->sel = 1;
    e->cy = nlines(e) - 1;
    e->cx = strlen(line_of(e, e->cy));
    e->reveal = 1;
  }
  else if (k == CTRL('c') || k == (K_INS | KM_CTRL)) copy(e);
  /* the rest (typing, Backspace, Delete, a paste) does nothing: the results are read only */
}


void searched_command (void *page, int cmd, SideAct *act) {
  SEd *e = (SEd *)page;
  (void)act;
  if (e == NULL) return;
  switch (cmd) {
    case CMD_SEARCHED_RERUN: run(e); break;
    case CMD_SEARCHED_CONTEXT: set_context(e, e->ctx_on ? 0 : (atoi(e->f[SE_CTX]) > 0 ? atoi(e->f[SE_CTX]) : 1)); break;
    case CMD_SEARCHED_MORE_CONTEXT: set_context(e, ctx_lines(e) + 1); break;
    case CMD_SEARCHED_LESS_CONTEXT: set_context(e, ctx_lines(e) - 1); break;
    case CMD_SEARCHED_FOCUS: e->in = SE_FIND; break;
    case CMD_SEARCHED_DELETE_FILE: delete_file(e); break;
  }
}


/* the keys the Search Editor has before the editor's (F12 is Go to Definition there) */
int searched_takes (int k) {
  return KEY_CODE(k) == K_F12 && !(k & (KM_CTRL | KM_ALT | KM_SHIFT));
}


void searched_key (void *page, int k, SideAct *act) {
  SEd *e = (SEd *)page;
  int code = KEY_CODE(k);
  if (e == NULL) return;
  if (k == ('c' | KM_ALT)) {	/* Alt+C, Alt+W, Alt+R: the toggles, as in the Search view */
    e->mcase = !e->mcase;
    typed(e);
  }
  else if (k == ('w' | KM_ALT)) {
    e->word = !e->word;
    typed(e);
  }
  else if (k == ('r' | KM_ALT)) {
    e->regex = !e->regex;
    typed(e);
  }
  else if (k == ('l' | KM_ALT)) searched_command(e, CMD_SEARCHED_CONTEXT, act);	/* Alt+L: Toggle Context Lines */
  else if (k == ('=' | KM_ALT) || k == ('+' | KM_ALT)) searched_command(e, CMD_SEARCHED_MORE_CONTEXT, act);
  else if (k == ('-' | KM_ALT)) searched_command(e, CMD_SEARCHED_LESS_CONTEXT, act);
  else if (k == ('r' | KM_CTRL | KM_SHIFT) || k == ('R' | KM_CTRL | KM_SHIFT)) run(e);	/* Ctrl+Shift+R: Rerun Search */
  else if (k == ('j' | KM_CTRL | KM_SHIFT) || k == ('J' | KM_CTRL | KM_SHIFT)) {	/* Toggle Search Details */
    e->more = !e->more;
    if (e->more) e->in = SE_INC;
    else if (e->in == SE_INC || e->in == SE_EXC) e->in = SE_FIND;
  }
  else if (code == K_BS && (k & KM_CTRL) && (k & KM_SHIFT)) delete_file(e);	/* Ctrl+Shift+Backspace */
  else if (code == K_TAB && !(k & (KM_CTRL | KM_ALT))) next_box(e, (k & KM_SHIFT) != 0);
  else if (e->in >= 0) {
    if (code == K_ENTER) run(e);	/* the search at once */
    else if (code == K_DOWN && e->in == SE_FIND && !e->more) e->in = -1;
    else if (code == K_F12) e->in = -1;
    else box_key(e, k);
  }
  else results_key(e, k, act);
}


/* the byte of line y under screen column mx */
static size_t byte_under (const SEd *e, size_t y, int mx) {
  int c = mx - (e->x + e->tx);
  return byte_at(line_of(e, y), e->left + (size_t)(c > 0 ? c : 0));
}


void searched_mouse (void *page, const Mouse *m, SideAct *act) {
  SEd *e = (SEd *)page;
  int row, col;
  if (e == NULL) return;
  row = m->y - e->y;
  col = m->x - e->x;
  if (m->wheel) {
    size_t st = (size_t)wheel_step(m->mods), n = nlines(e), h = e->rh > 0 ? (size_t)e->rh : 1;
    if (m->wheel < 0) e->top = e->top > st ? e->top - st : 0;
    else if (n > h) {
      e->top += st;
      if (e->top > n - h) e->top = n - h;
    }
    return;
  }
  if (m->button != 0) return;
  if (m->drag && e->drag) {	/* the selection follows the mouse, the view scrolls at its edges */
    size_t y;
    if (row < e->ry && e->top > 0) e->top--;
    if (row >= e->ry + e->rh && e->top + (size_t)e->rh < nlines(e)) e->top++;
    row = row < e->ry ? e->ry : row >= e->ry + e->rh ? e->ry + e->rh - 1 : row;
    y = e->top + (size_t)(row - e->ry);
    if (y >= nlines(e)) y = nlines(e) - 1;
    move(e, y, byte_under(e, y, m->x), 1, 0);
    e->reveal = 0;
    return;
  }
  if (!m->press) {
    e->drag = 0;
    return;
  }
  if (m->drag) return;
  if (row == 0) {	/* the query box, its toggles, the context lines */
    int bx = e->bx + e->bw;
    if (col == bx - 7) e->mcase = !e->mcase, typed(e);
    else if (col == bx - 5) e->word = !e->word, typed(e);
    else if (col == bx - 3) e->regex = !e->regex, typed(e);
    else if (col == e->ctx_x) searched_command(e, CMD_SEARCHED_CONTEXT, act);
    else if (e->ctx_on && col >= e->ctx_x + 2 && col < e->ctx_x + 7) e->in = SE_CTX;
    else if (col >= e->bx && col < bx) e->in = SE_FIND;
    return;
  }
  if (row == e->dots_y) {
    if (col >= e->bx + e->bw - 3 && col <= e->bx + e->bw - 1) {
      e->more = !e->more;
      if (!e->more && (e->in == SE_INC || e->in == SE_EXC)) e->in = SE_FIND;
    }
    return;
  }
  if (row == e->inc_y || row == e->exc_y) {
    e->in = row == e->inc_y ? SE_INC : SE_EXC;
    return;
  }
  if (row >= e->ry && row < e->ry + e->rh) {	/* in the results: the cursor; a double click goes there */
    size_t y = e->top + (size_t)(row - e->ry);
    long long now = os_now_us();
    int dbl = now - e->click_t < 500000 && m->x == e->click_x && m->y == e->click_y;
    if (y >= nlines(e)) y = nlines(e) - 1;
    e->in = -1;
    move(e, y, byte_under(e, y, m->x), (m->mods & KM_SHIFT) != 0, 0);
    e->reveal = 0;
    e->drag = 1;
    e->click_t = dbl ? 0 : now;
    e->click_x = m->x;
    e->click_y = m->y;
    if (dbl) {	/* search.searchEditor.doubleClickBehaviour "gotoLocation" */
      e->drag = 0;
      location(e, e->cy, e->cx, act);
    }
  }
}

/* }================================================================== */
