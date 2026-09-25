/*
** eside.c - the activity bar, the sidebar, and its first view: the
** Explorer, the folder tree like VS Code's
**
** A folder is read the first time it is opened, and read again (keeping
** what was open inside it) on F5 or when it is opened once more. Folders
** come first, then files, both sorted without regard to case; .git and
** friends are left out like VS Code's files.exclude does. The rows shown
** are the tree flattened: every node whose parents are all open.
** The icons are codicons and Seti's, from a Nerd Font.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*
** {==================================================================
** The activity bar
** ===================================================================
*/

static const uint32_t act_icon[VIEW_N] = {
  0xEAF0,	/* codicon files */
  0xEA6D,	/* search */
  0xEA68,	/* source-control */
  0xEB91,	/* debug-alt: Run and Debug */
  0xEAE6,	/* extensions */
  0xEA79	/* beaker: Testing */
};


void act_draw (int x, int y, int h, int view, int shown) {
  int v, i;
  for (i = 0; i < h; i++) scr_fill(x, y + i, ACT_W, S_ACT);
  for (v = 0; v < VIEW_N; v++) {
    int row = y + 1 + v * 2, on = shown && v == view;
    if (row >= y + h) break;
    if (on) scr_put(opt.side_right ? x + ACT_W - 1 : x, row, 0x258E, S_ACT_BAR);	/* the bar on the side of the editor */
    scr_put(x + 1, row, act_icon[v], on ? S_ACT_ON : S_ACT);
    if (row + 1 < y + h) {	/* VS Code's badge: a number at the icon's foot, white on blue */
      int dot = 0, n = act_badge(v, &dot);
      if (n > 0) {
        char b[8];
        int k;
        snprintf(b, sizeof(b), n > 99 ? "99" : "%d", n);
        for (k = 0; b[k]; k++) scr_put_rgb(x + 2 + k, row + 1, (unsigned char)b[k], 0xFFFFFF, 0x0078D4, RGB_BOLD);
      }
      else if (dot) {
        scr_put(x + 2, row + 1, 0x25CF, S_ACT);	/* ● */
        scr_set_fg(x + 2, row + 1, 0x0078D4);
      }
    }
  }
  if ((v = act_manage_row(y, h)) >= 0) scr_put(x + 1, v, 0xEAF8, S_ACT);	/* Manage: codicon settings-gear */
}


/* the Manage gear at the foot of the activity bar (the Accounts slot above it stays empty) */
int act_manage_row (int y, int h) {
  return h >= VIEW_N * 2 + 5 ? y + h - 2 : -1;
}


int act_hit (int y) {
  int v = (y - 1) / 2;
  if (y < 1 || v >= VIEW_N) return -1;
  return v;
}

/* }================================================================== */


/*
** {==================================================================
** The views, one after the other
** ===================================================================
*/

void side_draw (int view, int x, int y, int w, int h, int focus, const char *active) {
  if (view == VIEW_SEARCH) search_draw(x, y, w, h, focus);
  else if (view == VIEW_GIT) git_draw(x, y, w, h, focus);
  else if (view == VIEW_DEBUG) debug_draw(x, y, w, h, focus);
  else if (view == VIEW_EXT) ext_draw(x, y, w, h, focus);
  else if (view == VIEW_TEST) test_draw(x, y, w, h, focus);
  else files_draw(x, y, w, h, focus, active);
}


int side_key (int view, int k, SideAct *act) {
  act->what = SA_NONE;
  if (view == VIEW_SEARCH) return search_key(k, act);
  if (view == VIEW_GIT) return git_key(k, act);
  if (view == VIEW_DEBUG) return debug_key(k, act);
  if (view == VIEW_EXT) return ext_key(k, act);
  if (view == VIEW_TEST) return test_key(k, act);
  return files_key(k, act);
}


void side_click (int view, int row, int col, SideAct *act) {
  act->what = SA_NONE;
  if (view == VIEW_SEARCH) search_click(row, col, act);
  else if (view == VIEW_GIT) git_click(row, col, act);
  else if (view == VIEW_DEBUG) debug_click(row, col, act);
  else if (view == VIEW_EXT) ext_click(row, col, act);
  else if (view == VIEW_TEST) test_click(row, col, act);
  else files_click(row, col, act);
}


/*
** VS Code's slim scrollbar at the right edge of a pane: the thumb over the
** rows shown, of a list of 'total' rows starting at 'top'. Nothing is drawn
** when everything fits.
*/
void side_bar (int x, int y, int w, int h, size_t total, size_t top, size_t shown) {
  int len, at;
  if (h < 2 || shown == 0 || total <= shown) return;
  len = (int)((double)shown * h / (double)total + 0.5);
  if (len < 1) len = 1;
  if (len > h - 1) len = h - 1;
  at = (int)((double)top * (h - len) / (double)(total - shown) + 0.5);
  if (at < 0) at = 0;
  if (at > h - len) at = h - len;
  for (len += at; at < len; at++)	/* a half block, so it shows on any background */
    scr_put_rgb(x + w - 1, y + at, 0x2590, ui_color(C_THUMB), ui_color(C_SIDE_BG), 0);
}


void side_wheel (int view, int d) {
  if (view == VIEW_SEARCH) search_wheel(d);
  else if (view == VIEW_GIT) git_wheel(d);
  else if (view == VIEW_DEBUG) debug_wheel(d);
  else if (view == VIEW_EXT) ext_wheel(d);
  else if (view == VIEW_TEST) test_wheel(d);
  else files_wheel(d);
}


int side_idle (int view) {
  int r = ext_idle() | test_idle() | chat_idle();	/* downloads, test runs and Claude's answers go on whatever is shown */
  return (view == VIEW_SEARCH || search_busy() ? search_idle() : 0) | r;	/* a search goes on hidden too */
}

/* }================================================================== */


/*
** {==================================================================
** The Explorer
** ===================================================================
*/

typedef struct Node {
  char *name;
  char *path;	/* native */
  int dir, open, loaded, depth;	/* a file's open: its nested files shown the other way (fileNesting) */
  time_t mtime;
  struct Node *kid;
  size_t nkid;
} Node;

static Node g_root;	/* the folder; in a workspace its kids are the folders (g_ws) */
static int g_ws;
static Node **g_vis;	/* the rows: a compacted row ("a/b/c") is its last folder */
static Node **g_vhead;	/* the row's first folder ("a" of "a/b/c"), else the row's node */
static int *g_vdepth;	/* the row's indent */
static int *g_vnest;	/* a file's row: how many files are nested under it; -1: it is nested */
static size_t g_nvis, g_capvis;
static size_t g_sel, g_top;
static int g_bar_y;	/* the first row of the tree on the screen */
static int g_h = 1;	/* rows of the tree the last time it was drawn */
static int g_gap;	/* rows under "EXPLORER" given to OPEN EDITORS (mme.c draws it) */
static int g_sorted = -1;	/* the explorer.sortOrder the loaded folders are sorted by */

#define HEAD	(2 + g_gap)	/* "EXPLORER", OPEN EDITORS, the folder's name */

static char *g_keep;	/* the selection's path, while the rows are being made again */

static Vec g_msel;	/* the rows selected with Ctrl / Shift (paths); empty: only g_sel */
static char *g_anchor;	/* where Shift+click / Shift+Down extends from */
static int g_mods;	/* the mouse's KM_* for the next files_click */
static Vec g_drag;	/* what is dragged */
static long g_drop = -1;	/* the row of the folder a drop goes into; -2 the root; -1 none */
static char g_type[64];	/* what was typed to find a name (type navigation) */
static long long g_type_at;


/* p against s: * not over a separator, ** over anything, ? one, {a,b}; / and \ are the same */
static int fglob (const char *p, const char *s) {
  while (*p) {
    if (p[0] == '*' && p[1] == '*') {
      p += 2;
      if (*p == '/' || *p == '\\') p++;	/* "**" "/": no folder too */
      if (*p == '\0') return 1;
      for (;; s++) {
        if (fglob(p, s)) return 1;
        if (*s == '\0') return 0;
      }
    }
    if (*p == '*') {
      p++;
      for (;; s++) {
        if (fglob(p, s)) return 1;
        if (*s == '\0' || *s == '/' || *s == '\\') return 0;
      }
    }
    if (*p == '{') {	/* {a,b}rest */
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
          if (fglob(buf, s)) return 1;
        }
        a = c + 1;
      }
      return 0;
    }
    if (*s == '\0') return 0;
    if (*p == '?') {
      if (*s == '/' || *s == '\\') return 0;
    }
    else if ((*p == '/' || *p == '\\') ? !(*s == '/' || *s == '\\') :
             (*p >= 'A' && *p <= 'Z' ? *p + 32 : *p) != (*s >= 'A' && *s <= 'Z' ? *s + 32 : *s))
      return 0;
    p++;
    s++;
  }
  return *s == '\0';
}


/* files.exclude: its patterns set to true, or VS Code's defaults when there is no such setting */
static int exclude_match (const char *rel) {
  static const char *const def[] = {"**/.git", "**/.svn", "**/.hg", "**/CVS", "**/.DS_Store", "**/Thumbs.db"};
  const Json *j = settings_value("files.exclude");
  size_t i;
  if (j && j->type == J_OBJ) {
    for (i = 0; i < j->n; i++)
      if (j->kid[i]->type == J_BOOL && j->kid[i]->b && j->kid[i]->key && fglob(j->kid[i]->key, rel)) return 1;
    return 0;
  }
  for (i = 0; i < sizeof(def) / sizeof(def[0]); i++)
    if (fglob(def[i], rel)) return 1;
  return 0;
}


/*
** files.exclude as one comma separated list of globs, for Search's
** workers: they may not read the settings while the editor runs
*/
char *files_exclude_list (void) {
  static const char *const def[] = {"**/.git", "**/.svn", "**/.hg", "**/CVS", "**/.DS_Store", "**/Thumbs.db"};
  const Json *j = settings_value("files.exclude");
  Buf b;
  size_t i;
  buf_init(&b);
  if (j && j->type == J_OBJ) {
    for (i = 0; i < j->n; i++)
      if (j->kid[i]->type == J_BOOL && j->kid[i]->b && j->kid[i]->key && strchr(j->kid[i]->key, ',') == NULL) {
        if (b.len) buf_putc(&b, ',');
        buf_puts(&b, j->kid[i]->key);
      }
  }
  else
    for (i = 0; i < sizeof(def) / sizeof(def[0]); i++) {
      if (b.len) buf_putc(&b, ',');
      buf_puts(&b, def[i]);
    }
  buf_putc(&b, '\0');
  return buf_take(&b);
}


/* is rel (from the folder) hidden by files.exclude, it or a folder it is in? For the tree, Go to File, Search */
int files_excluded (const char *rel) {
  char buf[1024];
  size_t i, n = strlen(rel);
  if (n == 0 || n >= sizeof(buf)) return 0;
  memcpy(buf, rel, n + 1);
  for (i = 1; i <= n; i++)
    if (buf[i] == '/' || buf[i] == '\\' || buf[i] == '\0') {
      char c = buf[i];
      buf[i] = '\0';
      if (exclude_match(buf)) return 1;
      buf[i] = c;
    }
  return 0;
}


/* path from its folder (the workspace folder it is in), for files.exclude */
static const char *rel_of (const char *path) {
  size_t i, n;
  if (g_ws)
    for (i = 0; i < g_root.nkid; i++) {
      n = strlen(g_root.kid[i].path);
      if (m_fnncmp(path, g_root.kid[i].path, n) == 0 && path_is_sep(path[n])) return path + n + 1;
    }
  if (g_root.path) {
    n = strlen(g_root.path);
    if (m_fnncmp(path, g_root.path, n) == 0 && path_is_sep(path[n])) return path + n + 1;
  }
  return path_basename(path);
}


static void node_free (Node *n) {
  size_t i;
  for (i = 0; i < n->nkid; i++) node_free(&n->kid[i]);
  free(n->kid);
  free(n->name);
  free(n->path);
  memset(n, 0, sizeof(*n));
}


/* explorer.sortOrder: default (folders first), mixed, filesFirst, type (by extension), modified (newest first) */
static int cmp_node (const void *a, const void *b) {
  const Node *x = (const Node *)a, *y = (const Node *)b;
  int r;
  if (opt.exp_sort != EXS_MIXED && x->dir != y->dir)
    return opt.exp_sort == EXS_FILES_FIRST ? x->dir - y->dir : y->dir - x->dir;
  if (opt.exp_sort == EXS_TYPE && !x->dir && !y->dir) {
    const char *ex = strrchr(x->name, '.'), *ey = strrchr(y->name, '.');
    r = m_stricmp(ex ? ex : "", ey ? ey : "");
    if (r) return r;
  }
  if (opt.exp_sort == EXS_MODIFIED && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
  r = m_stricmp(x->name, y->name);
  return r ? r : strcmp(x->name, y->name);
}


/* the folders read so far sorted again (explorer.sortOrder changed) */
static void resort (Node *n) {
  size_t i;
  if (n->nkid && !(n == &g_root && g_ws)) qsort(n->kid, n->nkid, sizeof(Node), cmp_node);
  for (i = 0; i < n->nkid; i++)
    if (n->kid[i].dir) resort(&n->kid[i]);
}


static void load_kids (Node *n);

/* reads the folder n; what was open in it stays open */
static void load (Node *n) {
  if (g_keep == NULL && g_sel < g_nvis) g_keep = xstrdup(g_vis[g_sel]->path);	/* the rows point into n */
  g_nvis = 0;
  load_kids(n);
}


/* load without the rows: they are not touched (compact folders read what they need while the rows are made) */
static void load_kids (Node *n) {
  Vec v;
  size_t i, j;
  Node *kid;
  if (n == &g_root && g_ws) {	/* a workspace: its folders stay, what they have is read again */
    for (i = 0; i < n->nkid; i++)
      if (n->kid[i].open) load_kids(&n->kid[i]);
    return;
  }
  vec_init(&v);
  os_listdir(n->path, &v);
  kid = (Node *)xmalloc((v.n + 1) * sizeof(Node));
  j = 0;
  for (i = 0; i < v.n; i++) {
    Node *k = &kid[j];
    OsStat st;
    char *path = path_join(n->path, v.v[i]);
    if (files_excluded(rel_of(path))) {	/* files.exclude */
      free(path);
      continue;
    }
    memset(k, 0, sizeof(*k));
    k->name = xstrdup(v.v[i]);
    k->path = path;
    k->dir = os_stat(k->path, &st) == 0 && st.is_dir;
    k->mtime = st.mtime;
    k->depth = n->depth + 1;
    j++;
  }
  vec_free(&v);
  for (i = 0; i < j; i++) {	/* take over the old node's inside */
    size_t o;
    for (o = 0; o < n->nkid; o++) {
      Node *old = &n->kid[o];
      if (!kid[i].dir && !old->dir && strcmp(old->name, kid[i].name) == 0) {	/* a file: its nested files' state */
        kid[i].open = old->open;
        break;
      }
      if (kid[i].dir && old->dir && strcmp(old->name, kid[i].name) == 0) {
        kid[i].open = old->open;
        kid[i].loaded = old->loaded;
        kid[i].kid = old->kid;
        kid[i].nkid = old->nkid;
        old->kid = NULL;
        old->nkid = 0;
        break;
      }
    }
  }
  for (i = 0; i < n->nkid; i++) node_free(&n->kid[i]);
  free(n->kid);
  qsort(kid, j, sizeof(Node), cmp_node);
  n->kid = kid;
  n->nkid = j;
  n->loaded = 1;
}


static void vis_push (Node *head, Node *n, int depth, int nest) {
  if (g_nvis == g_capvis) {
    g_capvis = g_capvis ? g_capvis * 2 : 256;
    g_vis = (Node **)xrealloc(g_vis, g_capvis * sizeof(Node *));
    g_vhead = (Node **)xrealloc(g_vhead, g_capvis * sizeof(Node *));
    g_vdepth = (int *)xrealloc(g_vdepth, g_capvis * sizeof(int));
    g_vnest = (int *)xrealloc(g_vnest, g_capvis * sizeof(int));
  }
  g_vis[g_nvis] = n;
  g_vhead[g_nvis] = head;
  g_vdepth[g_nvis] = depth;
  g_vnest[g_nvis] = nest;
  g_nvis++;
}


/* does a key of explorer.fileNesting.patterns ("*.ts", "package.json") match name? *cap: the "*" part */
static int nest_key (const char *key, const char *name, char *cap, size_t capn) {
  size_t kl = strlen(key), nl = strlen(name);
  if (key[0] == '*' && strchr(key + 1, '*') == NULL) {
    if (nl < kl - 1 || m_stricmp(name + nl - (kl - 1), key + 1) != 0) return 0;
    snprintf(cap, capn, "%.*s", (int)(nl - (kl - 1)), name);
    return 1;
  }
  snprintf(cap, capn, "%s", name);
  return m_stricmp(key, name) == 0;
}


/* one value of the patterns ("${capture}.js, ${capture}.min.js") against name */
static int nest_value (const char *val, const char *cap, const char *name) {
  const char *p = val;
  while (*p) {
    char pat[256];
    size_t n = 0;
    while (*p == ',' || *p == ' ') p++;
    while (*p && *p != ',' && n + 1 < sizeof(pat)) {
      if (strncmp(p, "${capture}", 10) == 0) {
        n += (size_t)snprintf(pat + n, sizeof(pat) - n, "%s", cap);
        if (n >= sizeof(pat)) n = sizeof(pat) - 1;
        p += 10;
      }
      else pat[n++] = *p++;
    }
    while (n > 0 && pat[n - 1] == ' ') n--;
    pat[n] = '\0';
    if (n && fglob(pat, name)) return 1;
  }
  return 0;
}


/* explorer.fileNesting: is file b nested under file a? (VS Code's default patterns, or the setting's) */
static int nested_under (const char *a, const char *b) {
  static const char *const def[][2] = {
    {"*.ts", "${capture}.js"}, {"*.js", "${capture}.js.map, ${capture}.min.js, ${capture}.d.ts"},
    {"*.jsx", "${capture}.js"}, {"*.tsx", "${capture}.ts"}, {"tsconfig.json", "tsconfig.*.json"},
    {"package.json", "package-lock.json, yarn.lock, pnpm-lock.yaml, bun.lockb"}
  };
  const Json *j = settings_value("explorer.fileNesting.patterns");
  char cap[256];
  size_t i;
  if (m_fncmp(a, b) == 0) return 0;
  if (j && j->type == J_OBJ) {
    for (i = 0; i < j->n; i++)
      if (j->kid[i]->key && j->kid[i]->type == J_STR && nest_key(j->kid[i]->key, a, cap, sizeof(cap)) &&
          nest_value(j->kid[i]->str, cap, b))
        return 1;
    return 0;
  }
  for (i = 0; i < sizeof(def) / sizeof(def[0]); i++)
    if (nest_key(def[i][0], a, cap, sizeof(cap)) && nest_value(def[i][1], cap, b)) return 1;
  return 0;
}


/* the rows of n's kids at indent depth: compact folders ("a/b/c"), nested files */
static void vis_add (Node *n, int depth) {
  size_t i, j;
  long *par = NULL;
  int *cnt = NULL;
  if (opt.exp_nesting && n->nkid > 1 && n->nkid < 2000) {	/* which file each one is nested under */
    par = (long *)xmalloc(n->nkid * sizeof(long));
    cnt = (int *)calloc(n->nkid, sizeof(int));
    for (i = 0; i < n->nkid; i++) {
      par[i] = -1;
      if (n->kid[i].dir) continue;
      for (j = 0; j < n->nkid && par[i] < 0; j++)
        if (j != i && !n->kid[j].dir && nested_under(n->kid[j].name, n->kid[i].name)) par[i] = (long)j;
    }
    for (i = 0; i < n->nkid; i++)	/* one level: a nested file has none under it */
      if (par[i] >= 0 && par[par[i]] < 0 && cnt) cnt[par[i]]++;
      else if (par[i] >= 0) par[i] = -1;
  }
  for (i = 0; i < n->nkid; i++) {
    Node *k = &n->kid[i], *last = k;
    if (par && par[i] >= 0) continue;
    if (!k->dir) {
      int c = cnt ? cnt[i] : 0;
      vis_push(k, k, depth, c);
      if (c && (opt.exp_nest_expand ? !k->open : k->open))
        for (j = 0; j < n->nkid; j++)
          if (par[j] == (long)i) vis_push(&n->kid[j], &n->kid[j], depth + 1, -1);
      continue;
    }
    if (opt.exp_compact && !(n == &g_root && g_ws))	/* a chain of folders with one folder in each: one row */
      for (;;) {
        if (!last->loaded) load_kids(last);
        if (last->nkid != 1 || !last->kid[0].dir) break;
        last = &last->kid[0];
      }
    vis_push(k, last, depth, 0);
    if (last->open) vis_add(last, depth + 1);
  }
  free(par);
  free(cnt);
}


/* the rows again, the same one selected when it is still there */
static void flatten (void) {
  char *keep = g_keep ? g_keep : (g_sel < g_nvis) ? xstrdup(g_vis[g_sel]->path) : NULL;
  size_t i;
  g_keep = NULL;
  g_nvis = 0;
  if (g_sorted != opt.exp_sort) {
    g_sorted = opt.exp_sort;
    resort(&g_root);
  }
  vis_add(&g_root, 0);
  if (keep) {
    for (i = 0; i < g_nvis; i++)
      if (strcmp(g_vis[i]->path, keep) == 0) {
        g_sel = i;
        break;
      }
    free(keep);
  }
  if (g_sel >= g_nvis) g_sel = g_nvis ? g_nvis - 1 : 0;
}


static void set_open (Node *n, int open) {
  if (!n->dir) return;
  if (open) load(n);	/* opening shows what is there now */
  n->open = open;
  flatten();
}


/* a file row with nested files: shown or not */
static void set_nest (size_t k, int open) {
  Node *n = g_vis[k];
  if (n->dir || g_vnest[k] <= 0) return;
  n->open = opt.exp_nest_expand ? !open : open;
  flatten();
}


static int nest_open (size_t k) {
  return g_vnest[k] > 0 && (opt.exp_nest_expand ? !g_vis[k]->open : g_vis[k]->open);
}


void side_open (const char *native) {
  char *real = os_realpath(native);
  g_ws = 0;
  g_nvis = 0;	/* the rows point into the tree that goes */
  node_free(&g_root);
  g_root.path = real ? real : xstrdup(native);
  g_root.name = xstrdup(path_basename(g_root.path));
  g_root.dir = g_root.open = 1;
  g_root.depth = -1;
  load(&g_root);
  g_sel = g_top = 0;
  flatten();
}


/* a workspace's folders as the roots (names[i] "": the folder's own); title for the head */
void side_open_roots (char *const *paths, char *const *names, int n, const char *title) {
  int i;
  g_nvis = 0;	/* the rows point into the tree that goes */
  node_free(&g_root);
  g_ws = 1;
  g_root.path = xstrdup(paths[0]);	/* side_root: the first folder (git, terminals ...) */
  g_root.name = xstrdup(title);
  g_root.dir = g_root.open = g_root.loaded = 1;
  g_root.depth = -1;
  g_root.kid = (Node *)xmalloc((size_t)(n + 1) * sizeof(Node));
  g_root.nkid = (size_t)n;
  for (i = 0; i < n; i++) {
    Node *k = &g_root.kid[i];
    memset(k, 0, sizeof(*k));
    k->path = xstrdup(paths[i]);
    k->name = xstrdup(names[i] && names[i][0] ? names[i] : path_basename(paths[i]));
    k->dir = 1;
    k->open = n == 1 || i == 0;	/* the first one open, like VS Code at first */
    k->depth = 0;
    if (k->open) load(k);
  }
  g_sel = g_top = 0;
  flatten();
}


const char *side_root (void) {
  return g_root.path ? g_root.path : ".";
}


void side_refresh (void) {
  size_t i;
  if (g_root.path == NULL) return;
  load(&g_root);
  flatten();
  for (i = 0; i < g_nvis; i++)	/* the open folders too: from the top down */
    if (g_vis[i]->dir && g_vis[i]->open) {
      load(g_vis[i]);
      flatten();
    }
}


static void keep_in_view (void) {
  size_t h = (size_t)g_h;
  if (g_sel < g_top) g_top = g_sel;
  if (g_sel >= g_top + h) g_top = g_sel - h + 1;
  if (g_nvis <= h) g_top = 0;
  else if (g_top > g_nvis - h) g_top = g_nvis - h;
}


void side_reveal (const char *real);

/* the file in front shown in the tree, when explorer.autoReveal says so */
void side_follow (const char *real) {
  if (opt.exp_reveal) side_reveal(real);
}


/* opens the folders down to the file 'real' (a realpath) and selects it */
void side_reveal (const char *real) {
  size_t n, i;
  Node *at = &g_root;
  const char *p;
  if (real == NULL || g_root.path == NULL) return;
  if (g_ws) {	/* the folder of the workspace it is in */
    for (i = 0; i < g_root.nkid; i++) {
      n = strlen(g_root.kid[i].path);
      if (m_fnncmp(real, g_root.kid[i].path, n) == 0 && path_is_sep(real[n])) break;
    }
    if (i == g_root.nkid) return;
    at = &g_root.kid[i];
    at->open = 1;
    n = strlen(at->path);
  }
  else {
    n = strlen(g_root.path);
    if (m_fnncmp(real, g_root.path, n) != 0 || !path_is_sep(real[n])) return;
  }
  p = real + n;
  while (*p) {
    const char *e;
    Node *next = NULL;
    while (path_is_sep(*p)) p++;
    for (e = p; *e && !path_is_sep(*e); e++) ;
    if (e == p) break;
    if (!at->loaded) load(at);
    at->open = 1;
    for (i = 0; i < at->nkid; i++)
      if (strlen(at->kid[i].name) == (size_t)(e - p) &&
          m_fnncmp(at->kid[i].name, p, (size_t)(e - p)) == 0) {
        next = &at->kid[i];
        break;
      }
    if (next == NULL) break;
    at = next;
    p = e;
  }
  flatten();
  for (i = 0; i < g_nvis; i++)
    if (g_vis[i] == at) g_sel = i;
  keep_in_view();
}


/* Seti's icon and color for a file name */
uint32_t file_icon (const char *name, int *st) {
  static const struct {
    const char *ext;
    uint32_t icon;
    int st;
  } icons[] = {
    {".c", 0xE61E, S_ICON_BLUE}, {".h", 0xE61E, S_ICON_PURPLE},
    {".cpp", 0xE61D, S_ICON_BLUE}, {".cc", 0xE61D, S_ICON_BLUE},
    {".hpp", 0xE61D, S_ICON_PURPLE}, {".go", 0xE627, S_ICON_BLUE},
    {".js", 0xE60C, S_ICON_YELLOW}, {".mjs", 0xE60C, S_ICON_YELLOW},
    {".ts", 0xE628, S_ICON_BLUE}, {".tsx", 0xE7BA, S_ICON_BLUE},
    {".jsx", 0xE7BA, S_ICON_BLUE}, {".json", 0xE60B, S_ICON_YELLOW},
    {".md", 0xE609, S_ICON_BLUE}, {".py", 0xE606, S_ICON_YELLOW},
    {".rs", 0xE7A8, S_ICON_ORANGE}, {".zig", 0xE6A9, S_ICON_ORANGE},
    {".java", 0xE738, S_ICON_RED}, {".lua", 0xE620, S_ICON_BLUE},
    {".sh", 0xE795, S_ICON_GREEN}, {".bash", 0xE795, S_ICON_GREEN},
    {".bat", 0xE629, S_ICON_GREY}, {".cmd", 0xE629, S_ICON_GREY},
    {".ps1", 0xE795, S_ICON_BLUE}, {".html", 0xE60E, S_ICON_ORANGE},
    {".css", 0xE614, S_ICON_BLUE}, {".xml", 0xE619, S_ICON_ORANGE},
    {".yml", 0xE6A8, S_ICON_PURPLE}, {".yaml", 0xE6A8, S_ICON_PURPLE},
    {".toml", 0xE6B2, S_ICON_GREY}, {".txt", 0xF15C, S_ICON_GREY},
    {".png", 0xE60D, S_ICON_PURPLE}, {".jpg", 0xE60D, S_ICON_PURPLE},
    {".ico", 0xE60D, S_ICON_YELLOW}, {".svg", 0xE698, S_ICON_YELLOW},
    {".ttf", 0xE659, S_ICON_RED}, {".otf", 0xE659, S_ICON_RED},
    {".sql", 0xE706, S_ICON_PURPLE}, {".dart", 0xE798, S_ICON_BLUE},
    {".kt", 0xE634, S_ICON_ORANGE}, {".php", 0xE608, S_ICON_PURPLE},
    {".rb", 0xE791, S_ICON_RED}, {".exe", 0xEAE8, S_ICON_GREY}
  };
  const char *dot = strrchr(name, '.');
  size_t i;
  if (m_stricmp(name, "makefile") == 0) {
    *st = S_ICON_ORANGE;
    return 0xE779;
  }
  if (m_strnicmp(name, "license", 7) == 0 || m_strnicmp(name, "readme", 6) == 0) {
    *st = name[0] == 'r' || name[0] == 'R' ? S_ICON_BLUE : S_ICON_YELLOW;
    return name[0] == 'r' || name[0] == 'R' ? 0xE609 : 0xE60A;
  }
  if (name[0] == '.' && strncmp(name, ".git", 4) == 0) {
    *st = S_ICON_ORANGE;
    return 0xE702;
  }
  if (dot)
    for (i = 0; i < sizeof(icons) / sizeof(icons[0]); i++)
      if (m_stricmp(dot, icons[i].ext) == 0) {
        *st = icons[i].st;
        return icons[i].icon;
      }
  *st = S_ICON_GREY;
  return 0xEA7B;	/* codicon file */
}


static int git_style (int mark) {
  switch (mark) {
    case 'M': return S_GIT_M;
    case 'A': return S_GIT_A;
    case 'D': return S_GIT_D;
    case 'U': return S_GIT_U;
  }
  return 0;
}


/*
** The box a name is typed into, in the tree, like VS Code's: New File...,
** New Folder... (at the top of the folder), Rename (in place of the name).
*/
enum { IN_FILE, IN_FOLDER, IN_RENAME };

static struct {
  int on, kind;
  char *dir;	/* new: the folder it goes in; rename: the path renamed */
  size_t row;	/* the row of the tree it shows on */
  int depth;
  char text[256];
  size_t cur;	/* the cursor, a byte in text */
  size_t sel;	/* text[0 .. sel) is selected (Rename: the name without its extension) */
} g_in;

/* what Copy / Cut took, for Paste */
static Vec g_fclip;
static int g_fcut;

/* the paths an action points to: they live until the next one */
static char g_ap[4096], g_ap2[4096];

/* the folder row's icons, from the sidebar's left: New File, New Folder, Refresh, Collapse */
static int g_icon_x[4] = {-1, -1, -1, -1};


/* is path one of the rows selected with Ctrl / Shift? */
static int msel_has (const char *path) {
  size_t i;
  for (i = 0; i < g_msel.n; i++)
    if (strcmp(g_msel.v[i], path) == 0) return 1;
  return 0;
}


static void msel_clear (void) {
  vec_free(&g_msel);
  vec_init(&g_msel);
}


static void msel_toggle (const char *path) {
  size_t i;
  for (i = 0; i < g_msel.n; i++)
    if (strcmp(g_msel.v[i], path) == 0) {
      free(g_msel.v[i]);
      memmove(g_msel.v + i, g_msel.v + i + 1, (g_msel.n - i - 1) * sizeof(char *));
      g_msel.n--;
      return;
    }
  vec_push(&g_msel, xstrdup(path));
}


/* the rows from the anchor to k selected (Shift) */
static void msel_range (size_t k) {
  size_t a = k, i, lo, hi;
  if (g_anchor)
    for (i = 0; i < g_nvis; i++)
      if (strcmp(g_vis[i]->path, g_anchor) == 0) a = i;
  msel_clear();
  lo = a < k ? a : k;
  hi = a < k ? k : a;
  for (i = lo; i <= hi && i < g_nvis; i++) vec_push(&g_msel, xstrdup(g_vis[i]->path));
}


static void set_anchor (size_t k) {
  free(g_anchor);
  g_anchor = k < g_nvis ? xstrdup(g_vis[k]->path) : NULL;
}


/* is a inside the folder b (or b itself)? */
static int inside (const char *a, const char *b) {
  size_t n = strlen(b);
  return m_fnncmp(a, b, n) == 0 && (a[n] == '\0' || path_is_sep(a[n]));
}


/*
** What an action is on: the rows selected when the one the keys are on is
** one of them (like VS Code), else that one; what is inside another one
** there is left out (the folder takes it along).
*/
static void targets (Vec *out) {
  size_t i, j;
  vec_init(out);
  if (g_nvis == 0) return;
  if (g_msel.n && msel_has(g_vis[g_sel]->path)) {
    for (i = 0; i < g_msel.n; i++) {
      int in = 0;
      for (j = 0; j < g_msel.n && !in; j++)
        in = j != i && inside(g_msel.v[i], g_msel.v[j]) && strcmp(g_msel.v[i], g_msel.v[j]) != 0;
      if (!in) vec_push(out, xstrdup(g_msel.v[i]));
    }
  }
  else vec_push(out, xstrdup(g_vis[g_sel]->path));
}


/* the diagnostics' files, counted: for the tree's problem colors and badges */
typedef struct PCount {
  char *path;
  int e, w;
} PCount;

static PCount *g_pc;
static size_t g_npc;


static void problems_count (void) {
  size_t i, j, n;
  for (i = 0; i < g_npc; i++) free(g_pc[i].path);
  free(g_pc);
  g_pc = NULL;
  g_npc = 0;
  if (!opt.exp_dec_colors && !opt.exp_dec_badges) return;
  n = lsp_diag_files();
  if (n == 0) return;
  g_pc = (PCount *)xmalloc(n * sizeof(PCount));
  for (i = 0; i < n; i++) {
    char *path;
    size_t nd;
    const Diag *d = lsp_diag_file(i, &path, &nd);
    PCount *c = &g_pc[g_npc];
    c->e = c->w = 0;
    for (j = 0; j < nd; j++) {
      if (d[j].sev == 1) c->e++;
      else if (d[j].sev == 2) c->w++;
    }
    if (c->e + c->w == 0 || path == NULL) {
      free(path);
      continue;
    }
    c->path = path;
    g_npc++;
  }
}


/* the problems of a file (a folder: of what is in it) */
static void problems_of (const Node *n, int *e, int *w) {
  size_t i;
  *e = *w = 0;
  for (i = 0; i < g_npc; i++)
    if (n->dir ? inside(g_pc[i].path, n->path) : m_fncmp(g_pc[i].path, n->path) == 0) {
      *e += g_pc[i].e;
      *w += g_pc[i].w;
    }
}


static void draw_input (int x, int sy, int w, int depth, int dir_icon) {
  int cx = x + 1 + depth * 2, ist, iw;
  uint32_t icon = dir_icon ? 0xEAB6 : file_icon(g_in.text, &ist);
  size_t i, col = 0;
  scr_fill(x, sy, w, S_SIDE);
  if (cx + 6 >= x + w) return;
  if (dir_icon) ist = S_SIDE;
  scr_put(cx, sy, icon, ist);
  cx += 2;
  iw = x + w - cx - 1;
  scr_fill(cx, sy, iw, S_INPUT_ON);
  scr_putsw(cx, sy, iw, g_in.text, S_INPUT_ON);
  if (g_in.sel > 0) {	/* the selected part */
    char part[256];
    int sw;
    snprintf(part, sizeof(part), "%.*s", (int)g_in.sel, g_in.text);
    sw = (int)str_cols(part);
    scr_restyle(cx, sy, sw < iw ? sw : iw, S_SEL);
  }
  for (i = 0; i < g_in.cur; i++)	/* the cursor's column */
    if (((unsigned char)g_in.text[i] & 0xC0) != 0x80) col++;
  if ((int)col < iw) scr_cursor(cx + (int)col, sy);
}


/* rows of the tree given to OPEN EDITORS, under "EXPLORER" (mme.c draws it there) */
void files_gap (int rows) {
  g_gap = rows > 0 ? rows : 0;
}


/* the text of row k: a compacted row is "a/b/c" */
static const char *row_name (size_t k, char *buf, size_t n) {
  Node *h = g_vhead[k], *at = h;
  size_t len;
  if (h == g_vis[k]) return h->name;
  len = (size_t)snprintf(buf, n, "%s", h->name);
  while (at != g_vis[k] && at->nkid == 1 && len < n) {
    at = &at->kid[0];
    len += (size_t)snprintf(buf + len, n - len, "/%s", at->name);
  }
  return buf;
}


/* is row k in the folder the drop goes into (lit while dragging)? */
static int in_drop (size_t k) {
  size_t i;
  if (g_drop == -2) return 1;
  if (g_drop < 0 || (size_t)g_drop >= g_nvis) return 0;
  if (k < (size_t)g_drop) return 0;
  if (k == (size_t)g_drop) return 1;
  for (i = (size_t)g_drop + 1; i <= k; i++)
    if (g_vdepth[i] <= g_vdepth[g_drop]) return 0;
  return 1;
}


/* 'active' (a realpath or NULL) is the file in the editor */
void files_draw (int x, int y, int w, int h, int focus, const char *active) {
  char head[512];
  size_t k;
  int row, drawn_in = 0, head_y = y + 1 + g_gap;
  scr_box(x, y, w, h, S_SIDE);
  if (h <= HEAD) return;
  scr_puts(x + 2, y, "EXPLORER", S_SIDE_HEAD);
  snprintf(head, sizeof(head), "%s", g_root.name ? g_root.name : "NO FOLDER OPENED");	/* "X (WORKSPACE)" in one */
  for (k = 0; head[k]; k++)	/* upper case, like VS Code */
    if (head[k] >= 'a' && head[k] <= 'z') head[k] = (char)(head[k] - 32);
  if (g_gap)	/* a line above the folder's section, as under OPEN EDITORS' */
    for (k = 0; k < (size_t)w; k++) scr_put(x + (int)k, head_y - 1, 0x2500, S_BORDER);
  scr_put(x, head_y, 0xEAB4, S_SIDE_TITLE);	/* codicon chevron-down */
  scr_putsw(x + 2, head_y, w - (w >= 24 ? 12 : 3), head, S_SIDE_TITLE);
  g_icon_x[0] = g_icon_x[1] = g_icon_x[2] = g_icon_x[3] = -1;
  if (w >= 24 && g_root.path) {	/* New File, New Folder, Refresh, Collapse Folders */
    static const uint32_t ic[4] = {0xEA7F, 0xEA80, 0xEB37, 0xEAC5};
    int i;
    for (i = 0; i < 4; i++) {
      g_icon_x[i] = w - 9 + i * 2;	/* from x: clicks come as the column in the sidebar */
      scr_put(x + g_icon_x[i], head_y, ic[i], S_SIDE_TITLE);
    }
  }
  problems_count();
  g_h = h - HEAD;
  g_bar_y = y + HEAD;	/* where its scrollbar is, for the drag */
  if (g_nvis <= (size_t)g_h) g_top = 0;	/* the wheel may leave the selection out of view */
  else if (g_top > g_nvis - (size_t)g_h) g_top = g_nvis - (size_t)g_h;
  k = g_top;
  for (row = 0; row < g_h; row++) {
    Node *n;
    int st, cx, ind, ist, mark, sy = y + HEAD + row, nw, pe, pw, right, sel;
    uint32_t icon;
    char nbuf[512], badge[32];
    const char *name;
    if (g_in.on && g_in.kind != IN_RENAME && !drawn_in && k == g_in.row) {	/* the new name's box */
      draw_input(x, sy, w, g_in.depth, g_in.kind == IN_FOLDER);
      drawn_in = 1;
      continue;
    }
    if (k >= g_nvis) break;
    n = g_vis[k];
    if (g_in.on && g_in.kind == IN_RENAME && k == g_in.row) {
      draw_input(x, sy, w, g_vdepth[k], n->dir);
      k++;
      continue;
    }
    mark = git_mark(n->path, n->dir);
    problems_of(n, &pe, &pw);
    sel = (k == g_sel && focus) || (g_msel.n > 1 && msel_has(n->path));
    if (sel) st = focus ? S_SIDE_SEL : S_SIDE_CUR;
    else if (in_drop(k)) st = S_SIDE_CUR;	/* where a drop goes */
    else if (active && m_fncmp(n->path, active) == 0) st = S_SIDE_ACTIVE;
    else if (k == g_sel) st = S_SIDE_CUR;
    else if (g_fcut) {	/* cut: dim */
      size_t c;
      st = mark ? git_style(mark) : S_SIDE;
      for (c = 0; c < g_fclip.n; c++)
        if (strcmp(n->path, g_fclip.v[c]) == 0) st = S_SIDE_DIM;
    }
    else st = mark ? git_style(mark) : S_SIDE;
    scr_fill(x, sy, w, st);
    ind = 1 + g_vdepth[k] * 2;
    cx = x + ind;
    k++;
    if (ind + 4 >= w) continue;
    if (n->dir) icon = n->open ? 0xEAB4 : 0xEAB6, ist = st;	/* chevrons */
    else {
      if (g_vnest[k - 1] > 0) {	/* files nested under it: a chevron too */
        scr_put(cx, sy, nest_open(k - 1) ? 0xEAB4 : 0xEAB6, st);
        cx += 2;
      }
      icon = file_icon(n->name, &ist);
      if (st == S_SIDE_SEL || st == S_SIDE_CUR || st == S_SIDE_ACTIVE) ist = st;
    }
    scr_put(cx, sy, icon, ist);
    cx += 2;
    badge[0] = '\0';	/* on the right: "2, M" (the problems, git's letter), a dot for a folder */
    if (!n->dir && opt.exp_dec_badges && pe + pw > 0)
      snprintf(badge, sizeof(badge), "%d%s%c", pe + pw > 99 ? 99 : pe + pw, mark ? ", " : "", mark ? (char)mark : '\0');
    else if (mark) snprintf(badge, sizeof(badge), "%c", (char)mark);
    else if (n->dir && opt.exp_dec_colors && pe + pw > 0) snprintf(badge, sizeof(badge), "\xE2\x97\x8F");	/* ● */
    right = badge[0] ? (int)str_cols(badge) + 2 : 1;
    nw = x + w - cx - right;
    name = row_name(k - 1, nbuf, sizeof(nbuf));
    if (nw < 2) continue;
    if ((int)str_cols(name) > nw) {	/* cut with ... */
      scr_putsw(cx, sy, nw - 1, name, st);
      scr_put(cx + nw - 1, sy, 0x2026, st);
    }
    else scr_puts(cx, sy, name, st);
    if (st != S_SIDE_SEL && opt.exp_dec_colors && pe + pw > 0) {	/* problems: the name red / yellow */
      int i, nc = (int)str_cols(name);
      uint32_t fg = ui_color(pe ? C_ERROR : C_WARNING);
      if (nc > nw) nc = nw;
      for (i = 0; i < nc; i++) scr_set_fg(cx + i, sy, fg);
    }
    if (g_type[0] && k - 1 == g_sel && os_now_us() - g_type_at < 1500000 &&
        m_strnicmp(name, g_type, strlen(g_type)) == 0) {	/* type navigation: what matched */
      int tl = (int)str_cols(g_type);
      scr_restyle(cx, sy, tl < nw ? tl : nw, S_MATCH);
    }
    if (badge[0]) {
      int bx = x + w - 1 - (int)str_cols(badge), ms = st;
      if (!(st == S_SIDE_SEL || st == S_SIDE_CUR || st == S_SIDE_ACTIVE)) ms = mark ? git_style(mark) : st;
      scr_puts(bx, sy, badge, ms);
      if (st != S_SIDE_SEL && pe + pw > 0 && opt.exp_dec_colors) {	/* the count, in the problems' color */
        int i, cl = badge[0] == '\xE2' ? 1 : (int)strcspn(badge, ",");
        uint32_t fg = ui_color(pe ? C_ERROR : C_WARNING);
        for (i = 0; i < cl; i++) scr_set_fg(bx + i, sy, fg);
      }
    }
  }
  side_bar(x, y + HEAD, w, g_h, g_nvis, g_top, (size_t)g_h);
}


static size_t parent_of (size_t k) {
  int d = g_vdepth[k];
  while (k > 0 && g_vdepth[k] >= d) k--;
  return g_vdepth[k] < d ? k : g_sel;
}


/* typing goes to the next name that starts with what was typed (quickly one after the other) */
static void type_jump (uint32_t c) {
  size_t i, n;
  long long now = os_now_us();
  if (now - g_type_at > 800000) g_type[0] = '\0';
  g_type_at = now;
  n = strlen(g_type);
  if (n + 1 < sizeof(g_type) && c < 128) {
    g_type[n] = (char)c;
    g_type[n + 1] = '\0';
  }
  n = strlen(g_type);
  for (i = n > 1 ? 0 : 1; i <= g_nvis; i++) {	/* more letters: the one selected may still match */
    size_t k = (g_sel + i) % g_nvis;
    if (m_strnicmp(g_vis[k]->name, g_type, n) == 0) {
      g_sel = k;
      return;
    }
  }
}


/* the node of a path, NULL: not in the tree */
static Node *node_of (Node *n, const char *path) {
  size_t i;
  if (n->path && m_fncmp(n->path, path) == 0) return n;
  for (i = 0; i < n->nkid; i++) {
    Node *f = node_of(&n->kid[i], path);
    if (f) return f;
  }
  return NULL;
}


/* the folder the selection is in (a folder selected: itself) */
static Node *target_dir (void) {
  Node *n;
  if (g_nvis == 0 || g_sel >= g_nvis) return &g_root;
  n = g_vis[g_sel];
  if (n->dir) return n;
  {
    char *d = path_dirname(n->path);
    Node *p = node_of(&g_root, d);
    free(d);
    return p ? p : &g_root;
  }
}


/* the folder is read again, and path (when given) is selected */
static void reload (Node *dir, const char *path) {
  if (dir && dir->loaded) load(dir);
  flatten();
  if (path) {
    char *real = os_realpath(path);
    side_reveal(real ? real : path);
    free(real);
  }
  keep_in_view();
}


static void act_path (SideAct *act, int what, const char *p, const char *p2) {
  snprintf(g_ap, sizeof(g_ap), "%s", p ? p : "");
  snprintf(g_ap2, sizeof(g_ap2), "%s", p2 ? p2 : "");
  act->what = what;
  act->path = g_ap;
  act->path2 = g_ap2;
}


/* the box for a new name: kind IN_FILE / IN_FOLDER in the selection's folder, or IN_RENAME */
static void input_start (int kind) {
  Node *dir;
  size_t i;
  if (g_root.path == NULL) return;
  free(g_in.dir);
  memset(&g_in, 0, sizeof(g_in));
  g_in.kind = kind;
  if (kind == IN_RENAME) {
    Node *n;
    if (g_nvis == 0) return;
    n = g_vis[g_sel];
    g_in.dir = xstrdup(n->path);
    g_in.row = g_sel;
    g_in.depth = g_vdepth[g_sel];
    snprintf(g_in.text, sizeof(g_in.text), "%s", n->name);
    {	/* the name selected up to its extension, like VS Code: the cursor there */
      const char *dot = n->dir ? NULL : strrchr(n->name, '.');
      g_in.cur = dot && dot != n->name ? (size_t)(dot - n->name) : strlen(g_in.text);
      g_in.sel = g_in.cur;
    }
    g_in.on = 1;
    return;
  }
  dir = target_dir();
  if (dir != &g_root && !dir->open) set_open(dir, 1);
  g_in.dir = xstrdup(dir->path);
  g_in.depth = 0;
  g_in.row = 0;
  for (i = 0; i < g_nvis; i++)	/* under the folder's row */
    if (g_vis[i] == dir) {
      g_in.row = i + 1;
      g_in.depth = g_vdepth[i] + 1;
    }
  if (g_in.row < g_top) g_top = g_in.row;
  if (g_in.row >= g_top + (size_t)g_h) g_top = g_in.row - (size_t)g_h + 1;
  g_in.on = 1;
}


/* Enter: the file or folder is made, or renamed; 0 when the box stays */
static int input_commit (SideAct *act) {
  char *path, *dir;
  const char *name = g_in.text;
  while (*name == ' ') name++;
  if (*name == '\0' || (g_in.kind == IN_RENAME && strcmp(name, path_basename(g_in.dir)) == 0)) {
    g_in.on = 0;
    return 1;
  }
  dir = g_in.kind == IN_RENAME ? path_dirname(g_in.dir) : xstrdup(g_in.dir);
  path = path_join(dir, name);
  if (fs_exists(path) && !(g_in.kind == IN_RENAME && m_fncmp(path, g_in.dir) == 0)) {
    toast(1, "A file or folder %s already exists at this location. Please choose a different name.", name);
    free(path);
    free(dir);
    return 0;
  }
  if (g_in.kind == IN_FOLDER) {
    if (mkdir_p(path) != 0) toast(1, "Unable to create folder '%s'", name);
  }
  else if (g_in.kind == IN_FILE) {
    char *parent = path_dirname(path);
    int fd;
    mkdir_p(parent);	/* "a/b.c" makes the folder a too, like VS Code */
    free(parent);
    if ((fd = os_open(path, OS_EXCL)) >= 0) {
      os_close(fd);
      act_path(act, SA_GO, path, NULL);	/* the new file opens */
    }
    else toast(1, "Unable to create file '%s'", name);
  }
  else if (fs_move(g_in.dir, path) != 0) toast(1, "Unable to rename '%s'", path_basename(g_in.dir));
  else act_path(act, SA_RENAMED, g_in.dir, path);
  g_in.on = 0;
  {
    Node *d = node_of(&g_root, dir);
    reload(d ? d : &g_root, path);
  }
  side_refresh();
  reload(NULL, path);
  free(path);
  free(dir);
  return 1;
}


/* a key while the box shows: 1 always (it has the keys) */
static int input_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  size_t len = strlen(g_in.text);
  if (g_in.sel > 0 && (IS_TEXT(k) || code == K_BS || code == K_DEL || IS_PASTE(k))) {	/* it replaces the selection */
    memmove(g_in.text, g_in.text + g_in.sel, len - g_in.sel + 1);
    len -= g_in.sel;
    g_in.cur = 0;
    g_in.sel = 0;
    if (code == K_BS || code == K_DEL) return 1;
  }
  if (code != K_ENTER) g_in.sel = 0;
  if (code == K_ESC) g_in.on = 0;
  else if (code == K_ENTER) input_commit(act);
  else if (code == K_LEFT) {
    while (g_in.cur > 0 && ((unsigned char)g_in.text[--g_in.cur] & 0xC0) == 0x80) ;
  }
  else if (code == K_RIGHT) {
    if (g_in.cur < len) g_in.cur++;
    while (g_in.cur < len && ((unsigned char)g_in.text[g_in.cur] & 0xC0) == 0x80) g_in.cur++;
  }
  else if (code == K_HOME) g_in.cur = 0;
  else if (code == K_END) g_in.cur = len;
  else if (code == K_BS && g_in.cur > 0) {
    size_t a = g_in.cur;
    while (a > 0 && ((unsigned char)g_in.text[--a] & 0xC0) == 0x80) ;
    memmove(g_in.text + a, g_in.text + g_in.cur, len - g_in.cur + 1);
    g_in.cur = a;
  }
  else if (code == K_DEL && g_in.cur < len) {
    size_t b = g_in.cur + 1;
    while (b < len && ((unsigned char)g_in.text[b] & 0xC0) == 0x80) b++;
    memmove(g_in.text + g_in.cur, g_in.text + b, len - b + 1);
  }
  else if (IS_PASTE(k)) {
    Buf b;
    size_t i;
    buf_init(&b);
    paste_take(k, &b);
    for (i = 0; i < b.len && b.s[i] != '\n' && len + 1 < sizeof(g_in.text); i++, len++) {
      memmove(g_in.text + g_in.cur + 1, g_in.text + g_in.cur, len - g_in.cur + 1);
      g_in.text[g_in.cur++] = b.s[i];
    }
    buf_free(&b);
  }
  else if (IS_TEXT(k) && len + 5 < sizeof(g_in.text)) {
    char u[4];
    int n = utf8_encode((uint32_t)k, u);
    memmove(g_in.text + g_in.cur + n, g_in.text + g_in.cur, len - g_in.cur + 1);
    memcpy(g_in.text + g_in.cur, u, (size_t)n);
    g_in.cur += (size_t)n;
  }
  return 1;
}


int files_editing (void) {
  return g_in.on;
}


static void close_all (Node *n) {
  size_t i;
  for (i = 0; i < n->nkid; i++) {
    close_all(&n->kid[i]);
    if (n->kid[i].dir) n->kid[i].open = 0;
  }
}


/* Delete: to the Recycle Bin (or the Trash), asked first like VS Code; what is selected, all of it */
static void delete_selected (SideAct *act) {
  Vec v;
  char msg[600], detail[800];
  size_t i, len = 0;
  int r = 0, any_dir = 0, perm = 0;
#ifdef _WIN32
  static const char *const bin = "Recycle Bin";
#else
  static const char *const bin = "Trash";
#endif
  char b0[40];
  const char *bt[2];
  if (g_nvis == 0) return;
  targets(&v);
  if (v.n == 0) return;
  for (i = 0; i < v.n; i++) {
    Node *n = node_of(&g_root, v.v[i]);
    if (n && n->dir) any_dir = 1;
  }
  if (v.n == 1) snprintf(msg, sizeof(msg), any_dir ? "Are you sure you want to delete '%s' and its contents?"
                                                   : "Are you sure you want to delete '%s'?", path_basename(v.v[0]));
  else snprintf(msg, sizeof(msg), any_dir ? "Are you sure you want to delete the following %d files/directories and their contents?"
                                          : "Are you sure you want to delete the following %d files?", (int)v.n);
  detail[0] = '\0';
  for (i = 0; i < v.n && i < 8 && v.n > 1; i++) {	/* their names, like VS Code's list */
    /* snprintf answers what the whole name needed, not what it wrote: long names would run len past the end */
    int k = snprintf(detail + len, sizeof(detail) - len, "%s%s", i ? ", " : "", path_basename(v.v[i]));
    if (k < 0 || (size_t)k >= sizeof(detail) - len) {
      len = sizeof(detail) - 1;
      break;
    }
    len += (size_t)k;
  }
  if (v.n > 8 && len + 1 < sizeof(detail))
    snprintf(detail + len, sizeof(detail) - len, " ... and %d more", (int)(v.n - 8));
  snprintf(b0, sizeof(b0), "Move to %s", bin);
  bt[0] = b0;
  bt[1] = "Cancel";
  if (v.n == 1) snprintf(detail, sizeof(detail), "You can restore this file from the %s.", bin);
  if (opt.exp_confirm_del) r = dialog(msg, detail, bt, 2);
  if (r != 0) {
    vec_free(&v);
    return;
  }
  for (i = 0; i < v.n; i++) {
    const char *path = v.v[i];
    if (!perm && fs_trash(path) != 0) {
      static const char *const bt2[] = {"Delete Permanently", "Cancel"};
      snprintf(msg, sizeof(msg), "Failed to delete using the %s. Do you want to permanently delete instead?", bin);
      if (dialog(msg, NULL, bt2, 2) != 0) break;
      perm = 1;
    }
    if (perm && fs_remove(path) != 0) {
      toast(1, "Unable to delete '%s'", path_basename(path));
      continue;
    }
    if (v.n == 1) act_path(act, SA_DELETED, path, NULL);
    else explorer_deleted(path);	/* its tabs */
  }
  msel_clear();
  side_refresh();
  flatten();
  keep_in_view();
  vec_free(&v);
}


/* Paste: what Copy or Cut took goes into the selection's folder */
static void paste_files (SideAct *act) {
  Node *dir = target_dir();
  char *last = NULL;
  size_t i;
  int moved = 0;
  for (i = 0; i < g_fclip.n; i++) {
    const char *from = g_fclip.v[i];
    char *to;
    OsStat st;
    if (os_stat(from, &st) != 0 || !st.exists) continue;
    if (g_fcut) {
      to = path_join(dir->path, path_basename(from));
      if (m_fncmp(to, from) == 0) {	/* the same place: nothing to do */
        free(to);
        continue;
      }
      if (st.is_dir && inside(dir->path, from)) toast(1, "Cannot move '%s' into a subfolder of itself.", path_basename(from));
      else if (fs_exists(to)) toast(1, "A file or folder %s already exists in the destination folder.", path_basename(to));
      else if (fs_move(from, to) != 0) toast(1, "Unable to move '%s'", path_basename(from));
      else {
        if (g_fclip.n == 1) act_path(act, SA_RENAMED, from, to);
        else explorer_renamed(from, to);
        moved = 1;
      }
    }
    else {
      to = fs_copy_name(dir->path, path_basename(from), st.is_dir);
      if (fs_copy(from, to) != 0) toast(1, "Unable to copy '%s'", path_basename(from));
    }
    free(last);
    last = to;
  }
  if (moved) {	/* a cut is pasted once */
    vec_free(&g_fclip);
    vec_init(&g_fclip);
  }
  msel_clear();
  {	/* side_refresh reads the folders again: dir points into an array it freed */
    char *at = xstrdup(dir->path);
    side_refresh();
    reload(node_of(&g_root, at), last);
    free(at);
  }
  free(last);
}


/*
** {==================================================================
** Drag and drop
** ===================================================================
*/

/* the row of the tree under the sidebar's row (-1: none) */
static long tree_row (int row) {
  size_t k;
  if (row < HEAD || row >= HEAD + g_h) return -1;
  k = g_top + (size_t)(row - HEAD);
  return k < g_nvis ? (long)k : -1;
}


/* the folder a drop on row k goes into: its row, or -2 for the root */
static long drop_row (long k) {
  long i;
  if (k < 0) return -2;
  if (g_vis[k]->dir) return k;
  for (i = k - 1; i >= 0; i--)	/* a file: its folder's row */
    if (g_vdepth[i] < g_vdepth[k]) return i;
  return -2;
}


/* the mouse pressed on row: what would be dragged from there (1: something) */
int files_drag_start (int row) {
  long k = tree_row(row);
  vec_free(&g_drag);
  vec_init(&g_drag);
  g_drop = -1;
  if (k < 0) return 0;
  if (g_msel.n && msel_has(g_vis[k]->path)) {
    size_t i;
    for (i = 0; i < g_msel.n; i++) vec_push(&g_drag, xstrdup(g_msel.v[i]));
  }
  else vec_push(&g_drag, xstrdup(g_vis[k]->path));
  return 1;
}


/* the mouse over row while dragging (-1: outside the tree): the folder it would go into lit */
void files_drag_over (int row) {
  long k = row < 0 ? -1 : tree_row(row);
  g_drop = row < 0 ? -1 : (k < 0 && row >= HEAD + g_h) ? -1 : drop_row(k);
}


size_t files_drag_count (void) {
  return g_drag.n;
}


const char *files_drag_path (size_t i) {
  return i < g_drag.n ? g_drag.v[i] : NULL;
}


void files_drag_end (void) {
  vec_free(&g_drag);
  vec_init(&g_drag);
  g_drop = -1;
}


/* dropped on row: what is dragged goes into that folder, moved (copy: copied); VS Code asks first */
void files_drop (int row, int copy, SideAct *act) {
  long k = tree_row(row), dk;
  Node *dir;
  char msg[700], *last = NULL;
  size_t i;
  int moved = 0;
  act->what = SA_NONE;
  if (g_drag.n == 0 || (row < HEAD + g_h && row >= 0 && k < 0 && row < HEAD)) {
    files_drag_end();
    return;
  }
  dk = drop_row(k);
  dir = dk >= 0 ? g_vis[dk] : &g_root;
  if (g_ws && dk < 0) {	/* a workspace has no one folder to drop in */
    files_drag_end();
    return;
  }
  if (!copy && opt.exp_confirm_dnd) {
    static const char *const bt[] = {"Move", "Cancel"};
    if (g_drag.n == 1) snprintf(msg, sizeof(msg), "Are you sure you want to move '%s' into '%s'?",
                                path_basename(g_drag.v[0]), dir->name);
    else snprintf(msg, sizeof(msg), "Are you sure you want to move the following %d files into '%s'?",
                  (int)g_drag.n, dir->name);
    for (i = 0; i < g_drag.n; i++) {	/* nothing would move: no question */
      char *pd = path_dirname(g_drag.v[i]);
      int same = m_fncmp(pd, dir->path) == 0;
      free(pd);
      if (!same) break;
    }
    if (i == g_drag.n || dialog(msg, NULL, bt, 2) != 0) {
      files_drag_end();
      return;
    }
  }
  for (i = 0; i < g_drag.n; i++) {
    const char *from = g_drag.v[i];
    char *to;
    OsStat st;
    if (os_stat(from, &st) != 0 || !st.exists) continue;
    if (st.is_dir && inside(dir->path, from)) {
      toast(1, "Cannot move '%s' into a subfolder of itself.", path_basename(from));
      continue;
    }
    if (copy) {
      to = fs_copy_name(dir->path, path_basename(from), st.is_dir);
      if (fs_copy(from, to) != 0) toast(1, "Unable to copy '%s'", path_basename(from));
    }
    else {
      to = path_join(dir->path, path_basename(from));
      if (m_fncmp(to, from) == 0) {
        free(to);
        continue;
      }
      if (fs_exists(to)) {
        toast(1, "A file or folder with the name '%s' already exists in the destination folder.", path_basename(to));
        free(to);
        continue;
      }
      if (fs_move(from, to) != 0) {
        toast(1, "Unable to move '%s'", path_basename(from));
        free(to);
        continue;
      }
      explorer_renamed(from, to);	/* its tabs follow */
      moved = 1;
    }
    free(last);
    last = to;
  }
  (void)moved;
  msel_clear();
  files_drag_end();
  if (dir != &g_root && !dir->open) dir->open = 1;
  {	/* side_refresh reads the folders again: dir points into an array it freed */
    char *at = dir == &g_root ? NULL : xstrdup(dir->path);
    side_refresh();
    reload(at ? node_of(&g_root, at) : NULL, last);
    free(at);
  }
  free(last);
}

/* }================================================================== */


/* the Explorer's commands (FC_*), on the selection */
void files_cmd (int what, SideAct *act) {
  Node *n = g_nvis ? g_vis[g_sel] : NULL;
  const char *root = g_root.path;
  act->what = SA_NONE;
  switch (what) {
    case FC_NEW_FILE: input_start(IN_FILE); return;
    case FC_NEW_FOLDER: input_start(IN_FOLDER); return;
    case FC_RENAME: if (n) input_start(IN_RENAME); return;
    case FC_DELETE: if (n) delete_selected(act); return;
    case FC_COPY: case FC_CUT:
      if (!n) return;
      vec_free(&g_fclip);
      targets(&g_fclip);
      g_fcut = what == FC_CUT;
      return;
    case FC_SELECT_ALL: {
      size_t i;
      msel_clear();
      for (i = 0; i < g_nvis; i++) vec_push(&g_msel, xstrdup(g_vis[i]->path));
      return;
    }
    case FC_OPEN_SIDE:
      if (n && !n->dir) act_path(act, SA_OPEN_SIDE, n->path, NULL);
      return;
    case FC_FIND_FOLDER: {
      Node *d = n && n->dir ? n : target_dir();
      if (d->path) act_path(act, SA_FIND_FOLDER, d == &g_root ? "" : rel_of(d->path), NULL);
      return;
    }
    case FC_PASTE: paste_files(act); return;
    case FC_DUPLICATE:
      if (n) {
        char *d = path_dirname(n->path), *to = fs_copy_name(d, n->name, n->dir);
        Node *dn = node_of(&g_root, d);
        if (fs_copy(n->path, to) != 0) toast(1, "Unable to copy '%s'", n->name);
        reload(dn ? dn : &g_root, to);
        free(to);
        free(d);
      }
      return;
    case FC_COPY_PATH: case FC_COPY_REL: {
      const char *p = n ? n->path : root;
      size_t rl = root ? strlen(root) : 0;
      if (p == NULL) return;
      if (what == FC_COPY_REL && root && m_fnncmp(p, root, rl) == 0 && path_is_sep(p[rl])) p += rl + 1;
      act_path(act, SA_CLIP, p, NULL);
      return;
    }
    case FC_REVEAL: if (n || root) fs_reveal(n ? n->path : root); return;
    case FC_TERMINAL: {
      Node *d = n && n->dir ? n : target_dir();
      if (d->path) act_path(act, SA_TERMINAL, d->path, NULL);
      return;
    }
    case FC_COLLAPSE:
      close_all(&g_root);
      msel_clear();
      g_sel = g_top = 0;
      flatten();
      return;
    case FC_REFRESH:
      side_refresh();
      git_refresh();
      return;
  }
}


/* the path selected in the tree, NULL: none */
const char *files_selected (void) {
  return g_nvis && g_sel < g_nvis ? g_vis[g_sel]->path : NULL;
}


int files_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  Node *n;
  act->what = SA_NONE;
  if (g_in.on) return input_key(k, act);
  if (code == K_F5) {
    files_cmd(FC_REFRESH, act);
    return 1;
  }
  if (k == CTRL('v')) {
    files_cmd(FC_PASTE, act);
    return 1;
  }
  if (g_nvis == 0) return 0;
  if (g_sel >= g_nvis) g_sel = g_nvis - 1;
  n = g_vis[g_sel];
  if (k == ('F' | KM_ALT) || k == ('f' | KM_ALT | KM_SHIFT) || k == ('F' | KM_ALT | KM_SHIFT)) {	/* Shift+Alt+F */
    files_cmd(FC_FIND_FOLDER, act);
    return 1;
  }
  if (k == CTRL('a')) {	/* Ctrl+A: every row */
    files_cmd(FC_SELECT_ALL, act);
    return 1;
  }
  if ((code == K_UP || code == K_DOWN) && (k & KM_SHIFT) && !(k & (KM_CTRL | KM_ALT))) {	/* Shift: the selection grows */
    if (g_anchor == NULL) set_anchor(g_sel);
    if (code == K_UP && g_sel > 0) g_sel--;
    if (code == K_DOWN && g_sel + 1 < g_nvis) g_sel++;
    msel_range(g_sel);
    keep_in_view();
    return 1;
  }
  if (code == K_ENTER && (k & KM_CTRL)) {	/* Ctrl+Enter: Open to the Side */
    files_cmd(FC_OPEN_SIDE, act);
    return 1;
  }
  if (code == K_ESC && g_msel.n) {
    msel_clear();
    return 1;
  }
  if (k == CTRL('c') || k == CTRL('x')) {
    files_cmd(k == CTRL('c') ? FC_COPY : FC_CUT, act);
    return 1;
  }
  if (code == K_DEL) {
    files_cmd(FC_DELETE, act);
    return 1;
  }
  if (code == K_UP || code == K_DOWN || code == K_HOME || code == K_END || code == K_PGUP || code == K_PGDN) {
    msel_clear();	/* moving without Shift: one row again */
    free(g_anchor);
    g_anchor = NULL;
  }
  switch (code) {
    case K_UP: if (g_sel > 0) g_sel--; break;
    case K_DOWN: if (g_sel + 1 < g_nvis) g_sel++; break;
    case K_HOME: g_sel = 0; break;
    case K_END: g_sel = g_nvis - 1; break;
    case K_PGUP: g_sel = g_sel > (size_t)g_h ? g_sel - (size_t)g_h : 0; break;
    case K_PGDN:
      g_sel += (size_t)g_h;
      if (g_sel >= g_nvis) g_sel = g_nvis - 1;
      break;
    case K_RIGHT:
      if (n->dir && !n->open) set_open(n, 1);
      else if (!n->dir && g_vnest[g_sel] > 0 && !nest_open(g_sel)) set_nest(g_sel, 1);
      else if (g_sel + 1 < g_nvis && g_vdepth[g_sel + 1] > g_vdepth[g_sel]) g_sel++;
      break;
    case K_LEFT:
      if (n->dir && n->open) set_open(n, 0);
      else if (!n->dir && nest_open(g_sel)) set_nest(g_sel, 0);
      else g_sel = parent_of(g_sel);
      break;
    case K_ENTER:
    case ' ':
      if (n->dir) set_open(n, !n->open);
      else {
        act->what = code == K_ENTER ? SA_GO : SA_OPEN;	/* Space: a look, the focus stays */
        act->path = n->path;
      }
      break;
    default:
      if (code > ' ' && code < 127 && !(k & (KM_CTRL | KM_ALT))) type_jump((uint32_t)code);
      else return 0;
  }
  keep_in_view();
  return 1;
}


void files_click (int row, int col, SideAct *act) {
  size_t k;
  Node *n;
  act->what = SA_NONE;
  if (g_in.on) {	/* a click elsewhere: what was typed is kept */
    input_commit(act);
    g_in.on = 0;
    if (act->what != SA_NONE) return;
  }
  if (row == 1 + g_gap && col >= 0) {	/* the folder's row: its icons */
    static const int fc[4] = {FC_NEW_FILE, FC_NEW_FOLDER, FC_REFRESH, FC_COLLAPSE};
    int i;
    for (i = 0; i < 4; i++)
      if (g_icon_x[i] >= 0 && col == g_icon_x[i]) {
        files_cmd(fc[i], act);
        return;
      }
  }
  if (row < HEAD) return;
  k = g_top + (size_t)(row - HEAD);
  if (k >= g_nvis) return;
  n = g_vis[k];
  if (g_mods & KM_CTRL) {	/* Ctrl+click: this one in or out of the selection, nothing opens */
    if (g_msel.n == 0 && g_sel < g_nvis && g_sel != k) vec_push(&g_msel, xstrdup(g_vis[g_sel]->path));
    msel_toggle(n->path);
    g_sel = k;
    set_anchor(k);
    return;
  }
  if (g_mods & KM_SHIFT) {	/* Shift+click: from the anchor to here */
    if (g_anchor == NULL) set_anchor(g_sel);
    msel_range(k);
    g_sel = k;
    return;
  }
  msel_clear();
  g_sel = k;
  set_anchor(k);
  if (!n->dir && g_vnest[k] > 0 && col <= 2 + g_vdepth[k] * 2) {	/* the chevron of nested files */
    set_nest(k, !nest_open(k));
    return;
  }
  if (n->dir) set_open(n, !n->open);
  else if (g_mods & KM_ALT) act_path(act, SA_OPEN_SIDE, n->path, NULL);	/* Alt+click: to the side */
  else {
    act->what = SA_OPEN;
    act->path = n->path;
  }
}


/* the mouse's modifier keys (KM_*) for the next click */
void files_mods (int mods) {
  g_mods = mods;
}


/*
** The right button on row of the tree (the screen's x, y for the menu):
** VS Code's context menu for the file or folder there.
*/
void files_menu (int row, int x, int y, SideAct *act) {
  enum { M_NEW_FILE, M_NEW_FOLDER, M_REVEAL, M_TERM, M_CUT, M_COPY, M_PASTE, M_DUP, M_PATH,
         M_REL, M_RENAME, M_DELETE, M_SEL_CMP, M_CMP_SEL, M_SIDE, M_FIND, M_N };
  static const int fc[M_N] = {FC_NEW_FILE, FC_NEW_FOLDER, FC_REVEAL, FC_TERMINAL, FC_CUT, FC_COPY,
                              FC_PASTE, FC_DUPLICATE, FC_COPY_PATH, FC_COPY_REL, FC_RENAME, FC_DELETE,
                              -CMD_SELECT_COMPARE, -CMD_COMPARE_SELECTED, FC_OPEN_SIDE, FC_FIND_FOLDER};	/* < 0: a command */
#ifdef _WIN32
  static const char *const reveal = "Reveal in File Explorer";
#else
  static const char *const reveal = "Reveal in File Manager";
#endif
  const char *label[M_N + 5], *keys[M_N + 5];
  int map[M_N + 5], n = 0, r;
  size_t k;
  act->what = SA_NONE;
  if (g_in.on) g_in.on = 0;
  if (row >= HEAD && (k = g_top + (size_t)(row - HEAD)) < g_nvis) {
    if (!msel_has(g_vis[k]->path)) msel_clear();	/* on a row not selected: that one only */
    g_sel = k;
  }
#define ITEM(m, l, key)	(label[n] = (l), keys[n] = (key), map[n++] = (m))
#define LINE()	(label[n] = NULL, keys[n] = NULL, map[n++] = -1)
  if (g_nvis && !g_vis[g_sel]->dir) {
    ITEM(M_SIDE, "Open to the Side", "Ctrl+Enter");
    LINE();
  }
  ITEM(M_NEW_FILE, "New File...", "");
  ITEM(M_NEW_FOLDER, "New Folder...", "");
  ITEM(M_REVEAL, reveal, "Shift+Alt+R");
  ITEM(M_TERM, "Open in Integrated Terminal", "");
  LINE();
  if (g_nvis == 0 || g_vis[g_sel]->dir) {
    ITEM(M_FIND, "Find in Folder...", "Shift+Alt+F");
    LINE();
  }
  if (g_nvis) {
    ITEM(M_CUT, "Cut", "Ctrl+X");
    ITEM(M_COPY, "Copy", "Ctrl+C");
  }
  if (g_fclip.n) ITEM(M_PASTE, "Paste", "Ctrl+V");
  if (g_nvis) {
    ITEM(M_DUP, "Duplicate", "");
    LINE();
    if (!g_vis[g_sel]->dir) {	/* VS Code's compare, for files */
      if (compare_selected()) ITEM(M_CMP_SEL, "Compare with Selected", "");
      ITEM(M_SEL_CMP, "Select for Compare", "");
      LINE();
    }
    ITEM(M_PATH, "Copy Path", "Shift+Alt+C");
    ITEM(M_REL, "Copy Relative Path", "Ctrl+K Ctrl+Shift+C");
    LINE();
    ITEM(M_RENAME, "Rename...", "F2");
    ITEM(M_DELETE, "Delete", "Delete");
  }
#undef ITEM
#undef LINE
  r = context_menu(x, y, label, keys, n);
  if (r >= 0 && map[r] >= 0 && fc[map[r]] < 0) {
    act->what = SA_CMD;
    act->cmd = -fc[map[r]];
  }
  else if (r >= 0 && map[r] >= 0) files_cmd(fc[map[r]], act);
}


/* the tree's scrollbar: its first row on the screen, and how many rows it has */
int files_bar_y (void) {
  return g_bar_y;
}


int files_bar_rows (void) {
  return g_nvis > (size_t)g_h ? g_h : 0;	/* 0: everything fits, no bar */
}


/* the bar dragged or clicked at row (from its first one) */
void files_bar_to (int row, int rows) {
  size_t hidden = g_nvis > (size_t)g_h ? g_nvis - (size_t)g_h : 0;
  if (hidden == 0 || rows < 2) return;
  if (row < 0) row = 0;
  if (row >= rows) row = rows - 1;
  g_top = (size_t)((double)row * (double)hidden / (double)(rows - 1) + 0.5);
  if (g_top > hidden) g_top = hidden;
}


void files_wheel (int d) {
  size_t h = (size_t)g_h, st = (size_t)wheel_step(0);
  if (d < 0) g_top = g_top > st ? g_top - st : 0;
  else if (g_nvis > h) {
    g_top += st;
    if (g_top > g_nvis - h) g_top = g_nvis - h;
  }
}

/* }================================================================== */
