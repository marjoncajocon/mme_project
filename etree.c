/*
** etree.c - the extensions' tree views (createTreeView), in the side bar
**
** Each view an extension contributes (npm scripts, a Docker list, GitLens'
** commits ...) is a section of this view, as VS Code puts them in their
** container. The extension host fills them: mme asks for a node's children
** (mme/treeExpand) and they come as mme/treeItems; choosing a row sends
** mme/treeSelect (its command runs there), a menu's command mme/treeAction.
** A node is known by the host's handle, which stays the same when the view
** is filled again, so what was open stays open.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct TAct {	/* a menu's command: the view's title's, or a row's (view/item/context) */
  char *command, *title, *icon;
  int inline_;
} TAct;

typedef struct TNode {
  char *handle, *label, *desc, *tip, *icon, *file;
  int folder;
  int coll;	/* 0: a leaf, 1: closed, 2: open */
  int loaded;	/* its children came */
  int command;	/* choosing it runs its command */
  TAct *act;
  int nact;
  struct TNode **kid;
  int nkid;
} TNode;

typedef struct TView {
  char *id, *name, *title;	/* title: its container's ("Acme Chat") */
  char *head;	/* NAME: its tab's in the panel */
  int open;	/* the section is open */
  int asked;	/* its root was asked for */
  char *msg;	/* treeView.message, or the host's word when there is no provider */
  char *desc;
  int badge;
  TAct *tact;	/* view/title */
  int ntact;
  TNode root;
} TView;

enum { TR_HEAD, TR_NODE, TR_MSG };

typedef struct TRow {
  int kind, view, depth;
  TNode *n;
} TRow;

static TView *g_v;
static int g_nv;
static TRow *g_row;
static int g_nrow, g_caprow;
static int g_sel, g_top, g_h = 1;
static char *g_selh;	/* the selected row's handle, kept when a view is filled again */
static int g_selv = -1;

/*
** A view moved into the panel (View: Move View) is a tab of its own there:
** the calls are for the view tree_scope said, the side bar's (-1) or that
** one, each with its own selection and scrolling.
*/
static int g_scope = -1;
static int g_dx, g_dy;	/* where it was drawn */
static struct {
  int scope, sel, top, h, selv;
  char *selh;
} g_st[2] = {{-1, 0, 0, 1, -1, NULL}, {-1, 0, 0, 1, -1, NULL}};

#define HEAD	1	/* the title */


static int inum (const Json *j, int def) {
  double v = json_num(j, def);
  return v > -2e9 && v < 2e9 ? (int)v : def;
}


static void *zalloc (size_t n) {
  void *p = xmalloc(n);
  memset(p, 0, n);
  return p;
}


/* ------------------------------------------------------------------ the host */

static void send (const char *method, int v, const char *handle, const char *more) {
  Buf b;
  buf_init(&b);
  buf_puts(&b, "{\"view\":");
  json_put_str(&b, g_v[v].id, strlen(g_v[v].id));
  buf_puts(&b, ",\"handle\":");
  if (handle) json_put_str(&b, handle, strlen(handle));
  else buf_puts(&b, "null");
  if (more) buf_puts(&b, more);
  buf_putc(&b, '}');
  buf_putc(&b, '\0');
  lsp_ext_notify(method, b.s);
  buf_free(&b);
}


static void ask (int v, TNode *n, int open) {
  send("mme/treeExpand", v, n == &g_v[v].root ? NULL : n->handle, open ? ",\"expanded\":true" : ",\"expanded\":false");
}


/* ------------------------------------------------------------------ the nodes */

static void acts_free (TAct *a, int n) {
  int i;
  for (i = 0; i < n; i++) {
    free(a[i].command);
    free(a[i].title);
    free(a[i].icon);
  }
  free(a);
}


static void node_clear (TNode *n) {
  int i;
  for (i = 0; i < n->nkid; i++) {
    node_clear(n->kid[i]);
    free(n->kid[i]);
  }
  free(n->kid);
  n->kid = NULL;
  n->nkid = 0;
  free(n->handle);
  free(n->label);
  free(n->desc);
  free(n->tip);
  free(n->icon);
  free(n->file);
  acts_free(n->act, n->nact);
  n->handle = n->label = n->desc = n->tip = n->icon = n->file = NULL;
  n->act = NULL;
  n->nact = 0;
}


static TAct *acts_of (const Json *a, int *n) {
  TAct *r;
  size_t i;
  *n = 0;
  if (a == NULL || a->type != J_ARR || a->n == 0) return NULL;
  r = (TAct *)xmalloc(a->n * sizeof(TAct));
  for (i = 0; i < a->n; i++) {
    r[i].command = xstrdup(json_str(json_get(a->kid[i], "command"), ""));
    r[i].title = xstrdup(json_str(json_get(a->kid[i], "title"), ""));
    r[i].icon = xstrdup(json_str(json_get(a->kid[i], "icon"), ""));
    r[i].inline_ = json_bool(json_get(a->kid[i], "inline"), 0);
  }
  *n = (int)a->n;
  return r;
}


static TNode *find (TNode *n, const char *handle) {
  int i;
  if (n->handle && strcmp(n->handle, handle) == 0) return n;
  for (i = 0; i < n->nkid; i++) {
    TNode *r = find(n->kid[i], handle);
    if (r) return r;
  }
  return NULL;
}


static int view_of (const char *id) {
  int i;
  for (i = 0; id && i < g_nv; i++)
    if (strcmp(g_v[i].id, id) == 0) return i;
  return -1;
}


/* ------------------------------------------------------------------ what the host says */

/* mme/treeViews: every tree view of the running extensions; the open ones stay open */
void tree_views (const Json *views) {
  TView *old = g_v;
  int nold = g_nv, i, j;
  size_t k;
  g_v = NULL;
  g_nv = 0;
  if (views && views->type == J_ARR && views->n) {
    g_v = (TView *)zalloc(views->n * sizeof(TView));
    for (k = 0; k < views->n; k++) {
      TView *v = &g_v[g_nv++];
      v->id = xstrdup(json_str(json_get(views->kid[k], "id"), ""));
      v->name = xstrdup(json_str(json_get(views->kid[k], "name"), ""));
      v->title = xstrdup(json_str(json_get(views->kid[k], "title"), ""));
      v->head = xstrdup(v->name);
      for (i = 0; v->head[i]; i++)
        if (v->head[i] >= 'a' && v->head[i] <= 'z') v->head[i] = (char)(v->head[i] - 32);
      v->open = 1;
      for (j = 0; j < nold; j++)
        if (strcmp(old[j].id, v->id) == 0) v->open = old[j].open;
    }
  }
  for (i = 0; i < nold; i++) {
    free(old[i].id);
    free(old[i].name);
    free(old[i].title);
    free(old[i].head);
    free(old[i].msg);
    free(old[i].desc);
    acts_free(old[i].tact, old[i].ntact);
    node_clear(&old[i].root);
  }
  free(old);
  g_sel = g_top = 0;
  g_st[0].sel = g_st[0].top = g_st[1].sel = g_st[1].top = 0;
  g_st[1].scope = -1;
  scr_redraw();
}


int tree_count (void) {
  return g_nv;
}


int tree_side_count (void) {
  int v, n = 0;
  for (v = 0; v < g_nv; v++) n += !view_in_panel(MV_N + v);
  return n;
}


const char *tree_id (int k) {
  return k >= 0 && k < g_nv ? g_v[k].id : NULL;
}


const char *tree_name (int k) {
  return k >= 0 && k < g_nv ? g_v[k].name : "";
}


const char *tree_title (int k) {
  return k >= 0 && k < g_nv ? g_v[k].head : "";
}


void tree_scope (int k) {
  int from = g_scope >= 0, to = k >= 0;
  if (k == g_scope) return;
  g_st[from].scope = g_scope;	/* the one left keeps its selection */
  g_st[from].sel = g_sel;
  g_st[from].top = g_top;
  g_st[from].h = g_h;
  g_st[from].selv = g_selv;
  g_st[from].selh = g_selh;
  g_scope = k;
  if (to && g_st[1].scope != k) {	/* another view in the panel: from its top */
    free(g_st[1].selh);
    g_st[1].selh = NULL;
    g_st[1].sel = g_st[1].top = 0;
    g_st[1].selv = -1;
  }
  g_sel = g_st[to].sel;
  g_top = g_st[to].top;
  g_h = g_st[to].h;
  g_selv = g_st[to].selv;
  g_selh = g_st[to].selh;
  g_st[to].selh = NULL;
}


/* mme/treeReady: the extension made its view (it may have been asked before it was there) */
void tree_ready (const Json *p) {
  int v = view_of(json_str(json_get(p, "view"), NULL));
  if (v >= 0 && g_v[v].asked) ask(v, &g_v[v].root, 1);
}


/* mme/treeInfo: its message (shown when it has nothing), description, badge */
void tree_info (const Json *p) {
  int v = view_of(json_str(json_get(p, "view"), NULL));
  if (v < 0) return;
  free(g_v[v].msg);
  free(g_v[v].desc);
  g_v[v].msg = xstrdup(json_str(json_get(p, "message"), ""));
  g_v[v].desc = xstrdup(json_str(json_get(p, "description"), ""));
  g_v[v].badge = inum(json_get(p, "badge"), 0);
  scr_redraw();
}


/* mme/treeItems: the children of a node (handle null: the view's own) */
void tree_items (const Json *p) {
  int v = view_of(json_str(json_get(p, "view"), NULL));
  const Json *items = json_get(p, "items"), *par = json_get(p, "parent");
  TNode *n;
  size_t i;
  if (v < 0) return;
  if (par == NULL || par->type == J_NULL) {
    n = &g_v[v].root;
    free(g_v[v].msg);
    g_v[v].msg = xstrdup(json_str(json_get(p, "message"), ""));
    if (json_get(p, "titleActions")) {
      acts_free(g_v[v].tact, g_v[v].ntact);
      g_v[v].tact = acts_of(json_get(p, "titleActions"), &g_v[v].ntact);
    }
  }
  else if ((n = find(&g_v[v].root, json_str(par, ""))) == NULL) return;
  {	/* its children again: the ones of before go (and what was under them) */
    int k;
    for (k = 0; k < n->nkid; k++) {
      node_clear(n->kid[k]);
      free(n->kid[k]);
    }
    free(n->kid);
    n->kid = NULL;
    n->nkid = 0;
  }
  n->loaded = 1;
  if (items && items->type == J_ARR && items->n) {
    n->kid = (TNode **)xmalloc(items->n * sizeof(TNode *));
    for (i = 0; i < items->n; i++) {
      const Json *it = items->kid[i];
      TNode *c = (TNode *)zalloc(sizeof(TNode));
      c->handle = xstrdup(json_str(json_get(it, "handle"), ""));
      c->label = xstrdup(json_str(json_get(it, "label"), ""));
      c->desc = xstrdup(json_str(json_get(it, "description"), ""));
      c->tip = xstrdup(json_str(json_get(it, "tooltip"), ""));
      c->icon = xstrdup(json_str(json_get(it, "icon"), ""));
      c->file = xstrdup(json_str(json_get(it, "file"), ""));
      c->folder = json_bool(json_get(it, "folder"), 0);
      c->coll = inum(json_get(it, "collapsible"), 0);
      c->command = json_bool(json_get(it, "command"), 0);
      c->act = acts_of(json_get(it, "actions"), &c->nact);
      n->kid[n->nkid++] = c;
      if (c->coll == 2) ask(v, c, 1);	/* open (VS Code's Expanded, or open before): its children too */
    }
  }
  scr_redraw();
}


/* mme/treeRefresh: a node's children (handle null: the view's) changed */
void tree_refresh (const Json *p) {
  int v = view_of(json_str(json_get(p, "view"), NULL));
  const Json *h = json_get(p, "handle");
  TNode *n;
  if (v < 0 || !g_v[v].asked) return;
  if (h == NULL || h->type == J_NULL) n = &g_v[v].root;
  else if ((n = find(&g_v[v].root, json_str(h, ""))) == NULL) return;
  if (n == &g_v[v].root || n->coll == 2) ask(v, n, 1);
  else n->loaded = 0;	/* closed: asked when it is opened */
}


/* mme/treeReveal: treeView.reveal: the node selected, the view shown */
void tree_reveal (const Json *p) {
  int v = view_of(json_str(json_get(p, "view"), NULL));
  const char *h = json_str(json_get(p, "handle"), NULL);
  if (v < 0 || h == NULL) return;
  g_v[v].open = 1;
  tree_scope(view_in_panel(MV_N + v) ? v : -1);	/* where it is */
  free(g_selh);
  g_selh = xstrdup(h);
  g_selv = v;
  mme_show_trees(v);
}


/* ------------------------------------------------------------------ the rows */

static void row_add (int kind, int view, int depth, TNode *n) {
  if (g_nrow == g_caprow) {
    g_caprow = g_caprow ? g_caprow * 2 : 64;
    g_row = (TRow *)xrealloc(g_row, (size_t)g_caprow * sizeof(TRow));
  }
  g_row[g_nrow].kind = kind;
  g_row[g_nrow].view = view;
  g_row[g_nrow].depth = depth;
  g_row[g_nrow].n = n;
  g_nrow++;
}


static void rows_under (int v, TNode *n, int depth) {
  int i;
  for (i = 0; i < n->nkid; i++) {
    TNode *c = n->kid[i];
    row_add(TR_NODE, v, depth, c);
    if (c->coll == 2) rows_under(v, c, depth + 1);
  }
}


static void rows (void) {
  int v, i;
  g_nrow = 0;
  for (v = 0; v < g_nv; v++) {
    if (g_scope >= 0 ? v != g_scope : view_in_panel(MV_N + v)) continue;	/* in the panel / the side bar */
    row_add(TR_HEAD, v, 0, NULL);
    if (!g_v[v].open) continue;
    if (!g_v[v].asked) {	/* shown for the first time: its root is asked for */
      g_v[v].asked = 1;
      ask(v, &g_v[v].root, 1);
    }
    if (g_v[v].root.nkid == 0) row_add(TR_MSG, v, 0, NULL);
    else rows_under(v, &g_v[v].root, 0);
  }
  if (g_selh) {	/* the node selected before, where it is now */
    for (i = 0; i < g_nrow; i++)
      if (g_row[i].kind == TR_NODE && g_row[i].view == g_selv && strcmp(g_row[i].n->handle, g_selh) == 0) {
        g_sel = i;
        break;
      }
  }
  if (g_sel >= g_nrow) g_sel = g_nrow - 1;
  if (g_sel < 0) g_sel = 0;
}


static void remember (void) {	/* the selected row's handle */
  free(g_selh);
  g_selh = NULL;
  g_selv = -1;
  if (g_sel < g_nrow && g_row[g_sel].kind == TR_NODE) {
    g_selh = xstrdup(g_row[g_sel].n->handle);
    g_selv = g_row[g_sel].view;
  }
}


/* ------------------------------------------------------------------ drawing */

static void head_text (const TView *v, char *t, size_t n) {
  size_t i;
  if (v->title[0] && strcmp(v->title, v->name) != 0) snprintf(t, n, "%s: %s", v->title, v->name);
  else snprintf(t, n, "%s", v->name);
  for (i = 0; t[i]; i++)
    if (t[i] >= 'a' && t[i] <= 'z') t[i] = (char)(t[i] - 32);
}


void tree_draw (int x, int y, int w, int h, int focus) {
  int row;
  scr_box(x, y, w, h, S_SIDE);
  if (h <= HEAD) return;
  scr_puts(x + 2, y, "EXTENSION VIEWS", S_SIDE_HEAD);
  g_dx = x;
  g_dy = y;
  rows();
  g_h = h - HEAD;
  if (g_sel < g_top) g_top = g_sel;
  if (g_sel >= g_top + g_h) g_top = g_sel - g_h + 1;
  if (g_top < 0) g_top = 0;
  if (g_nv == 0) {
    scr_putsw(x + 2, y + HEAD, w - 3, "No extension has a view here. Running extensions with tree views", S_SIDE_DIM);
    scr_putsw(x + 2, y + HEAD + 1, w - 3, "(npm scripts, Docker, GitLens ...) show them here.", S_SIDE_DIM);
    return;
  }
  for (row = 0; row < g_h; row++) {
    int k = g_top + row, sy = y + HEAD + row, st, cx = x + 1, right = x + w - 1, i;
    const TRow *r;
    if (k >= g_nrow) break;
    r = &g_row[k];
    st = (k == g_sel && focus) ? S_SIDE_SEL : (k == g_sel ? S_SIDE_CUR : S_SIDE);
    scr_fill(x, sy, w, st);
    if (r->kind == TR_HEAD) {
      const TView *v = &g_v[r->view];
      char t[300];
      head_text(v, t, sizeof(t));
      for (i = v->ntact - 1; i >= 0 && v->open && right - 2 > x + 12; i--)	/* the title's actions with icons, at the right */
        if (v->tact[i].icon[0]) {
          right -= 2;
          scr_puts(right, sy, v->tact[i].icon, st == S_SIDE ? S_SIDE_HEAD : st);
        }
      cx += scr_put(cx, sy, v->open ? 0xEAB4 : 0xEAB6, st == S_SIDE ? S_SIDE_HEAD : st) + 1;
      cx += scr_putsw(cx, sy, right - cx - 1, t, st == S_SIDE ? S_SIDE_TITLE : st) + 1;
      if (v->badge > 0 && cx < right - 4) {
        char b[16];
        snprintf(b, sizeof(b), "%d", v->badge);
        scr_putsw(cx, sy, right - cx - 1, b, st == S_SIDE ? S_SIDE_DIM : st);
      }
      continue;
    }
    if (r->kind == TR_MSG) {
      const TView *v = &g_v[r->view];
      scr_putsw(x + 3, sy, w - 4, v->msg && v->msg[0] ? v->msg : v->asked && v->root.loaded ? "" : "Loading...", st == S_SIDE ? S_SIDE_DIM : st);
      continue;
    }
    {
      const TNode *n = r->n;
      cx += 2 + r->depth * 2;
      if (n->coll) cx += scr_put(cx, sy, n->coll == 2 ? 0xEAB4 : 0xEAB6, st) + 1;	/* chevron-down, chevron-right */
      else cx += 2;
      if (n->icon[0]) cx += scr_putsw(cx, sy, 2, n->icon, st) + 1;
      else if (n->file[0]) {
        int ist;
        uint32_t ic = n->folder ? 0xEA83 : file_icon(n->file, &ist);	/* codicon folder, or Seti's */
        cx += scr_put(cx, sy, ic, st == S_SIDE && !n->folder ? ist : st) + 1;
      }
      if (k == g_sel && n->nact > 0 && right - 2 > cx + 4) {	/* its menu: ... on the selected row */
        right -= 2;
        scr_put(right, sy, 0xEA7C, st);	/* codicon ellipsis */
      }
      cx += scr_putsw(cx, sy, right - cx - 1, n->label, st) + 1;
      if (n->desc[0] && cx < right - 2) scr_putsw(cx, sy, right - cx - 1, n->desc, st == S_SIDE ? S_SIDE_DIM : st);
    }
  }
  side_bar(x, y + HEAD, w, g_h, (size_t)g_nrow, (size_t)g_top, (size_t)g_h);
}


/* ------------------------------------------------------------------ doing */

static void toggle_head (int v) {
  g_v[v].open = !g_v[v].open;
  send("mme/treeExpand", v, NULL, g_v[v].open ? ",\"expanded\":true" : ",\"expanded\":false");
  if (g_v[v].open) g_v[v].asked = 1;
}


static void toggle_node (int v, TNode *n) {
  if (n->coll == 0) return;
  n->coll = n->coll == 2 ? 1 : 2;
  ask(v, n, n->coll == 2);
}


static void choose (int k) {	/* a row chosen: it is selected there, its command runs */
  const TRow *r;
  if (k < 0 || k >= g_nrow) return;
  r = &g_row[k];
  if (r->kind == TR_HEAD) toggle_head(r->view);
  else if (r->kind == TR_NODE) {
    if (r->n->coll && !r->n->command) toggle_node(r->view, r->n);
    else send("mme/treeSelect", r->view, r->n->handle, ",\"run\":true");
  }
}


static void run_act (int v, const TNode *n, const TAct *a) {
  Buf m;
  buf_init(&m);
  buf_puts(&m, ",\"command\":");
  json_put_str(&m, a->command, strlen(a->command));
  buf_putc(&m, '\0');
  send("mme/treeAction", v, n ? n->handle : NULL, m.s);
  buf_free(&m);
}


/* the menu of row k (a view's title: its actions; a node: its context menu), at x, y */
static void row_menu (int k, int x, int y) {
  const TRow *r;
  const TAct *a;
  const char *label[64], *keys[64];
  int n, i, pick;
  if (k < 0 || k >= g_nrow) return;
  r = &g_row[k];
  if (r->kind == TR_MSG) return;
  if (r->kind == TR_HEAD) {
    a = g_v[r->view].tact;
    n = g_v[r->view].ntact;
  }
  else {
    a = r->n->act;
    n = r->n->nact;
    send("mme/treeSelect", r->view, r->n->handle, ",\"run\":false");	/* what the menu's command is for */
  }
  if (n == 0) {
    toast(0, "There are no actions here");
    return;
  }
  if (n > 64) n = 64;
  for (i = 0; i < n; i++) {
    label[i] = a[i].title;
    keys[i] = "";
  }
  pick = context_menu(x, y, label, keys, n);
  if (pick >= 0 && pick < n) run_act(r->view, r->kind == TR_NODE ? r->n : NULL, &a[pick]);
}


int tree_key (int k, SideAct *act) {
  int code = KEY_CODE(k);
  const TRow *r;
  act->what = SA_NONE;
  rows();
  if (g_nrow == 0) return 0;
  r = &g_row[g_sel];
  switch (code) {
    case K_UP: if (g_sel > 0) g_sel--; remember(); return 1;
    case K_DOWN: if (g_sel + 1 < g_nrow) g_sel++; remember(); return 1;
    case K_PGUP: g_sel = g_sel > g_h ? g_sel - g_h : 0; remember(); return 1;
    case K_PGDN: g_sel = g_sel + g_h < g_nrow ? g_sel + g_h : g_nrow - 1; remember(); return 1;
    case K_HOME: g_sel = 0; remember(); return 1;
    case K_END: g_sel = g_nrow - 1; remember(); return 1;
    case K_LEFT:
      if (r->kind == TR_HEAD) {
        if (g_v[r->view].open) toggle_head(r->view);
      }
      else if (r->kind == TR_NODE && r->n->coll == 2) toggle_node(r->view, r->n);
      else {	/* to its parent */
        int d = r->depth, i;
        for (i = g_sel - 1; i >= 0; i--)
          if (g_row[i].view != r->view || g_row[i].kind == TR_HEAD || g_row[i].depth < d) break;
        if (i >= 0) g_sel = i;
        remember();
      }
      return 1;
    case K_RIGHT:
      if (r->kind == TR_HEAD && !g_v[r->view].open) toggle_head(r->view);
      else if (r->kind == TR_NODE && r->n->coll == 1) toggle_node(r->view, r->n);
      return 1;
    case K_ENTER: case ' ':
      if (r->kind == TR_NODE && r->n->coll && r->n->command && code == K_ENTER && (k & KM_CTRL)) toggle_node(r->view, r->n);
      else choose(g_sel);
      remember();
      return 1;
    case K_F10:
      if (k & KM_SHIFT) {	/* Shift+F10: the row's menu */
        row_menu(g_sel, (g_scope >= 0 ? g_dx : 0) + side_width() / 2, (g_scope >= 0 ? g_dy - 1 : 0) + g_sel - g_top + HEAD + 2);
        return 1;
      }
      break;
  }
  return 0;
}


void tree_click (int row, int col, SideAct *act) {
  int w = side_width(), k;
  const TRow *r;
  act->what = SA_NONE;
  if (row < HEAD) return;
  rows();
  k = g_top + row - HEAD;
  if (k >= g_nrow) return;
  g_sel = k;
  remember();
  r = &g_row[k];
  if (r->kind == TR_HEAD) {
    const TView *v = &g_v[r->view];
    int right = w - 1, i;
    for (i = v->ntact - 1; i >= 0 && v->open && right - 2 > 12; i--)	/* a title action's icon */
      if (v->tact[i].icon[0]) {
        right -= 2;
        if (col >= right && col <= right + 1) {
          run_act(r->view, NULL, &v->tact[i]);
          return;
        }
      }
    toggle_head(r->view);
    return;
  }
  if (r->kind != TR_NODE) return;
  if (r->n->nact > 0 && col >= w - 3) {	/* its ... */
    row_menu(k, col, row + 2);
    return;
  }
  if (r->n->coll && col <= 3 + r->depth * 2) {	/* its chevron */
    toggle_node(r->view, r->n);
    return;
  }
  choose(k);
}


/* the right button on a row: its menu */
void tree_menu (int row, int x, int y) {
  int k;
  if (row < HEAD) return;
  rows();
  k = g_top + row - HEAD;
  if (k >= g_nrow) return;
  g_sel = k;
  remember();
  row_menu(k, x, y);
}


void tree_scroll_to (size_t top) {	/* its scrollbar dragged */
  rows();
  g_top = (int)top;
  if (g_top > g_nrow - g_h) g_top = g_nrow - g_h;
  if (g_top < 0) g_top = 0;
  if (g_sel < g_top) g_sel = g_top;
  if (g_sel >= g_top + g_h) g_sel = g_top + g_h - 1;
  remember();
}


void tree_wheel (int d) {
  rows();
  g_top += d * wheel_step(0);
  if (g_top > g_nrow - g_h) g_top = g_nrow - g_h;
  if (g_top < 0) g_top = 0;
  if (g_sel < g_top) g_sel = g_top;
  if (g_sel >= g_top + g_h) g_sel = g_top + g_h - 1;
  remember();
}


/* the badges of the views together, for the activity bar (the side bar's) */
int tree_badge (void) {
  int v, n = 0;
  for (v = 0; v < g_nv; v++)
    if (!view_in_panel(MV_N + v)) n += g_v[v].badge;
  return n;
}


/* the side bar's view of the selected row, -1 none */
int tree_focused (void) {
  int was = g_scope, v = -1;
  tree_scope(-1);
  rows();
  if (g_sel < g_nrow) v = g_row[g_sel].view;
  tree_scope(was);
  return v;
}
