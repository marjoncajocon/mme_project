/*
** egit.c - git: the Source Control view and the diff editor, like VS Code's
**
** git is run as a program (git status, git diff ...) and its output read
** through a pipe. The view lists the staged changes and the changes of
** the working tree, with a box for the commit message on top. A change
** opens in the diff editor: the whole file, old on the left and new on the
** right (or one above the other when there is no room), the changed lines
** red and green and the changed letters in them brighter. Under the
** changes is the GRAPH of the commits (egitlog.c draws it); a commit's
** file opens the same diff editor between the commit and its parent.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** Running programs
** ===================================================================
*/

char *find_program (const char *name) {
  char *path = os_getenv("PATH");
  Vec dirs;
  size_t i;
  char *found = NULL;
  if (path == NULL) return NULL;
  vec_init(&dirs);
  path_list_split(path, &dirs);
  for (i = 0; i < dirs.n && found == NULL; i++) {
#ifdef _WIN32
    static const char *const ext[] = {".exe", ".cmd", ".bat"};
    size_t e;
    for (e = 0; e < 3 && found == NULL; e++) {
      char *base = path_join(dirs.v[i], name);
      char *p = xstrcat3(base, ext[e], "");
      free(base);
      if (os_is_exec(p)) found = p;
      else free(p);
    }
#else
    char *p = path_join(dirs.v[i], name);
    if (os_is_exec(p)) found = p;
    else free(p);
#endif
  }
  vec_free(&dirs);
  free(path);
  return found;
}


int run_capture (char **argv, Buf *out) {
  int fds[2], io[3], null;
  OsProc proc;
  long pid;
  char chunk[8192];
  long n;
#ifdef _WIN32
  null = os_open("NUL", OS_WRITE);
#else
  null = os_open("/dev/null", OS_WRITE);
#endif
  if (os_pipe(fds) != 0) return -1;
  io[0] = -1;
  io[1] = fds[1];
  io[2] = null;
  if (os_spawn(argv[0], argv, NULL, io, 3, &proc, &pid) != 0) {
    os_close(fds[0]);
    os_close(fds[1]);
    if (null >= 0) os_close(null);
    return -1;
  }
  os_close(fds[1]);
  if (null >= 0) os_close(null);
  while ((n = os_read(fds[0], chunk, sizeof(chunk))) > 0) buf_putn(out, chunk, (size_t)n);
  os_close(fds[0]);
  buf_putc(out, '\0');
  out->len--;
  return os_wait(proc);
}


static char *g_git;	/* the program */
static char *g_top;	/* the repository's folder, native; NULL: not one */
static char g_branch[128];
static int g_nogit;	/* git is not installed */


/* run_capture, with what the program says on stderr in out too */
static int run_capture_err (char **argv, Buf *out) {
  int fds[2], io[3];
  OsProc proc;
  long pid, n;
  char chunk[8192];
  if (os_pipe(fds) != 0) return -1;
  io[0] = -1;
  io[1] = fds[1];
  io[2] = fds[1];
  if (os_spawn(argv[0], argv, NULL, io, 3, &proc, &pid) != 0) {
    os_close(fds[0]);
    os_close(fds[1]);
    return -1;
  }
  os_close(fds[1]);
  while ((n = os_read(fds[0], chunk, sizeof(chunk))) > 0) buf_putn(out, chunk, (size_t)n);
  os_close(fds[0]);
  buf_putc(out, '\0');
  out->len--;
  return os_wait(proc);
}


static int have_git (void) {
  if (g_git == NULL) {
    g_git = find_program("git");
    os_setenv("GIT_TERMINAL_PROMPT", "0");	/* a password asked for would break the screen */
  }
  if (g_git == NULL) g_nogit = 1;
  return g_git != NULL;
}


/* git -C <top> with the arguments of args (NULL ends them); err: stderr into out too */
int git_exec (Buf *out, int err, const char *const *args) {
  char *argv[64];
  int n = 0, rc, i;
  size_t from = out->len;
  long long t0 = os_now_us();
  Buf cmd;
  if (!have_git()) return -1;
  argv[n++] = g_git;
  argv[n++] = (char *)"-C";
  argv[n++] = (char *)(g_top ? g_top : side_root());
  while (*args && n < 63) argv[n++] = (char *)*args++;
  argv[n] = NULL;
  rc = err ? run_capture_err(argv, out) : run_capture(argv, out);
  buf_init(&cmd);	/* the OUTPUT view's Git channel, like VS Code's: "> git status -z [12ms]" */
  for (i = 3; i < n; i++) buf_printf(&cmd, " %s", argv[i]);
  buf_putc(&cmd, '\0');
  out_log("Git", "[%s] > git%s [%ldms]", rc == 0 ? "info" : "error", cmd.s, (long)((os_now_us() - t0) / 1000));
  if (rc != 0 && out->len > from) out_append("Git", out->s + from, out->len - from < 4096 ? out->len - from : 4096);
  buf_free(&cmd);
  return rc;
}


/* the repository's folder, native; NULL: not one */
const char *git_root (void) {
  return g_top;
}


/* git -C <top or root> args...; the output in out, the exit code */
static int git (Buf *out, const char *a0, const char *a1, const char *a2,
                const char *a3, const char *a4, const char *a5) {
  const char *args[8];
  int n = 0;
  if (a0) args[n++] = a0;
  if (a1) args[n++] = a1;
  if (a2) args[n++] = a2;
  if (a3) args[n++] = a3;
  if (a4) args[n++] = a4;
  if (a5) args[n++] = a5;
  args[n] = NULL;
  return git_exec(out, 0, args);
}


static void chomp (char *s) {
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}


static void to_native (char *s) {
#ifdef _WIN32
  for (; *s; s++)
    if (*s == '/') *s = '\\';
#else
  (void)s;
#endif
}


/* path (native) from the repository's top, with '/'; NULL: not in it */
char *git_rel_of (const char *path) {
  char *real, *r, *c;
  size_t n;
  if (g_top == NULL || path == NULL) return NULL;
  real = os_realpath(path);
  if (real == NULL) real = xstrdup(path);
  n = strlen(g_top);
  if (m_fnncmp(real, g_top, n) != 0 || !path_is_sep(real[n])) {
    free(real);
    return NULL;
  }
  r = xstrdup(real + n + 1);
  free(real);
  for (c = r; *c; c++)
    if (*c == '\\') *c = '/';
  return r;
}


/* a blob as git keeps it: ":rel" the index's, "HEAD:rel" the last commit's; NULL: none */
char *git_blob (const char *spec, size_t *len) {
  Buf b;
  const char *a[] = {"cat-file", "blob", spec, NULL};
  if (g_top == NULL) return NULL;
  buf_init(&b);
  if (git_exec(&b, 0, a) != 0) {
    buf_free(&b);
    return NULL;
  }
  *len = b.len;
  if (b.s == NULL) buf_putc(&b, '\0');
  return buf_take(&b);
}


/*
** The index's copy of rel becomes s[0..n): the bytes stored as they are
** (hash-object --no-filters of a temporary file), its mode kept; VS Code
** stages a change the same way, with the text of the whole file.
*/
int git_index_put (const char *rel, const char *s, size_t n) {
  char name[64], *tmp, *dir, mode[16] = "100644", sha[64];
  Buf b;
  int fd, r;
  const char *ls[] = {"ls-files", "-s", "--", rel, NULL};
  if (g_top == NULL) return -1;
  buf_init(&b);
  if (git_exec(&b, 0, ls) == 0 && b.s && b.len > 7 && b.s[6] == ' ') {	/* "100644 <sha> 0\trel" */
    memcpy(mode, b.s, 6);
    mode[6] = '\0';
  }
  buf_free(&b);
  dir = os_getenv("TEMP");	/* where temporary files go */
  if (dir == NULL) dir = os_getenv("TMPDIR");
  if (dir == NULL) dir = xstrdup("/tmp");
  snprintf(name, sizeof(name), "mme-stage-%lld.tmp", os_now_us());
  tmp = path_join(dir ? dir : ".", name);
  free(dir);
  if ((fd = os_open(tmp, OS_WRITE)) < 0) {
    free(tmp);
    return -1;
  }
  os_write(fd, s, n);
  os_close(fd);
  buf_init(&b);
  {
    const char *h[] = {"hash-object", "-w", "--no-filters", "--", tmp, NULL};
    r = git_exec(&b, 0, h);
  }
  os_unlink(tmp);
  free(tmp);
  if (r != 0 || b.s == NULL || b.len < 40) {
    buf_free(&b);
    return -1;
  }
  memcpy(sha, b.s, 40);
  sha[40] = '\0';
  buf_free(&b);
  {
    char info[512];
    const char *u[] = {"update-index", "--add", "--cacheinfo", info, NULL};
    snprintf(info, sizeof(info), "%s,%s,%s", mode, sha, rel);
    buf_init(&b);
    r = git_exec(&b, 1, u);
    if (r != 0) git_error(&b, "Could not stage the change");
    buf_free(&b);
  }
  return r == 0 ? 0 : -1;
}

/* }================================================================== */


/*
** {==================================================================
** Status
** ===================================================================
*/

typedef struct Change {
  char *rel;	/* from the top, with '/' */
  char *path;	/* native */
  char x, y;	/* git status --porcelain: the index, the tree */
} Change;

static Change *g_ch;
static size_t g_nch, g_capch;

/*
** the rows: the message box, "Staged Changes", staged ones, "Changes",
** others; then the GRAPH: its commits, an open commit's files, "Load More"
*/
enum { R_MSG, R_STAGED_HEAD, R_STAGED, R_HEAD, R_CHANGE, R_GRAPH, R_COMMIT, R_CFILE, R_MORE,
       R_MERGE_HEAD, R_MERGE, R_BUTTON };	/* Merge Changes; the Publish / Sync button */

typedef struct GRow {
  int kind;
  size_t ch;	/* the change; R_COMMIT, R_CFILE: the commit */
  size_t f;	/* R_CFILE: the file of the commit */
} GRow;

static int g_graph_open = 1;	/* the GRAPH section shows its commits */

static GRow *g_row;
static size_t g_nrow, g_caprow;
static size_t g_sel, g_top_row;
static int g_h = 1;
static char g_msg[512];	/* the commit message */

#define HEAD	3	/* "SOURCE CONTROL", the branch, the message */


static void changes_free (void) {
  size_t i;
  for (i = 0; i < g_nch; i++) {
    free(g_ch[i].rel);
    free(g_ch[i].path);
  }
  g_nch = 0;
}


static void add_row (int kind, size_t ch) {
  if (g_nrow == g_caprow) {
    g_caprow = g_caprow ? g_caprow * 2 : 64;
    g_row = (GRow *)xrealloc(g_row, g_caprow * sizeof(GRow));
  }
  g_row[g_nrow].kind = kind;
  g_row[g_nrow].ch = ch;
  g_row[g_nrow].f = 0;
  g_nrow++;
}


/* a conflict of a merge: git status' DD AU UD UA DU AA UU */
static int is_merge (const Change *c) {
  return c->x == 'U' || c->y == 'U' || (c->x == 'A' && c->y == 'A') || (c->x == 'D' && c->y == 'D');
}


static int is_staged (const Change *c) {
  return c->x != ' ' && c->x != '?' && !is_merge(c);
}


static int is_changed (const Change *c) {
  return c->y != ' ' && !is_merge(c);
}


int git_conflicted (const char *path) {
  size_t i;
  for (i = 0; i < g_nch; i++)
    if (is_merge(&g_ch[i]) && m_fncmp(g_ch[i].path, path) == 0) return 1;
  return 0;
}


/* the button under the message box: nothing to commit and the branch not in step with its remote */
static int button_kind (void) {
  if (g_top == NULL || g_nch > 0 || g_branch[0] == '\0') return 0;
  if (!git_has_upstream()) return 1;	/* Publish Branch */
  if (git_sync_text()[0]) return 2;	/* Sync Changes 1↓ 2↑ */
  return 0;
}


static void build_rows (void) {
  size_t i, ns = 0, nc = 0, nm = 0;
  g_nrow = 0;
  if (button_kind()) add_row(R_BUTTON, 0);
  for (i = 0; i < g_nch; i++) {
    ns += is_staged(&g_ch[i]);
    nc += is_changed(&g_ch[i]);
    nm += is_merge(&g_ch[i]);
  }
  if (nm) {	/* VS Code: Merge Changes first */
    add_row(R_MERGE_HEAD, nm);
    for (i = 0; i < g_nch; i++)
      if (is_merge(&g_ch[i])) add_row(R_MERGE, i);
  }
  if (ns) {
    add_row(R_STAGED_HEAD, ns);
    for (i = 0; i < g_nch; i++)
      if (is_staged(&g_ch[i])) add_row(R_STAGED, i);
  }
  add_row(R_HEAD, nc);
  for (i = 0; i < g_nch; i++)
    if (is_changed(&g_ch[i])) add_row(R_CHANGE, i);
  if (g_top && graph_count() > 0) {
    add_row(R_GRAPH, graph_count());
    for (i = 0; g_graph_open && i < graph_count(); i++) {
      size_t f, nf;
      add_row(R_COMMIT, i);
      nf = graph_open(i) ? graph_nfiles(i) : 0;
      for (f = 0; f < nf; f++) {
        add_row(R_CFILE, i);
        g_row[g_nrow - 1].f = f;
      }
    }
    if (g_graph_open && graph_more()) add_row(R_MORE, 0);
  }
  if (g_sel > g_nrow) g_sel = g_nrow;
}


static int cmp_change (const void *a, const void *b) {
  return strcmp(((const Change *)a)->rel, ((const Change *)b)->rel);
}


void git_refresh (void) {
  Buf b;
  size_t i;
  blame_clear();	/* read again when it is asked for */
  quick_clear();
  changes_free();
  free(g_top);
  g_top = NULL;
  g_branch[0] = '\0';
  buf_init(&b);
  if (git(&b, "rev-parse", "--show-toplevel", NULL, NULL, NULL, NULL) != 0 || b.len == 0) {
    buf_free(&b);
    graph_load();
    build_rows();
    return;
  }
  chomp(b.s);
  to_native(b.s);
  g_top = buf_take(&b);
  buf_init(&b);
  if (git(&b, "branch", "--show-current", NULL, NULL, NULL, NULL) == 0) {
    chomp(b.s);
    snprintf(g_branch, sizeof(g_branch), "%s", b.s);
  }
  buf_free(&b);
  buf_init(&b);
  if (git(&b, "status", "--porcelain=v1", "-z", "-uall", NULL, NULL) == 0) {
    for (i = 0; i + 3 < b.len;) {	/* "XY path\0", renames: "XY new\0old\0" */
      const char *e = b.s + i;
      size_t len = strlen(e + 3);
      Change *c;
      if (g_nch == g_capch) {
        g_capch = g_capch ? g_capch * 2 : 64;
        g_ch = (Change *)xrealloc(g_ch, g_capch * sizeof(Change));
      }
      c = &g_ch[g_nch++];
      c->x = e[0];
      c->y = e[1];
      c->rel = xstrndup(e + 3, len);
      c->path = path_join(g_top, c->rel);
      to_native(c->path);
      i += 3 + len + 1;
      if (e[0] == 'R' || e[0] == 'C') i += strlen(b.s + i) + 1;
    }
    qsort(g_ch, g_nch, sizeof(Change), cmp_change);
  }
  buf_free(&b);
  graph_load();
  build_rows();
}


const char *git_branch (void) {
  return g_top ? (g_branch[0] ? g_branch : "HEAD") : "";
}


int git_count (void) {
  return (int)g_nch;
}


int git_mark (const char *path, int dir) {
  size_t i, n = strlen(path);
  for (i = 0; i < g_nch; i++) {
    const Change *c = &g_ch[i];
    if (dir) {
      if (m_fnncmp(c->path, path, n) == 0 && path_is_sep(c->path[n])) return 'M';
      continue;
    }
    if (m_fncmp(c->path, path) != 0) continue;
    if (c->y == '?') return 'U';
    if (c->y == 'D' || c->x == 'D') return 'D';
    if (c->x == 'A') return 'A';
    return 'M';
  }
  return 0;
}


/* the letter VS Code shows for a change in the list */
static int letter (const Change *c, int staged) {
  int l = staged ? c->x : c->y;
  if (l == '?') return 'U';
  return l;
}

/* }================================================================== */


/*
** {==================================================================
** The view
** ===================================================================
*/

#define S_BUTTON	S_STATUS	/* VS Code's blue button.background, as the dialogs' */
#define S_BUTTON_ON	S_STATUS_ITEM


int git_letter_style (int l) {
  switch (l) {
    case 'A': return S_GIT_A;
    case 'D': return S_GIT_D;
    case 'R': return S_GIT_A;
    case 'U': return S_GIT_U;
    case '!': return S_GIT_D;	/* a conflict */
  }
  return S_GIT_M;
}


void git_draw (int x, int y, int w, int h, int focus) {
  int row;
  scr_box(x, y, w, h, S_SIDE);
  if (h <= HEAD) return;
  scr_puts(x + 2, y, "SOURCE CONTROL", S_SIDE_HEAD);
  if (g_top && w > 24) {	/* VS Code's title actions: Commit, Refresh, More Actions */
    scr_put(x + w - 7, y, 0xEAB2, S_SIDE_HEAD);	/* codicon check */
    scr_put(x + w - 5, y, 0xEB37, S_SIDE_HEAD);	/* refresh */
    scr_put(x + w - 3, y, 0xEA7C, S_SIDE_HEAD);	/* ellipsis */
  }
  if (g_nogit) {
    scr_putsw(x + 2, y + 2, w - 3, "git was not found in PATH.", S_SIDE_DIM);
    return;
  }
  if (g_top == NULL) {	/* VS Code's welcome: Initialize Repository, Clone Repository */
    scr_putsw(x + 2, y + 2, w - 3, "The folder currently open doesn't", S_SIDE_DIM);
    scr_putsw(x + 2, y + 3, w - 3, "have a git repository.", S_SIDE_DIM);
    if (h > 9) {
      int bw = w - 4, i;
      static const char *const label[] = {"Initialize Repository", "Clone Repository"};
      for (i = 0; i < 2; i++) {
        int by = y + 5 + 2 * i, st = (focus && (int)g_sel == i) ? S_BUTTON_ON : S_BUTTON;
        int lw = (int)strlen(label[i]);
        scr_fill(x + 2, by, bw, st);
        scr_puts(x + 2 + (bw > lw ? (bw - lw) / 2 : 0), by, label[i], st);
      }
    }
    return;
  }
  scr_put(x + 2, y + 1, 0xE725, S_SIDE_DIM);	/* git-branch */
  scr_putsw(x + 4, y + 1, w - 5, g_branch[0] ? g_branch : "HEAD", S_SIDE_DIM);
  /* the message box: row 0 of the list */
  scr_fill(x + 1, y + 2, w - 2, S_INPUT);
  if (g_msg[0]) scr_putsw(x + 2, y + 2, w - 4, g_msg, S_INPUT_ON);
  else {
    char hint[160];
    snprintf(hint, sizeof(hint), "Message (Ctrl+Enter to commit on '%s')",
             g_branch[0] ? g_branch : "HEAD");
    scr_putsw(x + 2, y + 2, w - 4, hint, S_INPUT_HINT);
  }
  if (focus && g_sel == 0) {
    int cx = x + 2 + (int)str_cols(g_msg);
    scr_cursor(cx < x + w - 2 ? cx : x + w - 2, y + 2);
  }
  g_h = h - HEAD;
  if (g_sel > 0) {	/* rows of the list are g_sel - 1 */
    size_t k = g_sel - 1;
    if (k < g_top_row) g_top_row = k;
    if (k >= g_top_row + (size_t)g_h) g_top_row = k - (size_t)g_h + 1;
  }
  for (row = 0; row < g_h; row++) {
    size_t k = g_top_row + (size_t)row;
    const GRow *r;
    int sy = y + HEAD + row, st;
    if (k >= g_nrow) break;
    r = &g_row[k];
    st = (k + 1 == g_sel && focus) ? S_SIDE_SEL : (k + 1 == g_sel ? S_SIDE_CUR : S_SIDE);
    scr_fill(x, sy, w, st);
    if (r->kind == R_GRAPH) {	/* the GRAPH section's title */
      scr_put(x + 1, sy, g_graph_open ? 0xEAB4 : 0xEAB6, st == S_SIDE ? S_SIDE_HEAD : st);
      scr_puts(x + 3, sy, "GRAPH", st == S_SIDE ? S_SIDE_HEAD : st);
      continue;
    }
    if (r->kind == R_COMMIT) {
      graph_draw(r->ch, x + 1, sy, w - 1, st);
      continue;
    }
    if (r->kind == R_CFILE) {
      graph_draw_file(r->ch, r->f, x + 1, sy, w - 1, st);
      continue;
    }
    if (r->kind == R_MORE) {
      scr_putsw(x + 3, sy, w - 4, "Load More...", st == S_SIDE ? S_SIDE_DIM : st);
      continue;
    }
    if (r->kind == R_BUTTON) {	/* VS Code's big button: Publish Branch / Sync Changes */
      char t[96];
      int bst = (k + 1 == g_sel && focus) ? S_BUTTON_ON : S_BUTTON, bw = w - 2, tw;
      if (button_kind() == 1) snprintf(t, sizeof(t), "\xEE\xAB\x83 Publish Branch");	/* codicon cloud-upload */
      else snprintf(t, sizeof(t), "\xEE\xA9\xB7 Sync Changes %s", git_sync_text());	/* codicon sync */
      scr_fill(x, sy, w, S_SIDE);
      scr_fill(x + 1, sy, bw, bst);
      tw = (int)str_cols(t);
      scr_puts(x + 1 + (bw > tw ? (bw - tw) / 2 : 0), sy, t, bst);
      continue;
    }
    if (r->kind == R_STAGED_HEAD || r->kind == R_HEAD || r->kind == R_MERGE_HEAD) {
      char num[32];
      snprintf(num, sizeof(num), " %lu ", (unsigned long)r->ch);
      scr_put(x + 1, sy, 0xEAB4, st);
      scr_puts(x + 3, sy, r->kind == R_HEAD ? "Changes" : r->kind == R_MERGE_HEAD ? "Merge Changes" : "Staged Changes",
               st == S_SIDE ? S_SIDE_TITLE : st);
      scr_puts(x + w - (int)strlen(num) - 1, sy, num, st == S_SIDE ? S_TOGGLE_ON : st);
    }
    else {
      const Change *c = &g_ch[r->ch];
      const char *base = strrchr(c->rel, '/');
      int l = r->kind == R_MERGE ? '!' : letter(c, r->kind == R_STAGED), ist, cx = x + 3;
      uint32_t icon;
      base = base ? base + 1 : c->rel;
      icon = file_icon(base, &ist);
      scr_put(cx, sy, icon, st == S_SIDE ? ist : st);
      cx += 2;
      cx += scr_putsw(cx, sy, x + w - cx - 3, base, st == S_SIDE ? git_letter_style(l) : st);
      if (base != c->rel && cx + 2 < x + w - 3) {
        char *dir = xstrndup(c->rel, (size_t)(base - c->rel - 1));
        scr_putsw(cx + 1, sy, x + w - cx - 4, dir, st == S_SIDE ? S_SIDE_DIM : st);
        free(dir);
      }
      scr_put(x + w - 2, sy, (uint32_t)l, st == S_SIDE ? git_letter_style(l) : st);
    }
  }
}


static void commit (void);

/* Git: Commit (the check in the title): the message box's text; else it gets the keys */
int git_commit (int amend) {
  Buf b;
  int r;
  if (!amend) {
    if (g_msg[0] == '\0') {
      g_sel = 0;	/* type the message first */
      toast(0, "Please provide a commit message (Ctrl+Enter commits)");
      return 0;
    }
    commit();
    return 1;
  }
  buf_init(&b);
  if (g_msg[0]) {
    const char *a[] = {"commit", "-q", "--amend", "-m", g_msg, NULL};
    r = git_exec(&b, 1, a);
  }
  else {
    const char *a[] = {"commit", "-q", "--amend", "--no-edit", NULL};
    r = git_exec(&b, 1, a);
  }
  if (r != 0) git_error(&b, "git commit --amend failed");
  else {
    toast(0, "Amended the last commit");
    g_msg[0] = '\0';
  }
  buf_free(&b);
  git_refresh();
  return 1;
}


static void commit (void) {
  Buf b;
  size_t i, staged = 0;
  int r;
  for (i = 0; i < g_nch; i++) staged += is_staged(&g_ch[i]);
  if (g_msg[0] == '\0') {
    toast(1, "Please provide a commit message.");
    return;
  }
  if (staged == 0) {	/* VS Code's smart commit: git.enableSmartCommit, git.suggestSmartCommit */
    static const char *const bt[] = {"Yes", "Always", "Never", "Cancel"};
    if (g_nch == 0) {
      toast(1, "There are no changes to commit.");
      return;
    }
    if (!opt.smart_commit) {
      if (!opt.suggest_smart_commit) {
        toast(1, "There are no staged changes to commit.");
        return;
      }
      switch (dialog("There are no staged changes to commit.",
                     "Would you like to stage all your changes and commit them directly?", bt, 4)) {
        case 0: break;
        case 1:
          opt.smart_commit = 1;
          settings_put_raw("git.enableSmartCommit", "true");
          break;
        case 2:
          opt.suggest_smart_commit = 0;
          settings_put_raw("git.suggestSmartCommit", "false");
          return;
        default: return;
      }
    }
    buf_init(&b);
    git(&b, "add", "-A", NULL, NULL, NULL, NULL);
    buf_free(&b);
  }
  buf_init(&b);
  r = git(&b, "commit", "-q", "-m", g_msg, NULL, NULL);
  buf_free(&b);
  if (r != 0) toast(1, "git commit failed (is user.name / user.email set?)");
  else {
    toast(0, "Committed: %s", g_msg);
    g_msg[0] = '\0';
  }
  git_refresh();
}


static void stage (const Change *c, int on) {
  Buf b;
  buf_init(&b);
  if (on) git(&b, "add", "-A", "--", c->rel, NULL, NULL);
  else if (git(&b, "restore", "--staged", "--", c->rel, NULL, NULL) != 0)
    git(&b, "rm", "--cached", "-q", "--", c->rel, NULL);	/* no HEAD yet */
  buf_free(&b);
  git_refresh();
}


/* Enter or a click on a GRAPH row: open / close, a file's diff, more commits */
static int graph_row (size_t k, int go, SideAct *act) {
  const GRow *r = &g_row[k];
  switch (r->kind) {
    case R_GRAPH:
      g_graph_open = !g_graph_open;
      build_rows();
      return 1;
    case R_COMMIT:
      graph_toggle(r->ch);
      build_rows();
      return 1;
    case R_CFILE:
      if (graph_diff(r->ch, r->f) == 0) {
        act->what = SA_SHOW_DIFF;
        act->go = go;
      }
      return 1;
    case R_MORE:
      graph_load_more();
      build_rows();
      return 1;
  }
  return 0;
}


static void open_row (size_t k, int go, SideAct *act) {
  const GRow *r = &g_row[k];
  if (graph_row(k, go, act)) return;
  if (r->kind == R_BUTTON) {
    act->what = SA_CMD;
    act->cmd = button_kind() == 1 ? CMD_GIT_PUBLISH : CMD_GIT_SYNC;
    return;
  }
  if (r->kind == R_MERGE) {	/* a conflicted file: the file itself, with its markers */
    act->what = go ? SA_GO : SA_OPEN;
    act->path = g_ch[r->ch].path;
    return;
  }
  if (r->kind != R_STAGED && r->kind != R_CHANGE) return;
  act->what = SA_DIFF;
  act->path = g_ch[r->ch].path;
  act->staged = (r->kind == R_STAGED);
  act->go = go;
}


int git_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  size_t len = strlen(g_msg);
  if (code == K_F5) {
    git_refresh();
    return 1;
  }
  if (g_top == NULL && !g_nogit) {	/* the two buttons */
    if (code == K_UP || code == K_DOWN) g_sel = code == K_UP ? 0 : 1;
    else if (code == K_ENTER || code == ' ') {
      act->what = SA_CMD;
      act->cmd = g_sel == 1 ? CMD_GIT_CLONE : CMD_GIT_INIT;
    }
    else return 0;
    return 1;
  }
  if (code == K_UP) {
    if (g_sel > 0) g_sel--;
    return 1;
  }
  if (code == K_DOWN) {
    if (g_sel < g_nrow) g_sel++;
    return 1;
  }
  if (g_sel == 0) {	/* the message box */
    if (code == K_ENTER && (k & KM_CTRL)) commit();
    else if (code == K_ENTER) {
      if (len + 1 < sizeof(g_msg)) strcat(g_msg, "\n");
    }
    else if (k == CTRL('j')) commit();	/* Ctrl+Enter in a plain terminal */
    else if (code == K_BS) {
      while (len > 0 && ((unsigned char)g_msg[len - 1] & 0xC0) == 0x80) len--;
      if (len > 0) g_msg[len - 1] = '\0';
    }
    else if (code == K_PASTE) {
      Buf b;
      buf_init(&b);
      term_paste(&b);
      if (b.s && len + b.len + 1 < sizeof(g_msg)) strcat(g_msg, b.s);
      buf_free(&b);
    }
    else if (IS_TEXT(k) && len + 4 < sizeof(g_msg)) {
      len += (size_t)utf8_encode((uint32_t)k, g_msg + len);
      g_msg[len] = '\0';
    }
    else return 0;
    return 1;
  }
  {
    const GRow *r = &g_row[g_sel - 1];
    const Change *c = (r->kind == R_STAGED || r->kind == R_CHANGE) ? &g_ch[r->ch] : NULL;
    switch (code) {
      case K_PGUP: g_sel = g_sel > (size_t)g_h ? g_sel - (size_t)g_h : 1; return 1;
      case K_PGDN:
        g_sel += (size_t)g_h;
        if (g_sel > g_nrow) g_sel = g_nrow;
        return 1;
      case K_ENTER: open_row(g_sel - 1, 1, act); return 1;
      case ' ':
        if (r->kind != R_COMMIT && r->kind != R_GRAPH) open_row(g_sel - 1, 0, act);
        return 1;
      case K_RIGHT:	/* a commit opens, Left closes it (from its files too) */
        if (r->kind == R_COMMIT && !graph_open(r->ch)) open_row(g_sel - 1, 0, act);
        else if (r->kind == R_GRAPH && !g_graph_open) open_row(g_sel - 1, 0, act);
        return 1;
      case K_LEFT:
        if (r->kind == R_CFILE) {
          while (g_sel > 1 && g_row[g_sel - 1].kind != R_COMMIT) g_sel--;
          return 1;
        }
        if ((r->kind == R_COMMIT && graph_open(r->ch)) || (r->kind == R_GRAPH && g_graph_open))
          open_row(g_sel - 1, 0, act);
        return 1;
      case 's': case '+':	/* VS Code's + button; on a conflict: marked resolved */
        if (r->kind == R_MERGE) stage(&g_ch[r->ch], 1);
        else if (c && r->kind == R_CHANGE) stage(c, 1);
        return 1;
      case 'u': case '-':	/* and its - button */
        if (c && r->kind == R_STAGED) stage(c, 0);
        return 1;
      case 'o':	/* Open File */
        if (c && c->y != 'D') {
          act->what = SA_GO;
          act->path = c->path;
        }
        return 1;
    }
  }
  return 0;
}


void git_click (int row, int col, SideAct *act) {
  size_t k;
  if (g_top == NULL && !g_nogit) {	/* Initialize Repository, Clone Repository */
    if (row == 5 || row == 7) {
      g_sel = row == 7;
      act->what = SA_CMD;
      act->cmd = row == 7 ? CMD_GIT_CLONE : CMD_GIT_INIT;
    }
    return;
  }
  if (row == 0 && g_top) {	/* the title's actions */
    int w = side_width();
    if (col == w - 7) {
      act->what = SA_CMD;
      act->cmd = CMD_GIT_COMMIT;
    }
    else if (col == w - 5) git_refresh();
    else if (col == w - 3) {
      act->what = SA_CMD;
      act->cmd = CMD_GIT_MORE;
    }
    return;
  }
  if (row == 2) {
    g_sel = 0;
    return;
  }
  if (row < HEAD) return;
  k = g_top_row + (size_t)(row - HEAD);
  if (k >= g_nrow) return;
  g_sel = k + 1;
  open_row(k, 0, act);
}


void git_wheel (int d) {
  size_t h = (size_t)g_h;
  if (d < 0) g_top_row = g_top_row > 3 ? g_top_row - 3 : 0;
  else if (g_nrow > h) {
    g_top_row += 3;
    if (g_top_row > g_nrow - h) g_top_row = g_nrow - h;
  }
  if (g_sel > 0 && g_sel - 1 < g_top_row) g_sel = g_top_row + 1;
  if (g_sel > 0 && g_sel - 1 >= g_top_row + h) g_sel = g_top_row + h;
}

/* }================================================================== */


/*
** {==================================================================
** The diff editor
** ===================================================================
*/

typedef struct DLine {
  char kind;	/* ' ' both, '-' old only, '+' new only */
  size_t o, n;	/* line numbers, 0: none */
  char *s;
  size_t len;
  size_t h0, h1;	/* the changed bytes, brighter */
  unsigned char *tok;	/* its colors */
} DLine;

typedef struct SRow {
  long l, r;	/* DLine on the left / right, -1: nothing there */
} SRow;

static struct {
  int open;
  char *path, *title;
  DLine *line;
  size_t nline, capline;
  SRow *row;	/* side by side */
  size_t nrow, caprow;
  size_t top, left;
  int inline_mode;	/* one above the other, forced with 'i' */
  int split_now;	/* how it was drawn last */
  int h;
  size_t nmax;	/* the biggest line number */
  int kind;	/* DK_*: what old and new are, for Stage / Unstage / Revert Selected Ranges */
  char *rel;	/* DK_TREE, DK_INDEX: the file from the top */
  size_t cur;	/* the view row of the change F7 went to */
} D;

enum { DK_OTHER, DK_TREE, DK_INDEX };	/* index -> working tree, HEAD -> index */


static void dline_add (char kind, size_t o, size_t n, const char *s, size_t len) {
  DLine *l;
  if (D.nline == D.capline) {
    D.capline = D.capline ? D.capline * 2 : 256;
    D.line = (DLine *)xrealloc(D.line, D.capline * sizeof(DLine));
  }
  if (len > 0 && s[len - 1] == '\r') len--;
  l = &D.line[D.nline++];
  l->kind = kind;
  l->o = o;
  l->n = n;
  l->s = xstrndup(s, len);
  l->len = len;
  l->h0 = 0;
  l->h1 = len;
  l->tok = NULL;
  if (o > D.nmax) D.nmax = o;
  if (n > D.nmax) D.nmax = n;
}


static void srow_add (long l, long r) {
  if (D.nrow == D.caprow) {
    D.caprow = D.caprow ? D.caprow * 2 : 256;
    D.row = (SRow *)xrealloc(D.row, D.caprow * sizeof(SRow));
  }
  D.row[D.nrow].l = l;
  D.row[D.nrow].r = r;
  D.nrow++;
}


/* the changed middle of a pair: what is left without the same start and end */
static void pair (DLine *a, DLine *b) {
  size_t p = 0, s = 0;
  while (p < a->len && p < b->len && a->s[p] == b->s[p]) p++;
  while (s < a->len - p && s < b->len - p && a->s[a->len - 1 - s] == b->s[b->len - 1 - s]) s++;
  while (p > 0 && ((unsigned char)a->s[p] & 0xC0) == 0x80) p--;	/* whole characters */
  a->h0 = b->h0 = p;
  a->h1 = a->len - s;
  b->h1 = b->len - s;
}


/* the colors of the code: the old lines and the new lines are each read in order */
static void color_lines (const char *path) {
  const Syntax *sx = syntax_for(path);
  int ol = 0, ne = 0;
  size_t i;
  unsigned char *tmp = NULL;
  for (i = 0; i < D.nline; i++) {
    DLine *l = &D.line[i];
    l->tok = (unsigned char *)xmalloc(l->len + 1);
    if (l->kind == '-') ol = syntax_scan(sx, l->s, l->len, ol, l->tok);
    else {
      ne = syntax_scan(sx, l->s, l->len, ne, l->tok);
      if (l->kind == ' ') {	/* the old side goes on with the same line */
        tmp = (unsigned char *)xrealloc(tmp, l->len + 1);
        ol = syntax_scan(sx, l->s, l->len, ol, tmp);
      }
    }
  }
  free(tmp);
}


/* the rows side by side, and the brighter parts: a run of '-' then '+' pairs up */
static void build_split (void) {
  size_t i = 0;
  D.nrow = 0;
  while (i < D.nline) {
    size_t d0 = i, d1, a1, j;
    if (D.line[i].kind == ' ') {
      srow_add((long)i, (long)i);
      i++;
      continue;
    }
    while (i < D.nline && D.line[i].kind == '-') i++;
    d1 = i;
    while (i < D.nline && D.line[i].kind == '+') i++;
    a1 = i;
    for (j = 0; j < d1 - d0 || j < a1 - d1; j++) {
      long l = (j < d1 - d0) ? (long)(d0 + j) : -1;
      long r = (j < a1 - d1) ? (long)(d1 + j) : -1;
      if (l >= 0 && r >= 0) pair(&D.line[l], &D.line[r]);
      srow_add(l, r);
    }
  }
}


void diff_close (void) {
  size_t i;
  for (i = 0; i < D.nline; i++) {
    free(D.line[i].s);
    free(D.line[i].tok);
  }
  free(D.line);
  free(D.row);
  free(D.path);
  free(D.title);
  free(D.rel);
  memset(&D, 0, sizeof(D));
}


static const Change *change_of (const char *path) {
  size_t i;
  for (i = 0; i < g_nch; i++)
    if (m_fncmp(g_ch[i].path, path) == 0) return &g_ch[i];
  return NULL;
}


/* the lines of a unified diff with the whole file as context */
static void parse_unified (const char *p) {
  size_t o = 0, n = 0;
  while (*p) {	/* after its header */
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (p[0] == '@' && p[1] == '@') {
      unsigned long a = 0, c2 = 0;
      const char *q = strchr(p, '-'), *r = strchr(p, '+');
      if (q) a = strtoul(q + 1, NULL, 10);
      if (r) c2 = strtoul(r + 1, NULL, 10);
      o = a ? a - 1 : 0;
      n = c2 ? c2 - 1 : 0;
    }
    else if (D.nline > 0 || o > 0 || n > 0 || p[0] == ' ' || p[0] == '-' || p[0] == '+') {
      if (p[0] == ' ') dline_add(' ', ++o, ++n, p + 1, len ? len - 1 : 0);
      else if (p[0] == '-' && !(p[1] == '-' && p[2] == '-' && D.nline == 0 && o == 0))
        dline_add('-', ++o, 0, p + 1, len - 1);
      else if (p[0] == '+' && !(p[1] == '+' && p[2] == '+' && D.nline == 0 && n == 0))
        dline_add('+', 0, ++n, p + 1, len - 1);
    }
    p += len + (e ? 1 : 0);
  }
}


/*
** A commit's change of a file: old (its parent, or git's empty tree for the
** first commit) against new; rel and old_rel from the top, with '/'.
** VS Code's title: "file.c (abc1234^ <-> abc1234)".
*/
int diff_open_rev (const char *path, const char *rel, const char *old_rel,
                   const char *old, const char *new_, const char *title) {
  Buf b;
  int inl = D.inline_mode, r;
  const char *a[16];
  int k = 0;
  a[k++] = "diff";
  a[k++] = "--no-color";
  a[k++] = "--no-ext-diff";
  a[k++] = "-U100000";
  a[k++] = "-M";
  a[k++] = old;
  a[k++] = new_;
  a[k++] = "--";
  if (old_rel && strcmp(old_rel, rel) != 0) a[k++] = old_rel;
  a[k++] = rel;
  a[k] = NULL;
  buf_init(&b);
  r = git_exec(&b, 0, a);
  if (r != 0) {
    buf_free(&b);
    toast(1, "git diff failed");
    return -1;
  }
  diff_close();
  D.inline_mode = inl;
  D.path = xstrdup(path);
  D.title = xstrdup(title);
  parse_unified(b.s ? b.s : "");
  buf_free(&b);
  if (D.nline == 0) dline_add(' ', 1, 1, "", 0);	/* a change of mode only */
  build_split();
  color_lines(path);
  D.open = 1;
  diff_change(0);
  return 0;
}


/*
** Two files side by side (Local History's copy against the file): git diff
** --no-index, which needs no repository. The same text shows as it is.
*/
int diff_open_files (const char *path, const char *old_file, const char *new_file, const char *title) {
  const char *a[] = {"diff", "--no-index", "--no-color", "--no-ext-diff", "-U100000", "--", old_file, new_file, NULL};
  Buf b;
  int inl = D.inline_mode, r;
  buf_init(&b);
  r = git_exec(&b, 0, a);
  if (r != 0 && r != 1) {	/* 1: they differ */
    buf_free(&b);
    toast(1, "git diff failed");
    return -1;
  }
  diff_close();
  D.inline_mode = inl;
  D.path = xstrdup(path);
  D.title = xstrdup(title);
  parse_unified(b.s ? b.s : "");
  buf_free(&b);
  if (D.nline == 0) {	/* no difference: the text itself */
    size_t len, i, from = 0, n = 0;
    char *s = read_file(new_file, &len);
    for (i = 0; s && i <= len; i++) {
      if (i < len && s[i] != '\n') continue;
      if (i < len || i > from) {
        n++;
        dline_add(' ', n, n, s + from, i > from && s[i - 1] == '\r' ? i - from - 1 : i - from);
      }
      from = i + 1;
    }
    free(s);
    if (D.nline == 0) dline_add(' ', 1, 1, "", 0);
  }
  build_split();
  color_lines(path);
  D.open = 1;
  diff_change(0);
  return 0;
}


/*
** {==================================================================
** Two texts compared (File: Compare Active File with Saved ...): no git,
** Myers' diff of their lines
** ===================================================================
*/

typedef struct TLine {
  const char *s;
  size_t n;
  unsigned long h;
} TLine;


static TLine *text_lines (const char *s, size_t len, size_t *n) {
  size_t cap = 64, i, from = 0;
  TLine *v = (TLine *)xmalloc(cap * sizeof(TLine));
  *n = 0;
  for (i = 0; i <= len; i++) {
    unsigned long h = 5381;
    size_t k;
    if (i < len && s[i] != '\n') continue;
    if (i == len && i == from && *n > 0) break;	/* the newline at the end ends the last line */
    if (*n == cap) v = (TLine *)xrealloc(v, (cap *= 2) * sizeof(TLine));
    for (k = from; k < i; k++) h = h * 33 + (unsigned char)s[k];
    v[*n].s = s + from;
    v[*n].n = i - from;
    v[*n].h = h;
    (*n)++;
    from = i + 1;
  }
  return v;
}


static int same_line (const TLine *a, const TLine *b) {
  return a->h == b->h && a->n == b->n && memcmp(a->s, b->s, a->n) == 0;
}


/*
** The edit script from a to b: ops[i] is ' ', '-' or '+'. Greedy Myers
** with every step's furthest points kept to walk back; a script too long
** for that memory is made of the rest deleted and inserted.
*/
static char *edit_script (const TLine *a, size_t n, const TLine *b, size_t m, size_t *nops) {
  long max = (long)(n + m), d, k, off = max + 1, x, y, dmax;
  long *v, **trace;
  char *ops = (char *)xmalloc(n + m + 1);
  size_t no = 0;
  dmax = max;
  if ((double)max * (double)(2 * max + 3) > 60e6) dmax = (long)(60e6 / (double)(2 * max + 3));
  v = (long *)calloc((size_t)(2 * max + 3), sizeof(long));
  trace = (long **)xmalloc((size_t)(dmax + 1) * sizeof(long *));
  for (d = 0; d <= dmax; d++) {
    trace[d] = NULL;
    for (k = -d; k <= d; k += 2) {
      if (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) x = v[off + k + 1];
      else x = v[off + k - 1] + 1;
      y = x - k;
      while (x < (long)n && y < (long)m && same_line(&a[x], &b[y])) {
        x++;
        y++;
      }
      v[off + k] = x;
      if (x >= (long)n && y >= (long)m) {
        trace[d] = (long *)xmalloc((size_t)(2 * max + 3) * sizeof(long));
        memcpy(trace[d], v, (size_t)(2 * max + 3) * sizeof(long));
        goto found;
      }
    }
    trace[d] = (long *)xmalloc((size_t)(2 * max + 3) * sizeof(long));
    memcpy(trace[d], v, (size_t)(2 * max + 3) * sizeof(long));
  }
  /* too different: all of a goes, all of b comes */
  for (x = 0; x < (long)n; x++) ops[no++] = '-';
  for (y = 0; y < (long)m; y++) ops[no++] = '+';
  for (d = 0; d <= dmax; d++) free(trace[d]);
  free(trace);
  free(v);
  *nops = no;
  return ops;
found:
  {	/* back from the end: the moves in reverse */
    long dd = d, px, py;
    x = (long)n;
    y = (long)m;
    for (; dd > 0; dd--) {
      long *pv = trace[dd - 1];
      k = x - y;
      if (k == -dd || (k != dd && pv[off + k - 1] < pv[off + k + 1])) k = k + 1;	/* came down: an insert */
      else k = k - 1;	/* came right: a delete */
      px = pv[off + k];
      py = px - k;
      while (x > px && y > py) {	/* the diagonal */
        ops[no++] = ' ';
        x--;
        y--;
      }
      if (x == px) {
        ops[no++] = '+';
        y--;
      }
      else {
        ops[no++] = '-';
        x--;
      }
    }
    while (x > 0 && y > 0) {
      ops[no++] = ' ';
      x--;
      y--;
    }
  }
  for (k = 0; k < (long)no / 2; k++) {	/* the right way round */
    char c = ops[k];
    ops[k] = ops[no - 1 - (size_t)k];
    ops[no - 1 - (size_t)k] = c;
  }
  for (; d >= 0; d--) free(trace[d]);
  free(trace);
  free(v);
  *nops = no;
  return ops;
}


/*
** The changes from text a to text b, as ranges of lines (0 based): VS
** Code's quick diff (the gutter's bars) and its dirty diff peek use them.
** A '\r' at the end of a line does not count.
*/
QHunk *text_hunks (const char *a, size_t na, const char *b, size_t nb, size_t *nh) {
  size_t n, m, nops, i = 0, o = 0, w = 0, k, cap = 0;
  TLine *la = text_lines(a ? a : "", a ? na : 0, &n), *lb = text_lines(b ? b : "", b ? nb : 0, &m);
  QHunk *v = NULL;
  char *ops;
  for (k = 0; k < n + m; k++) {	/* without the '\r': its hash again */
    TLine *t = k < n ? &la[k] : &lb[k - n];
    size_t j;
    if (t->n == 0 || t->s[t->n - 1] != '\r') continue;
    t->n--;
    t->h = 5381;
    for (j = 0; j < t->n; j++) t->h = t->h * 33 + (unsigned char)t->s[j];
  }
  ops = edit_script(la, n, lb, m, &nops);
  *nh = 0;
  while (i < nops) {
    QHunk *h;
    if (ops[i] == ' ') {
      o++;
      w++;
      i++;
      continue;
    }
    if (*nh == cap) v = (QHunk *)xrealloc(v, (cap = cap ? cap * 2 : 16) * sizeof(QHunk));
    h = &v[(*nh)++];
    h->o0 = o;
    h->n0 = w;
    h->on = h->nn = 0;
    for (; i < nops && ops[i] != ' '; i++)
      if (ops[i] == '-') h->on++, o++;
      else h->nn++, w++;
  }
  free(ops);
  free(la);
  free(lb);
  return v;
}


int diff_open_texts (const char *path, const char *a, size_t na, const char *b, size_t nb,
                     const char *title) {
  size_t n, m, nops, i, o = 0, w = 0;
  TLine *la = text_lines(a ? a : "", a ? na : 0, &n), *lb = text_lines(b ? b : "", b ? nb : 0, &m);
  char *ops = edit_script(la, n, lb, m, &nops);
  int inl = D.inline_mode;
  diff_close();
  D.inline_mode = inl;
  D.path = xstrdup(path ? path : "");
  D.title = xstrdup(title);
  i = 0;
  while (i < nops) {	/* each change: what went, then what came, as git shows it */
    size_t j;
    if (ops[i] == ' ') {
      dline_add(' ', o + 1, w + 1, la[o].s, la[o].n);
      o++;
      w++;
      i++;
      continue;
    }
    for (j = i; j < nops && ops[j] != ' '; j++)
      if (ops[j] == '-') {
        dline_add('-', o + 1, 0, la[o].s, la[o].n);
        o++;
      }
    for (j = i; j < nops && ops[j] != ' '; j++)
      if (ops[j] == '+') {
        dline_add('+', 0, w + 1, lb[w].s, lb[w].n);
        w++;
      }
    i = j;
  }
  free(ops);
  free(la);
  free(lb);
  if (D.nline == 0) dline_add(' ', 1, 1, "", 0);
  build_split();
  color_lines(D.path);
  D.open = 1;
  diff_change(0);
  return 0;
}

/* }================================================================== */


int diff_open (const char *path, int staged) {
  const Change *c = change_of(path);
  Buf b;
  size_t i, n = 0;
  int inl = D.inline_mode;
  char *rel;
  if (c == NULL) {
    toast(1, "No changes in %s", path_basename(path));
    return -1;
  }
  rel = xstrdup(c->rel);
  diff_close();
  D.inline_mode = inl;
  D.path = xstrdup(path);
  D.kind = staged ? DK_INDEX : (c->y == '?' ? DK_OTHER : DK_TREE);
  D.rel = xstrdup(c->rel);
  buf_init(&b);
  if (!staged && c->y == '?') {	/* untracked: every line is new */
    size_t len, from = 0;
    char *s = read_file(path, &len);
    D.title = xstrcat3(path_basename(path), " (Untracked)", "");
    for (i = 0; s && i <= len; i++) {
      if (i < len && s[i] != '\n') continue;
      if (i < len || i > from) dline_add('+', 0, ++n, s + from, i - from);
      from = i + 1;
    }
    free(s);
  }
  else {
    {	/* the whole file as context: VS Code shows all of it */
      char *argv[12];
      int k = 0;
      argv[k++] = g_git;
      argv[k++] = (char *)"-C";
      argv[k++] = g_top;
      argv[k++] = (char *)"diff";
      if (staged) argv[k++] = (char *)"--cached";
      argv[k++] = (char *)"--no-color";
      argv[k++] = (char *)"--no-ext-diff";
      argv[k++] = (char *)"-U100000";
      argv[k++] = (char *)"--";
      argv[k++] = rel;
      argv[k] = NULL;
      if (g_git == NULL || g_top == NULL || run_capture(argv, &b) < 0) {
        free(rel);
        buf_free(&b);
        toast(1, "git diff failed");
        return -1;
      }
    }
    D.title = xstrcat3(path_basename(path), staged ? " (Index)" : " (Working Tree)", "");
    parse_unified(b.s ? b.s : "");
  }
  free(rel);
  buf_free(&b);
  build_split();
  color_lines(path);
  D.open = 1;
  diff_change(0);	/* to the first change */
  return 0;
}


int diff_active (void) {
  return D.open;
}


const char *diff_title (void) {
  return D.title ? D.title : "";
}


const char *diff_path (void) {
  return D.path;
}


static size_t nrows (void) {
  return D.split_now ? D.nrow : D.nline;
}


/* the DLine at the start of view row k */
static const DLine *row_line (size_t k) {
  if (!D.split_now) return k < D.nline ? &D.line[k] : NULL;
  if (k >= D.nrow) return NULL;
  return &D.line[D.row[k].r >= 0 ? D.row[k].r : D.row[k].l];
}


size_t diff_line (void) {
  size_t k;
  for (k = D.top; k < nrows(); k++) {
    const DLine *l = row_line(k);
    if (l && l->n) return l->n;
  }
  return 1;
}


static int is_change (size_t k) {
  const DLine *l = row_line(k);
  if (D.split_now && k < D.nrow && D.row[k].l >= 0 && D.line[D.row[k].l].kind != ' ') return 1;
  return l && l->kind != ' ';
}


void diff_change (int back) {
  size_t k = D.top, n = nrows(), i;
  for (i = 0; i < n; i++) {
    if (back) k = (k + n - 1) % n;
    else k = (k + 1) % n;
    if (i == 0 && !back && D.top == 0 && is_change(0)) {
      k = 0;
      break;
    }
    if (is_change(k) && (k == 0 || !is_change(k - 1))) break;	/* the start of a block */
  }
  if (n == 0) return;
  D.top = k > 3 ? k - 3 : 0;	/* a little context above, like VS Code */
  D.cur = k;
}


static void draw_side (int x, int y, int w, const DLine *l, int left, int nw) {
  char num[32];
  int st, hst;
  if (l == NULL) {	/* nothing on this side: VS Code's diagonal fill */
    int i;
    for (i = 0; i < w; i++) scr_put(x + i, y, 0x2571, S_DIFF_FILL);
    return;
  }
  st = l->kind == '+' ? S_DIFF_ADD : l->kind == '-' ? S_DIFF_DEL : S_TEXT;
  hst = l->kind == '+' ? B_ADD_HI : l->kind == '-' ? B_DEL_HI : B_EDITOR;
  snprintf(num, sizeof(num), "%*lu ", nw - 1, (unsigned long)(left ? l->o : l->n));
  scr_puts(x, y, num, S_DIFF_NUM);
  scr_fill(x + nw, y, w - nw, st);
  if (l->kind != ' ') scr_put(x + nw, y, l->kind == '+' ? '+' : '-', st);
  scr_code(x + nw + 2, y, w - nw - 2, l->s, l->len, D.left, l->tok,
           l->kind == '+' ? B_ADD : l->kind == '-' ? B_DEL : B_EDITOR,
           l->kind == ' ' ? 0 : l->h0, l->kind == ' ' ? 0 : l->h1, hst);
}


/* the view rows of the current change: F7's, else the first one shown */
static void current_rows (size_t *k0, size_t *k1) {
  size_t n = nrows(), k = D.cur;
  if (!(k < n && k >= D.top && k < D.top + (size_t)(D.h > 0 ? D.h : 1) && is_change(k)))
    for (k = D.top; k < n && !is_change(k); k++) ;
  if (k >= n) {
    *k0 = *k1 = 0;
    return;
  }
  while (k > 0 && is_change(k - 1)) k--;
  *k0 = k;
  while (k < n && is_change(k)) k++;
  *k1 = k;
}


static int in_current (size_t k) {
  size_t k0, k1;
  if (D.kind == DK_OTHER) return 0;
  current_rows(&k0, &k1);
  return k >= k0 && k < k1;
}


void diff_draw (int x, int y, int w, int h) {
  int nw = 2, row;
  size_t m = D.nmax;
  while (m >= 10) {
    m /= 10;
    nw++;
  }
  nw++;
  D.split_now = !D.inline_mode && w >= 80;
  D.h = h;
  if (D.top + (size_t)h > nrows()) D.top = nrows() > (size_t)h ? nrows() - (size_t)h : 0;
  scr_box(x, y, w, h, S_TEXT);
  for (row = 0; row < h; row++) {
    size_t k = D.top + (size_t)row;
    int sy = y + row;
    if (k >= nrows()) break;
    if (D.split_now) {
      int hw = (w - 1) / 2;
      const SRow *r = &D.row[k];
      draw_side(x, sy, hw, r->l >= 0 ? &D.line[r->l] : NULL, 1, nw);
      if (in_current(k)) scr_put(x, sy, 0x258E, S_TOGGLE_ON);	/* the change Stage Selected Ranges takes */
      scr_put(x + hw, sy, 0x2502, S_DIFF_FILL);
      draw_side(x + hw + 1, sy, w - hw - 1, r->r >= 0 ? &D.line[r->r] : NULL, 0, nw);
    }
    else {	/* inline: both numbers, then the line */
      const DLine *l = &D.line[k];
      char num[64];
      int st = l->kind == '+' ? S_DIFF_ADD : l->kind == '-' ? S_DIFF_DEL : S_TEXT;
      int hst = l->kind == '+' ? B_ADD_HI : l->kind == '-' ? B_DEL_HI : B_EDITOR;
      char a[24], b[24];
      if (l->o) snprintf(a, sizeof(a), "%lu", (unsigned long)l->o);
      else a[0] = '\0';
      if (l->n) snprintf(b, sizeof(b), "%lu", (unsigned long)l->n);
      else b[0] = '\0';
      snprintf(num, sizeof(num), "%*s %*s ", nw - 1, a, nw - 1, b);
      scr_puts(x, sy, num, S_DIFF_NUM);
      scr_fill(x + 2 * nw, sy, w - 2 * nw, st);
      if (l->kind != ' ') scr_put(x + 2 * nw, sy, (uint32_t)l->kind, st);
      scr_code(x + 2 * nw + 2, sy, w - 2 * nw - 2, l->s, l->len, D.left, l->tok,
               l->kind == '+' ? B_ADD : l->kind == '-' ? B_DEL : B_EDITOR,
               l->kind == ' ' ? 0 : l->h0, l->kind == ' ' ? 0 : l->h1, hst);
      if (in_current(k)) scr_put(x, sy, 0x258E, S_TOGGLE_ON);
    }
  }
}


int diff_key (int k) {
  int code = KEY_CODE(k);
  size_t h = D.h > 0 ? (size_t)D.h : 1;
  switch (code) {
    case K_UP: if (D.top > 0) D.top--; break;
    case K_DOWN: D.top++; break;
    case K_PGUP: D.top = D.top > h ? D.top - h : 0; break;
    case K_PGDN: D.top += h; break;
    case K_HOME: D.top = 0; D.left = 0; break;
    case K_END: D.top = nrows(); break;
    case K_LEFT: D.left = D.left > 4 ? D.left - 4 : 0; break;
    case K_RIGHT: D.left += 4; break;
    case K_F7: diff_change((k & KM_SHIFT) != 0); break;
    case 'i': D.inline_mode = !D.inline_mode; break;
    case K_ESC: return DIFF_CLOSE;
    case K_ENTER: case 'o': return DIFF_EDIT;
    default:
      if (k == CTRL('w')) return DIFF_CLOSE;
      return DIFF_NO;
  }
  return DIFF_YES;
}


/*
** The diff's text with its change from..to (DLines) taken from one side and
** the rest from the other: block_new / rest_new say which; joined with eol.
*/
static char *mixed (size_t from, size_t to, int block_new, int rest_new, const char *eol, size_t *n) {
  Buf b;
  size_t i;
  int first = 1;
  buf_init(&b);
  for (i = 0; i < D.nline; i++) {
    const DLine *l = &D.line[i];
    int nw = (i >= from && i < to) ? block_new : rest_new;
    if (nw ? l->kind == '-' : l->kind == '+') continue;
    if (!first) buf_puts(&b, eol);
    buf_putn(&b, l->s, l->len);
    first = 0;
  }
  if (!first) buf_puts(&b, eol);
  buf_putc(&b, '\0');
  *n = b.len - 1;
  return buf_take(&b);
}


/*
** Git: Stage / Unstage / Revert Selected Ranges in the diff editor: the
** change the view is at (its bar). act: 0 stage, 1 unstage, 2 revert.
** The index (or the file) gets its whole text with only that change made.
*/
int diff_range (int act) {
  static const char *const what[] = {"stage", "unstage", "revert"};
  size_t k0, k1, from, to, n, i, bl = 0;
  char *text, *blob, spec[600], *path;
  const char *eol;
  int r, staged = D.kind == DK_INDEX;
  if (!D.open || D.kind == DK_OTHER || D.rel == NULL) {
    toast(0, "There are no changes to %s here", what[act]);
    return -1;
  }
  if (act != 1 && D.kind != DK_TREE) {
    toast(0, "Open the file from Changes (not Staged Changes) to %s", what[act]);
    return -1;
  }
  if (act == 1 && D.kind != DK_INDEX) {
    toast(0, "Open the file from Staged Changes to unstage");
    return -1;
  }
  current_rows(&k0, &k1);
  if (k0 == k1) {
    toast(0, "There are no changes here");
    return -1;
  }
  from = (size_t)-1;	/* the DLines of those rows */
  to = 0;
  for (i = k0; i < k1; i++) {
    long a = D.split_now ? D.row[i].l : (long)i, b = D.split_now ? D.row[i].r : -1;
    if (a >= 0 && (size_t)a < from) from = (size_t)a;
    if (b >= 0 && (size_t)b < from) from = (size_t)b;
    if (a >= 0 && (size_t)a + 1 > to) to = (size_t)a + 1;
    if (b >= 0 && (size_t)b + 1 > to) to = (size_t)b + 1;
  }
  path = xstrdup(D.path);
  if (act == 2) {	/* the file's own line ends */
    blob = read_file(path, &bl);
  }
  else {	/* the index's */
    snprintf(spec, sizeof(spec), ":%s", D.rel);
    blob = git_blob(spec, &bl);
  }
  eol = blob && memchr(blob, '\r', bl) ? "\r\n" : "\n";
  free(blob);
  if (act == 0) text = mixed(from, to, 1, 0, eol, &n);	/* the index, with the change made */
  else text = mixed(from, to, 0, 1, eol, &n);	/* the index / the file, the change undone */
  if (act == 2) {
    int fd = os_open(path, OS_WRITE);
    r = fd >= 0 ? 0 : -1;
    if (fd >= 0) {
      os_write(fd, text, n);
      os_close(fd);
    }
    else toast(1, "Unable to write '%s'", path_basename(path));
  }
  else r = git_index_put(D.rel, text, n);
  free(text);
  if (r == 0) {
    git_refresh();
    if (act == 2) on_disk_changed();
    if (change_of(path)) diff_open(path, staged);	/* what is left of it */
    else diff_close();
    toast(0, act == 0 ? "Staged the change" : act == 1 ? "Unstaged the change" : "Reverted the change");
  }
  free(path);
  return r;
}


void diff_toggle_inline (void) {
  D.inline_mode = !D.inline_mode;
}


void diff_wheel (int d) {
  if (d < 0) D.top = D.top > 3 ? D.top - 3 : 0;
  else D.top += 3;
}

/* }================================================================== */
