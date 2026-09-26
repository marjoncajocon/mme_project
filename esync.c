/*
** esync.c - Settings Sync, as VS Code's: settings.json, keybindings.json
** and the snippets kept the same on every machine, through a secret GitHub
** Gist of the account "GitHub: Sign In" knows (its token needs the gist
** scope). The gist is found by its description, so a second machine
** signed in to the same account finds the first one's.
**
** Each file has two bases in mme-data/sync/base: what it was here, and
** what it was on GitHub, when last in sync. A side that differs from its
** base was changed there: the other side takes it. Both changed is a
** conflict: Sync Now asks which to keep (a sync of its own, in the
** background, only says so). Comparing each side with its own base, a
** file GitHub stores with other line ends is not taken for a change.
**
** It syncs when turned on, at start, a moment after one of the files is
** saved (or a setting changed in the Settings editor), and on Sync Now.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define DESC	"mme Settings Sync"	/* the gist's description: how another machine finds it */

enum { ST_NONE, ST_LIST, ST_GET, ST_PUT };

typedef struct {
  char *name;	/* the gist's file: settings.json, keybindings.json, snippets.c.json */
  char *l, *r, *bl, *br;	/* here, on GitHub, and their bases; NULL: not there */
  size_t ll, rl, bll, brl;
  int act;	/* A_* */
} Item;

enum { A_NONE, A_PULL, A_PUSH, A_SKIP };

static struct {
  GhReq *req;
  int st;	/* ST_*: what the request is */
  int inter;	/* Sync Now: it asks, and says what went wrong */
  long long due;	/* a sync of its own at this time (os_now_us), 0: none */
  char gist[64];
  Item *it;
  int n;
  int pulled;	/* what was written here: 1 settings, 2 keybindings, 4 snippets */
  int npull, npush;	/* files taken from GitHub, sent to it */
  int conflicts;	/* left for Sync Now */
} S;


/*
** {==================================================================
** Its files
** ===================================================================
*/

/* mme-data/sync/name (sub: in base/), the folders made */
static char *state_path (const char *name, int sub) {
  char *dir = data_path("sync"), *base, *p;
  os_mkdir(dir);
  if (!sub) {
    p = path_join(dir, name);
    free(dir);
    return p;
  }
  base = path_join(dir, "base");
  os_mkdir(base);
  p = path_join(base, name);
  free(base);
  free(dir);
  return p;
}


static int put_file (const char *path, const char *s, size_t n) {
  int fd = os_open(path, OS_WRITE);
  if (fd < 0) return -1;
  if (n) os_write(fd, s, n);
  os_close(fd);
  return 0;
}


static char *get_file (const char *path, size_t *n) {
  char *s = read_file(path, n);
  if (s && *n == 0) {	/* empty: as not there (a gist has no empty file) */
    free(s);
    s = NULL;
  }
  return s;
}


int sync_on (void) {
  char *f = state_path("on", 0);
  OsStat st;
  int on = os_stat(f, &st) == 0 && st.exists;
  free(f);
  return on;
}


static void set_on (int on) {
  char *f = state_path("on", 0);
  if (on) put_file(f, "1\n", 2);
  else os_unlink(f);
  free(f);
}


static void load_gist_id (void) {
  char *f = state_path("gist", 0), *s;
  size_t n;
  S.gist[0] = '\0';
  if ((s = read_file(f, &n)) != NULL) {
    snprintf(S.gist, sizeof(S.gist), "%.*s", (int)strcspn(s, "\r\n \t"), s);
    free(s);
  }
  free(f);
}


static void save_gist_id (const char *id) {
  char *f = state_path("gist", 0);
  snprintf(S.gist, sizeof(S.gist), "%s", id ? id : "");
  if (id && *id) put_file(f, id, strlen(id));
  else os_unlink(f);
  free(f);
}


/* where a gist's file is here: NULL when it is not one of the synced */
static char *local_path (const char *name) {
  if (strcmp(name, "settings.json") == 0) return settings_path();
  if (strcmp(name, "keybindings.json") == 0) return keys_path();
  if (strncmp(name, "snippets.", 9) == 0 && name[9] && !strchr(name + 9, '/') && !strchr(name + 9, '\\') &&
      strcmp(name + 9, "..") != 0) {
    char *d = snip_dir(), *p;
    if (d == NULL) return NULL;
    os_mkdir(d);
    p = path_join(d, name + 9);
    free(d);
    return p;
  }
  return NULL;
}


static int is_snippets (const char *f) {
  size_t n = strlen(f);
  return (n > 5 && strcmp(f + n - 5, ".json") == 0) || (n > 14 && strcmp(f + n - 14, ".code-snippets") == 0);
}

/* }================================================================== */


/*
** {==================================================================
** Merging
** ===================================================================
*/

static void items_free (void) {
  int i;
  for (i = 0; i < S.n; i++) {
    free(S.it[i].name);
    free(S.it[i].l);
    free(S.it[i].r);
    free(S.it[i].bl);
    free(S.it[i].br);
  }
  free(S.it);
  S.it = NULL;
  S.n = 0;
}


static Item *item (const char *name) {
  int i;
  for (i = 0; i < S.n; i++)
    if (strcmp(S.it[i].name, name) == 0) return &S.it[i];
  S.it = (Item *)xrealloc(S.it, (size_t)(S.n + 1) * sizeof(Item));
  memset(&S.it[S.n], 0, sizeof(Item));
  S.it[S.n].name = xstrdup(name);
  return &S.it[S.n++];
}


static int same (const char *a, size_t an, const char *b, size_t bn) {
  if (a == NULL || b == NULL) return a == b;
  return an == bn && memcmp(a, b, an) == 0;
}


/* every file: here (the synced ones there are), on GitHub (files: the gist's, or NULL), and the bases */
static void gather (const Json *files) {
  static const char *const fixed[] = {"settings.json", "keybindings.json"};
  char *d, *f;
  Vec v;
  size_t i;
  items_free();
  for (i = 0; i < 2; i++) item(fixed[i]);
  if ((d = snip_dir()) != NULL) {
    vec_init(&v);
    if (os_listdir(d, &v) == 0) {
      for (i = 0; i < v.n; i++) {
        if (is_snippets(v.v[i])) {
          char name[300];
          snprintf(name, sizeof(name), "snippets.%s", v.v[i]);
          item(name);
        }
      }
    }
    vec_free(&v);
    free(d);
  }
  for (i = 0; files && i < files->n; i++) {
    const Json *k = files->kid[i];
    char *lp = local_path(k->key);
    if (lp == NULL) continue;	/* not a file mme syncs */
    free(lp);
    item(k->key)->r = NULL;
  }
  d = state_path("", 1);
  vec_init(&v);
  if (os_listdir(d, &v) == 0) {	/* a file gone from both sides still has its bases */
    for (i = 0; i < v.n; i++) {
      size_t n = strlen(v.v[i]);
      if (n > 2 && strcmp(v.v[i] + n - 2, ".l") == 0) {
        char name[300];
        snprintf(name, sizeof(name), "%.*s", (int)(n - 2), v.v[i]);
        if ((f = local_path(name)) != NULL) {
          free(f);
          item(name);
        }
      }
    }
  }
  vec_free(&v);
  free(d);
  for (i = 0; i < (size_t)S.n; i++) {
    Item *it = &S.it[i];
    char b[320];
    if ((f = local_path(it->name)) != NULL) {
      it->l = get_file(f, &it->ll);
      free(f);
    }
    snprintf(b, sizeof(b), "%s.l", it->name);
    f = state_path(b, 1);
    it->bl = get_file(f, &it->bll);
    free(f);
    snprintf(b, sizeof(b), "%s.r", it->name);
    f = state_path(b, 1);
    it->br = get_file(f, &it->brl);
    free(f);
    if (files) {
      size_t j;
      for (j = 0; j < files->n; j++) {
        const Json *k = files->kid[j];
        if (k->key && strcmp(k->key, it->name) == 0) {
          const Json *c = json_get(k, "content");
          if (json_get(k, "truncated") && json_get(k, "truncated")->b) {	/* over 1 MB: left as it is */
            out_log("Settings Sync", "%s is too big to sync (over 1 MB on GitHub): left as it is", it->name);
            it->act = A_SKIP;
          }
          else if (c && c->str && c->len) {
            it->rl = c->len;
            it->r = (char *)xmalloc(c->len + 1);
            memcpy(it->r, c->str, c->len + 1);
          }
        }
      }
    }
  }
}


static void keep_base (Item *it, const char *l, size_t ll, const char *r, size_t rl) {
  char b[320], *f;
  snprintf(b, sizeof(b), "%s.l", it->name);
  f = state_path(b, 1);
  if (l) put_file(f, l, ll);
  else os_unlink(f);
  free(f);
  snprintf(b, sizeof(b), "%s.r", it->name);
  f = state_path(b, 1);
  if (r) put_file(f, r, rl);
  else os_unlink(f);
  free(f);
}


/* GitHub's copy written here (NULL: the file goes; settings.json and keybindings.json never do) */
static void pull (Item *it) {
  char *f = local_path(it->name);
  if (f == NULL) return;
  if (it->r) put_file(f, it->r, it->rl);
  else if (strncmp(it->name, "snippets.", 9) == 0) os_unlink(f);
  free(f);
  out_log("Settings Sync", "%s: %s from GitHub", it->name, it->r ? "updated" : "deleted");
  S.pulled |= strcmp(it->name, "settings.json") == 0 ? 1 : strcmp(it->name, "keybindings.json") == 0 ? 2 : 4;
  S.npull++;
}


/* a conflict: which one stays (Sync Now asks; one of its own leaves it) */
static int resolve (Item *it) {
  static const char *const btn[] = {"Keep This Machine's", "Take GitHub's", "Skip"};
  char msg[400];
  int c;
  if (!S.inter) {
    S.conflicts++;
    return A_SKIP;
  }
  if (it->bl == NULL && it->br == NULL)
    snprintf(msg, sizeof(msg), "Settings Sync: %s is not the same here and on GitHub.", it->name);
  else if (it->l == NULL) snprintf(msg, sizeof(msg), "Settings Sync: %s was deleted here and changed on another machine.", it->name);
  else if (it->r == NULL) snprintf(msg, sizeof(msg), "Settings Sync: %s was changed here and deleted on another machine.", it->name);
  else snprintf(msg, sizeof(msg), "Settings Sync: %s was changed both here and on another machine.", it->name);
  c = dialog(msg, "Keep one of the two: the other is replaced by it.", btn, 3);
  return c == 0 ? A_PUSH : c == 1 ? A_PULL : A_SKIP;
}


/*
** What each file needs, the pulls done; the pushes as the body of the
** request that makes (or updates) the gist, or NULL: nothing to send
*/
static char *merge (void) {
  Buf b;
  int i, pushes = 0;
  S.pulled = S.conflicts = S.npull = S.npush = 0;
  buf_init(&b);
  buf_puts(&b, "{\"description\": \"" DESC "\"");
  if (S.gist[0] == '\0') buf_puts(&b, ", \"public\": false");
  buf_puts(&b, ", \"files\": {");
  for (i = 0; i < S.n; i++) {
    Item *it = &S.it[i];
    int lchg = !same(it->l, it->ll, it->bl, it->bll), rchg = !same(it->r, it->rl, it->br, it->brl);
    if (it->act == A_SKIP) continue;
    if (it->l == NULL && it->r == NULL) it->act = A_NONE;
    else if (it->bl == NULL && it->br == NULL)	/* never synced: the side that has it, or a question */
      it->act = it->r == NULL ? A_PUSH : it->l == NULL ? A_PULL : same(it->l, it->ll, it->r, it->rl) ? A_NONE : resolve(it);
    else if (lchg && rchg) it->act = same(it->l, it->ll, it->r, it->rl) ? A_NONE : resolve(it);
    else if (lchg) it->act = A_PUSH;
    else if (rchg) it->act = A_PULL;
    else it->act = A_NONE;
    if (it->act == A_PULL && it->r == NULL && strncmp(it->name, "snippets.", 9) != 0)
      it->act = it->l ? A_PUSH : A_NONE;	/* settings.json is not deleted here: GitHub gets it back */
    if (it->act == A_PULL) pull(it);
    if (it->act == A_PUSH) {
      if (it->l == NULL && S.gist[0] == '\0') {	/* nothing to delete in a gist not made yet */
        it->act = A_NONE;
        continue;
      }
      buf_puts(&b, pushes++ ? ", " : "");
      json_put_str(&b, it->name, strlen(it->name));
      if (it->l) {
        buf_puts(&b, ": {\"content\": ");
        json_put_str(&b, it->l, it->ll);
        buf_putc(&b, '}');
      }
      else buf_puts(&b, ": null");
      out_log("Settings Sync", "%s: %s on GitHub", it->name, it->l ? "sent" : "deleted");
    }
  }
  buf_puts(&b, "}}");
  S.npush = pushes;
  if (S.pulled) user_files_changed(S.pulled & 1, S.pulled & 2, S.pulled & 4);
  if (pushes == 0) {
    buf_free(&b);
    return NULL;
  }
  buf_putc(&b, '\0');
  return b.s;
}


/* in sync: the bases are what both sides have now (files: the gist as it was sent back, or NULL) */
static void commit (const Json *files) {
  int i;
  for (i = 0; i < S.n; i++) {
    Item *it = &S.it[i];
    const char *r = it->r;
    size_t rl = it->rl;
    if (it->act == A_SKIP) continue;
    if (it->act == A_PUSH) {
      const Json *c = NULL;
      size_t j;
      r = it->l;
      rl = it->ll;
      for (j = 0; files && j < files->n; j++)	/* GitHub's copy, as it keeps it */
        if (files->kid[j]->key && strcmp(files->kid[j]->key, it->name) == 0) c = json_get(files->kid[j], "content");
      if (c && c->str && c->len) {
        r = c->str;
        rl = c->len;
      }
      keep_base(it, it->l, it->ll, it->l ? r : NULL, it->l ? rl : 0);
    }
    else if (it->act == A_PULL) keep_base(it, r, rl, r, rl);
    else keep_base(it, it->l, it->ll, r, rl);
  }
}

/* }================================================================== */


/*
** {==================================================================
** Talking to GitHub
** ===================================================================
*/

static void ask_done (void *ud, int choice) {
  (void)ud;
  if (choice == 0) sync_command(CMD_SYNC_NOW);
}


static void finish (int ok) {
  if (ok && S.inter) {
    if (S.npush == 0 && S.npull == 0) toast(0, "Settings Sync: in sync, nothing changed.");
    else if (S.npull == 0) toast(0, "Settings Sync: in sync, %d file%s sent to GitHub.", S.npush, S.npush == 1 ? "" : "s");
    else if (S.npush == 0) toast(0, "Settings Sync: in sync, %d file%s from GitHub applied.", S.npull, S.npull == 1 ? "" : "s");
    else toast(0, "Settings Sync: in sync, %d file%s sent to GitHub, %d from it applied.", S.npush, S.npush == 1 ? "" : "s",
               S.npull);
  }
  else if (ok && S.npull)
    toast(0, "Settings Sync: %d file%s changed on another machine applied.", S.npull, S.npull == 1 ? "" : "s");
  if (ok && S.conflicts) {
    static const char *const act[] = {"Sync Now"};
    char msg[160];
    snprintf(msg, sizeof(msg), "%d file%s changed both here and on another machine.", S.conflicts,
             S.conflicts == 1 ? " was" : "s were");
    toast_ask(1, "Settings Sync", msg, act, 1, ask_done, NULL);
    S.conflicts = 0;
  }
  items_free();
  S.st = ST_NONE;
}


static int begin (int st, const char *method, const char *path, const char *body) {
  S.st = st;
  S.req = gh_start(method, path, body);
  if (S.req == NULL) {
    finish(0);
    return -1;
  }
  return 0;
}


static void failed (int status, const Buf *out, const char *what) {
  out_log("Settings Sync", "%s: HTTP %d %.200s", what, status, out->s ? out->s : "");
  if (S.inter || status == 401) {
    if ((status == 404 || status == 403) && S.st == ST_PUT)
      toast(1, "Settings Sync: GitHub refused the gist (the token needs the \"gist\" scope: GitHub: Sign In with one).");
    else gh_fail(status, out);
  }
  finish(0);
}


static void send (const char *body) {
  char path[120];
  if (S.gist[0]) {
    snprintf(path, sizeof(path), "/gists/%s", S.gist);
    begin(ST_PUT, "PATCH", path, body);
  }
  else begin(ST_PUT, "POST", "/gists", body);
}


static void merged (const Json *files) {
  char *body;
  gather(files);
  body = merge();
  if (body == NULL) {
    commit(NULL);
    finish(1);
    return;
  }
  send(body);
  free(body);
}


static void answer (int status, Buf *out) {
  Json *j = out->s ? json_parse(out->s, out->len) : NULL;
  size_t i;
  switch (S.st) {
    case ST_LIST:	/* the account's gists: the one of Settings Sync */
      if (status != 200) {
        failed(status, out, "GET /gists");
        break;
      }
      for (i = 0; j && i < j->n; i++)
        if (strcmp(json_str(json_get(j->kid[i], "description"), ""), DESC) == 0) {
          char path[120];
          save_gist_id(json_str(json_get(j->kid[i], "id"), ""));
          out_log("Settings Sync", "found the gist %s", S.gist);
          snprintf(path, sizeof(path), "/gists/%s", S.gist);
          begin(ST_GET, "GET", path, NULL);
          json_free(j);
          return;
        }
      merged(NULL);	/* none yet: this machine's files make it */
      break;
    case ST_GET:
      if (status == 404) {	/* deleted on GitHub: made again */
        out_log("Settings Sync", "the gist %s is gone: a new one is made", S.gist);
        save_gist_id(NULL);
        merged(NULL);
      }
      else if (status != 200) failed(status, out, "GET /gists/id");
      else merged(json_get(j, "files"));
      break;
    case ST_PUT:
      if (status != 200 && status != 201) {
        failed(status, out, S.gist[0] ? "PATCH /gists/id" : "POST /gists");
        break;
      }
      if (!S.gist[0]) {
        save_gist_id(json_str(json_get(j, "id"), ""));
        out_log("Settings Sync", "made the gist %s", S.gist);
      }
      commit(json_get(j, "files"));
      finish(1);
      break;
  }
  json_free(j);
}


static void start (int inter) {
  load_gist_id();
  S.inter = inter;
  S.due = 0;
  if (S.gist[0]) {
    char path[120];
    snprintf(path, sizeof(path), "/gists/%s", S.gist);
    begin(ST_GET, "GET", path, NULL);
  }
  else begin(ST_LIST, "GET", "/gists?per_page=100", NULL);
}


/* the main loop's: the request's answer, or a sync of its own that is due */
void sync_poll (void) {
  if (S.req) {
    Buf out;
    int st = gh_poll(S.req, &out);
    if (st < 0) return;
    S.req = NULL;
    answer(st, &out);
    buf_free(&out);
  }
  else if (S.due && os_now_us() >= S.due && sync_on()) start(0);
}


/* one of the synced files changed here (saved, a setting set): synced a moment later */
void sync_changed (void) {
  if (S.st == ST_NONE && sync_on()) S.due = os_now_us() + 1500000;
}


void sync_init (void) {	/* at start: synced once the window is up */
  if (sync_on()) S.due = os_now_us() + 2500000;
}


/* Sync Now: waited for, with its questions */
static int sync_wait (int inter) {
  while (S.req) {	/* one of its own already on its way: its answer first */
    th_nap(30);
    sync_poll();
  }
  gh_busy("Settings Sync");
  start(inter);
  while (S.req) {
    th_nap(30);
    sync_poll();
  }
  return 0;
}

/* }================================================================== */


/*
** {==================================================================
** The commands
** ===================================================================
*/

static void show (void) {
  Buf md, out;
  char path[120];
  int st;
  Json *j;
  size_t i;
  while (S.req) {	/* a sync on its way: its answer first */
    th_nap(30);
    sync_poll();
  }
  load_gist_id();
  buf_init(&md);
  buf_puts(&md, "# Settings Sync\n\n");
  buf_printf(&md, "Sync is **%s**.\n\n", sync_on() ? "on" : "off");
  if (!S.gist[0]) {
    buf_puts(&md, "Nothing was synced from this machine yet: **Settings Sync: Turn On** starts it.\n");
    md_page("sync", "Settings Sync.md", md.s);
    buf_free(&md);
    return;
  }
  gh_busy("Settings Sync");
  snprintf(path, sizeof(path), "/gists/%s", S.gist);
  st = gh_request("GET", path, NULL, &out);
  if (st != 200) {
    gh_fail(st, &out);
    buf_free(&out);
    buf_free(&md);
    return;
  }
  j = json_parse(out.s ? out.s : "", out.len);
  buf_printf(&md, "The data is in a secret gist of your GitHub account: <%s>, changed %s.\n\n",
             json_str(json_get(j, "html_url"), "?"), json_str(json_get(j, "updated_at"), "?"));
  buf_puts(&md, "| File | On GitHub | State |\n|---|---:|---|\n");
  gather(json_get(j, "files"));
  for (i = 0; i < (size_t)S.n; i++) {
    Item *it = &S.it[i];
    int lchg = !same(it->l, it->ll, it->bl, it->bll), rchg = !same(it->r, it->rl, it->br, it->brl);
    const char *state;
    if (it->l == NULL && it->r == NULL) continue;
    if (it->act == A_SKIP) state = "too big to sync";
    else if (same(it->l, it->ll, it->r, it->rl) || ((it->bl || it->br) && !lchg && !rchg)) state = "in sync";
    else if (it->r == NULL) state = "not on GitHub yet";
    else if (it->l == NULL) state = "not here yet";
    else if (it->bl == NULL && it->br == NULL) state = "not the same";
    else state = lchg && rchg ? "changed here and on GitHub" : lchg ? "changed here" : "changed on GitHub";
    if (it->r) buf_printf(&md, "| %s | %d bytes | %s |\n", it->name, (int)it->rl, state);
    else buf_printf(&md, "| %s | - | %s |\n", it->name, state);
  }
  items_free();
  buf_puts(&md, "\nSynced: settings.json, keybindings.json and the snippets (of the profile in use).\n");
  md_page("sync", "Settings Sync.md", md.s);
  json_free(j);
  buf_free(&out);
  buf_free(&md);
}


static void turn_off (void) {
  static const char *const btn[] = {"Turn Off", "Turn Off and Delete the Data on GitHub"};
  int c;
  if (!sync_on()) {
    toast(0, "Settings Sync is off.");
    return;
  }
  c = dialog("Turn off Settings Sync?", "The files here stay as they are.", btn, 2);
  if (c < 0) return;
  set_on(0);
  S.due = 0;
  load_gist_id();
  if (c == 1 && S.gist[0]) {
    Buf out;
    char path[120];
    int st;
    gh_busy("Settings Sync");
    snprintf(path, sizeof(path), "/gists/%s", S.gist);
    st = gh_request("DELETE", path, NULL, &out);
    if (st != 204 && st != 404) {
      gh_fail(st, &out);
      buf_free(&out);
      return;
    }
    buf_free(&out);
    save_gist_id(NULL);
    {	/* the bases go with it: another Turn On starts anew */
      char *d = state_path("", 1);
      Vec v;
      size_t i;
      vec_init(&v);
      if (os_listdir(d, &v) == 0)
        for (i = 0; i < v.n; i++) {
          char *f = path_join(d, v.v[i]);
          os_unlink(f);
          free(f);
        }
      vec_free(&v);
      free(d);
    }
    toast(0, "Settings Sync is off, and its data on GitHub is deleted.");
    return;
  }
  toast(0, "Settings Sync is off.");
}


void sync_command (int cmd) {
  switch (cmd) {
    case CMD_SYNC_ON:
      if (!gh_signed_in()) {
        static const char *const btn[] = {"Sign In"};
        if (dialog("Settings Sync keeps your settings in a secret gist of your GitHub account.",
                   "Sign in with a personal access token that has the \"gist\" scope.", btn, 1) != 0) return;
        gh_command(CMD_GH_SIGNIN);
        if (!gh_signed_in()) return;
      }
      if (sync_on()) {
        sync_wait(1);
        return;
      }
      set_on(1);
      sync_wait(1);
      if (!S.gist[0]) {	/* it did not get through: off again */
        set_on(0);
        return;
      }
      out_log("Settings Sync", "turned on");
      break;
    case CMD_SYNC_OFF: turn_off(); break;
    case CMD_SYNC_NOW:
      if (!sync_on()) {
        toast(0, "Settings Sync is off: Settings Sync: Turn On.");
        return;
      }
      sync_wait(1);
      break;
    case CMD_SYNC_SHOW: show(); break;
  }
}

/* }================================================================== */
