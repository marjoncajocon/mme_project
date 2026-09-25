/*
** ekeys.c - keyboard shortcuts: VS Code's keybindings.json
**
** keybindings.json (in mme-data) is a list of {"key": "ctrl+shift+k", "command":
** "editor.action.rename"}, as VS Code's is; a key can be two ("ctrl+k
** ctrl+t"). Those keys come before the ones mme has. As in VS Code an entry
** can have a "when" clause, "args", and "-command" takes a default key away.
*/

#include "mme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct Bind {
  int k1, k2;	/* k2: 0 for one key */
  int cmd;
  int remove;	/* "-command": mme's own key of cmd goes */
  char *when;	/* NULL: always */
  char *args;	/* the args, as JSON text; NULL: none */
} Bind;

static Bind *g_bind;
static size_t g_nbind, g_capbind;


static const struct {
  const char *name;
  int key;
} named[] = {
  {"up", K_UP}, {"down", K_DOWN}, {"left", K_LEFT}, {"right", K_RIGHT},
  {"home", K_HOME}, {"end", K_END}, {"pageup", K_PGUP}, {"pagedown", K_PGDN},
  {"insert", K_INS}, {"delete", K_DEL}, {"enter", K_ENTER}, {"escape", K_ESC},
  {"tab", K_TAB}, {"backspace", K_BS}, {"space", ' '},
  {"f1", K_F1}, {"f2", K_F2}, {"f3", K_F3}, {"f4", K_F4}, {"f5", K_F5}, {"f6", K_F6},
  {"f7", K_F7}, {"f8", K_F8}, {"f9", K_F9}, {"f10", K_F10}, {"f11", K_F11}, {"f12", K_F12}
};

#define NNAMED	(sizeof(named) / sizeof(named[0]))


/*
** "ctrl+shift+p" as term_key says it: Ctrl and a letter alone is the old
** control code (CTRL('p')), the rest the key with KM_ bits. 0: not a key.
*/
static int parse_one (const char *s, size_t n) {
  int mods = 0, key = 0;
  size_t i = 0;
  while (i < n) {
    size_t j = i, k;
    char part[32];
    while (j < n && s[j] != '+') j++;
    if (j == i && j < n) j++;	/* "ctrl++": the + key */
    if (j - i >= sizeof(part)) return 0;
    memcpy(part, s + i, j - i);
    part[j - i] = '\0';
    for (k = 0; part[k]; k++)
      if (part[k] >= 'A' && part[k] <= 'Z') part[k] = (char)(part[k] + 32);
    i = j + (j < n);
    if (strcmp(part, "ctrl") == 0 || strcmp(part, "cmd") == 0) mods |= KM_CTRL;
    else if (strcmp(part, "shift") == 0) mods |= KM_SHIFT;
    else if (strcmp(part, "alt") == 0 || strcmp(part, "meta") == 0) mods |= KM_ALT;
    else if (strlen(part) == 1) key = (unsigned char)part[0];
    else {
      for (k = 0; k < NNAMED; k++)
        if (strcmp(part, named[k].name) == 0) key = named[k].key;
      if (key == 0) return 0;
    }
  }
  if (key == 0) return 0;
  if ((mods & KM_CTRL) && !(mods & KM_SHIFT) && key >= 'a' && key <= 'z')
    return CTRL(key) | (mods & KM_ALT);
  if (key < K_UP && !(mods & (KM_CTRL | KM_ALT))) {	/* a plain character: shift is in it */
    if ((mods & KM_SHIFT) && key >= 'a' && key <= 'z') return key - 32;
    return key;
  }
  return key | mods;
}


/* "ctrl+k ctrl+t": one or two keys; 0 when it is not */
int key_parse (const char *s, int *k2) {
  const char *sp = strchr(s, ' ');
  *k2 = 0;
  if (sp == NULL) return parse_one(s, strlen(s));
  *k2 = parse_one(sp + 1, strlen(sp + 1));
  return *k2 ? parse_one(s, (size_t)(sp - s)) : 0;
}


/* a key as keybindings.json writes it ("ctrl+shift+p"), or as menus show it (pretty) */
void key_name (int k, int pretty, char *out, size_t n) {
  int code = KEY_CODE(k), mods = k & (KM_CTRL | KM_ALT | KM_SHIFT);
  char base[16];
  size_t i;
  if (code > 0 && code < 32 && code != K_TAB && code != K_ENTER && code != K_ESC) {
    mods |= KM_CTRL;	/* CTRL('p'): Ctrl+P */
    code += 96;
  }
  base[0] = '\0';
  for (i = 0; i < NNAMED; i++)
    if (named[i].key == code) snprintf(base, sizeof(base), "%s", named[i].name);
  if (base[0] == '\0') {
    if (code >= 'A' && code <= 'Z' && !(mods & KM_CTRL)) {	/* Shift+letter */
      mods |= KM_SHIFT;
      code += 32;
    }
    base[0] = (char)code;
    base[1] = '\0';
  }
  if (pretty) {	/* Ctrl+Shift+P */
    if (base[0] >= 'a' && base[0] <= 'z') base[0] = (char)(base[0] - 32);
    snprintf(out, n, "%s%s%s%s", (mods & KM_CTRL) ? "Ctrl+" : "", (mods & KM_SHIFT) ? "Shift+" : "",
             (mods & KM_ALT) ? "Alt+" : "", base);
  }
  else snprintf(out, n, "%s%s%s%s", (mods & KM_CTRL) ? "ctrl+" : "", (mods & KM_SHIFT) ? "shift+" : "",
                (mods & KM_ALT) ? "alt+" : "", base);
}


char *keys_path (void) {
  return profile_file("keybindings.json");	/* the profile in use's */
}


/*
** {==================================================================
** When clauses: "editorTextFocus && editorLangId == 'go'"
** ===================================================================
*/

typedef struct WP {
  const char *s;
} WP;


static void wskip (WP *p) {
  while (*p->s == ' ' || *p->s == '\t') p->s++;
}


static int wword (int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         (c != '\0' && strchr("_.-:/\\@$#%", c) != NULL);
}


/* a key's name or a value ('quoted' or bare) into out */
static void wtoken (WP *p, char *out, size_t n) {
  size_t k = 0;
  wskip(p);
  if (*p->s == '\'' || *p->s == '"') {
    char q = *p->s++;
    while (*p->s && *p->s != q) {
      if (k + 1 < n) out[k++] = *p->s;
      p->s++;
    }
    if (*p->s) p->s++;
  }
  else
    while (wword((unsigned char)*p->s)) {
      if (k + 1 < n) out[k++] = *p->s;
      p->s++;
    }
  out[k] = '\0';
}


static int truthy (const char *v) {
  return v && *v && strcmp(v, "false") != 0 && strcmp(v, "0") != 0;
}


static int w_or (WP *p);

static int w_term (WP *p) {
  char key[128], val[256];
  const char *v;
  wskip(p);
  if (*p->s == '!') {
    p->s++;
    return !w_term(p);
  }
  if (*p->s == '(') {
    int r;
    p->s++;
    r = w_or(p);
    wskip(p);
    if (*p->s == ')') p->s++;
    return r;
  }
  wtoken(p, key, sizeof(key));
  if (strcmp(key, "true") == 0) return 1;
  if (strcmp(key, "false") == 0) return 0;
  v = when_ctx(key);
  wskip(p);
  if ((p->s[0] == '=' || p->s[0] == '!') && p->s[1] == '=') {	/* == != (and === !==) */
    int ne = p->s[0] == '!';
    p->s += 2;
    if (*p->s == '=') p->s++;
    wtoken(p, val, sizeof(val));
    return (strcmp(v ? v : "false", val) == 0) != ne;
  }
  if (p->s[0] == '=' && p->s[1] == '~') {	/* =~ /regex/i */
    char pat[256];
    size_t k = 0;
    int icase = 0, r = 0;
    Regex *re;
    const char *err;
    p->s += 2;
    wskip(p);
    if (*p->s == '/') {
      p->s++;
      while (*p->s && *p->s != '/') {
        if (*p->s == '\\' && p->s[1] == '/') p->s++;
        if (k + 1 < sizeof(pat)) pat[k++] = *p->s;
        p->s++;
      }
      if (*p->s) p->s++;
      while (*p->s >= 'a' && *p->s <= 'z') icase |= *p->s++ == 'i';
    }
    pat[k] = '\0';
    if ((re = re_compile(pat, icase, &err)) != NULL) {
      size_t a, b;
      r = v && re_find(re, v, strlen(v), 0, &a, &b);
      re_free(re);
    }
    return r;
  }
  if (p->s[0] == '<' || p->s[0] == '>') {	/* numbers */
    int lt = p->s[0] == '<', eq = p->s[1] == '=';
    double a = v ? atof(v) : 0, b;
    p->s += eq ? 2 : 1;
    wtoken(p, val, sizeof(val));
    b = atof(val);
    return lt ? (eq ? a <= b : a < b) : (eq ? a >= b : a > b);
  }
  return truthy(v);
}


static int w_and (WP *p) {
  int r = w_term(p);
  for (;;) {
    wskip(p);
    if (p->s[0] != '&' || p->s[1] != '&') return r;
    p->s += 2;
    r = w_term(p) && r;
  }
}


static int w_or (WP *p) {
  int r = w_and(p);
  for (;;) {
    wskip(p);
    if (p->s[0] != '|' || p->s[1] != '|') return r;
    p->s += 2;
    r = w_and(p) || r;
  }
}


/* a when clause holds now (NULL or empty: always) */
int when_eval (const char *expr) {
  WP p;
  if (expr == NULL || *expr == '\0') return 1;
  p.s = expr;
  return w_or(&p);
}

/* }================================================================== */


/*
** {==================================================================
** The user's bindings
** ===================================================================
*/

static const Json *g_args;	/* the args of the binding found last */
static Json *g_args_own;


/* mme's own key for cmd (the menus'), as k1 / k2; 0: none */
static int default_key (int cmd, int *k2) {
  const char *s = cmd_default_keys(cmd);
  char one[64];
  const char *comma;
  *k2 = 0;
  if (s == NULL || *s == '\0') return 0;
  comma = strstr(s, ", ");	/* "Ctrl+P, Ctrl+E": the first */
  snprintf(one, sizeof(one), "%.*s", comma ? (int)(comma - s) : (int)strlen(s), s);
  return key_parse(one, k2);
}


static void show_keys (void) {	/* the menus show the user's keys */
  size_t i;
  char a[48], b[48], both[100];
  int c;
  for (c = 1; c < CMD_N; c++) cmd_set_keys(c, NULL);
  for (i = 0; i < g_nbind; i++)	/* a removed default: no key shown */
    if (g_bind[i].remove) cmd_set_keys(g_bind[i].cmd, "");
  for (i = 0; i < g_nbind; i++) {
    if (g_bind[i].remove) continue;
    key_name(g_bind[i].k1, 1, a, sizeof(a));
    if (g_bind[i].k2) {
      key_name(g_bind[i].k2, 1, b, sizeof(b));
      snprintf(both, sizeof(both), "%s %s", a, b);
    }
    else snprintf(both, sizeof(both), "%s", a);
    cmd_set_keys(g_bind[i].cmd, both);
  }
}


static void clear_all (void) {
  size_t i;
  for (i = 0; i < g_nbind; i++) {
    free(g_bind[i].when);
    free(g_bind[i].args);
  }
  g_nbind = 0;
}


static void add (int cmd, int k1, int k2, int remove, const char *when, const char *args) {
  Bind *b;
  if (g_nbind == g_capbind) {
    g_capbind = g_capbind ? g_capbind * 2 : 16;
    g_bind = (Bind *)xrealloc(g_bind, g_capbind * sizeof(Bind));
  }
  b = &g_bind[g_nbind++];
  b->cmd = cmd;
  b->k1 = k1;
  b->k2 = k2;
  b->remove = remove;
  b->when = when && *when ? xstrdup(when) : NULL;
  b->args = args ? xstrdup(args) : NULL;
}


/* one entry of a keybindings.json list; 0: not one mme can use */
static int add_json (const Json *e) {
  const char *key = json_str(json_get(e, "key"), NULL);
  const char *cmd = json_str(json_get(e, "command"), NULL);
  const Json *args = json_get(e, "args");
  int k1 = 0, k2 = 0, c, remove = 0;
  if (cmd == NULL) return 0;
  if (cmd[0] == '-') {	/* "-editor.action.rename": the default key goes */
    remove = 1;
    cmd++;
  }
  if ((c = cmd_by_id(cmd)) == CMD_NONE) return 0;
  if (key && (k1 = key_parse(key, &k2)) == 0) return 0;
  if (key == NULL && !remove) return 0;
  if (key == NULL) k1 = default_key(c, &k2);	/* every default of it: mme has one */
  {
    Buf b;
    buf_init(&b);
    if (args) {
      json_write(&b, args);
      buf_putc(&b, '\0');
    }
    add(c, k1, k2, remove, json_str(json_get(e, "when"), NULL), args ? b.s : NULL);
    buf_free(&b);
  }
  return 1;
}


/* reads keybindings.json; -1 when it is not good JSON */
int keys_load (void) {
  char *f = keys_path(), *s;
  size_t len, i;
  Json *j;
  if (f == NULL) {
    clear_all();
    show_keys();
    return 0;
  }
  s = read_file(f, &len);
  free(f);
  if (s == NULL) {
    clear_all();
    show_keys();
    return 0;
  }
  j = json_parse(s, len);
  free(s);
  if (j == NULL || j->type != J_ARR) {	/* the file is broken: the bindings there are stay, or keys_save wipes it */
    json_free(j);
    return -1;
  }
  clear_all();
  for (i = 0; i < j->n; i++) add_json(j->kid[i]);
  json_free(j);
  show_keys();
  return 0;
}


/*
** The command of k (after first, for a second key), the last entry whose
** when holds winning, like VS Code; -1: the first of two keys; KEYS_BLOCK:
** a "-command" entry took mme's own key away.
*/
int keys_find (int first, int k) {
  size_t i = g_nbind;
  int starts = 0, blocked = 0;
  while (i-- > 0) {
    const Bind *b = &g_bind[i];
    if (first ? !(b->k1 == first && b->k2 == k) : b->k1 != k) continue;
    if (!first && b->k2) {
      starts = 1;
      continue;
    }
    if (b->remove) {	/* the default of that command is not there: unless a later entry has the key */
      int d2, d1 = default_key(b->cmd, &d2);
      if (d1 == b->k1 && d2 == b->k2) blocked = 1;	/* another entry may still have the key */
      continue;
    }
    if (!when_eval(b->when)) continue;
    json_free(g_args_own);
    g_args_own = b->args ? json_parse(b->args, strlen(b->args)) : NULL;
    g_args = g_args_own;
    return b->cmd;
  }
  return starts ? -1 : blocked ? KEYS_BLOCK : CMD_NONE;
}


/* the args of the binding keys_find found last (NULL: none); keys_args_clear after its command ran */
const Json *keys_args (void) {
  return g_args;
}


void keys_args_clear (void) {
  g_args = NULL;
}


int keys_count (void) {
  return (int)g_nbind;
}


/* entry i: its command, keys, when; 1 when it is a "-command" removal */
int keys_entry (int i, int *cmd, int *k1, int *k2, const char **when) {
  const Bind *b = &g_bind[i];
  *cmd = b->cmd;
  *k1 = b->k1;
  *k2 = b->k2;
  *when = b->when;
  return b->remove;
}


int keys_default (int cmd, int *k2) {
  return default_key(cmd, k2);
}


/* the list written back to keybindings.json */
void keys_save (void) {
  size_t i;
  Buf b;
  char *f = keys_path(), name[48];
  int fd;
  buf_init(&b);
  buf_puts(&b, "// mme keybindings, like VS Code's keybindings.json\n[");
  for (i = 0; i < g_nbind; i++) {
    const Bind *e = &g_bind[i];
    buf_puts(&b, i ? ",\n  {" : "\n  {");
    if (e->k1) {
      buf_puts(&b, "\"key\": \"");
      key_name(e->k1, 0, name, sizeof(name));
      buf_puts(&b, name);
      if (e->k2) {
        key_name(e->k2, 0, name, sizeof(name));
        buf_printf(&b, " %s", name);
      }
      buf_puts(&b, "\", ");
    }
    buf_printf(&b, "\"command\": \"%s%s\"", e->remove ? "-" : "", cmd_id(e->cmd));
    if (e->when) {
      buf_puts(&b, ", \"when\": ");
      json_put_str(&b, e->when, strlen(e->when));
    }
    if (e->args) buf_printf(&b, ", \"args\": %s", e->args);
    buf_putc(&b, '}');
  }
  buf_puts(&b, "\n]\n");
  if (f && (fd = os_open(f, OS_WRITE)) >= 0) {
    os_write(fd, b.s, b.len);
    os_close(fd);
  }
  buf_free(&b);
  free(f);
  show_keys();
}


/* cmd gets the key(s) (its other user keys go, and the key's old command); written */
void keys_set (int cmd, int k1, int k2) {
  size_t i, n = 0;
  char *when = NULL;
  for (i = 0; i < g_nbind; i++) {
    Bind *b = &g_bind[i];
    if ((b->cmd == cmd && !b->remove) || (!b->remove && b->k1 == k1 && b->k2 == k2)) {
      if (b->cmd == cmd && when == NULL && b->when) when = xstrdup(b->when);	/* its when stays */
      free(b->when);
      free(b->args);
      continue;
    }
    g_bind[n++] = *b;
  }
  g_nbind = n;
  add(cmd, k1, k2, 0, when, NULL);
  free(when);
  keys_save();
}


/* entry i: another when (NULL: none) */
void keys_set_when (int i, const char *when) {
  free(g_bind[i].when);
  g_bind[i].when = when && *when ? xstrdup(when) : NULL;
  keys_save();
}


/* a user entry with mme's default key of cmd and a when: "Change When Expression" on a default */
void keys_add (int cmd, int k1, int k2, int remove, const char *when) {
  add(cmd, k1, k2, remove, when, NULL);
  keys_save();
}


void keys_del (int i) {
  free(g_bind[i].when);
  free(g_bind[i].args);
  memmove(g_bind + i, g_bind + i + 1, (g_nbind - (size_t)i - 1) * sizeof(Bind));
  g_nbind--;
  keys_save();
}


/* Reset Keybinding: the user's entries for cmd go, its default is back */
void keys_reset (int cmd) {
  size_t i, n = 0;
  for (i = 0; i < g_nbind; i++) {
    if (g_bind[i].cmd == cmd) {
      free(g_bind[i].when);
      free(g_bind[i].args);
      continue;
    }
    g_bind[n++] = g_bind[i];
  }
  g_nbind = n;
  keys_save();
}


/* VS Code's keybindings.json entries (a JSON list) added after mme's; how many were used */
int keys_import (const Json *list) {
  size_t i;
  int n = 0;
  if (list == NULL || list->type != J_ARR) return 0;
  for (i = 0; i < list->n; i++) n += add_json(list->kid[i]);
  if (n) keys_save();
  return n;
}

/* }================================================================== */
