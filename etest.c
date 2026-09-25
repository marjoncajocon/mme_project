/*
** etest.c - the Testing view (VS Code's Test Explorer)
**
** The tests of the folder are found by reading their files: Go
** (func TestXxx in *_test.go), Python (def test_ in test_*.py, pytest or
** unittest runs them), Rust (#[test] fns, cargo test) and a package.json
** "test" script. They run in the background, one run after the other;
** what they print goes to the OUTPUT view's "Test Results", and each
** test's result to the tree, the gutter by its line (▷ ✓ ✗) and, for a
** failure, the Problems at the line that failed.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


enum { FW_GO, FW_PY, FW_RUST, FW_JS };

typedef struct TTest {
  char *name;	/* TestAdd, test_sum, "test" (npm) */
  char *cls;	/* Python: the class it is in, or NULL */
  size_t line;	/* from 0 */
  int state;	/* TM_* */
  long ms;	/* how long it ran; -1: not said */
  char *msg;	/* why it failed */
  char *floc;	/* where: the file (native), NULL: its own */
  size_t fline;
} TTest;

typedef struct TFile {
  char *path;	/* native, real */
  char *rel;	/* from the folder, with '/' */
  int fw;
  TTest *t;
  int n;
  int open;	/* its tests shown in the tree */
} TFile;

static TFile *g_f;
static int g_nf;
static int g_found;	/* the folder was looked through */

#define CHAN	"Test Results"


/*
** {==================================================================
** Finding the tests
** ===================================================================
*/

static int ends (const char *s, const char *e) {
  size_t n = strlen(s), m = strlen(e);
  return n >= m && strcmp(s + n - m, e) == 0;
}


static int ident (int c) {
  return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}


/* the identifier at s, copied */
static char *ident_at (const char *s, size_t n) {
  size_t k = 0;
  char *r;
  while (k < n && ident((unsigned char)s[k])) k++;
  if (k == 0) return NULL;
  r = (char *)xmalloc(k + 1);
  memcpy(r, s, k);
  r[k] = '\0';
  return r;
}


static void add_test (TFile *f, char *name, const char *cls, size_t line) {
  TTest *t;
  f->t = (TTest *)xrealloc(f->t, (size_t)(f->n + 1) * sizeof(TTest));
  t = &f->t[f->n++];
  memset(t, 0, sizeof(*t));
  t->name = name;
  t->cls = cls ? xstrdup(cls) : NULL;
  t->line = line;
  t->state = TM_UNSET;
  t->ms = -1;
}


/* the framework of a file's name; -1: not a test file */
static int fw_of (const char *name) {
  if (ends(name, "_test.go")) return FW_GO;
  if ((strncmp(name, "test_", 5) == 0 && ends(name, ".py")) || ends(name, "_test.py")) return FW_PY;
  if (ends(name, ".rs")) return FW_RUST;
  if (strcmp(name, "package.json") == 0) return FW_JS;
  return -1;
}


/* the tests of a file's text */
static void parse (TFile *f, const char *s, size_t len) {
  size_t i = 0, line = 0;
  char *cls = NULL;
  int pending = 0;	/* Rust: #[test] seen, its fn to come */
  if (f->fw == FW_JS) {	/* package.json: its "test" script */
    Json *j = json_parse(s, len);
    const char *t = json_str(json_get(j, "scripts.test"), NULL);
    if (t && strstr(t, "no test specified") == NULL) {
      const char *at = strstr(s, "\"test\"");
      size_t k, ln = 0;
      for (k = 0; at && s + k < at; k++)
        if (s[k] == '\n') ln++;
      add_test(f, xstrdup("test"), NULL, ln);
    }
    json_free(j);
    return;
  }
  while (i < len) {
    size_t e = i, ind;
    const char *l = s + i;
    while (e < len && s[e] != '\n') e++;
    for (ind = 0; i + ind < e && (l[ind] == ' ' || l[ind] == '\t'); ind++) ;
    if (f->fw == FW_GO && strncmp(l, "func ", 5) == 0) {
      const char *nm = l + 5;
      size_t rest = e - i - 5;
      char *name = ident_at(nm, rest);
      if (name) {
        size_t k = strlen(name);
        int ok = 0;
        if (strncmp(name, "Test", 4) == 0 && !(name[4] >= 'a' && name[4] <= 'z'))
          ok = memchr(nm, '*', rest) && strstr(nm, "testing.T") && (size_t)(strstr(nm, "testing.T") - nm) < rest;
        else if (strncmp(name, "Fuzz", 4) == 0 && !(name[4] >= 'a' && name[4] <= 'z'))
          ok = strstr(nm, "testing.F") && (size_t)(strstr(nm, "testing.F") - nm) < rest;
        else if (strncmp(name, "Example", 7) == 0 && k < rest && nm[k] == '(' && k + 1 < rest && nm[k + 1] == ')')
          ok = 1;
        if (ok) add_test(f, name, NULL, line);
        else free(name);
      }
    }
    else if (f->fw == FW_PY) {
      if (ind == 0 && e > i && l[0] != '#' && l[0] != '@' && l[0] != '\r') {	/* at the left: out of a class */
        free(cls);
        cls = NULL;
        if (strncmp(l, "class ", 6) == 0) {
          char *c = ident_at(l + 6, e - i - 6);
          if (c && (strncmp(c, "Test", 4) == 0 || (strstr(l, "TestCase") && (size_t)(strstr(l, "TestCase") - l) < e - i)))
            cls = c;
          else free(c);
        }
      }
      {
        const char *d = l + ind;
        size_t rest = e - i - ind;
        if (rest > 6 && strncmp(d, "async ", 6) == 0) {
          d += 6;
          rest -= 6;
        }
        if (rest > 8 && strncmp(d, "def test", 8) == 0 && (ind == 0 || cls)) {
          char *name = ident_at(d + 4, rest - 4);
          if (name) add_test(f, name, ind == 0 ? NULL : cls, line);
        }
      }
    }
    else if (f->fw == FW_RUST) {
      const char *d = l + ind;
      size_t rest = e - i - ind;
      if (rest >= 7 && (strncmp(d, "#[test]", 7) == 0 || strncmp(d, "#[tokio::test", 13) == 0 ||
                        strncmp(d, "#[async_std::test", 17) == 0 || strncmp(d, "#[rstest", 8) == 0))
        pending = 1;
      else if (pending && rest > 0 && d[0] != '#' && !(rest > 1 && d[0] == '/' && d[1] == '/')) {
        const char *fn = NULL;
        size_t k;
        for (k = 0; k + 3 <= rest; k++)
          if (strncmp(d + k, "fn ", 3) == 0 && (k == 0 || d[k - 1] == ' ')) {
            fn = d + k + 3;
            break;
          }
        if (fn) {
          char *name = ident_at(fn, rest - (size_t)(fn - d));
          if (name) add_test(f, name, NULL, line);
        }
        pending = 0;
      }
    }
    i = e + 1;
    line++;
  }
  free(cls);
}


static void free_test (TTest *t) {
  free(t->name);
  free(t->cls);
  free(t->msg);
  free(t->floc);
}


static void free_file (TFile *f) {
  int i;
  for (i = 0; i < f->n; i++) free_test(&f->t[i]);
  free(f->t);
  free(f->path);
  free(f->rel);
}


static TFile *file_of (const char *path) {
  int i;
  for (i = 0; i < g_nf; i++)
    if (m_fncmp(g_f[i].path, path) == 0) return &g_f[i];
  return NULL;
}


/* rel with '/', from the folder; the base name when it is outside */
static char *rel_of (const char *path) {
  const char *root = side_root();
  char *real = os_realpath(root), *r, *c;
  const char *rt = real ? real : root;
  size_t n = strlen(rt);
  if (m_strnicmp(path, rt, n) == 0 && path_is_sep(path[n])) r = xstrdup(path + n + 1);
  else r = xstrdup(path_basename(path));
  free(real);
  for (c = r; *c; c++)
    if (*c == '\\') *c = '/';
  return r;
}


/* a file read again: its tests keep their results by name */
static void load_file (TFile *f) {
  size_t len;
  char *s = read_file(f->path, &len);
  TTest *old = f->t;
  int nold = f->n, i, k;
  f->t = NULL;
  f->n = 0;
  if (s) {
    if (f->fw != FW_RUST || strstr(s, "#[")) parse(f, s, len);
    free(s);
  }
  for (i = 0; i < f->n; i++)
    for (k = 0; k < nold; k++)
      if (old[k].name && strcmp(old[k].name, f->t[i].name) == 0) {
        f->t[i].state = old[k].state;
        f->t[i].ms = old[k].ms;
        f->t[i].msg = old[k].msg;
        f->t[i].floc = old[k].floc;
        f->t[i].fline = old[k].fline;
        old[k].msg = old[k].floc = NULL;
        break;
      }
  for (k = 0; k < nold; k++) free_test(&old[k]);
  free(old);
}


/* the file at path (real) known, read when it is new */
static TFile *add_file (const char *path, int fw) {
  TFile *f = file_of(path);
  if (f) return f;
  g_f = (TFile *)xrealloc(g_f, (size_t)(g_nf + 1) * sizeof(TFile));
  f = &g_f[g_nf++];
  memset(f, 0, sizeof(*f));
  f->path = xstrdup(path);
  f->rel = rel_of(path);
  f->fw = fw;
  load_file(f);
  return f;
}


static int cmp_file (const void *a, const void *b) {
  return strcmp(((const TFile *)a)->rel, ((const TFile *)b)->rel);
}


static const char *const skip_dir[] = {"node_modules", "target", "vendor", "dist", "build", "__pycache__",
                                       "mme-data", "venv", "env", "out", "obj", "bin", NULL};

static void walk (const char *dir, int depth, int *budget) {
  Vec v;
  size_t i;
  vec_init(&v);
  if (depth > 12 || *budget <= 0 || os_listdir(dir, &v) != 0) return;
  vec_sort(&v);
  for (i = 0; i < v.n && *budget > 0; i++) {
    const char *nm = v.v[i];
    char *full;
    OsStat st;
    int k, skip = 0, fw;
    (*budget)--;
    if (nm[0] == '.') continue;
    full = path_join(dir, nm);
    if (os_stat(full, &st) != 0) {
      free(full);
      continue;
    }
    if (st.is_dir) {
      for (k = 0; skip_dir[k]; k++)
        if (strcmp(nm, skip_dir[k]) == 0) skip = 1;
      if (!skip) walk(full, depth + 1, budget);
    }
    else if ((fw = fw_of(nm)) >= 0 && st.size < (4 << 20)) {
      char *real = os_realpath(full);
      TFile *f = add_file(real ? real : full, fw);
      load_file(f);	/* known already: read again */
      free(real);
    }
    free(full);
  }
  vec_free(&v);
}


/* Test: Refresh Tests, and the first time the view shows */
static void discover (void) {
  int budget = 50000, i, n = 0;
  walk(side_root(), 0, &budget);
  g_found = 1;
  for (i = 0; i < g_nf; i++) {	/* files that went: out */
    OsStat st;
    if (os_stat(g_f[i].path, &st) != 0 || !st.exists) free_file(&g_f[i]);
    else g_f[n++] = g_f[i];
  }
  g_nf = n;
  qsort(g_f, (size_t)g_nf, sizeof(TFile), cmp_file);
}


void test_saved (const char *path) {
  TFile *f = file_of(path);
  if (f) load_file(f);
  else if (g_found && fw_of(path_basename(path)) >= 0) {
    add_file(path, fw_of(path_basename(path)));
    qsort(g_f, (size_t)g_nf, sizeof(TFile), cmp_file);
  }
}


void test_show (void) {
  if (!g_found) discover();
}

/* }================================================================== */


/*
** {==================================================================
** Running them
** ===================================================================
*/

typedef struct Run {
  int fw;
  char *cwd;
  Vec argv;
  Vec keys;	/* "path\n name" of the tests it is for */
  char *cov;	/* with coverage: the file it is written to (LCOV, Go's profile); NULL: none */
  char *covdata;	/* coverage.py's data file */
  long long t0;	/* when it started (npm's coverage/lcov.info must be newer) */
} Run;

static Run *g_q;	/* waiting */
static int g_nq;
static Run g_run;	/* running, when g_on */
static int g_on;
static OsProc g_proc;
static long g_pid;
static int g_fd = -1;
static Buf g_out;
static Run *g_last;	/* Test: Rerun Last Run: what was asked last */
static int g_nlast;
static Vec g_dpaths;	/* the files whose Problems have a test's failure */


static char *key_of (const TFile *f, const TTest *t) {
  return xstrcat3(f->path, "\n", t->name);
}


static TTest *test_of_key (const char *key, TFile **pf) {
  const char *nl = strchr(key, '\n');
  char *path;
  TFile *f;
  int i;
  if (nl == NULL) return NULL;
  path = xstrndup(key, (size_t)(nl - key));
  f = file_of(path);
  free(path);
  for (i = 0; f && i < f->n; i++)
    if (strcmp(f->t[i].name, nl + 1) == 0) {
      if (pf) *pf = f;
      return &f->t[i];
    }
  return NULL;
}


static void run_free (Run *r) {
  free(r->cov);
  free(r->covdata);
  free(r->cwd);
  vec_free(&r->argv);
  vec_free(&r->keys);
  memset(r, 0, sizeof(*r));
}


static void run_copy (Run *to, const Run *from) {
  size_t i;
  memset(to, 0, sizeof(*to));
  to->fw = from->fw;
  to->cwd = xstrdup(from->cwd);
  to->cov = from->cov ? xstrdup(from->cov) : NULL;
  to->covdata = from->covdata ? xstrdup(from->covdata) : NULL;
  vec_init(&to->argv);
  vec_init(&to->keys);
  for (i = 0; i < from->argv.n; i++) vec_push(&to->argv, xstrdup(from->argv.v[i]));
  for (i = 0; i < from->keys.n; i++) vec_push(&to->keys, xstrdup(from->keys.v[i]));
}


static void enqueue (Run *r) {
  size_t i;
  for (i = 0; i < r->keys.n; i++) {
    TTest *t = test_of_key(r->keys.v[i], NULL);
    if (t) t->state = TM_QUEUED;
  }
  g_q = (Run *)xrealloc(g_q, (size_t)(g_nq + 1) * sizeof(Run));
  g_q[g_nq++] = *r;
}


/* where Cargo.toml is, from the file's folder up; NULL: none */
static char *crate_dir (const char *path) {
  char *d = path_dirname(path);
  int k;
  for (k = 0; k < 16 && d; k++) {
    char *c = path_join(d, "Cargo.toml"), *up;
    OsStat st;
    int there = os_stat(c, &st) == 0 && st.exists;
    free(c);
    if (there) return d;
    up = path_dirname(d);
    if (up == NULL || strcmp(up, d) == 0) {
      free(up);
      break;
    }
    free(d);
    d = up;
  }
  free(d);
  return NULL;
}


static char *python (void) {
  char *p = find_program("python3");
  return p ? p : find_program("python");
}


/* the module of a Python file: "pkg.test_x" from pkg/test_x.py */
static char *py_module (const char *rel) {
  char *m = xstrdup(rel), *c;
  if (ends(m, ".py")) m[strlen(m) - 3] = '\0';
  for (c = m; *c; c++)
    if (*c == '/') *c = '.';
  return m;
}


/*
** {==================================================================
** Test coverage, VS Code's: "Test: Run All Tests with Coverage" runs the
** tests with the tool's own coverage (go test -coverprofile, coverage.py,
** cargo llvm-cov; npm's script writes coverage/lcov.info), and what they
** covered is read: the lines of each file, covered or not. The editor
** colors the line numbers by it (and the lines too, with inline coverage),
** the view lists each file with the part of its lines covered.
** ===================================================================
*/

typedef struct CovFile {
  char *path;	/* native */
  char *real;	/* os_realpath's, to find it again from an editor */
  char *rel;	/* shown */
  unsigned char *st;	/* each line: 0 no code, 1 covered, 2 not */
  size_t nst;
  size_t hit, total;	/* lines covered, lines with code */
} CovFile;

static CovFile *g_cov;
static int g_ncov;
static int g_cov_on;	/* coverage is shown */
static int g_cov_inline;	/* Test: Toggle Inline Coverage: the lines too */
static int g_cover;	/* the runs planned now take coverage */
static unsigned g_cov_seq;


static void cov_clear (void) {
  int i;
  for (i = 0; i < g_ncov; i++) {
    free(g_cov[i].path);
    free(g_cov[i].real);
    free(g_cov[i].rel);
    free(g_cov[i].st);
  }
  free(g_cov);
  g_cov = NULL;
  g_ncov = 0;
  g_cov_on = 0;
}


/* a path from a tool: absolute? ("C:\x", "/x"); and in the system's separators */
static int cov_abs (const char *s) {
  return path_is_sep(s[0]) || s[0] == '/' || (s[0] && s[1] == ':');
}


static void cov_native (char *s) {
#ifdef _WIN32
  for (; *s; s++)
    if (*s == '/') *s = '\\';
#else
  (void)s;
#endif
}


/* the file's entry (native path, or relative to base), made the first time */
static CovFile *cov_file (const char *name, const char *base) {
  char *p = cov_abs(name) || base == NULL ? xstrdup(name) : path_join(base, name), *real, *c;
  CovFile *f;
  int i;
  cov_native(p);
  real = os_realpath(p);
  if (real == NULL) real = xstrdup(p);
  for (i = 0; i < g_ncov; i++)
    if (m_fncmp(g_cov[i].real, real) == 0) {
      free(p);
      free(real);
      return &g_cov[i];
    }
  g_cov = (CovFile *)xrealloc(g_cov, (size_t)(g_ncov + 1) * sizeof(CovFile));
  f = &g_cov[g_ncov++];
  memset(f, 0, sizeof(*f));
  f->path = p;
  f->real = real;
  f->rel = rel_of(p);
  for (c = f->rel; *c; c++)
    if (*c == '\\') *c = '/';
  return f;
}


/* line (from 1) ran count times; covered once is covered */
static void cov_line (CovFile *f, size_t line, long count) {
  if (line == 0 || line > 10000000) return;
  if (line > f->nst) {
    f->st = (unsigned char *)xrealloc(f->st, line);
    memset(f->st + f->nst, 0, line - f->nst);
    f->nst = line;
  }
  if (count > 0) f->st[line - 1] = 1;
  else if (f->st[line - 1] == 0) f->st[line - 1] = 2;
}


static int cmp_cov (const void *a, const void *b) {
  return strcmp(((const CovFile *)a)->rel, ((const CovFile *)b)->rel);
}


/* each file's covered lines counted; the files in order */
static void cov_totals (void) {
  int i;
  size_t k;
  for (i = 0; i < g_ncov; i++) {
    CovFile *f = &g_cov[i];
    f->hit = f->total = 0;
    for (k = 0; k < f->nst; k++) {
      f->total += f->st[k] != 0;
      f->hit += f->st[k] == 1;
    }
  }
  qsort(g_cov, (size_t)g_ncov, sizeof(CovFile), cmp_cov);
}


/* an LCOV file (SF: the file, DA:line,count); paths from base; 0 read */
static int load_lcov (const char *file, const char *base) {
  size_t len;
  char *s = read_file(file, &len), *p, *e;
  CovFile *f = NULL;
  if (s == NULL) return -1;
  for (p = s; p < s + len; p = e + 1) {
    e = strchr(p, '\n');
    if (e == NULL) e = s + len;
    *e = '\0';
    if (e > p && e[-1] == '\r') e[-1] = '\0';
    if (strncmp(p, "SF:", 3) == 0) f = cov_file(p + 3, base);
    else if (strncmp(p, "DA:", 3) == 0 && f) {
      char *q;
      unsigned long line = strtoul(p + 3, &q, 10);
      if (*q == ',') cov_line(f, line, strtol(q + 1, NULL, 10));
    }
    else if (strcmp(p, "end_of_record") == 0) f = NULL;
  }
  free(s);
  return 0;
}


/* Go's module ("module x" in go.mod) and its folder, from dir up */
static char *go_module (const char *dir, char **mdir) {
  char *d = xstrdup(dir);
  int k;
  *mdir = NULL;
  for (k = 0; k < 16; k++) {
    char *gm = path_join(d, "go.mod"), *s, *up;
    size_t len;
    s = read_file(gm, &len);
    free(gm);
    if (s) {
      char *m = strstr(s, "module "), *r = NULL;
      if (m && (m == s || m[-1] == '\n')) {
        size_t n = 0;
        m += 7;
        while (*m == ' ' || *m == '\t') m++;
        while (m[n] && m[n] != '\n' && m[n] != '\r' && m[n] != ' ') n++;
        r = xstrndup(m, n);
      }
      free(s);
      if (r) {
        *mdir = d;
        return r;
      }
    }
    up = path_dirname(d);
    if (up == NULL || strcmp(up, d) == 0) {
      free(up);
      break;
    }
    free(d);
    d = up;
  }
  free(d);
  return NULL;
}


/* go test -coverprofile's: "mod/pkg/a.go:3.14,5.2 1 1" (its lines, how many statements, how many times) */
static int load_goprofile (const char *file, const char *cwd) {
  size_t len;
  char *s = read_file(file, &len), *p, *e, *mdir, *mod = go_module(cwd, &mdir);
  size_t ml = mod ? strlen(mod) : 0;
  if (s == NULL) {
    free(mod);
    free(mdir);
    return -1;
  }
  for (p = s; p < s + len; p = e + 1) {
    char *sp, *colon, *q, *name;
    unsigned long l0, l1, l;
    long count;
    CovFile *f;
    e = strchr(p, '\n');
    if (e == NULL) e = s + len;
    *e = '\0';
    if (strncmp(p, "mode:", 5) == 0 || (sp = strchr(p, ' ')) == NULL) continue;
    *sp = '\0';
    if ((colon = strrchr(p, ':')) == NULL) continue;
    *colon = '\0';
    l0 = strtoul(colon + 1, &q, 10);
    if ((q = strchr(q, ',')) == NULL) continue;
    l1 = strtoul(q + 1, NULL, 10);
    q = strchr(sp + 1, ' ');	/* after the statements: the count */
    count = q ? strtol(q + 1, NULL, 10) : 0;
    if (mod && strncmp(p, mod, ml) == 0 && p[ml] == '/') name = path_join(mdir, p + ml + 1);
    else name = xstrdup(p);
    f = cov_file(name, cwd);
    free(name);
    for (l = l0; l <= l1 && l - l0 < 100000; l++) cov_line(f, l, count);
  }
  free(s);
  free(mod);
  free(mdir);
  return 0;
}


/* where a run's coverage goes: in mme-data, not in the project */
static char *cov_out (const char *ext) {
  char name[64], *d = data_path("coverage"), *p;
  mkdir_p(d);
  snprintf(name, sizeof(name), "run-%u.%s", ++g_cov_seq, ext);
  p = path_join(d, name);
  free(d);
  return p;
}


/* the run that ended had coverage: it is read */
static void cov_collect (const Run *r) {
  OsStat st;
  int ok = -1;
  if (r->fw == FW_PY) {	/* coverage.py's data, as LCOV */
    char *py = python(), *argv[8], *here = os_getcwd();
    Buf b;
    buf_init(&b);
    argv[0] = py ? py : (char *)"python";
    argv[1] = (char *)"-m";
    argv[2] = (char *)"coverage";
    argv[3] = (char *)"lcov";
    argv[4] = (char *)"-o";
    argv[5] = r->cov;
    argv[6] = NULL;
    os_setenv("COVERAGE_FILE", r->covdata);
    os_chdir(r->cwd);
    if (run_capture(argv, &b) == 0) ok = load_lcov(r->cov, r->cwd);
    else out_log(CHAN, "coverage lcov: %.*s", (int)b.len, b.s ? b.s : "");
    if (here) os_chdir(here);
    os_setenv("COVERAGE_FILE", NULL);
    free(here);
    free(py);
    buf_free(&b);
    if (ok != 0) toast(1, "No coverage: coverage.py is needed (pip install coverage)");
  }
  else if (r->fw == FW_GO) {
    ok = load_goprofile(r->cov, r->cwd);
    if (ok != 0) toast(1, "No coverage: go test wrote no profile");
  }
  else if (r->fw == FW_RUST) {
    ok = load_lcov(r->cov, r->cwd);
    if (ok != 0) toast(1, "No coverage: cargo-llvm-cov is needed (cargo install cargo-llvm-cov)");
  }
  else {	/* npm test: its coverage/lcov.info, when this run wrote it */
    if (os_stat(r->cov, &st) == 0 && st.exists && (long long)st.mtime + 2 >= r->t0) ok = load_lcov(r->cov, r->cwd);
    if (ok != 0) toast(0, "No coverage: make the test script write coverage/lcov.info (jest --coverage, c8 --reporter=lcov)");
  }
  if (ok == 0) {
    size_t hit = 0, total = 0;
    int i;
    cov_totals();
    for (i = 0; i < g_ncov; i++) {
      hit += g_cov[i].hit;
      total += g_cov[i].total;
    }
    g_cov_on = 1;
    out_log(CHAN, "Coverage: %lu of %lu lines (%.1f%%) in %d file%s", (unsigned long)hit, (unsigned long)total,
            total ? 100.0 * (double)hit / (double)total : 0.0, g_ncov, g_ncov == 1 ? "" : "s");
  }
}


/* the coverage of a file's line (from 0): 0 none known, 1 covered, 2 not */
int test_cov (const char *real, size_t line) {
  static int last = -1;
  int i;
  if (!g_cov_on || real == NULL) return 0;
  if (!(last >= 0 && last < g_ncov && m_fncmp(g_cov[last].real, real) == 0)) {	/* the same file row after row */
    for (last = -1, i = 0; i < g_ncov && last < 0; i++)
      if (m_fncmp(g_cov[i].real, real) == 0) last = i;
    if (last < 0) return 0;
  }
  return line < g_cov[last].nst ? g_cov[last].st[line] : 0;
}


int test_cov_inline (void) {
  return g_cov_on && g_cov_inline;
}

/* }================================================================== */


/*
** The runs for the tests picked (sel[k]: file k's tests; t -1 all of them),
** grouped as the tools want them: a Go package, a Rust crate, all Python.
*/
typedef struct Pick1 {
  int f, t;
} Pick1;

static void plan (const Pick1 *sel, int n) {
  int i, k;
  char *py = NULL, *pytest = find_program("pytest");
  Run pr;
  int have_py = 0;
  memset(&pr, 0, sizeof(pr));
  for (i = 0; i < n; i++) {
    TFile *f = &g_f[sel[i].f];
    if (f->fw == FW_GO || f->fw == FW_RUST || f->fw == FW_JS) {	/* one run for each package / crate / file */
      char *cwd = f->fw == FW_RUST ? crate_dir(f->path) : path_dirname(f->path);
      Run *r = NULL;
      if (cwd == NULL) {
        toast(1, "No Cargo.toml for %s", f->rel);
        continue;
      }
      for (k = 0; k < g_nq; k++)	/* already one for it: the tests join it */
        if (g_q[k].fw == f->fw && g_q[k].argv.n == 0 && m_fncmp(g_q[k].cwd, cwd) == 0 && f->fw != FW_JS) r = &g_q[k];
      if (r == NULL) {
        Run nr;
        memset(&nr, 0, sizeof(nr));
        nr.fw = f->fw;
        nr.cwd = cwd;
        vec_init(&nr.argv);
        vec_init(&nr.keys);
        enqueue(&nr);
        r = &g_q[g_nq - 1];
      }
      else free(cwd);
      for (k = 0; k < f->n; k++)
        if (sel[i].t < 0 || sel[i].t == k) {
          vec_push(&r->keys, key_of(f, &f->t[k]));
          f->t[k].state = TM_QUEUED;
        }
    }
    else if (f->fw == FW_PY) {
      if (!have_py) {
        vec_init(&pr.argv);
        vec_init(&pr.keys);
        pr.fw = FW_PY;
        pr.cwd = xstrdup(side_root());
        have_py = 1;
      }
      for (k = 0; k < f->n; k++)
        if (sel[i].t < 0 || sel[i].t == k) {
          vec_push(&pr.keys, key_of(f, &f->t[k]));
          f->t[k].state = TM_QUEUED;
        }
    }
  }
  if (have_py) enqueue(&pr);
  /* the command lines, now that each run knows its tests */
  for (i = 0; i < g_nq; i++) {
    Run *r = &g_q[i];
    size_t j;
    if (r->argv.n) continue;
    if (r->fw == FW_GO) {
      char *go = find_program("go");
      Buf re;
      if (go == NULL) {
        toast(1, "go was not found in the PATH");
        go = xstrdup("go");
      }
      vec_push(&r->argv, go);
      vec_push(&r->argv, xstrdup("test"));
      if (g_cover) {	/* its coverage profile */
        r->cov = cov_out("out");
        vec_push(&r->argv, xstrcat3("-coverprofile=", r->cov, ""));
      }
      vec_push(&r->argv, xstrdup("-json"));
      buf_init(&re);
      for (j = 0; j < r->keys.n && j < 200; j++) {
        const char *nl = strchr(r->keys.v[j], '\n');
        buf_puts(&re, j ? "|" : "^(");
        buf_puts(&re, nl + 1);
      }
      if (r->keys.n && r->keys.n <= 200) {
        buf_puts(&re, ")$");
        vec_push(&r->argv, xstrdup("-run"));
        vec_push(&r->argv, xstrndup(re.s, re.len));
      }
      buf_free(&re);
      vec_push(&r->argv, xstrdup("."));
    }
    else if (r->fw == FW_RUST) {
      char *cargo = find_program("cargo");
      vec_push(&r->argv, cargo ? cargo : xstrdup("cargo"));
      if (g_cover) {	/* cargo-llvm-cov runs the tests, and writes LCOV */
        r->cov = cov_out("info");
        vec_push(&r->argv, xstrdup("llvm-cov"));
        vec_push(&r->argv, xstrdup("--lcov"));
        vec_push(&r->argv, xstrdup("--output-path"));
        vec_push(&r->argv, xstrdup(r->cov));
      }
      else vec_push(&r->argv, xstrdup("test"));
      if (r->keys.n == 1) vec_push(&r->argv, xstrdup(strchr(r->keys.v[0], '\n') + 1));
    }
    else if (r->fw == FW_JS) {
#ifdef _WIN32
      char *cs = os_getenv("ComSpec");
      vec_push(&r->argv, cs ? cs : xstrdup("cmd.exe"));
      vec_push(&r->argv, xstrdup("/d"));
      vec_push(&r->argv, xstrdup("/c"));
      vec_push(&r->argv, xstrdup("npm"));
#else
      char *npm = find_program("npm");
      vec_push(&r->argv, npm ? npm : xstrdup("npm"));
#endif
      vec_push(&r->argv, xstrdup("test"));
      if (g_cover) {	/* what the script writes, if it does */
        char *cd = path_join(r->cwd, "coverage");
        r->cov = path_join(cd, "lcov.info");
        free(cd);
      }
    }
    else if (r->fw == FW_PY) {	/* pytest by nodeid, else unittest by dotted name */
      size_t nbase;
      if (py == NULL) py = python();
      if (g_cover) {	/* coverage.py runs it: python -m coverage run -m pytest ... */
        r->cov = cov_out("info");
        r->covdata = cov_out("data");
        vec_push(&r->argv, py ? xstrdup(py) : xstrdup("python"));
        vec_push(&r->argv, xstrdup("-m"));
        vec_push(&r->argv, xstrdup("coverage"));
        vec_push(&r->argv, xstrdup("run"));
        vec_push(&r->argv, xstrdup("-m"));
        vec_push(&r->argv, xstrdup(pytest ? "pytest" : "unittest"));
        if (pytest) {
          vec_push(&r->argv, xstrdup("-rA"));
          vec_push(&r->argv, xstrdup("-p"));
          vec_push(&r->argv, xstrdup("no:cacheprovider"));
        }
        else vec_push(&r->argv, xstrdup("-v"));
      }
      else if (pytest) {
        vec_push(&r->argv, xstrdup(pytest));
        vec_push(&r->argv, xstrdup("-rA"));
        vec_push(&r->argv, xstrdup("-p"));
        vec_push(&r->argv, xstrdup("no:cacheprovider"));
      }
      else {
        vec_push(&r->argv, py ? xstrdup(py) : xstrdup("python"));
        vec_push(&r->argv, xstrdup("-m"));
        vec_push(&r->argv, xstrdup("unittest"));
        vec_push(&r->argv, xstrdup("-v"));
      }
      nbase = r->argv.n;
      {
        Vec keep;	/* the tests it can run */
        vec_init(&keep);
      for (j = 0; j < r->keys.n; j++) {
        TFile *f = NULL;
        TTest *t = test_of_key(r->keys.v[j], &f);
        char *id;
        if (t == NULL) continue;
        if (pytest) {
          id = t->cls ? xstrcat3(f->rel, "::", t->cls) : xstrdup(f->rel);
          {
            char *id2 = xstrcat3(id, "::", t->name);
            free(id);
            id = id2;
          }
        }
        else if (t->cls) {
          char *m = py_module(f->rel), *a = xstrcat3(m, ".", t->cls);
          id = xstrcat3(a, ".", t->name);
          free(a);
          free(m);
        }
        else {	/* unittest runs methods of TestCase classes only */
          t->state = TM_SKIP;
          free(t->msg);
          t->msg = xstrdup("unittest runs the test methods of TestCase classes: install pytest to run test functions");
          continue;
        }
        vec_push(&r->argv, id);
        vec_push(&keep, xstrdup(r->keys.v[j]));
      }
        vec_free(&r->keys);
        r->keys = keep;
      }
      if (!pytest && r->argv.n == nbase) {	/* nothing unittest can run */
        run_free(r);
        memmove(g_q + i, g_q + i + 1, (size_t)(g_nq - i - 1) * sizeof(Run));
        g_nq--;
        i--;
      }
    }
  }
  free(py);
  free(pytest);
}


/* the run in front starts */
static void start_next (void) {
  int fds[2], io[3], r;
  char **argv;
  size_t i;
  char *here;
  Buf cl;
  if (g_on || g_nq == 0) return;
  g_run = g_q[0];
  memmove(g_q, g_q + 1, (size_t)(g_nq - 1) * sizeof(Run));
  g_nq--;
  for (i = 0; i < g_run.keys.n; i++) {
    TTest *t = test_of_key(g_run.keys.v[i], NULL);
    if (t) t->state = TM_RUNNING;
  }
  buf_init(&cl);
  for (i = 0; i < g_run.argv.n; i++) {
    if (i) buf_putc(&cl, ' ');
    buf_puts(&cl, i == 0 ? path_basename(g_run.argv.v[0]) : g_run.argv.v[i]);
  }
  buf_putc(&cl, '\0');
  out_log(CHAN, "Running tests: %s (in %s)", cl.s, g_run.cwd);
  buf_free(&cl);
  argv = (char **)xmalloc((g_run.argv.n + 1) * sizeof(char *));
  for (i = 0; i < g_run.argv.n; i++) argv[i] = g_run.argv.v[i];
  argv[i] = NULL;
  buf_init(&g_out);
  if (os_pipe(fds) != 0) {
    free(argv);
    run_free(&g_run);
    return;
  }
  io[0] = -1;
  io[1] = io[2] = fds[1];
  here = os_getcwd();
  os_chdir(g_run.cwd);
  g_run.t0 = (long long)time(NULL);
  if (g_run.covdata) os_setenv("COVERAGE_FILE", g_run.covdata);	/* coverage.py's data: in mme-data */
  r = os_spawn(argv[0], argv, NULL, io, 3, &g_proc, &g_pid);
  if (g_run.covdata) os_setenv("COVERAGE_FILE", NULL);
  if (r != 0) {
    os_close(fds[0]);
    os_close(fds[1]);
    if (here) os_chdir(here);
    free(here);
    toast(1, "%s could not be started", path_basename(argv[0]));
    for (i = 0; i < g_run.keys.n; i++) {
      TTest *t = test_of_key(g_run.keys.v[i], NULL);
      if (t) t->state = TM_UNSET;
    }
    free(argv);
    run_free(&g_run);
    return;
  }
  if (here) os_chdir(here);
  free(here);
  free(argv);
  os_close(fds[1]);
  g_fd = fds[0];
  g_on = 1;
}


/* the line of text at s, until \n: a copy without \r */
static char *line_copy (const char *s, size_t n) {
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
  return xstrndup(s, n);
}


/* "x_test.go:12: got 4" in s: the file (from dir), the line, the message after */
static int find_loc (const char *s, const char *dir, const char *ext, char **file, size_t *line, char **msg) {
  const char *p = s;
  size_t el = strlen(ext);
  while ((p = strstr(p, ext)) != NULL) {
    const char *b = p, *q = p + el;
    if (*q == ':' && q[1] >= '0' && q[1] <= '9') {
      unsigned long ln = strtoul(q + 1, (char **)&q, 10);
      char *f;
      while (b > s && !(b[-1] == ' ' || b[-1] == '\t' || b[-1] == '\n' || b[-1] == '"' || b[-1] == '\'' || b[-1] == '('))
        b--;
      f = xstrndup(b, (size_t)(p + el - b));
      if (!(f[0] == '/' || f[0] == '\\' || (f[0] && f[1] == ':'))) {
        char *j = path_join(dir, f[0] == '.' && (f[1] == '/' || f[1] == '\\') ? f + 2 : f);
        free(f);
        f = j;
      }
      {
        char *real = os_realpath(f);
        if (real) {
          free(f);
          f = real;
        }
      }
      *file = f;
      *line = ln > 0 ? ln - 1 : 0;
      while (*q == ':' || (*q >= '0' && *q <= '9')) q++;
      while (*q == ' ') q++;
      if (msg) {
        const char *e = q;
        while (*e && *e != '\n') e++;
        *msg = line_copy(q, (size_t)(e - q));
      }
      return 1;
    }
    p += el;
  }
  return 0;
}


static void set_result (TTest *t, int state, long ms, char *msg) {
  t->state = state;
  t->ms = ms;
  free(t->msg);
  t->msg = msg;
  free(t->floc);
  t->floc = NULL;
}


/* a failure's place: in the message, else the test's own line */
static void place (TFile *f, TTest *t, const char *text, const char *dir, const char *ext) {
  char *file = NULL, *m = NULL;
  size_t line;
  if (text && find_loc(text, dir, ext, &file, &line, &m)) {
    t->floc = file;
    t->fline = line;
    if (m && *m && (t->msg == NULL || *t->msg == '\0')) {
      free(t->msg);
      t->msg = m;
    }
    else free(m);
  }
  else {
    t->floc = xstrdup(f->path);
    t->fline = t->line;
  }
}


/* the text a test printed, without Go's own "=== RUN" and "--- FAIL" lines */
static char *go_text (const Buf *b) {
  Buf o;
  size_t i = 0;
  buf_init(&o);
  while (i < b->len) {
    size_t e = i, k;
    while (e < b->len && b->s[e] != '\n') e++;
    for (k = i; k < e && (b->s[k] == ' ' || b->s[k] == '\t'); k++) ;
    if (!(strncmp(b->s + k, "=== ", 4) == 0 || strncmp(b->s + k, "--- ", 4) == 0 ||
          strncmp(b->s + k, "FAIL", 4) == 0 || strncmp(b->s + k, "PASS", 4) == 0 || k == e)) {
      buf_putn(&o, b->s + k, e - k);
      buf_putc(&o, '\n');
    }
    i = e + 1;
  }
  while (o.len && o.s[o.len - 1] == '\n') o.len--;
  buf_putc(&o, '\0');
  return buf_take(&o);
}


static void finish_go (const Run *r, int code) {
  Vec names;
  Buf *outs = NULL, other, text;
  size_t i = 0, k;
  vec_init(&names);
  buf_init(&other);
  buf_init(&text);
  while (i < g_out.len) {
    size_t e = i;
    while (e < g_out.len && g_out.s[e] != '\n') e++;
    if (g_out.s[i] == '{') {
      Json *j = json_parse(g_out.s + i, e - i);
      const char *act = json_str(json_get(j, "Action"), ""), *test = json_str(json_get(j, "Test"), NULL);
      if (strcmp(act, "output") == 0) buf_puts(&text, json_str(json_get(j, "Output"), ""));	/* go test's words, as text */
      if (test) {
        char *top = xstrdup(test), *sl = strchr(top, '/');
        size_t at;
        if (sl) *sl = '\0';
        for (at = 0; at < names.n && strcmp(names.v[at], top) != 0; at++) ;
        if (at == names.n) {
          vec_push(&names, xstrdup(top));
          outs = (Buf *)xrealloc(outs, names.n * sizeof(Buf));
          buf_init(&outs[at]);
        }
        if (strcmp(act, "output") == 0) buf_puts(&outs[at], json_str(json_get(j, "Output"), ""));
        else if (sl == NULL && (strcmp(act, "pass") == 0 || strcmp(act, "fail") == 0 || strcmp(act, "skip") == 0)) {
          for (k = 0; k < r->keys.n; k++) {	/* the tests of this package with that name */
            TFile *f = NULL;
            TTest *t = test_of_key(r->keys.v[k], &f);
            char *d;
            if (t == NULL || strcmp(t->name, top) != 0) continue;
            d = path_dirname(f->path);
            if (m_fncmp(d, r->cwd) == 0) {
              long ms = (long)(json_num(json_get(j, "Elapsed"), 0) * 1000);
              if (act[0] == 'p') set_result(t, TM_PASS, ms, NULL);
              else if (act[0] == 's') set_result(t, TM_SKIP, ms, go_text(&outs[at]));
              else {
                char *txt = xstrndup(outs[at].s ? outs[at].s : "", outs[at].len);
                set_result(t, TM_FAIL, ms, NULL);
                place(f, t, txt, r->cwd, ".go");
                if (t->msg == NULL) t->msg = go_text(&outs[at]);
                free(txt);
              }
            }
            free(d);
          }
        }
        free(top);
      }
      else if (json_get(j, "Output")) buf_puts(&other, json_str(json_get(j, "Output"), ""));
      json_free(j);
    }
    else {	/* not JSON: the build's errors */
      buf_putn(&other, g_out.s + i, e - i);
      buf_putc(&other, '\n');
      buf_putn(&text, g_out.s + i, e - i);
      buf_putc(&text, '\n');
    }
    i = e + 1;
  }
  buf_putc(&other, '\0');
  out_append(CHAN, text.s ? text.s : "", text.len);
  buf_free(&text);
  for (k = 0; k < r->keys.n; k++) {	/* no result: the build failed, or it did not run */
    TFile *f = NULL;
    TTest *t = test_of_key(r->keys.v[k], &f);
    if (t == NULL || t->state != TM_RUNNING) continue;
    if (code != 0) {
      set_result(t, TM_FAIL, -1, NULL);
      place(f, t, other.s, r->cwd, ".go");
      if (t->msg == NULL) t->msg = line_copy(other.s, strlen(other.s) < 300 ? strlen(other.s) : 300);
    }
    else set_result(t, TM_UNSET, -1, NULL);
  }
  for (k = 0; k < names.n; k++) buf_free(&outs[k]);
  free(outs);
  vec_free(&names);
  buf_free(&other);
}


/* pytest -rA: "PASSED a/test_x.py::TestC::test_y", "FAILED ... - message"; the failures' "file:12:" lines */
static void finish_py (const Run *r, int code) {
  size_t i = 0, k;
  char *section = NULL;	/* the failure being read: "test_y", "TestC.test_y" */
  int pytest = r->argv.n && strstr(path_basename(r->argv.v[0]), "pytest") != NULL;
  while (i < g_out.len) {
    size_t e = i;
    char *l;
    while (e < g_out.len && g_out.s[e] != '\n') e++;
    l = line_copy(g_out.s + i, e - i);
    if (pytest) {
      static const char *const st[] = {"PASSED ", "FAILED ", "ERROR ", "XFAIL ", "XPASS "};
      int s;
      size_t ll = strlen(l);
      for (s = 0; s < 5; s++)
        if (strncmp(l, st[s], strlen(st[s])) == 0) break;
      if (s < 5) {
        const char *id = l + strlen(st[s]), *dash = strstr(id, " - ");
        char *nid = xstrndup(id, dash ? (size_t)(dash - id) : strlen(id)), *br = strchr(nid, '[');
        if (br) *br = '\0';	/* test_x[1]: the test */
        for (k = 0; k < r->keys.n; k++) {
          TFile *f = NULL;
          TTest *t = test_of_key(r->keys.v[k], &f);
          char *want, *a;
          if (t == NULL) continue;
          a = t->cls ? xstrcat3(f->rel, "::", t->cls) : xstrdup(f->rel);
          want = xstrcat3(a, "::", t->name);
          free(a);
          if (strcmp(want, nid) == 0) {
            if (s == 0 || s == 4) set_result(t, TM_PASS, -1, NULL);
            else if (s == 3) set_result(t, TM_SKIP, -1, xstrdup("expected to fail"));
            else if (t->state != TM_FAIL || t->msg == NULL) {
              set_result(t, TM_FAIL, -1, dash ? xstrdup(dash + 3) : NULL);
              t->floc = xstrdup(f->path);
              t->fline = t->line;
            }
          }
          free(want);
        }
        free(nid);
      }
      else if (ll > 8 && l[0] == '_' && l[ll - 1] == '_') {	/* ____ TestC.test_y ____ */
        char *a = l, *b = l + ll;
        while (*a == '_' || *a == ' ') a++;
        while (b > a && (b[-1] == '_' || b[-1] == ' ')) b--;
        free(section);
        section = xstrndup(a, (size_t)(b - a));
      }
      else if (section && strstr(l, ".py:")) {	/* "a/test_x.py:12: AssertionError": the test's place */
        char *file = NULL;
        size_t line;
        if (find_loc(l, r->cwd, ".py", &file, &line, NULL)) {
          for (k = 0; k < r->keys.n; k++) {
            TFile *f = NULL;
            TTest *t = test_of_key(r->keys.v[k], &f);
            char *nm;
            if (t == NULL) continue;
            nm = t->cls ? xstrcat3(t->cls, ".", t->name) : xstrdup(t->name);
            if (strcmp(nm, section) == 0 && m_fncmp(file, f->path) == 0) {
              free(t->floc);
              t->floc = xstrdup(file);
              t->fline = line;
            }
            free(nm);
          }
          free(file);
        }
      }
    }
    else {	/* unittest -v: "test_y (mod.TestC) ... ok" / "... FAIL" / "... ERROR" / "... skipped 'why'" */
      char *dots = strstr(l, " ... ");
      if (dots && strchr(l, '(')) {
        char *name = ident_at(l, strlen(l)), *res = dots + 5;
        for (k = 0; name && k < r->keys.n; k++) {
          TFile *f = NULL;
          TTest *t = test_of_key(r->keys.v[k], &f);
          if (t == NULL || strcmp(t->name, name) != 0 || t->cls == NULL || strstr(l, t->cls) == NULL) continue;
          if (strncmp(res, "ok", 2) == 0) set_result(t, TM_PASS, -1, NULL);
          else if (strncmp(res, "skipped", 7) == 0) set_result(t, TM_SKIP, -1, xstrdup(res + 7));
          else {
            set_result(t, TM_FAIL, -1, NULL);
            t->floc = xstrdup(f->path);
            t->fline = t->line;
          }
        }
        free(name);
      }
      else if (strncmp(l, "FAIL: ", 6) == 0 || strncmp(l, "ERROR: ", 7) == 0) {
        free(section);
        section = ident_at(strchr(l, ' ') + 1, strlen(l));
      }
      else if (section && strstr(l, "File \"") && strstr(l, ", line ")) {	/* the traceback's place in the test file */
        char *q = strstr(l, "File \"") + 6, *qe = strchr(q, '"');
        unsigned long ln = strtoul(strstr(l, ", line ") + 7, NULL, 10);
        if (qe) {
          char *file = xstrndup(q, (size_t)(qe - q)), *real = os_realpath(file);
          for (k = 0; k < r->keys.n; k++) {
            TFile *f = NULL;
            TTest *t = test_of_key(r->keys.v[k], &f);
            if (t && strcmp(t->name, section) == 0 && m_fncmp(real ? real : file, f->path) == 0) {
              free(t->floc);
              t->floc = xstrdup(f->path);
              t->fline = ln > 0 ? ln - 1 : 0;
            }
          }
          free(real);
          free(file);
        }
      }
      else if (section && (strncmp(l, "AssertionError", 14) == 0 || (strstr(l, "Error: ") && l[0] != ' '))) {
        for (k = 0; k < r->keys.n; k++) {
          TTest *t = test_of_key(r->keys.v[k], NULL);
          if (t && strcmp(t->name, section) == 0 && t->msg == NULL) t->msg = xstrdup(l);
        }
      }
    }
    free(l);
    i = e + 1;
  }
  free(section);
  for (k = 0; k < r->keys.n; k++) {	/* not said: skipped when all went well; else it could not run */
    TFile *f = NULL;
    TTest *t = test_of_key(r->keys.v[k], &f);
    if (t == NULL || t->state != TM_RUNNING) continue;
    if (code == 0) set_result(t, TM_SKIP, -1, NULL);
    else {
      set_result(t, TM_FAIL, -1, xstrdup("The test could not run: see Test Results"));
      t->floc = xstrdup(f->path);
      t->fline = t->line;
    }
  }
}


/* cargo test: "test m::name ... ok"; "thread 'm::name' panicked at src/x.rs:10:5:" and the message */
static void finish_rust (const Run *r, int code) {
  size_t i = 0, k;
  char *panicked = NULL;	/* the test whose panic is being read */
  int want_msg = 0;
  while (i < g_out.len) {
    size_t e = i;
    char *l;
    while (e < g_out.len && g_out.s[e] != '\n') e++;
    l = line_copy(g_out.s + i, e - i);
    if (want_msg && panicked) {	/* the message after "panicked at x.rs:1:2:" */
      for (k = 0; k < r->keys.n; k++) {
        TTest *t = test_of_key(r->keys.v[k], NULL);
        if (t && strcmp(t->name, panicked) == 0) {
          free(t->msg);
          t->msg = xstrdup(l);
        }
      }
      want_msg = 0;
    }
    if (strncmp(l, "test ", 5) == 0 && strstr(l + 5, " ... ")) {	/* from l + 5: "test ... x" would give a length of -1 */
      char *full = xstrndup(l + 5, (size_t)(strstr(l + 5, " ... ") - l - 5)), *nm = strrchr(full, ':');
      const char *res = strstr(l, " ... ") + 5;
      nm = nm ? nm + 1 : full;
      for (k = 0; k < r->keys.n; k++) {
        TFile *f = NULL;
        TTest *t = test_of_key(r->keys.v[k], &f);
        if (t == NULL || strcmp(t->name, nm) != 0) continue;
        if (strncmp(res, "ok", 2) == 0) set_result(t, TM_PASS, -1, NULL);
        else if (strncmp(res, "ignored", 7) == 0) set_result(t, TM_SKIP, -1, NULL);
        else if (t->state != TM_FAIL) {
          set_result(t, TM_FAIL, -1, NULL);
          t->floc = xstrdup(f->path);
          t->fline = t->line;
        }
      }
      free(full);
    }
    else if (strncmp(l, "thread '", 8) == 0 && strstr(l, " panicked at ")) {	/* "thread 'm::t' (123) panicked at" */
      char *q = strchr(l, '\''), *qe = q ? strchr(q + 1, '\'') : NULL;
      if (q && qe) {
        char *full = xstrndup(q + 1, (size_t)(qe - q - 1)), *nm = strrchr(full, ':'), *file = NULL, *m = NULL;
        size_t line;
        free(panicked);
        panicked = xstrdup(nm ? nm + 1 : full);
        if (find_loc(qe, r->cwd, ".rs", &file, &line, &m)) {
          for (k = 0; k < r->keys.n; k++) {
            TTest *t = test_of_key(r->keys.v[k], NULL);
            if (t && strcmp(t->name, panicked) == 0) {
              t->state = TM_FAIL;
              free(t->floc);
              t->floc = xstrdup(file);
              t->fline = line;
              if (m && *m && m[0] != '\0') {	/* the old form: panicked at 'msg', x.rs:1:2 */
                free(t->msg);
                t->msg = xstrdup(m);
              }
            }
          }
          want_msg = m == NULL || *m == '\0';
          free(file);
        }
        free(m);
        free(full);
      }
    }
    free(l);
    i = e + 1;
  }
  free(panicked);
  for (k = 0; k < r->keys.n; k++) {
    TFile *f = NULL;
    TTest *t = test_of_key(r->keys.v[k], &f);
    if (t == NULL || t->state != TM_RUNNING) continue;
    if (code == 0) set_result(t, TM_UNSET, -1, NULL);
    else {
      char *file = NULL, *m = NULL;
      size_t line;
      set_result(t, TM_FAIL, -1, NULL);
      if (g_out.s && find_loc(g_out.s, r->cwd, ".rs", &file, &line, &m)) {	/* a build error: " --> src/x.rs:5:9" */
        t->floc = file;
        t->fline = line;
        free(m);
      }
      else {
        t->floc = xstrdup(f->path);
        t->fline = t->line;
      }
      t->msg = xstrdup("The crate did not build: see Test Results");
    }
  }
}


static void finish_js (const Run *r, int code) {
  size_t k;
  for (k = 0; k < r->keys.n; k++) {
    TFile *f = NULL;
    TTest *t = test_of_key(r->keys.v[k], &f);
    if (t == NULL) continue;
    if (code == 0) set_result(t, TM_PASS, -1, NULL);
    else {
      size_t e = g_out.len, b;
      while (e > 0 && (g_out.s[e - 1] == '\n' || g_out.s[e - 1] == '\r' || g_out.s[e - 1] == ' ')) e--;
      for (b = e; b > 0 && g_out.s[b - 1] != '\n'; b--) ;
      set_result(t, TM_FAIL, -1, e > b ? line_copy(g_out.s + b, e - b) : xstrdup("npm test failed"));
      t->floc = xstrdup(f->path);
      t->fline = t->line;
    }
  }
}


/* the failures in the Problems, at the line that failed */
static void publish (void) {
  Vec paths;
  size_t i, k;
  int a, b;
  vec_init(&paths);
  for (a = 0; a < g_nf; a++)
    for (b = 0; b < g_f[a].n; b++) {
      const TTest *t = &g_f[a].t[b];
      if (t->state == TM_FAIL && t->floc) {
        for (k = 0; k < paths.n && m_fncmp(paths.v[k], t->floc) != 0; k++) ;
        if (k == paths.n) vec_push(&paths, xstrdup(t->floc));
      }
    }
  for (i = 0; i < paths.n; i++) {
    Diag *v = NULL;
    size_t n = 0;
    for (a = 0; a < g_nf; a++)
      for (b = 0; b < g_f[a].n; b++) {
        const TTest *t = &g_f[a].t[b];
        char m[600];
        if (t->state != TM_FAIL || t->floc == NULL || m_fncmp(t->floc, paths.v[i]) != 0) continue;
        v = (Diag *)xrealloc(v, (n + 1) * sizeof(Diag));
        v[n].a.y = v[n].b.y = t->fline;
        v[n].a.x = 0;
        v[n].b.x = (size_t)1 << 20;	/* the whole line */
        v[n].sev = 1;
        snprintf(m, sizeof(m), "%s failed: %s", t->name, t->msg && *t->msg ? t->msg : "see Test Results");
        v[n++].msg = xstrdup(m);
      }
    lsp_task_diags(paths.v[i], v, n);	/* it copies them */
    for (k = 0; k < n; k++) free(v[k].msg);
    free(v);
  }
  for (i = 0; i < g_dpaths.n; i++) {	/* a file that had a failure, and has none now */
    for (k = 0; k < paths.n && m_fncmp(paths.v[k], g_dpaths.v[i]) != 0; k++) ;
    if (k == paths.n) lsp_task_diags(g_dpaths.v[i], NULL, 0);
  }
  vec_free(&g_dpaths);
  g_dpaths = paths;
}


static void finish (int code) {
  int pass = 0, fail = 0;
  size_t k;
  buf_putc(&g_out, '\0');	/* the text ends: it is read as a string too */
  g_out.len--;
  if (g_run.fw == FW_GO) finish_go(&g_run, code);
  else if (g_run.fw == FW_PY) finish_py(&g_run, code);
  else if (g_run.fw == FW_RUST) finish_rust(&g_run, code);
  else finish_js(&g_run, code);
  if (g_run.cov) cov_collect(&g_run);	/* what the tests covered */
  for (k = 0; k < g_run.keys.n; k++) {
    TTest *t = test_of_key(g_run.keys.v[k], NULL);
    if (t && t->state == TM_PASS) pass++;
    else if (t && t->state == TM_FAIL) fail++;
  }
  out_log(CHAN, "%d passed, %d failed (exit code %d)", pass, fail, code);
  publish();
  buf_free(&g_out);
  run_free(&g_run);
  g_on = 0;
}


int test_idle (void) {
  int changed = 0;
  if (g_on) {
    char chunk[16384];
    while (os_wait_readable(g_fd, 0) == 1) {
      long n = os_read(g_fd, chunk, sizeof(chunk));
      if (n <= 0) {
        os_close(g_fd);
        g_fd = -1;
        finish(os_wait(g_proc));
        changed = 1;
        break;
      }
      buf_putn(&g_out, chunk, (size_t)n);
      if (g_run.fw != FW_GO) out_append(CHAN, chunk, (size_t)n);	/* go test -json's: its text, at the end */
      changed = 1;
    }
  }
  if (!g_on && g_nq > 0) {
    start_next();
    changed = 1;
  }
  return changed;
}


/* the tests picked go to the queue; what was asked is kept for Rerun */
static void run_picks (const Pick1 *sel, int n) {
  int i, q0;
  if (n > 0 && !trust_require("Running tests")) return;	/* Restricted Mode: the tests are the folder's code */
  if (n == 0) {
    toast(0, "No tests found to run.");
    return;
  }
  editor_save_all();	/* testing.saveBeforeTest */
  if (g_cover) cov_clear();	/* a run with coverage: the last one's goes */
  q0 = g_nq;
  plan(sel, n);
  for (i = 0; i < g_nlast; i++) run_free(&g_last[i]);
  free(g_last);
  g_nlast = g_nq - q0;
  g_last = (Run *)xmalloc((size_t)(g_nlast + 1) * sizeof(Run));
  for (i = 0; i < g_nlast; i++) run_copy(&g_last[i], &g_q[q0 + i]);
  test_idle();
}


static void run_all (void) {
  Pick1 *sel;
  int i, n = 0;
  if (!g_found) discover();
  sel = (Pick1 *)xmalloc((size_t)(g_nf + 1) * sizeof(Pick1));
  for (i = 0; i < g_nf; i++)
    if (g_f[i].n > 0) {
      sel[n].f = i;
      sel[n++].t = -1;
    }
  run_picks(sel, n);
  free(sel);
}


static void cancel (void) {
  size_t k;
  int i, a, b;
  for (i = 0; i < g_nq; i++) run_free(&g_q[i]);
  g_nq = 0;
  if (g_on) {
    os_kill(g_pid, 9);
    out_log(CHAN, "%s", "The test run was cancelled.");
  }
  for (k = 0; g_on && k < g_run.keys.n; k++) {
    TTest *t = test_of_key(g_run.keys.v[k], NULL);
    if (t) t->state = TM_UNSET;
  }
  for (a = 0; a < g_nf; a++)
    for (b = 0; b < g_f[a].n; b++)
      if (g_f[a].t[b].state == TM_QUEUED) g_f[a].t[b].state = TM_UNSET;
}


void test_shutdown (void) {
  cancel();
}

/* }================================================================== */


/*
** {==================================================================
** The view
** ===================================================================
*/

#define VHEAD	2	/* the title, the summary */

enum { RW_FILE, RW_TEST, RW_MSG, RW_COVHEAD, RW_COV };	/* RW_COV: f is the file of g_cov */

typedef struct TRow {
  int kind, f, t;
} TRow;

static TRow *g_row;
static int g_nrow, g_sel, g_top, g_h;


static void rows (void) {
  int f, t;
  g_nrow = 0;
  for (f = 0; f < g_nf; f++) {
    if (g_f[f].n == 0) continue;
    g_row = (TRow *)xrealloc(g_row, (size_t)(g_nrow + 2 * g_f[f].n + 2) * sizeof(TRow));
    g_row[g_nrow].kind = RW_FILE;
    g_row[g_nrow].f = f;
    g_row[g_nrow++].t = -1;
    if (!g_f[f].open) continue;
    for (t = 0; t < g_f[f].n; t++) {
      g_row[g_nrow].kind = RW_TEST;
      g_row[g_nrow].f = f;
      g_row[g_nrow++].t = t;
      if (g_f[f].t[t].state == TM_FAIL && g_f[f].t[t].msg) {	/* why it failed, under it */
        g_row[g_nrow].kind = RW_MSG;
        g_row[g_nrow].f = f;
        g_row[g_nrow++].t = t;
      }
    }
  }
  if (g_cov_on) {	/* TEST COVERAGE: the files, each with how much of it ran */
    g_row = (TRow *)xrealloc(g_row, (size_t)(g_nrow + g_ncov + 2) * sizeof(TRow));
    g_row[g_nrow].kind = RW_COVHEAD;
    g_row[g_nrow].f = g_row[g_nrow].t = -1;
    g_nrow++;
    for (f = 0; f < g_ncov; f++) {
      if (g_cov[f].total == 0) continue;
      g_row[g_nrow].kind = RW_COV;
      g_row[g_nrow].f = f;
      g_row[g_nrow++].t = -1;
    }
  }
  if (g_sel >= g_nrow) g_sel = g_nrow - 1;
  if (g_sel < 0) g_sel = 0;
}


/* a file's state: failed, running, queued, passed when all did, else not run */
static int file_state (const TFile *f) {
  int i, pass = 0, run = 0, queued = 0, fail = 0;
  for (i = 0; i < f->n; i++) {
    int s = f->t[i].state;
    fail += s == TM_FAIL;
    run += s == TM_RUNNING;
    queued += s == TM_QUEUED;
    pass += s == TM_PASS || s == TM_SKIP;
  }
  if (run) return TM_RUNNING;
  if (queued) return TM_QUEUED;
  if (fail) return TM_FAIL;
  if (pass == f->n && f->n > 0) return TM_PASS;
  return TM_UNSET;
}


/* the icon and color of a state, as VS Code's testing icons */
static uint32_t state_icon (int s, uint32_t *rgb) {
  switch (s) {
    case TM_PASS: *rgb = 0x73C991; return 0xEBA4;	/* pass */
    case TM_FAIL: *rgb = 0xF14C4C; return 0xEA87;	/* error */
    case TM_RUNNING: *rgb = 0xCCA700; return 0xEB19;	/* loading */
    case TM_QUEUED: *rgb = 0xCCA700; return 0xEA82;	/* history */
    case TM_SKIP: *rgb = 0x848484; return 0xEABD;	/* circle-slash */
  }
  *rgb = 0x848484;
  return 0xEABC;	/* circle-outline */
}


/* the part covered, as VS Code writes it and colors it (testing.coverageBarThresholds: red, yellow, green) */
static uint32_t pct_text (size_t hit, size_t total, char *t, size_t n) {
  double p = total ? 100.0 * (double)hit / (double)total : 0.0;
  snprintf(t, n, "%.1f%%", p);
  return p < 60 ? 0xF14C4C : p < 90 ? 0xCCA700 : 0x73C991;
}


/* TEST COVERAGE's title (every file together, and its x), or a file's row */
static void draw_cov_row (const TRow *r, int x, int sy, int w, int st) {
  char t[64];
  uint32_t rgb, bg = ui_color(C_SIDE_BG);
  int cx = x + 1, i, pw;
  if (r->kind == RW_COVHEAD) {
    size_t hit = 0, total = 0;
    for (i = 0; i < g_ncov; i++) {
      hit += g_cov[i].hit;
      total += g_cov[i].total;
    }
    cx += scr_puts(cx, sy, "TEST COVERAGE", st == S_SIDE ? S_SIDE_TITLE : st) + 1;
    rgb = pct_text(hit, total, t, sizeof(t));
    pw = (int)strlen(t);
    if (cx + pw < x + w - 4) {
      for (i = 0; t[i]; i++) {
        if (st == S_SIDE) scr_put_rgb(cx + i, sy, (unsigned char)t[i], rgb, bg, 0);
        else scr_put(cx + i, sy, (unsigned char)t[i], st);
      }
    }
    if (w > 12) scr_put(x + w - 3, sy, 0xEA76, st);	/* close: Test: Close Coverage */
    return;
  }
  {
    const CovFile *c = &g_cov[r->f];
    const char *sl = strrchr(c->rel, '/');
    int ist;
    uint32_t icon = file_icon(c->rel, &ist);
    rgb = pct_text(c->hit, c->total, t, sizeof(t));
    pw = (int)strlen(t);
    cx += 2;
    cx += scr_put(cx, sy, icon, st == S_SIDE ? ist : st) + 1;
    cx += scr_putsw(cx, sy, x + w - pw - 2 - cx, sl ? sl + 1 : c->rel, st) + 1;
    if (sl && cx < x + w - pw - 4) {
      char dir[512];
      snprintf(dir, sizeof(dir), "%.*s", (int)(sl - c->rel), c->rel);
      scr_putsw(cx, sy, x + w - pw - 2 - cx, dir, st == S_SIDE ? S_SIDE_DIM : st);
    }
    for (i = 0; t[i] && x + w - 1 - pw + i < x + w; i++) {
      int px = x + w - 1 - pw + i;
      if (st == S_SIDE) scr_put_rgb(px, sy, (unsigned char)t[i], rgb, bg, 0);
      else scr_put(px, sy, (unsigned char)t[i], st);
    }
  }
}


void test_draw (int x, int y, int w, int h, int focus) {
  int row, np = 0, nf = 0, ns = 0, nt = 0, a, b;
  char t[160];
  uint32_t bg = ui_color(C_SIDE_BG);
  test_show();
  scr_box(x, y, w, h, S_SIDE);
  if (h <= VHEAD) return;
  scr_puts(x + 2, y, "TESTING", S_SIDE_HEAD);
  if (w > 20) {	/* Run All (or Cancel), Refresh, Collapse All */
    if (g_on || g_nq) scr_put(x + w - 7, y, 0xEAD7, S_SIDE_HEAD);	/* debug-stop */
    else scr_put(x + w - 7, y, 0xEB9E, S_SIDE_HEAD);	/* run-all */
    scr_put(x + w - 5, y, 0xEB37, S_SIDE_HEAD);	/* refresh */
    scr_put(x + w - 3, y, 0xEAC5, S_SIDE_HEAD);	/* collapse-all */
  }
  for (a = 0; a < g_nf; a++)
    for (b = 0; b < g_f[a].n; b++) {
      int s = g_f[a].t[b].state;
      nt++;
      np += s == TM_PASS;
      nf += s == TM_FAIL;
      ns += s == TM_SKIP;
    }
  if (nt == 0) scr_putsw(x + 2, y + 1, w - 3, g_found ? "No tests have been found in this workspace yet." : "", S_SIDE_DIM);
  else if (g_on) {
    snprintf(t, sizeof(t), "Running tests... %d/%d", np + nf + ns, nt);
    scr_putsw(x + 2, y + 1, w - 3, t, S_SIDE_DIM);
  }
  else {	/* ✓ 3  ✗ 1  of 7 */
    int cx = x + 2;
    cx += scr_put_rgb(cx, y + 1, 0xEBA4, 0x73C991, bg, 0) + 1;
    snprintf(t, sizeof(t), "%d ", np);
    cx += scr_puts(cx, y + 1, t, S_SIDE_DIM);
    cx += scr_put_rgb(cx, y + 1, 0xEA87, 0xF14C4C, bg, 0) + 1;
    snprintf(t, sizeof(t), "%d ", nf);
    cx += scr_puts(cx, y + 1, t, S_SIDE_DIM);
    if (ns) {
      cx += scr_put_rgb(cx, y + 1, 0xEABD, 0x848484, bg, 0) + 1;
      snprintf(t, sizeof(t), "%d ", ns);
      cx += scr_puts(cx, y + 1, t, S_SIDE_DIM);
    }
    snprintf(t, sizeof(t), " %d test%s", nt, nt == 1 ? "" : "s");
    scr_putsw(cx, y + 1, x + w - cx - 1, t, S_SIDE_DIM);
  }
  rows();
  g_h = h - VHEAD;
  if (g_sel < g_top) g_top = g_sel;
  if (g_sel >= g_top + g_h) g_top = g_sel - g_h + 1;
  if (g_top < 0) g_top = 0;
  for (row = 0; row < g_h; row++) {
    int k = g_top + row, sy = y + VHEAD + row, st, cx = x + 1, s;
    const TRow *r;
    const TFile *f;
    uint32_t icon, rgb;
    if (k >= g_nrow) break;
    r = &g_row[k];
    st = (k == g_sel && focus) ? S_SIDE_SEL : (k == g_sel ? S_SIDE_CUR : S_SIDE);
    scr_fill(x, sy, w, st);
    if (r->kind == RW_COVHEAD || r->kind == RW_COV) {
      draw_cov_row(r, x, sy, w, st);
      continue;
    }
    f = &g_f[r->f];
    if (r->kind == RW_FILE) {
      const char *sl = strrchr(f->rel, '/');
      s = file_state(f);
      cx += scr_put(cx, sy, f->open ? 0xEAB4 : 0xEAB6, st) + 1;
      icon = state_icon(s, &rgb);
      if (st == S_SIDE) cx += scr_put_rgb(cx, sy, icon, rgb, bg, 0) + 1;
      else cx += scr_put(cx, sy, icon, st) + 1;
      cx += scr_putsw(cx, sy, x + w - cx - 4, sl ? sl + 1 : f->rel, st) + 1;
      if (sl && cx < x + w - 6) scr_putsw(cx, sy, x + w - cx - 4, f->rel, st == S_SIDE ? S_SIDE_DIM : st);
    }
    else if (r->kind == RW_TEST) {
      const TTest *tt = &f->t[r->t];
      cx += 4;
      icon = state_icon(tt->state, &rgb);
      if (st == S_SIDE) cx += scr_put_rgb(cx, sy, icon, rgb, bg, 0) + 1;
      else cx += scr_put(cx, sy, icon, st) + 1;
      if (tt->cls) snprintf(t, sizeof(t), "%s.%s", tt->cls, tt->name);
      else snprintf(t, sizeof(t), "%s", f->fw == FW_JS ? "npm test" : tt->name);
      cx += scr_putsw(cx, sy, x + w - cx - 4, t, st) + 1;
      if (tt->ms >= 0 && cx < x + w - 10) {
        snprintf(t, sizeof(t), "%ldms", tt->ms);
        scr_putsw(cx, sy, x + w - cx - 4, t, st == S_SIDE ? S_SIDE_DIM : st);
      }
    }
    else {	/* the failure's message, dim, under the test */
      const TTest *tt = &f->t[r->t];
      char *m = xstrdup(tt->msg), *c;
      for (c = m; *c; c++)
        if (*c == '\n' || *c == '\t' || *c == '\r') *c = ' ';
      scr_putsw(cx + 7, sy, x + w - cx - 8, m, st == S_SIDE ? S_SIDE_DIM : st);
      free(m);
      continue;
    }
    if (k == g_sel && w > 12) scr_put(x + w - 3, sy, 0xEB2C, st);	/* run: ▷ on the selected row, like VS Code's hover */
  }
  side_bar(x, y + VHEAD, w, g_h, (size_t)g_nrow, (size_t)g_top, (size_t)g_h);
}


static void open_row (int k, SideAct *act, int go) {
  const TRow *r;
  const TFile *f;
  if (k < 0 || k >= g_nrow) return;
  r = &g_row[k];
  if (r->kind == RW_COVHEAD) return;
  if (r->kind == RW_COV) {	/* a file of the coverage: open, its lines colored */
    act->what = go ? SA_GO : SA_OPEN;
    act->path = g_cov[r->f].path;
    act->line = 1;
    act->col = act->len = 0;
    return;
  }
  f = &g_f[r->f];
  act->what = go ? SA_GO : SA_OPEN;
  act->path = f->path;
  act->col = act->len = 0;
  if (r->kind == RW_FILE) act->line = 1;
  else if (r->kind == RW_MSG && f->t[r->t].floc) {
    act->path = f->t[r->t].floc;
    act->line = f->t[r->t].fline + 1;
  }
  else act->line = f->t[r->t].line + 1;
}


static void run_row (int k) {
  Pick1 p;
  if (k < 0 || k >= g_nrow || g_row[k].kind >= RW_COVHEAD) return;
  p.f = g_row[k].f;
  p.t = g_row[k].kind == RW_FILE ? -1 : g_row[k].t;
  run_picks(&p, 1);
}


int test_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  act->what = SA_NONE;
  rows();
  switch (code) {
    case K_UP: if (g_sel > 0) g_sel--; return 1;
    case K_DOWN: if (g_sel + 1 < g_nrow) g_sel++; return 1;
    case K_PGUP: g_sel = g_sel > g_h ? g_sel - g_h : 0; return 1;
    case K_PGDN: g_sel = g_sel + g_h < g_nrow ? g_sel + g_h : g_nrow - 1; return 1;
    case K_HOME: g_sel = 0; return 1;
    case K_END: g_sel = g_nrow - 1; return 1;
    case K_LEFT:
      if (g_nrow == 0 || g_row[g_sel].kind >= RW_COVHEAD) return 1;
      if (g_row[g_sel].kind == RW_FILE) g_f[g_row[g_sel].f].open = 0;
      else {	/* to its file */
        while (g_sel > 0 && g_row[g_sel].kind != RW_FILE) g_sel--;
      }
      return 1;
    case K_RIGHT:
      if (g_nrow && g_row[g_sel].kind == RW_FILE) g_f[g_row[g_sel].f].open = 1;
      return 1;
    case K_ENTER:
      if (k & KM_CTRL) run_row(g_sel);	/* Ctrl+Enter: run it */
      else if (g_nrow && g_row[g_sel].kind == RW_FILE && !g_f[g_row[g_sel].f].open) g_f[g_row[g_sel].f].open = 1;
      else open_row(g_sel, act, 1);
      return 1;
    case ' ': open_row(g_sel, act, 0); return 1;
  }
  if (k == 'r' || k == 'R') {	/* run the one selected */
    run_row(g_sel);
    return 1;
  }
  return 0;
}


void test_click (int row, int col, SideAct *act) {
  int w = side_width(), k;
  act->what = SA_NONE;
  if (row == 0) {	/* the title's icons */
    if (col >= w - 8 && col <= w - 6) test_command(g_on || g_nq ? CMD_TEST_CANCEL : CMD_TEST_RUN_ALL);
    else if (col >= w - 6 && col <= w - 4) test_command(CMD_TEST_REFRESH);
    else if (col >= w - 4) test_command(CMD_TEST_COLLAPSE);
    return;
  }
  if (row < VHEAD) return;
  rows();
  k = g_top + row - VHEAD;
  if (k >= g_nrow) return;
  g_sel = k;
  if (g_row[k].kind == RW_COVHEAD) {	/* its x: the coverage goes */
    if (col >= w - 4) test_command(CMD_TEST_COV_CLOSE);
    return;
  }
  if (col >= w - 4 && g_row[k].kind != RW_MSG && g_row[k].kind != RW_COV) {	/* ▷ */
    run_row(k);
    return;
  }
  if (g_row[k].kind == RW_FILE) {	/* a file: its tests shown, hidden */
    g_f[g_row[k].f].open = !g_f[g_row[k].f].open;
    return;
  }
  open_row(k, act, 0);
}


void test_wheel (int d) {
  rows();
  g_top += d * wheel_step(0);
  if (g_top > g_nrow - g_h) g_top = g_nrow - g_h;
  if (g_top < 0) g_top = 0;
  if (g_sel < g_top) g_sel = g_top;
  if (g_sel >= g_top + g_h) g_sel = g_top + g_h - 1;
}

/* }================================================================== */


/*
** {==================================================================
** The editor's side: the gutter, the commands
** ===================================================================
*/

/* how many failed, for the activity bar's badge */
int test_failed (void) {
  int a, b, n = 0;
  for (a = 0; a < g_nf; a++)
    for (b = 0; b < g_f[a].n; b++) n += g_f[a].t[b].state == TM_FAIL;
  return n;
}


/* the gutter's icon at a test's line: ▷ not run, ✓, ✗ (0: none); the file is read the first time */
int test_mark (const char *path, size_t line) {
  TFile *f;
  int i, fw;
  if (path == NULL || !opt.test_gutter) return TM_NONE;
  f = file_of(path);
  if (f == NULL) {
    if ((fw = fw_of(path_basename(path))) < 0) return TM_NONE;
    f = add_file(path, fw);
    if (g_found) qsort(g_f, (size_t)g_nf, sizeof(TFile), cmp_file);
    f = file_of(path);
  }
  for (i = 0; f && i < f->n; i++)
    if (f->t[i].line == line) return f->t[i].state;
  return TM_NONE;
}


void test_gutter (const char *path, size_t line) {
  TFile *f = file_of(path);
  int i;
  for (i = 0; f && i < f->n; i++)
    if (f->t[i].line == line) {
      Pick1 p;
      p.f = (int)(f - g_f);
      p.t = i;
      run_picks(&p, 1);
      return;
    }
}


/* the test the cursor is in: the last one that starts at or above it */
static int at_cursor (Pick1 *p) {
  const char *path = editor_file();
  size_t line = editor_line(), best = 0;
  TFile *f;
  int i, found = -1;
  if (path == NULL) return 0;
  test_mark(path, 0);	/* read it when it is new */
  f = file_of(path);
  for (i = 0; f && i < f->n; i++)
    if (f->t[i].line <= line && (found < 0 || f->t[i].line >= best)) {
      found = i;
      best = f->t[i].line;
    }
  if (found < 0) return 0;
  p->f = (int)(f - g_f);
  p->t = found;
  return 1;
}


/* Test: Debug Test at Cursor: the debugger, with the test's configuration */
static void debug_at_cursor (void) {
  Pick1 p;
  TFile *f;
  TTest *t;
  Buf b;
  char *dir;
  if (!at_cursor(&p)) {
    toast(0, "No test found at the cursor.");
    return;
  }
  f = &g_f[p.f];
  t = &f->t[p.t];
  buf_init(&b);
  dir = path_dirname(f->path);
  if (f->fw == FW_GO) {
    buf_puts(&b, "{\"name\":\"Debug Test\",\"type\":\"go\",\"request\":\"launch\",\"mode\":\"test\",\"program\":");
    json_put_str(&b, dir, strlen(dir));
    buf_printf(&b, ",\"args\":[\"-test.run\",\"^%s$\"]}", t->name);
  }
  else if (f->fw == FW_PY) {
    char *id, *a = t->cls ? xstrcat3(f->rel, "::", t->cls) : xstrdup(f->rel);
    id = xstrcat3(a, "::", t->name);
    buf_puts(&b, "{\"name\":\"Debug Test\",\"type\":\"debugpy\",\"request\":\"launch\",\"module\":\"pytest\",\"args\":[");
    json_put_str(&b, id, strlen(id));
    buf_puts(&b, "],\"cwd\":");
    json_put_str(&b, side_root(), strlen(side_root()));
    buf_puts(&b, ",\"console\":\"internalConsole\"}");
    free(id);
    free(a);
  }
  else {
    toast(0, "Debugging %s tests is not supported: run it instead.", f->fw == FW_RUST ? "Rust" : "npm");
    buf_free(&b);
    free(dir);
    return;
  }
  buf_putc(&b, '\0');
  editor_save_all();
  dbg_start_json(b.s);
  buf_free(&b);
  free(dir);
}


void test_command (int cmd) {
  Pick1 p;
  int i;
  switch (cmd) {
    case CMD_TEST_RUN_ALL: run_all(); break;
    case CMD_TEST_REFRESH:
      discover();
      toast(0, "Found tests in %d file%s", g_nf, g_nf == 1 ? "" : "s");
      break;
    case CMD_TEST_CANCEL: cancel(); break;
    case CMD_TEST_COLLAPSE:
      for (i = 0; i < g_nf; i++) g_f[i].open = 0;
      break;
    case CMD_TEST_RUN_CURSOR:
      if (at_cursor(&p)) run_picks(&p, 1);
      else toast(0, "No test found at the cursor.");
      break;
    case CMD_TEST_RUN_FILE: {
      const char *path = editor_file();
      TFile *f;
      if (path) test_mark(path, 0);
      f = path ? file_of(path) : NULL;
      if (f == NULL || f->n == 0) {
        toast(0, "No tests found in this file.");
        break;
      }
      p.f = (int)(f - g_f);
      p.t = -1;
      run_picks(&p, 1);
      break;
    }
    case CMD_TEST_RERUN:
      if (g_nlast > 0 && !trust_require("Running tests")) break;	/* Restricted Mode */
      if (g_nlast == 0) {
        toast(0, "No test run to run again.");
        break;
      }
      for (i = 0; i < g_nlast; i++) {
        Run r;
        run_copy(&r, &g_last[i]);
        enqueue(&r);
      }
      editor_save_all();
      test_idle();
      break;
    case CMD_TEST_DEBUG_CURSOR: debug_at_cursor(); break;
    case CMD_TEST_COV_ALL: case CMD_TEST_COV_FILE: case CMD_TEST_COV_CURSOR:	/* the same, with coverage */
      g_cover = 1;
      test_command(cmd == CMD_TEST_COV_ALL ? CMD_TEST_RUN_ALL : cmd == CMD_TEST_COV_FILE ? CMD_TEST_RUN_FILE : CMD_TEST_RUN_CURSOR);
      g_cover = 0;
      break;
    case CMD_TEST_COV_CLOSE:
      if (!g_cov_on) toast(0, "No coverage is shown.");
      cov_clear();
      break;
    case CMD_TEST_COV_INLINE:
      g_cov_inline = !g_cov_inline;
      if (!g_cov_on) toast(0, "Run tests with coverage first (Test: Run All Tests with Coverage).");
      break;
  }
}

/* }================================================================== */
