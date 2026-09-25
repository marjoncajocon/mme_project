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
    char info[2048];
    const char *u[] = {"update-index", "--add", "--cacheinfo", info, NULL};
    if (snprintf(info, sizeof(info), "%s,%s,%s", mode, sha, rel) >= (int)sizeof(info))
      return -1;	/* cut short it would stage the blob under another name */
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
  side_bar(x, y + HEAD, w, g_h, g_nrow, g_top_row, (size_t)g_h);
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
  size_t h = (size_t)g_h, st = (size_t)wheel_step(0);
  if (d < 0) g_top_row = g_top_row > st ? g_top_row - st : 0;
  else if (g_nrow > h) {
    g_top_row += st;
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
  char *os;	/* ' ' with diffEditor.ignoreTrimWhitespace: the old side's text when its spaces differ */
  size_t olen;
  size_t *hr;	/* the changed words: nhr ranges [hr[2i], hr[2i+1]) */
  size_t nhr;
} DLine;

typedef struct VRow {	/* a row shown: underlying row k, or a fold of hidden rows k .. k+hidden-1 */
  size_t k, hidden;
} VRow;

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
  char *ta, *tb;	/* the old and the new text, whole: diffEditor.ignoreTrimWhitespace diffs them again */
  size_t tna, tnb;
  int trim;	/* the lines are for ignoreTrimWhitespace so */
  VRow *vis;	/* diffEditor.hideUnchangedRegions: the rows shown; nvis 0: all */
  size_t nvis, capvis;
  unsigned char *exp;	/* a fold opened (by its first row) */
  size_t nexp;
  int vis_split, vis_hide;	/* what vis was made for */
  size_t crow;	/* the row of the cursor (Enter opens the file there) */
  size_t ccol;	/* its byte in the new side's line: the modified side is typed in */
  int edit;	/* the new side is the file in the working tree: it can be typed in */
  int cx, cy;	/* where the caret was drawn, -1 none */
  int x, y, w, hw, arrow_x, mmw;	/* where it was drawn: for the mouse */
  int act_x[6], nact;	/* the title's icons */
  int wrap;	/* editor.wordWrap: a long line goes on in the rows under it */
  int nw, tcl, tcr;	/* the numbers' width; the text's on the left / right (inline: tcr) */
  int sbw, hsb;	/* the scrollbar's width (1 or 0); the horizontal one shows (under the rows) */
  int hbx[2], hbw[2], nhb;	/* the horizontal ones: one under each side's text */
  int drag_sb, drag_hsb;	/* a thumb is held: 1 + where it was taken */
  size_t wide;	/* the widest line's columns */
  int wide_ok;
  size_t lastrow, lastcol;	/* the caret when last drawn: the view follows it when it moves */
  size_t *sri, *srs;	/* each screen row drawn: its view row, and which row of it (word wrap) */
  int nsr, capsr;
} D;

#define CTX	3	/* hideUnchangedRegions: the lines kept around a change */
#define MIN_HIDE	3	/* and the fewest lines worth a fold */

static int g_gaveup;	/* edit_script gave up: too different for its memory */

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
  l->os = NULL;
  l->olen = 0;
  l->hr = NULL;
  l->nhr = 0;
  if (o > D.nmax) D.nmax = o;
  if (n > D.nmax) D.nmax = n;
  D.wide_ok = 0;
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


#define WORD_LINE_MAX	10000	/* a longer line keeps the whole-line colour: no word diff */
#define WORD_CELLS_MAX	1000000	/* the word LCS is quadratic: the cells one pair of lines may cost */
#define WORD_CELLS_ALL	8000000	/* and what every pair of one diff may cost together */
#define PAIR_LINES_MAX	256	/* choosing the pairs of a block is quadratic too */
#define PAIR_CELLS_MAX	4096
#define SIG_BYTES	2048	/* of a line, for how alike two of them are */

static long long g_wcells;	/* the word diff's work left, this rebuild */


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


static int wclass (unsigned char c) {
  if (c == ' ' || c == '\t') return 0;
  if (c == '_' || c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 1;
  return 2;
}


/* the words of s: runs of letters, runs of spaces, each other character; at[k] their starts, at[count] = n */
static size_t words (const char *s, size_t n, size_t *at) {
  size_t i = 0, k = 0;
  while (i < n) {
    int c = wclass((unsigned char)s[i]);
    at[k++] = i;
    if (c == 2) i++;
    else
      while (i < n && wclass((unsigned char)s[i]) == c) i++;
  }
  at[k] = n;
  return k;
}


/* the words of l not in the common part (keep[k] 0) as byte ranges; spaces between two changes join them */
static void word_ranges (DLine *l, const size_t *at, size_t nt, const unsigned char *keep) {
  size_t k = 0, cap = 0;
  l->nhr = 0;
  while (k < nt) {
    size_t a;
    if (keep[k]) {
      k++;
      continue;
    }
    a = at[k];
    while (k < nt && (!keep[k] || (wclass((unsigned char)l->s[at[k]]) == 0 && k + 1 < nt && !keep[k + 1]))) k++;
    if (2 * l->nhr + 2 > cap) l->hr = (size_t *)xrealloc(l->hr, (cap = cap ? cap * 2 : 8) * sizeof(size_t));
    l->hr[2 * l->nhr] = a;
    l->hr[2 * l->nhr + 1] = at[k];
    l->nhr++;
  }
}


/*
** The changed words of a line and the line it became, like VS Code's
** character diff: the longest common run of words stays plain. Very long
** lines fall back to what is left without the same start and end.
*/
static void word_pair (DLine *a, DLine *b) {
  size_t *wa, *wb, na, nb, i, j, cells;
  unsigned short *lcs;
  unsigned char *ka, *kb;
  pair(a, b);	/* the fallback, and h0 / h1 for the old way of drawing */
  if (a->len > WORD_LINE_MAX || b->len > WORD_LINE_MAX) return;
  wa = (size_t *)xmalloc((a->len + 2) * sizeof(size_t));
  wb = (size_t *)xmalloc((b->len + 2) * sizeof(size_t));
  na = words(a->s, a->len, wa);
  nb = words(b->s, b->len, wb);
  cells = (na + 1) * (nb + 1);	/* the table below: a minified file must not hang on it */
  if (cells > WORD_CELLS_MAX || (long long)cells > g_wcells) {
    free(wa);
    free(wb);
    return;
  }
  g_wcells -= (long long)cells;
  lcs = (unsigned short *)calloc((na + 1) * (nb + 1), sizeof(unsigned short));
  ka = (unsigned char *)calloc(na + 1, 1);
  kb = (unsigned char *)calloc(nb + 1, 1);
  if (lcs == NULL || ka == NULL || kb == NULL) {
    free(lcs);
    free(ka);
    free(kb);
    free(wa);
    free(wb);
    return;
  }
#define LCS(x, y)	lcs[(x) * (nb + 1) + (y)]
  for (i = na; i-- > 0;)
    for (j = nb; j-- > 0;) {
      size_t la = wa[i + 1] - wa[i], lb = wb[j + 1] - wb[j];
      if (la == lb && memcmp(a->s + wa[i], b->s + wb[j], la) == 0) LCS(i, j) = (unsigned short)(LCS(i + 1, j + 1) + 1);
      else LCS(i, j) = LCS(i + 1, j) > LCS(i, j + 1) ? LCS(i + 1, j) : LCS(i, j + 1);
    }
  for (i = 0, j = 0; i < na && j < nb;) {	/* the common words, walked */
    size_t la = wa[i + 1] - wa[i], lb = wb[j + 1] - wb[j];
    if (la == lb && memcmp(a->s + wa[i], b->s + wb[j], la) == 0) {
      ka[i++] = 1;
      kb[j++] = 1;
    }
    else if (LCS(i + 1, j) >= LCS(i, j + 1)) i++;
    else j++;
  }
#undef LCS
  word_ranges(a, wa, na, ka);
  word_ranges(b, wb, nb, kb);
  free(lcs);
  free(ka);
  free(kb);
  free(wa);
  free(wb);
}


/*
** A line's words, hashed with how long each is: two lines are alike when
** they share many, which is what pair_block goes by.
*/
typedef struct WSig {
  unsigned long long *w;	/* (hash << 16) | the word's bytes, sorted */
  size_t n;
} WSig;


static int cmp_word (const void *a, const void *b) {
  unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
  return x < y ? -1 : x > y ? 1 : 0;
}


/* the words of the line, hashed and sorted; only its start, which is enough to tell lines apart */
static void wsig_make (WSig *g, const DLine *l) {
  size_t i = 0, k, n = l->len < SIG_BYTES ? l->len : SIG_BYTES;
  g->w = (unsigned long long *)xmalloc((n + 1) * sizeof(unsigned long long));
  g->n = 0;
  while (i < n) {
    size_t a = i;
    unsigned h = 2166136261u;	/* FNV-1a */
    int c = wclass((unsigned char)l->s[i]);
    if (c == 2) i++;
    else
      while (i < n && wclass((unsigned char)l->s[i]) == c) i++;
    for (k = a; k < i; k++) h = (h ^ (unsigned char)l->s[k]) * 16777619u;
    g->w[g->n++] = ((unsigned long long)h << 16) | (unsigned long long)(i - a);
  }
  qsort(g->w, g->n, sizeof(unsigned long long), cmp_word);
}


/* how alike two lines are, per mille: twice the bytes of the words they share */
static int wsig_alike (const WSig *a, const WSig *b, size_t la, size_t lb) {
  size_t i = 0, j = 0;
  unsigned long long same = 0;
  while (i < a->n && j < b->n) {
    if (a->w[i] < b->w[j]) i++;
    else if (a->w[i] > b->w[j]) j++;
    else {
      same += a->w[i] & 0xFFFF;
      i++;
      j++;
    }
  }
  if (la + lb == 0) return 1000;
  return (int)(2000 * same / (la + lb));
}


/*
** Which removed line became which added one, inside one block of changes.
** A diff prints a block as everything that went and then everything that
** came, so the k'th of each is not always the pair: one line inserted
** above shifts them all, and pairing them in order then finds nothing in
** common and paints whole lines. VS Code diffs the block as a whole, so
** mme pairs the lines by how many words they share, in order; a line left
** without a partner is new or gone and keeps its whole-line colour.
*/
static void pair_block (size_t d0, size_t d1, size_t a1) {
  size_t nd = d1 - d0, na = a1 - d1, i, j, w = na + 1;
  int *sim, *best;
  WSig *sg;
  for (i = d0; i < a1; i++) D.line[i].nhr = 0;	/* a line left without a partner keeps no ranges */
  if (nd == 0 || na == 0) return;
  if ((nd == 1 && na == 1) || nd > PAIR_LINES_MAX || na > PAIR_LINES_MAX || nd * na > PAIR_CELLS_MAX) {
    for (i = 0; i < nd && i < na; i++) word_pair(&D.line[d0 + i], &D.line[d1 + i]);	/* nothing to choose */
    return;
  }
  sg = (WSig *)xmalloc((nd + na) * sizeof(WSig));
  for (i = 0; i < nd + na; i++) wsig_make(&sg[i], &D.line[i < nd ? d0 + i : d1 + i - nd]);
  sim = (int *)xmalloc(nd * na * sizeof(int));
  for (i = 0; i < nd; i++)
    for (j = 0; j < na; j++)
      sim[i * na + j] = wsig_alike(&sg[i], &sg[nd + j], D.line[d0 + i].len, D.line[d1 + j].len);
  best = (int *)xmalloc((nd + 1) * w * sizeof(int));	/* best[i][j]: the most two tails can share */
  for (i = 0; i <= nd; i++) best[i * w + na] = 0;
  for (j = 0; j <= na; j++) best[nd * w + j] = 0;
  for (i = nd; i-- > 0;)
    for (j = na; j-- > 0;) {
      int m = sim[i * na + j] + best[(i + 1) * w + j + 1], u = best[(i + 1) * w + j], v = best[i * w + j + 1];
      best[i * w + j] = m >= u && m >= v ? m : u >= v ? u : v;
    }
  for (i = 0, j = 0; i < nd && j < na;) {	/* the pairs it chose, walked; a tie keeps them in order */
    int m = sim[i * na + j] + best[(i + 1) * w + j + 1], u = best[(i + 1) * w + j], v = best[i * w + j + 1];
    if (m >= u && m >= v) {
      word_pair(&D.line[d0 + i], &D.line[d1 + j]);
      i++;
      j++;
    }
    else if (u >= v) i++;
    else j++;
  }
  for (i = 0; i < nd + na; i++) free(sg[i].w);
  free(sg);
  free(sim);
  free(best);
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
  g_wcells = WORD_CELLS_ALL;	/* the words are diffed once here, never while drawing */
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
    pair_block(d0, d1, a1);	/* which line became which, and the words in them that changed */
    for (j = 0; j < d1 - d0 || j < a1 - d1; j++) {
      long l = (j < d1 - d0) ? (long)(d0 + j) : -1;
      long r = (j < a1 - d1) ? (long)(d1 + j) : -1;
      srow_add(l, r);
    }
  }
}


static void lines_free (void) {
  size_t i;
  for (i = 0; i < D.nline; i++) {
    free(D.line[i].s);
    free(D.line[i].tok);
    free(D.line[i].os);
    free(D.line[i].hr);
  }
  D.nline = 0;
  D.nmax = 0;
  D.wide_ok = 0;
}


void diff_close (void) {
  lines_free();
  free(D.line);
  free(D.row);
  free(D.path);
  free(D.title);
  free(D.rel);
  free(D.ta);
  free(D.tb);
  free(D.vis);
  free(D.exp);
  free(D.sri);
  free(D.srs);
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
  int hunk = 0;	/* a "@@" came: "---" and "+++" are the file's own lines from here on */
  while (*p) {	/* after its header */
    const char *e = strchr(p, '\n');
    size_t len = e ? (size_t)(e - p) : strlen(p);
    if (p[0] == '@' && p[1] == '@') {
      unsigned long a = 0, c2 = 0;
      hunk = 1;
      const char *q = strchr(p, '-'), *r = strchr(p, '+');
      if (q) a = strtoul(q + 1, NULL, 10);
      if (r) c2 = strtoul(r + 1, NULL, 10);
      o = a ? a - 1 : 0;
      n = c2 ? c2 - 1 : 0;
    }
    else if (D.nline > 0 || o > 0 || n > 0 || p[0] == ' ' || p[0] == '-' || p[0] == '+') {
      if (p[0] == ' ') dline_add(' ', ++o, ++n, p + 1, len ? len - 1 : 0);
      else if (p[0] == '-' && !(p[1] == '-' && p[2] == '-' && !hunk))	/* a deleted "-- comment" is not the "---" header */
        dline_add('-', ++o, 0, p + 1, len - 1);
      else if (p[0] == '+' && !(p[1] == '+' && p[2] == '+' && !hunk))
        dline_add('+', 0, ++n, p + 1, len - 1);
    }
    p += len + (e ? 1 : 0);
  }
}


static void finish (void);

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
  finish();
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
  finish();
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
  g_gaveup = 1;
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
  finish();
  return 0;
}

/*
** The old and the new text, whole, from the lines the diff was made of:
** ignoreTrimWhitespace diffs them again.
*/
static void texts_of_lines (void) {
  Buf a, b;
  size_t i;
  if (D.ta) return;
  buf_init(&a);
  buf_init(&b);
  for (i = 0; i < D.nline; i++) {
    const DLine *l = &D.line[i];
    if (l->kind != '+') {
      buf_putn(&a, l->os ? l->os : l->s, l->os ? l->olen : l->len);
      buf_putc(&a, '\n');
    }
    if (l->kind != '-') {
      buf_putn(&b, l->s, l->len);
      buf_putc(&b, '\n');
    }
  }
  buf_putc(&a, '\0');
  buf_putc(&b, '\0');
  D.tna = a.len - 1;
  D.tnb = b.len - 1;
  D.ta = buf_take(&a);
  D.tb = buf_take(&b);
}


/* a line for comparing: without its '\r', and with trim without its spaces at both ends */
static void cmp_line (TLine *t, int trim) {
  size_t j;
  if (t->n > 0 && t->s[t->n - 1] == '\r') t->n--;
  if (trim) {
    while (t->n > 0 && (t->s[0] == ' ' || t->s[0] == '\t')) {
      t->s++;
      t->n--;
    }
    while (t->n > 0 && (t->s[t->n - 1] == ' ' || t->s[t->n - 1] == '\t' || t->s[t->n - 1] == '\r')) t->n--;
  }
  t->h = 5381;
  for (j = 0; j < t->n; j++) t->h = t->h * 33 + (unsigned char)t->s[j];
}


/*
** The lines again from the two texts, Myers' diff, the lines compared with
** or without their spaces at both ends (VS Code's ignoreTrimWhitespace). 0
** done; -1: too different for it, the lines as they were stay.
*/
static int relines (int trim) {
  size_t n, m, nops, i, o = 0, w = 0;
  TLine *la, *lb, *ca, *cb;
  char *ops;
  D.trim = trim;
  if (D.ta == NULL) return -1;
  la = text_lines(D.ta, D.tna, &n);
  lb = text_lines(D.tb, D.tnb, &m);
  ca = (TLine *)xmalloc((n + 1) * sizeof(TLine));
  cb = (TLine *)xmalloc((m + 1) * sizeof(TLine));
  memcpy(ca, la, n * sizeof(TLine));
  memcpy(cb, lb, m * sizeof(TLine));
  for (i = 0; i < n; i++) cmp_line(&ca[i], trim);
  for (i = 0; i < m; i++) cmp_line(&cb[i], trim);
  g_gaveup = 0;
  ops = edit_script(ca, n, cb, m, &nops);
  if (g_gaveup) {
    free(ops);
    free(la);
    free(lb);
    free(ca);
    free(cb);
    return -1;
  }
  lines_free();
  i = 0;
  while (i < nops) {
    size_t j;
    if (ops[i] == ' ') {
      DLine *l;
      dline_add(' ', o + 1, w + 1, lb[w].s, lb[w].n);
      l = &D.line[D.nline - 1];
      {	/* the old side's text, when only its spaces differ */
        size_t on = la[o].n;
        if (on > 0 && la[o].s[on - 1] == '\r') on--;
        if (on != l->len || memcmp(la[o].s, l->s, on) != 0) {
          l->os = xstrndup(la[o].s, on);
          l->olen = on;
        }
      }
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
  if (D.nline == 0) dline_add(' ', 1, 1, "", 0);
  free(ops);
  free(la);
  free(lb);
  free(ca);
  free(cb);
  return 0;
}


static void vis_build (void);

/* the rows, the words changed, the colors: again from D.line */
static void rebuild (void) {
  build_split();
  color_lines(D.path);
  D.vis_split = -1;	/* vis again when it is drawn */
}


/* every diff opened ends here */
static void finish (void) {
  static int side_set;
  if (!side_set) {	/* diffEditor.renderSideBySide, the first time */
    D.inline_mode = !vopt.diff_side;
    side_set = 1;
  }
  texts_of_lines();
  D.trim = 0;
  if (vopt.diff_trim) relines(1);
  rebuild();
  D.open = 1;
  D.crow = 0;
  diff_change(0);
  D.crow = D.cur;
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
  D.edit = !staged;	/* the new side is the file itself: VS Code types in it */
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
  finish();
  return 0;
}


int diff_active (void) {
  return D.open;
}


/*
** The modified side is the file in the working tree: VS Code lets it be
** typed in, so mme does too (a commit's or the index's diff is read only).
*/
int diff_editable (void) {
  return D.open && D.edit && D.path && D.path[0];
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


/* the new side's line of row k (0: the row has none) */
static size_t new_line_of (size_t k) {
  const DLine *l;
  if (D.split_now) {
    if (k >= D.nrow || D.row[k].r < 0) return 0;
    return D.line[D.row[k].r].n;
  }
  l = k < D.nline ? &D.line[k] : NULL;
  return l ? l->n : 0;
}


/* the file's line at the cursor's row (a row only on the old side: the next one's), from 1 */
size_t diff_line (void) {
  size_t k, n = nrows();
  for (k = D.crow; k < n; k++) {
    const DLine *l = row_line(k);
    if (l && l->n) return l->n;
  }
  for (k = D.crow < n ? D.crow : n; k-- > 0;) {
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


static void vpush (size_t k, size_t hidden) {
  if (D.nvis == D.capvis) D.vis = (VRow *)xrealloc(D.vis, (D.capvis = D.capvis ? D.capvis * 2 : 256) * sizeof(VRow));
  D.vis[D.nvis].k = k;
  D.vis[D.nvis].hidden = hidden;
  D.nvis++;
}


/*
** diffEditor.hideUnchangedRegions: a run of unchanged rows keeps CTX rows
** next to each change; the rest is one row "N hidden lines", opened by a
** click (Enter). No change at all: everything shows.
*/
static void vis_build (void) {
  size_t n = nrows(), k = 0;
  int any = 0;
  D.nvis = 0;
  D.vis_split = D.split_now;
  D.vis_hide = vopt.diff_hide;
  if (D.nexp != n) {	/* other rows (side by side, inline): the folds opened are forgotten */
    free(D.exp);
    D.exp = (unsigned char *)calloc(n + 1, 1);
    D.nexp = n;
  }
  if (!vopt.diff_hide) return;
  for (k = 0; k < n && !any; k++) any = is_change(k);
  if (!any) return;
  k = 0;
  while (k < n) {
    size_t s0, e, head, tail, j;
    if (is_change(k)) {
      vpush(k++, 0);
      continue;
    }
    s0 = k;
    while (k < n && !is_change(k)) k++;
    e = k;
    head = s0 == 0 ? 0 : CTX;
    tail = e == n ? 0 : CTX;
    if (e - s0 > head + tail && e - s0 - head - tail >= MIN_HIDE && !(D.exp && D.exp[s0 + head])) {
      for (j = s0; j < s0 + head; j++) vpush(j, 0);
      vpush(s0 + head, e - s0 - head - tail);
      for (j = e - tail; j < e; j++) vpush(j, 0);
    }
    else
      for (j = s0; j < e; j++) vpush(j, 0);
  }
}


static size_t vcount (void) {
  return D.nvis ? D.nvis : nrows();
}


static size_t vk (size_t i) {	/* the row shown i-th */
  return D.nvis ? D.vis[i].k : i;
}


static size_t vpos (size_t k) {	/* the place of row k among those shown (a hidden one: its fold's) */
  size_t lo = 0, hi;
  if (!D.nvis) return k;
  hi = D.nvis;
  while (hi - lo > 1) {
    size_t mid = (lo + hi) / 2;
    if (D.vis[mid].k <= k) lo = mid;
    else hi = mid;
  }
  return lo;
}


/* the cursor d rows on (of those shown); the view follows */
/* the columns cp takes at column col, as scr_code draws it */
static size_t ccw (uint32_t cp, size_t col) {
  if (cp == '\t') return (size_t)TABW - col % (size_t)TABW;
  if (cp < 32 || cp == 127) return 2;
  return (size_t)uc_width(cp);
}


/* the column byte c of s is drawn at (tabs expanded, like scr_code) */
static size_t dcol (const char *s, size_t n, size_t c) {
  size_t i = 0, col = 0, len;
  while (i < n && i < c) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    col += ccw(cp, col);
    i += len;
  }
  return col;
}


/* the byte of s drawn at column col */
static size_t dbyte (const char *s, size_t n, size_t col) {
  size_t i = 0, c = 0, len;
  while (i < n && c < col) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    c += ccw(cp, c);
    i += len;
  }
  return i;
}


/* the widest line's columns, either side */
static size_t diff_wide (void) {
  size_t i;
  if (D.wide_ok) return D.wide;
  D.wide = 0;
  for (i = 0; i < D.nline; i++) {
    size_t c = dcol(D.line[i].s, D.line[i].len, D.line[i].len);
    if (D.line[i].os) {
      size_t o = dcol(D.line[i].os, D.line[i].olen, D.line[i].olen);
      if (o > c) c = o;
    }
    if (c > D.wide) D.wide = c;
  }
  D.wide_ok = 1;
  return D.wide;
}


/*
** Word wrap in the diff: a line wider than its side's text is cut into
** rows, after a space when there is one, like the editor's. dsegs gives
** where each row starts (bytes); side by side, a row is as high as its
** higher side.
*/
#define DSEG	512

static size_t dsegs (const char *s, size_t n, int tw, size_t *st) {
  size_t k = 1, i = 0, col = 0, segcol = 0, sp = 0, len;
  st[0] = 0;
  if (!D.wrap || tw < 4) return 1;
  while (i < n) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    size_t cw = ccw(cp, col);
    if (col + cw - segcol > (size_t)tw && i > st[k - 1]) {	/* it does not fit: a new row */
      size_t cut = sp > st[k - 1] ? sp : i;
      if (k == DSEG) break;
      st[k++] = cut;
      i = cut;
      col = segcol = dcol(s, n, cut);
      sp = 0;
      continue;
    }
    if (cp == ' ' || cp == '\t') sp = i + len;
    col += cw;
    i += len;
  }
  return k;
}


/* the text a side shows of l: the old side of an unchanged line may have its old spaces */
static const char *side_text (const DLine *l, int left, size_t *n, const unsigned char **tok) {
  if (left && l->kind == ' ' && l->os) {
    *n = l->olen;
    if (tok) *tok = NULL;
    return l->os;
  }
  *n = l->len;
  if (tok) *tok = l->tok;
  return l->s;
}


/* the rows of view row i on the screen: 1 but with word wrap */
static size_t row_h (size_t i) {
  size_t st[DSEG], k, n, a = 1, b = 1;
  const char *s;
  if (!D.wrap || (D.nvis && D.vis[i].hidden)) return 1;
  k = vk(i);
  if (D.split_now) {
    if (k >= D.nrow) return 1;
    if (D.row[k].l >= 0) {
      s = side_text(&D.line[D.row[k].l], 1, &n, NULL);
      a = dsegs(s, n, D.tcl, st);
    }
    if (D.row[k].r >= 0) {
      s = side_text(&D.line[D.row[k].r], 0, &n, NULL);
      b = dsegs(s, n, D.tcr, st);
    }
    return a > b ? a : b;
  }
  return k < D.nline ? dsegs(D.line[k].s, D.line[k].len, D.tcr, st) : 1;
}


/* the last view row that can be the top: the rows from it fill the screen to the end */
static size_t last_top (void) {
  size_t n = vcount(), i = n, used = 0, h = D.h > 0 ? (size_t)D.h : 1;
  if (!D.wrap) return n > h ? n - h : 0;
  while (i > 0) {
    size_t rh = row_h(i - 1);
    if (used + rh > h) break;
    used += rh;
    i--;
  }
  return i < n ? i : (n ? n - 1 : 0);
}


/* the view follows view row i: it goes on the screen, whole when it fits */
static void show_row (size_t i) {
  size_t i0 = vpos(D.top), h = D.h > 0 ? (size_t)D.h : 1, t = i, used;
  if (i < i0) {
    D.top = vk(i);
    return;
  }
  used = row_h(i);
  while (t > i0 && used + row_h(t - 1) <= h) used += row_h(--t);
  if (t > i0) D.top = vk(t);
}


/* the new side's line at the caret's row, "" when the row has none */
static const DLine *caret_dline (void) {
  if (D.split_now) return (D.crow < D.nrow && D.row[D.crow].r >= 0) ? &D.line[D.row[D.crow].r] : NULL;
  return (D.crow < D.nline && D.line[D.crow].kind != '-') ? &D.line[D.crow] : NULL;
}


static const char *caret_line (void) {
  const DLine *l = caret_dline();
  return l ? l->s : "";
}


static size_t caret_len (void) {
  const DLine *l = caret_dline();
  return l ? l->len : 0;
}


/* the byte before / after c in s (UTF-8) */
static size_t prev_byte (const char *s, size_t c) {
  while (c > 0 && ((unsigned char)s[--c] & 0xC0) == 0x80) ;
  return c;
}


static size_t next_byte (const char *s, size_t c) {
  size_t len = strlen(s);
  if (c >= len) return len;
  c++;
  while (c < len && ((unsigned char)s[c] & 0xC0) == 0x80) c++;
  return c;
}


static void crow_move (long d) {
  size_t n = vcount(), i;
  long p;
  if (n == 0) return;
  p = (long)vpos(D.crow) + d;
  if (p < 0) p = 0;
  if (p >= (long)n) p = (long)n - 1;
  i = (size_t)p;
  D.crow = vk(i);
  if (is_change(D.crow)) D.cur = D.crow;	/* the change Stage / Revert Selected Ranges takes */
  show_row(i);
}


/* where the caret is in the file: its line (from 1) and byte; 0: nowhere to type */
size_t diff_caret (size_t *col) {
  size_t line, i = vpos(D.crow);
  if (D.nvis && i < D.nvis && D.vis[i].hidden && D.vis[i].k == D.crow) return 0;	/* a hidden region: Enter opens it */
  line = new_line_of(D.crow);
  if (line == 0) line = diff_line();	/* a row of the old side only: the next line of the file */
  if (col) *col = D.ccol;
  return line;
}


/* the caret on the file's line (from 1) and byte, the row shown */
void diff_set_caret (size_t line, size_t col) {
  size_t n = nrows(), k, best = (size_t)-1;
  for (k = 0; k < n; k++) {
    size_t l = new_line_of(k);
    if (l == line) {
      best = k;
      break;
    }
    if (l && l < line) best = k;
  }
  if (best == (size_t)-1) return;
  D.crow = best;
  D.ccol = col;
  show_row(vpos(D.crow));
}


/*
** The modified side's text now (the editor's buffer, as it is typed in):
** the lines are made again from it, so the diff follows the typing.
*/
void diff_new_text (const char *s, size_t n) {
  size_t top = D.top, crow = D.crow, cur = D.cur;
  if (!D.open || D.ta == NULL) return;
  free(D.tb);
  D.tb = xstrndup(s ? s : "", n);
  D.tnb = n;
  if (relines(vopt.diff_trim) != 0) return;	/* too different for it: what is drawn stays */
  rebuild();
  vis_build();
  D.top = top < nrows() ? top : 0;
  D.crow = crow < nrows() ? crow : 0;
  D.cur = cur < nrows() ? cur : 0;
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
  if (D.nvis) D.top = vk(vpos(D.top));
  D.cur = k;
  D.crow = k;
}


/* the changed words of a line drawn at x (from its column left on): their background brighter */
static void paint_words (int x, int y, int w, const char *s, size_t n, const DLine *l, size_t left, uint32_t rgb) {
  size_t i = 0, col = 0, len, right = left + (size_t)(w > 0 ? w : 0), r = 0;
  while (i < n && col < right && r < l->nhr) {
    uint32_t cp = utf8_decode(s + i, n - i, &len);
    size_t cw = cp == '\t' ? (size_t)TABW - col % (size_t)TABW : (cp < 32 || cp == 127) ? 2 : (size_t)uc_width(cp), k;
    while (r < l->nhr && i >= l->hr[2 * r + 1]) r++;
    if (r < l->nhr && i >= l->hr[2 * r])
      for (k = 0; k < cw; k++)
        if (col + k >= left && col + k < right) scr_set_bg(x + (int)(col + k - left), y, rgb);
    col += cw;
    i += len;
  }
}


/* row seg of l's text s (word wrap; without it, the part from D.left on) at x, in w columns */
static void draw_text (int x, int y, int w, const DLine *l, const char *s, size_t n, const unsigned char *tok,
                       size_t seg, int cur) {
  size_t st[DSEG], ns = dsegs(s, n, w, st), c0 = D.left, cw = w > 0 ? (size_t)w : 0;
  int plain = l->kind == ' ' || l->nhr;
  if (seg >= ns) return;
  if (D.wrap) {
    size_t c1 = dcol(s, n, seg + 1 < ns ? st[seg + 1] : n);
    c0 = dcol(s, n, st[seg]);
    if (c1 - c0 < cw) cw = c1 - c0;
  }
  scr_code(x, y, (int)cw, s, n, c0, tok, l->kind == '+' ? B_ADD : l->kind == '-' ? B_DEL : cur ? B_LINE : B_EDITOR,
           plain ? 0 : l->h0, plain ? 0 : l->h1, l->kind == '+' ? B_ADD_HI : l->kind == '-' ? B_DEL_HI : B_EDITOR);
  if (l->nhr) paint_words(x, y, (int)cw, s, n, l, c0, ui_color(l->kind == '+' ? C_DIFF_ADD_HI : C_DIFF_DEL_HI));
}


/* where the caret (byte c of s, drawn at x in tw columns) is on row seg; -1: not on it */
static int caret_x (int x, const char *s, size_t n, int tw, size_t seg, size_t c) {
  size_t st[DSEG], ns = dsegs(s, n, tw, st), j = 0;
  if (!D.wrap) return seg == 0 ? x + (int)dcol(s, n, c) - (int)D.left : -1;
  while (j + 1 < ns && st[j + 1] <= c) j++;
  if (j != seg) return -1;
  return x + (int)(dcol(s, n, c) - dcol(s, n, st[j]));
}


static void draw_side (int x, int y, int w, const DLine *l, int left, int nw, int cur, size_t seg) {
  char num[32];
  int st;
  const char *s;
  size_t len;
  const unsigned char *tok;
  if (l == NULL) {	/* nothing on this side: VS Code's diagonal fill */
    int i;
    for (i = 0; i < w; i++) scr_put(x + i, y, 0x2571, S_DIFF_FILL);
    return;
  }
  s = side_text(l, left, &len, &tok);
  st = l->kind == '+' ? S_DIFF_ADD : l->kind == '-' ? S_DIFF_DEL : cur ? S_LINE : S_TEXT;
  if (seg == 0) snprintf(num, sizeof(num), "%*lu ", nw - 1, (unsigned long)(left ? l->o : l->n));
  else snprintf(num, sizeof(num), "%*s ", nw - 1, "");	/* a wrapped line's next rows: no number */
  scr_puts(x, y, num, cur ? S_GUTTER_CUR : S_DIFF_NUM);
  scr_fill(x + nw, y, w - nw, st);
  if (seg == 0 && l->kind != ' ') scr_put(x + nw, y, l->kind == '+' ? '+' : '-', st);
  draw_text(x + nw + 2, y, w - nw - 2, l, s, len, tok, seg, cur);
}


/* a fold of hidden rows: "⋯ 12 hidden lines", VS Code's */
static void draw_fold (int x, int y, int w, size_t hidden) {
  char t[64];
  snprintf(t, sizeof(t), "  \xE2\x8B\xAF  %lu hidden line%s", (unsigned long)hidden, hidden == 1 ? "" : "s");
  scr_fill(x, y, w, S_DIFF_HUNK);
  scr_putsw(x, y, w, t, S_DIFF_HUNK);
}


#define DMM_W	10	/* the diff's minimap: columns */
#define DMM_CH	5	/* characters of a line in one of its dots */

/*
** The minimap of the diff, like VS Code's: the rows of the diff in small,
** green where lines came and red where they went, with the part shown
** lighter. rows: how many rows of the diff one of its rows holds.
*/
static void draw_dmap (int x, int y, int h, size_t i0) {
  static const unsigned char bit[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
  size_t n = vcount(), per = (n + (size_t)h - 1) / (size_t)h, r;
  if (per == 0) per = 1;
  for (r = 0; (int)r < h; r++) {
    size_t i, kinds = 0;
    uint32_t dot[4][2 * DMM_W], bg;
    int in_view = (r + 1) * per > i0 && r * per < i0 + (size_t)h, c, k;
    for (k = 0; k < 4; k++)
      for (c = 0; c < 2 * DMM_W; c++) dot[k][c] = 0xFFFFFFFFu;
    for (k = 0; k < 4; k++) {	/* the lines of this row: one in each quarter */
      const DLine *l;
      size_t col = 0, b = 0;
      i = r * per + (size_t)k * per / 4;
      if (i >= n) break;
      l = row_line(vk(i));
      if (l == NULL) continue;
      if (l->kind == '+') kinds |= 1;
      else if (l->kind == '-') kinds |= 2;
      while (b < l->len && col / DMM_CH < 2 * DMM_W) {
        unsigned char ch = (unsigned char)l->s[b];
        if (ch != ' ' && ch != '\t' && dot[k][col / DMM_CH] == 0xFFFFFFFFu)
          dot[k][col / DMM_CH] = tok_color(l->tok ? l->tok[b] : 0);
        col += ch == '\t' ? (size_t)TABW - col % (size_t)TABW : 1;
        b++;
      }
    }
    bg = ui_color(kinds == 1 ? C_DIFF_ADD : kinds ? C_DIFF_DEL : in_view ? C_MINIMAP_SLIDER : C_EDITOR_BG);
    if (in_view && kinds) bg = ui_color(kinds == 1 ? C_DIFF_ADD_HI : C_DIFF_DEL_HI);
    for (c = 0; c < DMM_W; c++) {
      uint32_t fg = 0xFFFFFFFFu;
      unsigned m = 0;
      for (k = 0; k < 4; k++) {
        int d;
        for (d = 0; d < 2; d++)
          if (dot[k][2 * c + d] != 0xFFFFFFFFu) {
            m |= bit[k][d];
            if (fg == 0xFFFFFFFFu) fg = dot[k][2 * c + d];
          }
      }
      scr_put_rgb(x + c, y + (int)r, m ? 0x2800 + m : ' ', m ? fg : bg, bg, 0);
    }
  }
}


/* the first row of a block of changes: a revert arrow goes there */
static int block_start (size_t k) {
  return is_change(k) && (k == 0 || !is_change(k - 1));
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


#define SB_ADD	0x2EA043	/* the scrollbar's marks: the editor's overview ruler colors */
#define SB_MOD	0x0078D4
#define SB_DEL	0xF85149

/* what row k changes: 1 a line came, 2 one went, 3 both */
static unsigned row_kinds (size_t k) {
  unsigned m = 0;
  if (D.split_now) {
    if (k >= D.nrow) return 0;
    if (D.row[k].l >= 0 && D.line[D.row[k].l].kind == '-') m |= 2;
    if (D.row[k].r >= 0 && D.line[D.row[k].r].kind == '+') m |= 1;
    return m;
  }
  if (k >= D.nline) return 0;
  return D.line[k].kind == '+' ? 1 : D.line[k].kind == '-' ? 2 : 0;
}


/* the vertical scrollbar's thumb in h rows: its size, and where it is */
static int vthumb (int h, int *pos) {
  size_t lt = last_top(), i0 = vpos(D.top);
  int size;
  *pos = 0;
  if (lt == 0 || h < 1) return h;
  size = (int)((size_t)h * (size_t)h / (lt + (size_t)h));
  if (size < 1) size = 1;
  *pos = (int)((double)(i0 < lt ? i0 : lt) * (double)(h - size) / (double)lt + 0.5);
  if (*pos > h - size) *pos = h - size;
  return size;
}


/* the vertical scrollbar at x: the thumb, and the changes where they are, like the editor's */
static void draw_vbar (int x, int y, int h) {
  int pos, size = vthumb(h, &pos), r;
  size_t n = vcount(), all = n > (size_t)h ? n : (size_t)h;	/* all fits: a row of it is a row of the bar */
  for (r = 0; r < h; r++) {
    size_t a = all * (size_t)r / (size_t)h, b = all * (size_t)(r + 1) / (size_t)h, i;
    unsigned kinds = 0;
    int on = size < h && r >= pos && r < pos + size;
    uint32_t bg = ui_color(on ? (D.drag_sb ? C_THUMB_ON : C_THUMB) : C_EDITOR_BG);
    if (b <= a) b = a + 1;
    for (i = a; i < b && i < n; i++)
      if (!(D.nvis && D.vis[i].hidden)) kinds |= row_kinds(vk(i));
    if (kinds) scr_put_rgb(x, y + r, 0x258C, kinds == 1 ? SB_ADD : kinds == 2 ? SB_DEL : SB_MOD, bg, 0);
    else scr_put_rgb(x, y + r, ' ', bg, bg, 0);
  }
}


/* the columns the text shows side by side: the narrower side's */
static size_t text_cols (void) {
  int tc = D.tcl < D.tcr ? D.tcl : D.tcr;
  return tc > 1 ? (size_t)tc : 1;
}


/* a horizontal scrollbar's thumb in bw columns: its size, and where it is */
static int hthumb (int bw, int *pos) {
  size_t wide = diff_wide() + 1, tc = text_cols();
  int size;
  *pos = 0;
  if (wide <= tc || bw < 1) return bw;
  size = (int)((size_t)bw * tc / wide);
  if (size < 2) size = 2;
  if (size > bw) size = bw;
  *pos = (int)((double)D.left * (double)(bw - size) / (double)(wide - tc) + 0.5);
  if (*pos > bw - size) *pos = bw - size;
  return size;
}


/* the horizontal scrollbar: half a row high, the lower half of the cell, like the editor's */
static void draw_hbar (int x, int y, int bw) {
  int pos, size = hthumb(bw, &pos), i;
  for (i = 0; i < bw; i++) {
    int on = i >= pos && i < pos + size;
    uint32_t fg = ui_color(on ? (D.drag_hsb ? C_THUMB_ON : C_THUMB) : C_EDITOR_BG);
    scr_put_rgb(x + i, y, 0x2584, fg, ui_color(C_EDITOR_BG), 0);
  }
}


void diff_draw (int x, int y, int w, int h, int wrap) {
  int nw = 2, sy;
  size_t m = D.nmax, n, i0, i, lt;
  if (D.ta && D.trim != vopt.diff_trim) {	/* ignoreTrimWhitespace changed: the lines again */
    relines(vopt.diff_trim);
    rebuild();
    if (D.crow >= nrows()) D.crow = 0;
  }
  while (m >= 10) {
    m /= 10;
    nw++;
  }
  nw++;
  D.split_now = !D.inline_mode && w >= 80;
  D.wrap = wrap;
  D.cx = D.cy = -1;
  D.x = x;
  D.y = y;
  D.w = w;
  D.nw = nw;
  D.mmw = (opt.minimap && w >= 100) ? DMM_W : 0;	/* its minimap, when there is room */
  D.sbw = (w >= 30 && h >= 2) ? 1 : 0;	/* its scrollbar, at the right edge like the editor's */
  w -= D.mmw + D.sbw;
  D.hw = (w - 1) / 2;
  D.arrow_x = 2 * nw - 1;
  if (D.split_now) {
    D.tcl = D.hw - nw - 2;
    D.tcr = w - D.hw - 1 - nw - 2;
  }
  else D.tcl = D.tcr = w - 2 * nw - 2;
  if (D.vis_split != D.split_now || D.vis_hide != vopt.diff_hide) vis_build();
  D.hsb = !wrap && h > 3 && diff_wide() + 1 > text_cols();	/* a line wider than the text: a scrollbar under it */
  if (D.hsb) h--;
  D.h = h;
  if (wrap) D.left = 0;
  else {
    size_t wide = diff_wide() + 1, tc = text_cols(), maxl = wide > tc ? wide - tc : 0;
    if (diff_editable() && (D.crow != D.lastrow || D.ccol != D.lastcol)) {	/* the caret moved: the view follows it */
      size_t c = dcol(caret_line(), caret_len(), D.ccol < caret_len() ? D.ccol : caret_len());
      size_t tw = D.tcr > 1 ? (size_t)D.tcr : 1;
      if (c < D.left) D.left = c;
      else if (c >= D.left + tw) D.left = c - tw + 1;
    }
    if (D.left > maxl) D.left = maxl;
  }
  D.lastrow = D.crow;
  D.lastcol = D.ccol;
  n = vcount();
  i0 = vpos(D.top);
  lt = last_top();
  if (i0 > lt) i0 = lt;
  D.top = n ? vk(i0) : 0;
  if (D.capsr < h) {
    D.capsr = h;
    D.sri = (size_t *)xrealloc(D.sri, (size_t)h * sizeof(size_t));
    D.srs = (size_t *)xrealloc(D.srs, (size_t)h * sizeof(size_t));
  }
  D.nsr = 0;
  scr_box(x, y, w, h, S_TEXT);
  for (i = i0, sy = y; i < n && sy < y + h; i++) {
    size_t k = vk(i), rh, seg;
    int cur;
    if (D.nvis && D.vis[i].hidden) {
      draw_fold(x, sy, w, D.vis[i].hidden);
      D.sri[D.nsr] = i;
      D.srs[D.nsr++] = 0;
      sy++;
      continue;
    }
    cur = k == D.crow;
    rh = row_h(i);
    for (seg = 0; seg < rh && sy < y + h; seg++, sy++) {
      D.sri[D.nsr] = i;
      D.srs[D.nsr++] = seg;
      if (D.split_now) {
        int hw = D.hw;
        const SRow *r = &D.row[k];
        draw_side(x, sy, hw, r->l >= 0 ? &D.line[r->l] : NULL, 1, nw, cur, seg);
        if (in_current(k)) scr_put(x, sy, 0x258E, S_TOGGLE_ON);	/* the change Stage Selected Ranges takes */
        if (seg == 0 && D.kind == DK_TREE && block_start(k)) scr_put(x + hw, sy, 0x2192, S_TOGGLE_ON);	/* → Revert Block */
        else scr_put(x + hw, sy, 0x2502, S_DIFF_FILL);
        draw_side(x + hw + 1, sy, w - hw - 1, r->r >= 0 ? &D.line[r->r] : NULL, 0, nw, cur, seg);
        if (cur && diff_editable() && r->r >= 0) {	/* the caret: the modified side is typed in */
          const DLine *l = &D.line[r->r];
          int cx = caret_x(x + hw + 1 + nw + 2, l->s, l->len, D.tcr, seg, D.ccol < l->len ? D.ccol : l->len);
          if (cx >= 0) {
            D.cx = cx;
            D.cy = sy;
          }
        }
      }
      else {	/* inline: both numbers, then the line */
        const DLine *l = &D.line[k];
        char num[64];
        int st = l->kind == '+' ? S_DIFF_ADD : l->kind == '-' ? S_DIFF_DEL : cur ? S_LINE : S_TEXT;
        char a[24], b[24];
        a[0] = b[0] = '\0';
        if (seg == 0 && l->o) snprintf(a, sizeof(a), "%lu", (unsigned long)l->o);
        if (seg == 0 && l->n) snprintf(b, sizeof(b), "%lu", (unsigned long)l->n);
        snprintf(num, sizeof(num), "%*s %*s ", nw - 1, a, nw - 1, b);
        scr_puts(x, sy, num, cur ? S_GUTTER_CUR : S_DIFF_NUM);
        scr_fill(x + 2 * nw, sy, w - 2 * nw, st);
        if (seg == 0 && l->kind != ' ') scr_put(x + 2 * nw, sy, (uint32_t)l->kind, st);
        draw_text(x + 2 * nw + 2, sy, w - 2 * nw - 2, l, l->s, l->len, l->tok, seg, cur);
        if (in_current(k)) scr_put(x, sy, 0x258E, S_TOGGLE_ON);
        if (seg == 0 && D.kind == DK_TREE && block_start(k)) scr_put(x + D.arrow_x, sy, 0x2192, S_TOGGLE_ON);	/* → Revert Block */
        if (cur && diff_editable() && l->kind != '-') {	/* the caret */
          int cx = caret_x(x + 2 * nw + 2, l->s, l->len, D.tcr, seg, D.ccol < l->len ? D.ccol : l->len);
          if (cx >= 0) {
            D.cx = cx;
            D.cy = sy;
          }
        }
      }
    }
  }
  if (D.mmw > 0) draw_dmap(x + w, y, h, i0);
  if (D.sbw > 0) draw_vbar(x + w + D.mmw, y, h);
  D.nhb = 0;
  if (D.hsb) {	/* the row under the text: a scrollbar under each side's text */
    scr_fill(x, y + h, D.w, S_TEXT);
    if (D.split_now) {
      scr_put(x + D.hw, y + h, 0x2502, S_DIFF_FILL);
      D.hbx[0] = x + nw + 2;
      D.hbw[0] = D.tcl;
      D.hbx[1] = x + D.hw + 1 + nw + 2;
      D.hbw[1] = D.tcr;
      D.nhb = 2;
    }
    else {
      D.hbx[0] = x + 2 * nw + 2;
      D.hbw[0] = D.tcr;
      D.nhb = 1;
    }
    for (i = 0; i < (size_t)D.nhb; i++)
      if (D.hbw[i] > 0) draw_hbar(D.hbx[i], y + h, D.hbw[i]);
  }
  if (D.cx >= x && D.cx < x + w && D.cy >= y) scr_cursor(D.cx, D.cy);
}


/* the vertical thumb follows the mouse at row my */
static void vbar_mouse (int my) {
  int pos, size = vthumb(D.h, &pos), ry = my - D.y;
  size_t lt = last_top(), i;
  if (lt == 0 || D.h - size <= 0) return;
  if (!D.drag_sb) {
    if (ry >= pos && ry < pos + size) D.drag_sb = 1 + (ry - pos);	/* the thumb is taken */
    else D.drag_sb = 1 + size / 2;	/* elsewhere: it jumps there, like VS Code */
  }
  ry -= D.drag_sb - 1;
  if (ry < 0) ry = 0;
  i = (size_t)((double)ry * (double)lt / (double)(D.h - size) + 0.5);
  D.top = vk(i < lt ? i : lt);
}


/* a horizontal thumb (the one taken, hbar) follows the mouse at column mx */
static void hbar_mouse (int b, int mx) {
  int bw = D.hbw[b], pos, size = hthumb(bw, &pos), rx = mx - D.hbx[b];
  size_t wide = diff_wide() + 1, tc = text_cols(), left;
  if (wide <= tc || bw - size <= 0) return;
  if (!D.drag_hsb) {
    if (rx >= pos && rx < pos + size) D.drag_hsb = 1 + (rx - pos);
    else D.drag_hsb = 1 + size / 2;
  }
  rx -= D.drag_hsb - 1;
  if (rx < 0) rx = 0;
  left = (size_t)((double)rx * (double)(wide - tc) / (double)(bw - size) + 0.5);
  D.left = left < wide - tc ? left : wide - tc;
}


static int g_hbar;	/* the horizontal scrollbar taken: 0 left, 1 right */

/* a press at mx, my on the diff's scrollbars: 1 it was one of them (its thumb is taken) */
int diff_bar_press (int mx, int my) {
  int b;
  D.drag_sb = D.drag_hsb = 0;
  if (!D.open) return 0;
  if (D.sbw && mx == D.x + D.w - 1 && my >= D.y && my < D.y + D.h) {
    vbar_mouse(my);
    return 1;
  }
  if (!D.hsb || my != D.y + D.h || mx < D.x || mx >= D.x + D.w) return 0;
  for (b = 0; b < D.nhb; b++)
    if (mx >= D.hbx[b] && mx < D.hbx[b] + D.hbw[b]) {
      g_hbar = b;
      hbar_mouse(b, mx);
    }
  return 1;	/* the row of the scrollbars: nothing else is there */
}


int diff_bar_held (void) {
  return D.open && (D.drag_sb || D.drag_hsb);
}


void diff_bar_drag (int mx, int my) {
  if (D.drag_sb) vbar_mouse(my);
  else if (D.drag_hsb) hbar_mouse(g_hbar, mx);
}


void diff_bar_up (void) {
  D.drag_sb = D.drag_hsb = 0;
}


/*
** The diff's actions in the tab bar, right-aligned before x1 like VS Code's
** editor title: previous / next change, show whitespace changes (¶),
** hidden unchanged regions, inline / side by side, open the file.
*/
int diff_title_draw (int x1, int y) {
  static const uint32_t icon[6] = {0xEAA1, 0xEA9A, 0x00B6, 0xEAC5, 0xEB56, 0xEA94};
  int i, x = x1 - 2 * 6 - 1;
  if (!D.open || x < 0) {
    D.nact = 0;
    return x1;
  }
  scr_fill(x, y, x1 - x, S_TABS);
  for (i = 0; i < 6; i++) {
    int on = (i == 2 && !vopt.diff_trim) || (i == 3 && vopt.diff_hide) || (i == 4 && !D.split_now);
    D.act_x[i] = x + 1 + 2 * i;
    scr_put(D.act_x[i], y, icon[i], on ? S_TOGGLE_ON : S_TABS);
  }
  D.nact = 6;
  return x;
}


/* diffEditor.ignoreTrimWhitespace, the ¶ toggle */
void diff_toggle_trim (void) {
  vopt.diff_trim = !vopt.diff_trim;
  settings_put_json("diffEditor.ignoreTrimWhitespace", vopt.diff_trim ? "true" : "false");
  toast(0, vopt.diff_trim ? "Changes of spaces at the ends of lines are not shown" : "Changes of spaces at the ends of lines are shown");
}


/* diffEditor.hideUnchangedRegions.enabled */
void diff_toggle_hide (void) {
  vopt.diff_hide = !vopt.diff_hide;
  settings_put_json("diffEditor.hideUnchangedRegions.enabled", vopt.diff_hide ? "true" : "false");
  if (D.exp) memset(D.exp, 0, D.nexp);
}


/* a click on the title's icons: DIFF_NO not one of them */
int diff_title_click (int x) {
  int i;
  for (i = 0; i < D.nact; i++)
    if (x == D.act_x[i] || x == D.act_x[i] + 1) {
      switch (i) {
        case 0: diff_change(1); break;
        case 1: diff_change(0); break;
        case 2: diff_toggle_trim(); break;
        case 3: diff_toggle_hide(); break;
        case 4: D.inline_mode = !D.inline_mode; break;
        default: return DIFF_EDIT;
      }
      return DIFF_YES;
    }
  return DIFF_NO;
}


/* a click in the diff at screen mx, my: a fold opens, an arrow reverts, else the cursor goes there */
int diff_click (int mx, int my) {
  size_t i, k, seg;
  int row = my - D.y;
  if (!D.open || row < 0 || row >= D.h) return DIFF_NO;
  if (D.mmw > 0 && mx >= D.x + D.w - D.sbw - D.mmw && mx < D.x + D.w - D.sbw) {	/* its minimap: the rows there */
    size_t n = vcount(), per = (n + (size_t)D.h - 1) / (size_t)D.h, want;
    if (per == 0) per = 1;
    want = (size_t)row * per;
    if (want > last_top()) want = last_top();
    D.top = n ? vk(want) : 0;
    return DIFF_YES;
  }
  if (row >= D.nsr) return DIFF_YES;	/* under the end */
  i = D.sri[row];
  seg = D.srs[row];
  k = vk(i);
  if (D.nvis && D.vis[i].hidden) {	/* the fold opens */
    if (D.exp && k < D.nexp) D.exp[k] = 1;
    vis_build();
    return DIFF_YES;
  }
  D.crow = k;
  if (diff_editable()) {	/* the caret where it was clicked, on the new side */
    int nw = 2, cx;
    size_t m = D.nmax;
    while (m >= 10) {
      m /= 10;
      nw++;
    }
    nw++;
    cx = mx - (D.split_now ? D.x + D.hw + 1 + nw + 2 : D.x + 2 * nw + 2);
    if (D.wrap) {	/* the row of the line clicked: from where it starts */
      size_t st[DSEG], ns = dsegs(caret_line(), caret_len(), D.tcr, st);
      if (seg >= ns) D.ccol = caret_len();
      else {
        size_t c0 = dcol(caret_line(), caret_len(), st[seg]), e = seg + 1 < ns ? st[seg + 1] : caret_len();
        D.ccol = dbyte(caret_line(), caret_len(), c0 + (size_t)(cx > 0 ? cx : 0));
        if (seg + 1 < ns && D.ccol >= e) D.ccol = prev_byte(caret_line(), e);	/* past the row's end: its last character */
      }
    }
    else D.ccol = cx > 0 ? dbyte(caret_line(), caret_len(), D.left + (size_t)cx) : 0;
    if (D.ccol > caret_len()) D.ccol = caret_len();
  }
  if (is_change(k)) D.cur = k;
  if (D.kind == DK_TREE && seg == 0 && block_start(k) && mx == D.x + (D.split_now ? D.hw : D.arrow_x)) return DIFF_REVERT;
  return DIFF_YES;
}


int diff_key (int k) {
  int code = KEY_CODE(k);
  size_t h = D.h > 0 ? (size_t)D.h : 1;
  switch (code) {
    case K_UP:
      crow_move(-1);
      if (D.ccol > caret_len()) D.ccol = caret_len();
      break;
    case K_DOWN:
      crow_move(1);
      if (D.ccol > caret_len()) D.ccol = caret_len();
      break;
    case K_PGUP: crow_move(-(long)h); break;
    case K_PGDN: crow_move((long)h); break;
    case K_HOME:
      D.left = 0;
      if (diff_editable()) D.ccol = 0;
      else crow_move(-(long)vcount());
      break;
    case K_END:
      if (diff_editable()) D.ccol = caret_len();
      else crow_move((long)vcount());
      break;
    case K_LEFT:
      if (!diff_editable()) D.left = D.left > 4 ? D.left - 4 : 0;
      else if (D.ccol > 0) D.ccol = prev_byte(caret_line(), D.ccol);
      else crow_move(-1), D.ccol = caret_len();
      break;
    case K_RIGHT:
      if (!diff_editable()) D.left += 4;
      else if (D.ccol < caret_len()) D.ccol = next_byte(caret_line(), D.ccol);
      else {
        crow_move(1);
        D.ccol = 0;
      }
      break;
    case K_F7: diff_change((k & KM_SHIFT) != 0); break;
    case 'i':
      if (diff_editable() && !(k & KM_ALT)) return DIFF_NO;	/* it is typed there: Alt+I toggles instead */
      D.inline_mode = !D.inline_mode;
      break;
    case K_ESC: return DIFF_CLOSE;
    case 'o':
      if (diff_editable() && !(k & KM_ALT)) return DIFF_NO;	/* Alt+O opens the file instead */
      /* fall through */
    case K_ENTER: {
      size_t i = vpos(D.crow);
      if (D.nvis && i < D.nvis && D.vis[i].hidden && D.vis[i].k == D.crow) {	/* on a fold: it opens */
        if (D.exp && D.crow < D.nexp) D.exp[D.crow] = 1;
        vis_build();
        break;
      }
      return DIFF_EDIT;
    }
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
    if (!nw && l->kind == ' ' && l->os) buf_putn(&b, l->os, l->olen);	/* its old spaces kept */
    else buf_putn(&b, l->s, l->len);
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


void diff_wheel (int d, int mods) {
  size_t i = vpos(D.top), n = vcount(), st = (size_t)wheel_step(mods & ~KM_SHIFT), lt = last_top();
  if ((mods & KM_SHIFT) && !D.wrap) {	/* Shift+wheel: to the side, like VS Code (diff_draw keeps it in) */
    st *= 2;
    D.left = d < 0 ? (D.left > st ? D.left - st : 0) : D.left + st;
    return;
  }
  if (d < 0) i = i > st ? i - st : 0;
  else i = i + st < lt ? i + st : lt;
  D.top = n ? vk(i) : 0;
}

/* }================================================================== */
