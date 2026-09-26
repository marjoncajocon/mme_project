/*
** ehost.c - the extension host: VS Code extensions' code, run by node
**
** An extension is JavaScript written against VS Code's `vscode` API; C has
** no way to run it, so mme starts mme-exthost.js (ehost_js.h, written into
** mme-data) with node, the way VS Code starts its extension host. elsp.c
** talks to it as to any language server (it is the "*ext" one, bound to
** every document): the language features the extensions provide come back
** as LSP answers. What LSP has no word for comes as mme/... messages,
** answered here: the quick pick, the input box, output channels, status bar
** items, the commands the extensions add to the Command Palette.
**
** Which extensions run: those named in mme.extensions.run, and those
** installed into mme-data (Install from VSIX...) that have code. Every
** other extension still gives its themes, snippets and grammars (eext.c).
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const char *const host_lines[] = {	/* mme-exthost.js, a line each (C's strings are short) */
#include "ehost_js.h"
  NULL
};


/*
** {==================================================================
** Which extensions run, and the command that runs them
** ===================================================================
*/

typedef struct HExt {
  char *id, *dir;
  char *state;	/* "running", "error", or NULL: not started yet */
} HExt;

static HExt *g_run;
static size_t g_nrun;
static Vec g_langs;	/* the languages they answer for: their package.json's, then what the host says */
static Vec g_ghost;	/* the languages an extension gives ghost text for (Supermaven ...): asked instead of Copilot */
static int g_ghost_all;	/* one gives it for every file */
static int g_ready;	/* g_run worked out */
static unsigned g_gen;	/* for this reading of the extensions (ext_generation) */
static char *g_dirs;	/* the folders that run, joined: another set starts the host again */
static char *g_cmdline;
static int g_restart;	/* another set to run while the host runs: it starts again (ehost_idle) */
static int g_opened;	/* the open files were given to the host since it had something to run */

static void forget (void);


static int in_list (const char *key, const char *id) {
  const Json *list = settings_get(key);
  size_t i;
  if (list == NULL || list->type != J_ARR) return 0;
  for (i = 0; i < list->n; i++) {
    const char *s = json_str(list->kid[i], "");
    if (m_stricmp(s, id) == 0) return 1;
  }
  return 0;
}


static int in_run_list (const char *id) {
  return in_list("mme\\.extensions\\.run", id);
}


/* mme.extensions.disabled: its code does not run, wherever it is installed (its themes, snippets still do) */
int ehost_disabled (const char *id) {
  return in_list("mme\\.extensions\\.disabled", id);
}


static void add_lang (const char *l) {
  size_t i;
  for (i = 0; i < g_langs.n; i++)
    if (strcmp(g_langs.v[i], l) == 0) return;
  vec_push(&g_langs, xstrdup(l));
}


/* an extension's package.json: has it code, and the languages it starts on */
static int read_pkg (const char *dir) {
  char *f = path_join(dir, "package.json"), *s;
  size_t len, i;
  Json *j;
  int code;
  s = read_file(f, &len);
  free(f);
  if (s == NULL) return 0;
  j = json_parse(s, len);
  free(s);
  if (j == NULL) return 0;
  code = json_str(json_get(j, "main"), NULL) != NULL;
  if (code) {
    const Json *ev = json_get(j, "activationEvents"), *ls = json_get(j, "contributes.languages");
    for (i = 0; ev && ev->type == J_ARR && i < ev->n; i++) {
      const char *e = json_str(ev->kid[i], "");
      if (strncmp(e, "onLanguage:", 11) == 0) add_lang(e + 11);
    }
    for (i = 0; ls && ls->type == J_ARR && i < ls->n; i++) {
      const char *id = json_str(json_get(ls->kid[i], "id"), NULL);
      if (id) add_lang(id);
    }
  }
  json_free(j);
  return code;
}


static void work_out (void) {
  size_t i, n;
  Buf b;
  if (g_ready && g_gen == ext_generation()) return;
  forget();
  g_ready = 1;
  g_gen = ext_generation();
  n = ext_count();
  for (i = 0; i < n; i++) {
    const char *id = ext_id_at(i), *dir = ext_dir_at(i);
    if (ehost_disabled(id)) continue;	/* Disable in the Extensions view */
    if (!in_run_list(id) && ext_is_vscode(i)) continue;	/* VS Code's run only when named */
    if (!read_pkg(dir)) continue;	/* no code: eext.c has all of it already */
    g_run = (HExt *)xrealloc(g_run, (g_nrun + 1) * sizeof(HExt));
    g_run[g_nrun].id = xstrdup(id);
    g_run[g_nrun].dir = xstrdup(dir);
    g_run[g_nrun].state = NULL;
    g_nrun++;
  }
  buf_init(&b);
  for (i = 0; i < g_nrun; i++) buf_printf(&b, "%s;", g_run[i].dir);
  buf_putc(&b, '\0');
  if (g_dirs && strcmp(g_dirs, b.s) != 0) {	/* installed, uninstalled: the host starts again with them */
    free(g_dirs);
    g_dirs = buf_take(&b);
    if (lsp_running(EXT_LANG)) g_restart = 1;	/* not here: this may be in the middle of starting it */
  }
  else {
    free(g_dirs);
    g_dirs = buf_take(&b);
  }
}


/* mme-exthost.js in mme-data, as this mme has it */
static char *host_script (void) {
  char *p = data_path("mme-exthost.js"), *old;
  size_t have = 0, i;
  Buf b;
  buf_init(&b);
  for (i = 0; host_lines[i]; i++) buf_puts(&b, host_lines[i]);
  old = read_file(p, &have);
  if (old == NULL || have != b.len || memcmp(old, b.s, b.len) != 0) {
    int fd = os_open(p, OS_WRITE);
    if (fd >= 0) {
      os_write(fd, b.s, b.len);
      os_close(fd);
    }
  }
  free(old);
  buf_free(&b);
  return p;
}


/* node: mme.extensions.nodePath, else the PATH's */
static char *node_path (void) {
  const char *s = json_str(settings_get("mme\\.extensions\\.nodePath"), "");
  if (s[0]) return xstrdup(s);
  return find_program("node");
}


const char *ehost_command (void) {
  work_out();
  if (g_cmdline == NULL) {
    char *node, *script;
    Buf b;
    if (g_nrun == 0 || (node = node_path()) == NULL) {
      g_cmdline = xstrdup("");
      return g_cmdline;
    }
    script = host_script();
    buf_init(&b);
    buf_printf(&b, "\"%s\" \"%s\"", node, script);
    g_cmdline = buf_take(&b);
    free(node);
    free(script);
  }
  return g_cmdline;
}


int ehost_serves (const char *lang) {
  size_t i;
  if (lang == NULL || *ehost_command() == '\0') return 0;
  for (i = 0; i < g_langs.n; i++)
    if (strcmp(g_langs.v[i], lang) == 0) return 1;
  return 0;
}


/* an extension gives ghost text for this language: the host is asked, not mme.inlineCompletionServer */
int ehost_inline (const char *lang) {
  size_t i;
  if (g_ghost_all) return 1;
  for (i = 0; lang && i < g_ghost.n; i++)
    if (strcmp(g_ghost.v[i], lang) == 0) return 1;
  return 0;
}


const char *ehost_state (const char *id) {
  size_t i;
  work_out();
  for (i = 0; i < g_nrun; i++)
    if (m_stricmp(g_run[i].id, id) == 0) return g_run[i].state ? g_run[i].state : "starting";
  return NULL;
}


static void forget (void) {
  size_t i;
  g_opened = 0;
  for (i = 0; i < g_nrun; i++) {
    free(g_run[i].id);
    free(g_run[i].dir);
    free(g_run[i].state);
  }
  free(g_run);
  g_run = NULL;
  g_nrun = 0;
  vec_free(&g_langs);
  vec_free(&g_ghost);
  g_ghost_all = 0;
  g_ready = 0;
  free(g_cmdline);
  g_cmdline = NULL;
}


/* the main loop: the extensions read again (installed, uninstalled) start the host again with them */
void ehost_idle (void) {
  work_out();
  if (g_restart) {
    g_restart = 0;
    lsp_restart(EXT_LANG);
  }
  if (!g_opened && *ehost_command()) {	/* something to run now (the extensions were read after the files opened) */
    g_opened = 1;
    if (!lsp_running(EXT_LANG)) mme_lsp_reopen();
  }
}


/* the settings changed: which run is worked out again (the caller restarts the host) */
void ehost_reset (void) {
  forget();
  free(g_dirs);
  g_dirs = NULL;
}

/* }================================================================== */


/*
** {==================================================================
** The extensions' commands: numbers from CMD_N up, for the palette,
** the keys and the status bar
** ===================================================================
*/

typedef struct HCmd {
  char *id, *title, *category;
  char *args;	/* JSON arguments ("[\"sb1\"]" for a status bar item's), or NULL */
  int hidden;	/* not in the palette */
} HCmd;

static HCmd *g_cmd;
static size_t g_ncmd;

typedef struct HKey {
  char *key, *when;
  int cmd;
} HKey;

static HKey *g_key;
static size_t g_nkey;


static int cmd_add (const char *id, const char *title, const char *category, const char *args, int hidden) {
  size_t i;
  for (i = 0; i < g_ncmd; i++)
    if (strcmp(g_cmd[i].id, id) == 0 && ((args == NULL && g_cmd[i].args == NULL) ||
                                         (args && g_cmd[i].args && strcmp(args, g_cmd[i].args) == 0))) {
      if (title && *title) {	/* a runtime one given its title later */
        free(g_cmd[i].title);
        g_cmd[i].title = xstrdup(title);
      }
      return CMD_N + (int)i;
    }
  g_cmd = (HCmd *)xrealloc(g_cmd, (g_ncmd + 1) * sizeof(HCmd));
  g_cmd[g_ncmd].id = xstrdup(id);
  g_cmd[g_ncmd].title = xstrdup(title ? title : id);
  g_cmd[g_ncmd].category = xstrdup(category ? category : "");
  g_cmd[g_ncmd].args = args ? xstrdup(args) : NULL;
  g_cmd[g_ncmd].hidden = hidden;
  return CMD_N + (int)g_ncmd++;
}


int ehost_ncmd (void) {
  return (int)g_ncmd;
}


int ehost_listed (int cmd) {	/* in the palette */
  size_t i = (size_t)(cmd - CMD_N);
  return cmd >= CMD_N && i < g_ncmd && !g_cmd[i].hidden;
}


/* "Category: Title", as VS Code's palette says it */
const char *ehost_cmd_title (int cmd) {
  static char buf[256];
  size_t i = (size_t)(cmd - CMD_N);
  if (cmd < CMD_N || i >= g_ncmd) return "";
  if (g_cmd[i].category[0]) snprintf(buf, sizeof(buf), "%s: %s", g_cmd[i].category, g_cmd[i].title);
  else snprintf(buf, sizeof(buf), "%s", g_cmd[i].title);
  return buf;
}


const char *ehost_cmd_id (int cmd) {
  size_t i = (size_t)(cmd - CMD_N);
  return (cmd >= CMD_N && i < g_ncmd) ? g_cmd[i].id : "";
}


int ehost_cmd_by_id (const char *id) {
  size_t i;
  for (i = 0; i < g_ncmd; i++)
    if (g_cmd[i].args == NULL && strcmp(g_cmd[i].id, id) == 0) return CMD_N + (int)i;
  return CMD_NONE;
}


void ehost_run (int cmd) {
  size_t i = (size_t)(cmd - CMD_N);
  if (cmd < CMD_N || i >= g_ncmd) return;
  if (!lsp_ext_command(g_cmd[i].id, g_cmd[i].args)) toast(1, "The extension host is not running");
}


/* ekeys.c, after the user's keybindings.json: the extensions' own keys (the user's win, being later) */
void ehost_keys (void (*add) (int cmd, const char *key, const char *when)) {
  size_t i;
  for (i = 0; i < g_nkey; i++) add(g_key[i].cmd, g_key[i].key, g_key[i].when);
}

/* }================================================================== */


/*
** {==================================================================
** Status bar items
** ===================================================================
*/

typedef struct HBar {
  char key[16], *name, *text, *tip;
  int right, prio, visible, cmd, sev;
} HBar;

static HBar *g_bar;
static size_t g_nbar;


static void bar_set (const Json *p) {
  const char *key = json_str(json_get(p, "id"), "");
  HBar *b = NULL;
  size_t i;
  for (i = 0; i < g_nbar; i++)
    if (strcmp(g_bar[i].key, key) == 0) b = &g_bar[i];
  if (b == NULL) {
    g_bar = (HBar *)xrealloc(g_bar, (g_nbar + 1) * sizeof(HBar));
    b = &g_bar[g_nbar++];
    memset(b, 0, sizeof(*b));
    snprintf(b->key, sizeof(b->key), "%s", key);
  }
  free(b->name);
  free(b->text);
  free(b->tip);
  b->name = xstrdup(json_str(json_get(p, "name"), ""));
  b->text = xstrdup(json_str(json_get(p, "text"), ""));
  b->tip = xstrdup(json_str(json_get(p, "tooltip"), ""));
  b->right = json_bool(json_get(p, "right"), 0);
  b->prio = (int)json_num(json_get(p, "priority"), 0);
  b->visible = json_bool(json_get(p, "visible"), 0);
  b->sev = json_bool(json_get(p, "error"), 0) ? 2 : json_bool(json_get(p, "warning"), 0) ? 1 : 0;
  b->cmd = CMD_NONE;
  if (json_str(json_get(p, "command"), "")[0]) {
    char args[48];
    snprintf(args, sizeof(args), "[\"%s\"]", b->key);
    b->cmd = cmd_add("_mme.statusBar", b->text, "", args, 1);
  }
}


/* mme.c's draw_status: the items shown now */
void ehost_status (void) {
  size_t i;
  for (i = 0; i < g_nbar; i++) {
    char id[40];
    if (!g_bar[i].visible || g_bar[i].text[0] == '\0') continue;
    snprintf(id, sizeof(id), "ext.%s", g_bar[i].key);
    status_add(id, g_bar[i].name[0] ? g_bar[i].name : "Extension", g_bar[i].right, g_bar[i].prio, g_bar[i].text,
               g_bar[i].tip, g_bar[i].cmd);
  }
}

/* }================================================================== */


/*
** {==================================================================
** What the host says (notifications) and asks (requests)
** ===================================================================
*/

static void contributions (const Json *p) {
  const Json *cs = json_get(p, "commands"), *ks = json_get(p, "keybindings"), *cf = json_get(p, "configuration");
  size_t i;
  for (i = 0; i < g_nkey; i++) {	/* a restarted host says them all again */
    free(g_key[i].key);
    free(g_key[i].when);
  }
  g_nkey = 0;
  settings_ext_clear();
  for (i = 0; cf && cf->type == J_ARR && i < cf->n; i++) {	/* their settings, in the Settings editor */
    const Json *c = cf->kid[i];
    settings_ext_add(json_str(json_get(c, "key"), ""), json_str(json_get(c, "type"), "string"), json_get(c, "default"),
                     json_get(c, "enum"), json_str(json_get(c, "description"), ""));
  }
  for (i = 0; cs && cs->type == J_ARR && i < cs->n; i++) {
    const Json *c = cs->kid[i];
    cmd_add(json_str(json_get(c, "id"), ""), json_str(json_get(c, "title"), ""), json_str(json_get(c, "category"), ""), NULL, 0);
  }
  for (i = 0; ks && ks->type == J_ARR && i < ks->n; i++) {
    const Json *k = ks->kid[i];
    const char *id = json_str(json_get(k, "command"), "");
    int c = ehost_cmd_by_id(id);
    if (c == CMD_NONE) c = cmd_add(id, id, "", NULL, 1);
    g_key = (HKey *)xrealloc(g_key, (g_nkey + 1) * sizeof(HKey));
    g_key[g_nkey].key = xstrdup(json_str(json_get(k, "key"), ""));
    g_key[g_nkey].when = xstrdup(json_str(json_get(k, "when"), ""));
    g_key[g_nkey].cmd = c;
    g_nkey++;
  }
  if (g_nkey) keys_load();	/* the new keys join the user's */
}


int ehost_message (const char *method, const Json *p) {
  if (strcmp(method, "mme/output") == 0) {
    const char *ch = json_str(json_get(p, "channel"), "Extension Host");
    const Json *t = json_get(p, "text");
    if (json_bool(json_get(p, "clear"), 0)) out_clear_chan(ch);
    if (t && t->type == J_STR && t->len) out_append(ch, t->str, t->len);
    if (json_bool(json_get(p, "show"), 0)) on_show_output(ch);
  }
  else if (strcmp(method, "mme/statusBar") == 0) bar_set(p);
  else if (strcmp(method, "mme/contributions") == 0) contributions(p);
  else if (strcmp(method, "mme/languages") == 0) {
    const Json *ls = json_get(p, "languages");
    size_t i;
    for (i = 0; ls && ls->type == J_ARR && i < ls->n; i++) add_lang(json_str(ls->kid[i], ""));
  }
  else if (strcmp(method, "mme/inlineLanguages") == 0) {
    const Json *ls = json_get(p, "languages");
    size_t i;
    vec_free(&g_ghost);
    for (i = 0; ls && ls->type == J_ARR && i < ls->n; i++) vec_push(&g_ghost, xstrdup(json_str(ls->kid[i], "")));
    g_ghost_all = json_bool(json_get(p, "all"), 0);
  }
  else if (strcmp(method, "mme/extensionState") == 0) {
    const char *id = json_str(json_get(p, "id"), "");
    size_t i;
    for (i = 0; i < g_nrun; i++)
      if (m_stricmp(g_run[i].id, id) == 0) {
        free(g_run[i].state);
        g_run[i].state = xstrdup(json_str(json_get(p, "state"), "running"));
      }
    if (strcmp(json_str(json_get(p, "state"), ""), "error") == 0)
      toast(1, "Extension %s failed to start: %s", id, json_str(json_get(p, "message"), ""));
  }
  else if (strcmp(method, "mme/terminal") == 0) {
    Buf c;
    const Json *args = json_get(p, "shellArgs");
    size_t i;
    buf_init(&c);
    buf_puts(&c, json_str(json_get(p, "shellPath"), ""));
    for (i = 0; args && args->type == J_ARR && i < args->n; i++) buf_printf(&c, " %s", json_str(args->kid[i], ""));
    buf_putc(&c, '\0');
    on_task_terminal(json_str(json_get(p, "name"), "Terminal"), c.s, json_str(json_get(p, "cwd"), ""), 0);
    buf_free(&c);
  }
  else if (strcmp(method, "mme/terminalSend") == 0) {
    const Json *t = json_get(p, "text");
    if (t && t->type == J_STR) panel_send(t->str, t->len);
  }
  else if (strcmp(method, "mme/select") == 0) {
    char *path = lsp_path(json_str(json_get(p, "uri"), ""));
    if (path) on_show_document(path, NULL, (long)json_num(json_get(p, "range.start.line"), 0),
                               (long)json_num(json_get(p, "range.start.character"), 0));
    free(path);
  }
  else return 0;
  return 1;
}


/* mme/hostInit: what to run, the settings, the folders */
static void host_init (Buf *r) {
  size_t i;
  int k;
  char *dd = data_path(""), *root;
  work_out();
  buf_puts(r, "{\"extensions\":[");
  for (i = 0; i < g_nrun; i++) {
    if (i) buf_putc(r, ',');
    json_put_str(r, g_run[i].dir, strlen(g_run[i].dir));
  }
  buf_puts(r, "],\"settings\":");
  if (settings_all()) json_write(r, settings_all());
  else buf_puts(r, "{}");
  buf_puts(r, ",\"folders\":[");
  for (k = 0; k < ws_count(); k++) {
    if (k) buf_putc(r, ',');
    json_put_str(r, ws_folder(k), strlen(ws_folder(k)));
  }
  root = xstrdup(dd);
  buf_puts(r, "],\"dataDir\":");
  json_put_str(r, root, strlen(root));
  buf_puts(r, ",\"appRoot\":");
  json_put_str(r, root, strlen(root));
  buf_putc(r, '}');
  free(root);
  free(dd);
}


/* mme/quickPick: the items in mme's picker; the index picked, or null */
static void quick_pick (const Json *p, Buf *r) {
  const Json *items = json_get(p, "items");
  const char *ph = json_str(json_get(p, "placeHolder"), ""), *title = json_str(json_get(p, "title"), "");
  int many = json_bool(json_get(p, "canPickMany"), 0), pick;
  size_t i, n = items && items->type == J_ARR ? items->n : 0;
  char *chosen = (char *)xmalloc(n + 1);
  Pick pk;
  for (i = 0; i < n; i++) chosen[i] = (char)json_bool(json_get(items->kid[i], "picked"), 0);
  for (;;) {	/* canPickMany: each item picked toggles its check, "Done" (the first) ends */
    pick_init(&pk, ph[0] ? ph : title[0] ? title : NULL);
    pk.keep_order = 0;
    if (many) pick_add(&pk, "Done", "the checked ones", 0xEAB2);
    for (i = 0; i < n; i++) {
      const Json *it = items->kid[i];
      const char *desc = json_str(json_get(it, "description"), ""), *det = json_str(json_get(it, "detail"), "");
      char d[512];
      snprintf(d, sizeof(d), "%s%s%s", desc, desc[0] && det[0] ? "  " : "", det);
      pick_add(&pk, json_str(json_get(it, "label"), ""), d[0] ? d : NULL, many ? (chosen[i] ? 0xEAB2 : ' ') : 0);
    }
    pick = pick_run(&pk);
    pick_free(&pk);
    if (!many || pick < 0 || pick == 0) break;
    chosen[pick - 1] = (char)!chosen[pick - 1];
  }
  if (!many) {
    if (pick >= 0) buf_printf(r, "%d", pick);
    else buf_puts(r, "null");
  }
  else if (pick < 0) buf_puts(r, "null");
  else {
    int first = 1;
    buf_putc(r, '[');
    for (i = 0; i < n; i++)
      if (chosen[i]) {
        buf_printf(r, "%s%lu", first ? "" : ",", (unsigned long)i);
        first = 0;
      }
    buf_putc(r, ']');
  }
  free(chosen);
}


int ehost_request (const char *method, const Json *p, Buf *r) {
  if (strcmp(method, "mme/hostInit") == 0) host_init(r);
  else if (strcmp(method, "mme/quickPick") == 0) quick_pick(p, r);
  else if (strcmp(method, "mme/inputBox") == 0) {
    const char *prompt = json_str(json_get(p, "prompt"), ""), *title = json_str(json_get(p, "title"), "");
    const char *ph = json_str(json_get(p, "placeHolder"), "");
    char *s = ask_text(title[0] ? title : prompt[0] ? prompt : ph, json_str(json_get(p, "value"), ""));
    if (s) json_put_str(r, s, strlen(s));
    else buf_puts(r, "null");
    free(s);
  }
  else if (strcmp(method, "mme/executeCommand") == 0) {	/* one of mme's own, by its VS Code id */
    int c = cmd_by_id(json_str(json_get(p, "command"), ""));
    if (c != CMD_NONE && c < CMD_N) {
      mme_command(c);
      buf_puts(r, "true");
    }
    else buf_puts(r, "null");
  }
  else if (strcmp(method, "mme/settingsUpdate") == 0) {
    const char *key = json_str(json_get(p, "key"), NULL);
    const Json *v = json_get(p, "value");
    if (key && v && v->type != J_NULL) {
      Buf j;
      buf_init(&j);
      json_write(&j, v);
      buf_putc(&j, '\0');
      settings_put_json(key, j.s);
      buf_free(&j);
    }
    else if (key) settings_reset(key);
    mme_settings_changed();
    buf_puts(r, "null");
  }
  else if (strcmp(method, "mme/clipboard") == 0) {
    const char *t = json_str(json_get(p, "text"), NULL);
    if (t) {
      clip_set(t, strlen(t));
      buf_puts(r, "null");
    }
    else {
      size_t n = 0;
      const char *s = clip_get(&n);
      if (s) json_put_str(r, s, n);
      else buf_puts(r, "\"\"");
    }
  }
  else if (strcmp(method, "mme/save") == 0) {
    char *path = lsp_path(json_str(json_get(p, "uri"), ""));
    buf_puts(r, path && mme_save_path(path) == 0 ? "true" : "false");
    free(path);
  }
  else return 0;
  return 1;
}

/* }================================================================== */
